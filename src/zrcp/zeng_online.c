/*
    ZEsarUX  ZX Second-Emulator And Released for UniX
    Copyright (C) 2013 Cesar Hernandez Bano

    This file is part of ZEsarUX.

    ZEsarUX is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

/*

Online Network Play (using a central server) Server related code

*/

/*

Snapshots:
-cuando se pide un snapshot desde un slave, se asigna memoria y se copia ahí el contenido del último snapshot.
Mientras se copia se incrementa un contador atómico, de tal manera que si hay dos conexiones pidiendo snapshot al mismo tiempo,
contador será 2 por ejemplo

-cuando un máster envía un slave, primero se envía a una memoria temporal. Y luego se enviará a la memoria de snapshot ,
antes esperando a que el contador atómico esté a 0. problema: puede estar a 0 pero cuando se vaya a enviar el nuevo snapshot,
puede entrar lectura de snapshot desde slave. como solventarlo?



Rooms:
Al crear una habitacion, se genera un password que es el que debe usarse siempre para hacer acciones sobre esa habitacion, como:
-establecer maximo de jugadores
-envio snapshot
-envio eventos teclado/joystick
Para obtener ese password, hay que unirse a la habitacion

Nota: el master hace:
-crear habitacion
-unirse a habitacion creada
-y en bucle: enviar snapshots

El slave hace:
-unirse a una habitacion que se selecione
-y en bucle: enviar eventos, obtener snapshots


TODO: cada nueva conexión a ZRCP ocupa 32MB, según este código en remote.c:
char *buffer_lectura_socket=malloc(MAX_LENGTH_PROTOCOL_COMMAND);

32 MB son muchos MB para cada conexión... con 100 conexiones nos comemos 3 GB. Como solventar esto?

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>



#include "cpu.h"
#include "debug.h"
#include "utils.h"
#include "network.h"
#include "compileoptions.h"
#include "zeng_online.h"
#include "remote.h"
#include "snap_zsf.h"
#include "autoselectoptions.h"
#include "ay38912.h"
#include "atomic.h"
#include "stats.h"
#include "textspeech.h"
#include "settings.h"
#include "zip.h"
#include "timer.h"





//Variables, estructuras,funciones etc que se pueden compilar aun sin soporte de pthreads

//Numero maximo de habitaciones para esta sesion que se pueden crear
//Interesa que este valor se pueda bajar o subir (pero no subir nunca mas alla de ZENG_ONLINE_MAX_ROOMS),
//porque segun la potencia del server se puede permitir mas o menos
int zeng_online_current_max_rooms=10;


//Maximo jugadores por habitacion por defecto
int zeng_online_current_max_players_per_room=ZENG_ONLINE_MAX_PLAYERS_PER_ROOM;

int zeng_online_enabled=0;



//Array de habitaciones en zeng online
struct zeng_online_room zeng_online_rooms_list[ZENG_ONLINE_MAX_ROOMS];

void zoc_begin_lock_joined_users(int room_number)
{
	while(z_atomic_test_and_set(&zeng_online_rooms_list[room_number].semaphore_joined_users)) {
		//printf("Esperando a liberar lock en zoc_begin_lock_joined_users\n");
	}
}

void zoc_end_lock_joined_users(int room_number)
{
    z_atomic_reset(&zeng_online_rooms_list[room_number].semaphore_joined_users);
}

void zoc_begin_lock_allowed_keys(int room_number)
{
	while(z_atomic_test_and_set(&zeng_online_rooms_list[room_number].semaphore_allowed_keys)) {
		//printf("Esperando a liberar lock en zoc_begin_lock_joined_users\n");
	}
}

void zoc_end_lock_allowed_keys(int room_number)
{
    z_atomic_reset(&zeng_online_rooms_list[room_number].semaphore_allowed_keys);
}

//Agregar usuario a la lista de joined_users
void zoc_add_user_to_joined_users(int room_number,char *nickname,char *uuid)
{
    //Para evitar escribir dos a la vez
	zoc_begin_lock_joined_users(room_number);

    int i;
    int agregado=0;

    for (i=0;i<zeng_online_rooms_list[room_number].max_players;i++) {
        if (zeng_online_rooms_list[room_number].joined_users_uuid[i][0]==0) {

            //Activar el ultimo tiempo de alive antes de agregarlo para que no expire al momento,
            //pues inicialmente el campo de tiempo es indefinido
            timer_stats_current_time(&zeng_online_rooms_list[room_number].joined_users_last_alive_time[i]);

            strcpy(zeng_online_rooms_list[room_number].joined_users[i],nickname);
            strcpy(zeng_online_rooms_list[room_number].joined_users_uuid[i],uuid);
            agregado=1;
            break; //para salir del bucle y liberar lock
        }
    }

    zeng_online_rooms_list[room_number].current_players++;

	//Liberar lock
	zoc_end_lock_joined_users(room_number);


    //Y si llega al final sin haber agregado usuario, es un error aunque no lo reportaremos
    if (!agregado) DBG_PRINT_ZENG_ONLINE VERBOSE_DEBUG,"ZENG Online: Reached maximum users on join_list names");



}

//Quita usuario de la lista de joined_users
//Retorna en nickname el nickname del usuario que se ha ido
void zoc_del_user_to_joined_users(int room_number,char *uuid,char *nickname)
{

    //Para evitar escribir dos a la vez
	zoc_begin_lock_joined_users(room_number);

    int i;
    int borrado=0;

    for (i=0;i<zeng_online_rooms_list[room_number].max_players;i++) {
        if (!strcmp(zeng_online_rooms_list[room_number].joined_users_uuid[i],uuid)) {
            strcpy(nickname,zeng_online_rooms_list[room_number].joined_users[i]);
            zeng_online_rooms_list[room_number].joined_users[i][0]=0;
            zeng_online_rooms_list[room_number].joined_users_uuid[i][0]=0;
            borrado=1;
            break; //para salir del bucle y liberar lock
        }
    }

    //Aunque nunca deberia ser <0, pero por si acaso
    if (zeng_online_rooms_list[room_number].current_players>0) {
        zeng_online_rooms_list[room_number].current_players--;
    }

	//Liberar lock
	zoc_end_lock_joined_users(room_number);

    //Y si llega al final sin haber encontrado usuario, es un error aunque no lo reportaremos
    if (!borrado) DBG_PRINT_ZENG_ONLINE VERBOSE_DEBUG,"ZENG Online: Can not find user with uuid %s to delete from join list",uuid);



}

void zeng_online_set_alive_user(int room_number,char *uuid)
{


    int i;
    int encontrado=0;

    for (i=0;i<zeng_online_rooms_list[room_number].max_players;i++) {
        if (!strcmp(zeng_online_rooms_list[room_number].joined_users_uuid[i],uuid)) {
            timer_stats_current_time(&zeng_online_rooms_list[room_number].joined_users_last_alive_time[i]);
            encontrado=1;
            DBG_PRINT_ZENG_ONLINE VERBOSE_DEBUG,"ZENG Online: Set alive time for user %s",uuid);
            break; //para salir del bucle y liberar lock
        }
    }


    //Y si llega al final sin haber encontrado usuario, es un error aunque no lo reportaremos
    if (!encontrado) DBG_PRINT_ZENG_ONLINE VERBOSE_DEBUG,"ZENG Online: Can not find user with uuid %s to set alive time",uuid);

}

void zeng_online_destroy_room_aux(int room_number)
{

    zeng_online_rooms_list[room_number].max_players=zeng_online_current_max_players_per_room;

    zeng_online_rooms_list[room_number].current_players=0;

    strcpy(zeng_online_rooms_list[room_number].name,"<free>");

    if (zeng_online_rooms_list[room_number].snapshot_memory!=NULL) {
        free(zeng_online_rooms_list[room_number].snapshot_memory);
    }

    zeng_online_rooms_list[room_number].autojoin_enabled=0;

    zeng_online_rooms_list[room_number].created=0;


}


void zeng_online_expire_non_alive_users(void)
{

    DBG_PRINT_ZENG_ONLINE VERBOSE_INFO,"ZENG Online: Expire non alive user");


    int room_number;

    struct timeval tiempo_ahora;
    timer_stats_current_time(&tiempo_ahora);

    long difftime;

    int i;

    for (room_number=0;room_number<zeng_online_current_max_rooms;room_number++) {
        if (zeng_online_rooms_list[room_number].created) {
            DBG_PRINT_ZENG_ONLINE VERBOSE_DEBUG,"ZENG Online: Looking at room %d",room_number);


            for (i=0;i<zeng_online_rooms_list[room_number].max_players;i++) {
                if (zeng_online_rooms_list[room_number].joined_users[i][0]) {


                    difftime=timer_stats_diference_time(&zeng_online_rooms_list[room_number].joined_users_last_alive_time[i],&tiempo_ahora);
                    //en microsegundos
                    difftime /=1000000;

                    DBG_PRINT_ZENG_ONLINE VERBOSE_DEBUG,"ZENG Online: Looking at user %s - %s. difftime=%ld",
                        zeng_online_rooms_list[room_number].joined_users_uuid[i],
                        zeng_online_rooms_list[room_number].joined_users[i],
                        difftime);

                    //ahora en segundos
                    if (difftime>ZOC_TIMEOUT_ALIVE_USER) {
                        DBG_PRINT_ZENG_ONLINE VERBOSE_INFO,"ZENG Online: Expiring user uuid %s",zeng_online_rooms_list[room_number].joined_users_uuid[i]);
                        char nickname_left[ZOC_MAX_NICKNAME_LENGTH+1];
                        //Por si acaso no lo encuentra
                        nickname_left[0]=0;
                        zoc_del_user_to_joined_users(room_number,zeng_online_rooms_list[room_number].joined_users_uuid[i],nickname_left);
                        //printf("Expired user nickname %s\n",nickname_left);
                    }
                }
            }
        }

    }



}

void zeng_online_destroy_empty_rooms(void)
{
    int room_number;

    DBG_PRINT_ZENG_ONLINE VERBOSE_INFO,"ZENG Online: Expire non alive rooms");

    int i;

    for (room_number=0;room_number<zeng_online_current_max_rooms;room_number++) {
        if (zeng_online_rooms_list[room_number].created) {
            DBG_PRINT_ZENG_ONLINE VERBOSE_DEBUG,"ZENG Online: Looking at room %d",room_number);


            int hay_jugadores_en_room=0;
            for (i=0;i<zeng_online_rooms_list[room_number].max_players && !hay_jugadores_en_room;i++) {
                if (zeng_online_rooms_list[room_number].joined_users[i][0]) hay_jugadores_en_room++;

            }

            if (!hay_jugadores_en_room) {
                DBG_PRINT_ZENG_ONLINE VERBOSE_INFO,"ZENG Online: There are no players on room %d. Destroying it",room_number);
                zeng_online_destroy_room_aux(room_number);
            }
        }

    }

}

int contador_timer_zeng_online_server=0;
//Aqui se llama desde el timer cada 1 segundo
void timer_zeng_online_server(void)
{
    if (remote_protocol_enabled.v==0) return;

    if (!zeng_online_enabled) return;

    //printf("timer_zeng_online_expire_non_alive_users\n");


    //Cada 60 segundos
    contador_timer_zeng_online_server++;

    if ((contador_timer_zeng_online_server % 60)!=0) return;

    //Expirar usuarios que no existan
    zeng_online_expire_non_alive_users();

    //Destruir rooms que no tengan usuarios
    if (zeng_online_destroy_rooms_without_players.v) zeng_online_destroy_empty_rooms();

}


int join_list_return_last_element(int room_number)
{

    int indice_inicial=zeng_online_rooms_list[room_number].index_waiting_join_first;
    int indice_final=indice_inicial+zeng_online_rooms_list[room_number].total_waiting_join;

    indice_final %=ZOC_MAX_JOIN_WAITING; //dar la vuelta al contador

    return indice_final;
}

void join_delete_first_element(int room_number)
{

    int indice_inicial=zeng_online_rooms_list[room_number].index_waiting_join_first;
    indice_inicial++;

    indice_inicial %=ZOC_MAX_JOIN_WAITING; //dar la vuelta al contador

    zeng_online_rooms_list[room_number].index_waiting_join_first=indice_inicial;
    zeng_online_rooms_list[room_number].total_waiting_join--;

}

//Agregar elemento join a lista
int join_list_add_element(int room_number,char *nickname)
{
    //Si llena la lista, esperar
    while (zeng_online_rooms_list[room_number].total_waiting_join==ZOC_MAX_JOIN_WAITING) {
        DBG_PRINT_ZENG_ONLINE VERBOSE_DEBUG,"ZENG Online: Waiting queue is full. let's wait");
        sleep(1);
    }

    //bloqueo de usuarios en lista desde aqui
    zoc_begin_lock_joined_users(room_number);



    //Obtener indice del siguiente
    int indice_final=join_list_return_last_element(room_number);
    zeng_online_rooms_list[room_number].join_waiting_list[indice_final].waiting=1;
    strcpy(zeng_online_rooms_list[room_number].join_waiting_list[indice_final].nickname,nickname);

    zeng_online_rooms_list[room_number].total_waiting_join++;

    //bloqueo hasta aqui
    zoc_end_lock_joined_users(room_number);

    return indice_final;



}

//Ver si el evento es valido, que no tenga restriccion de teclas para ese uuid
int zengonline_valid_event(int room_number,char *uuid,int tecla)
{
    /*
    //Perfiles de teclas permitidas en usuarios
    //Primer indice [] apunta al id de perfil.
    //Segundo indice [] apunta a la tecla; si vale 0, indica final de perfil. Final de perfil tambien indicado por el ultimo item al llenarse el array
    int allowed_keys[ZOC_MAX_KEYS_PROFILES][ZOC_MAX_KEYS_ITEMS];
    //Perfiles asignados a cada uuid. Si es "", no esta asignado
    char allowed_keys_assigned[ZOC_MAX_KEYS_PROFILES][STATS_UUID_MAX_LENGTH+1];
    */

    int i;

    for (i=0;i<ZOC_MAX_KEYS_PROFILES;i++) {
        //buscar primero el que corresponde al uuid. Solo valido 1 perfil máximo por uuid
        if (!strcmp(uuid,zeng_online_rooms_list[room_number].allowed_keys_assigned[i])) {
            //Ver si esa tecla esta en la lista. Lista finaliza con el ultimo item o cuando item es 0
            int j;
            for (j=0;j<ZOC_MAX_KEYS_ITEMS && zeng_online_rooms_list[room_number].allowed_keys[i][j];j++) {
                if (zeng_online_rooms_list[room_number].allowed_keys[i][j]==tecla) {
                    //printf("Tecla %d es valida para el uuid %s\n",tecla,uuid);
                    return 1;
                }
            }

            //no tecla valida
            //printf("Tecla %d NO es valida para el uuid %s\n",tecla,uuid);
            return 0;
        }
    }

    //printf("No hay restricción de tecla para el uuid %s\n",uuid);
    return 1;
}

//Agregar evento de tecla/joystick
void zengonline_add_event(int room_number,char *uuid,int tecla,int event_type,int nomenu)
{
    //Para evitar escribir dos a la vez
	while(z_atomic_test_and_set(&zeng_online_rooms_list[room_number].semaphore_events)) {
		//printf("Esperando a liberar lock en zengonline_add_event\n");
	}


    int index_event=zeng_online_rooms_list[room_number].index_event;
    zeng_online_rooms_list[room_number].events[index_event].tecla=tecla;
    zeng_online_rooms_list[room_number].events[index_event].pressrelease=event_type;
    zeng_online_rooms_list[room_number].events[index_event].nomenu=nomenu;
    strcpy(zeng_online_rooms_list[room_number].events[index_event].uuid,uuid);

    //Obtenemos siguiente indice
    index_event++;
    if (index_event>=ZENG_ONLINE_MAX_EVENTS) index_event=0;

    zeng_online_rooms_list[room_number].index_event=index_event;



	//Liberar lock
	z_atomic_reset(&zeng_online_rooms_list[room_number].semaphore_events);
}

//Obtiene el snapshot de una habitacion y mirando que no haya nadie escribiendo (o sea un put snapshot en curso)
//Es el problema tipico del readers-writers (aunque en mi caso solo tengo un writer)
//Quiero que puedan leer muchos simultaneamente, pero que solo pueda escribir cuando no hay nadie leyendo
//https://www.tutorialspoint.com/readers-writers-problem
int zengonline_get_snapshot(int room,z80_byte *destino)
{
    //Adquirir lock mutex
	while(z_atomic_test_and_set(&zeng_online_rooms_list[room].mutex_reading_snapshot)) {
		//printf("Esperando a liberar lock mutex_reading_snapshot en zengonline_put_snapshot\n");
	}

    //Incrementar contador de cuantos leen
    zeng_online_rooms_list[room].reading_snapshot_count++;
    if (zeng_online_rooms_list[room].reading_snapshot_count==1) {
        //Si es el primer lector, bloqueamos escritura

    	while(z_atomic_test_and_set(&zeng_online_rooms_list[room].semaphore_writing_snapshot)) {
		    //printf("Esperando a liberar lock semaphore_writing_snapshot en zengonline_get_snapshot\n");
	    }
    }

    //Liberar lock mutex
	z_atomic_reset(&zeng_online_rooms_list[room].mutex_reading_snapshot);

    int longitud_snapshot=zeng_online_rooms_list[room].snapshot_size;

    memcpy(destino,zeng_online_rooms_list[room].snapshot_memory,longitud_snapshot);

    //Adquirir lock mutex
	while(z_atomic_test_and_set(&zeng_online_rooms_list[room].mutex_reading_snapshot)) {
	    //printf("Esperando a liberar lock mutex_reading_snapshot en zengonline_put_snapshot\n");
	}


    zeng_online_rooms_list[room].reading_snapshot_count--;
    //Si somos el ultimo lector, liberar bloqueo escritura

    if (zeng_online_rooms_list[room].reading_snapshot_count==0) {
        z_atomic_reset(&zeng_online_rooms_list[room].semaphore_writing_snapshot);
    }

    //Liberar lock mutex
    z_atomic_reset(&zeng_online_rooms_list[room].mutex_reading_snapshot);

    return longitud_snapshot;


}

//Lo mueve de una memoria a la memoria del snapshot de esa habitacion
//Es el problema tipico del readers-writers (aunque en mi caso solo tengo un writer)
//Quiero que puedan leer muchos simultaneamente, pero que solo pueda escribir cuando no hay nadie leyendo
//https://www.tutorialspoint.com/readers-writers-problem
void zengonline_put_snapshot(int room,z80_byte *origen,int longitud)
{
    z80_byte *destino_snapshot;
    destino_snapshot=util_malloc(longitud,"Can not allocate memory for new snapshot");

    memcpy(destino_snapshot,origen,longitud);

    //Aqui llega la parte exclusiva, parte del problema de writer-readers
	//Adquirir lock
	while(z_atomic_test_and_set(&zeng_online_rooms_list[room].semaphore_writing_snapshot)) {
		//printf("Esperando a liberar lock semaphore_writing_snapshot en zengonline_put_snapshot\n");
	}


    //Aqui cambiamos el snapshot de la habitacion por ese otro
    if (zeng_online_rooms_list[room].snapshot_memory!=NULL) free(zeng_online_rooms_list[room].snapshot_memory);

    zeng_online_rooms_list[room].snapshot_memory=destino_snapshot;
    zeng_online_rooms_list[room].snapshot_size=longitud;
    //Incrementamos el id de snapshot
    zeng_online_rooms_list[room].snapshot_id++;


	//Liberar lock
	z_atomic_reset(&zeng_online_rooms_list[room].semaphore_writing_snapshot);

    //prueba
    /*
    int longitud_comprimido;
    z80_byte *comprimido=util_compress_memory_zip(destino_snapshot,longitud,&longitud_comprimido,"snapshot.zsf");

    printf("Snapshot uncompressed: %d compressed: %d\n",longitud,longitud_comprimido);

    int longitud_descomprimido;
    z80_byte *descomprimido=util_uncompress_memory_zip(comprimido,longitud_comprimido,&longitud_descomprimido,"snapshot.zsf");

    printf("Snapshot compressed: %d uncompressed: %d\n",longitud_comprimido,longitud_descomprimido);
    */

}

//Lo mueve de una memoria a la memoria del display de esa habitacion
void zengonline_streaming_put_display(int room,z80_byte *origen,int longitud,int slot)
{
    z80_byte *destino_display;
    destino_display=util_malloc(longitud,"Can not allocate memory for new display");

    memcpy(destino_display,origen,longitud);

    //Aqui llega la parte exclusiva, parte del problema de writer-readers
	//Adquirir lock
	while(z_atomic_test_and_set(&zeng_online_rooms_list[room].semaphore_writing_streaming_display)) {
		//printf("Esperando a liberar lock semaphore_writing_snapshot en zengonline_put_snapshot\n");
	}


    //Aqui cambiamos el display de la habitacion por ese otro
    if (zeng_online_rooms_list[room].streaming_display_slots_memory[slot]!=NULL) free(zeng_online_rooms_list[room].streaming_display_slots_memory[slot]);

    zeng_online_rooms_list[room].streaming_display_slots_memory[slot]=destino_display;
    zeng_online_rooms_list[room].streaming_display_slots_size[slot]=longitud;

    //printf("zengonline_streaming_put_display slot %d longitud %d\n",slot,longitud);


	//Liberar lock
	z_atomic_reset(&zeng_online_rooms_list[room].semaphore_writing_streaming_display);


}

int zengonline_streaming_get_display(int room,z80_byte *destino,int slot)
{
    //Adquirir lock mutex
	while(z_atomic_test_and_set(&zeng_online_rooms_list[room].mutex_reading_streaming_display)) {
		//printf("Esperando a liberar lock mutex_reading_streaming_display en zengonline_streaming_get_display\n");
	}

    //Incrementar contador de cuantos leen
    zeng_online_rooms_list[room].reading_streaming_display_count++;
    if (zeng_online_rooms_list[room].reading_streaming_display_count==1) {
        //Si es el primer lector, bloqueamos escritura

    	while(z_atomic_test_and_set(&zeng_online_rooms_list[room].semaphore_writing_streaming_display)) {
		    //printf("Esperando a liberar lock semaphore_writing_streaming_display en zengonline_streaming_get_display\n");
	    }
    }

    //Liberar lock mutex
	z_atomic_reset(&zeng_online_rooms_list[room].mutex_reading_streaming_display);

    int longitud_display=zeng_online_rooms_list[room].streaming_display_slots_size[slot];
    memcpy(destino,zeng_online_rooms_list[room].streaming_display_slots_memory[slot],longitud_display);


    //Adquirir lock mutex
	while(z_atomic_test_and_set(&zeng_online_rooms_list[room].mutex_reading_streaming_display)) {
	    //printf("Esperando a liberar lock mutex_reading_streaming_display en zengonline_streaming_get_display\n");
	}


    zeng_online_rooms_list[room].reading_streaming_display_count--;
    //Si somos el ultimo lector, liberar bloqueo escritura

    if (zeng_online_rooms_list[room].reading_streaming_display_count==0) {
        z_atomic_reset(&zeng_online_rooms_list[room].semaphore_writing_streaming_display);
    }

    //Liberar lock mutex
    z_atomic_reset(&zeng_online_rooms_list[room].mutex_reading_streaming_display);

    return longitud_display;


}


//Lo mueve de una memoria a la memoria del audio de esa habitacion
void zengonline_streaming_put_audio(int room,z80_byte *origen,int longitud)
{
    z80_byte *destino_audio;
    destino_audio=util_malloc(longitud,"Can not allocate memory for new audio");

    memcpy(destino_audio,origen,longitud);

    //Aqui llega la parte exclusiva, parte del problema de writer-readers
	//Adquirir lock
	while(z_atomic_test_and_set(&zeng_online_rooms_list[room].semaphore_writing_streaming_audio)) {
		//printf("Esperando a liberar lock semaphore_writing_snapshot en zengonline_put_snapshot\n");
	}


    //Aqui cambiamos el audio de la habitacion por ese otro
    if (zeng_online_rooms_list[room].streaming_audio_memory!=NULL) free(zeng_online_rooms_list[room].streaming_audio_memory);

    zeng_online_rooms_list[room].streaming_audio_memory=destino_audio;
    zeng_online_rooms_list[room].streaming_audio_size=longitud;


    //Incrementamos el id de audio
    zeng_online_rooms_list[room].audio_streaming_id++;

	//Liberar lock
	z_atomic_reset(&zeng_online_rooms_list[room].semaphore_writing_streaming_audio);


}

int zengonline_streaming_get_audio(int room,z80_byte *destino)
{
    //Adquirir lock mutex
	while(z_atomic_test_and_set(&zeng_online_rooms_list[room].mutex_reading_streaming_audio)) {
		//printf("Esperando a liberar lock mutex_reading_streaming_audio en zengonline_streaming_get_audio\n");
	}

    //Incrementar contador de cuantos leen
    zeng_online_rooms_list[room].reading_streaming_audio_count++;
    if (zeng_online_rooms_list[room].reading_streaming_audio_count==1) {
        //Si es el primer lector, bloqueamos escritura

    	while(z_atomic_test_and_set(&zeng_online_rooms_list[room].semaphore_writing_streaming_audio)) {
		    //printf("Esperando a liberar lock semaphore_writing_streaming_audio en zengonline_streaming_get_audio\n");
	    }
    }

    //Liberar lock mutex
	z_atomic_reset(&zeng_online_rooms_list[room].mutex_reading_streaming_audio);

    int longitud_audio=zeng_online_rooms_list[room].streaming_audio_size;
    memcpy(destino,zeng_online_rooms_list[room].streaming_audio_memory,longitud_audio);


    //Adquirir lock mutex
	while(z_atomic_test_and_set(&zeng_online_rooms_list[room].mutex_reading_streaming_audio)) {
	    //printf("Esperando a liberar lock mutex_reading_streaming_audio en zengonline_streaming_get_audio\n");
	}


    zeng_online_rooms_list[room].reading_streaming_audio_count--;
    //Si somos el ultimo lector, liberar bloqueo escritura

    if (zeng_online_rooms_list[room].reading_streaming_audio_count==0) {
        z_atomic_reset(&zeng_online_rooms_list[room].semaphore_writing_streaming_audio);
    }

    //Liberar lock mutex
    z_atomic_reset(&zeng_online_rooms_list[room].mutex_reading_streaming_audio);

    return longitud_audio;

}


void zoc_send_broadcast_message(int room_number,char *message)
{
    //No hace falta indicar room number dado que solo se mostraran mensajes de nuestro room
    strcpy(zeng_online_rooms_list[room_number].broadcast_message,message);

    zeng_online_rooms_list[room_number].broadcast_message_id++;

}

void init_zeng_online_rooms(void)
{

    DBG_PRINT_ZENG_ONLINE VERBOSE_INFO,"ZENG Online: Initializing ZENG Online rooms");

    int i;

    for (i=0;i<ZENG_ONLINE_MAX_ROOMS;i++) {
        zeng_online_rooms_list[i].created=0;
        zeng_online_rooms_list[i].max_players=zeng_online_current_max_players_per_room;
        zeng_online_rooms_list[i].current_players=0;
        strcpy(zeng_online_rooms_list[i].name,"<free>");
        zeng_online_rooms_list[i].snapshot_memory=NULL;
        zeng_online_rooms_list[i].snapshot_size=0;
        zeng_online_rooms_list[i].snapshot_id=0;
        zeng_online_rooms_list[i].reading_snapshot_count=0;
        zeng_online_rooms_list[i].index_event=0;
        zeng_online_rooms_list[i].index_waiting_join_first=0;
        zeng_online_rooms_list[i].total_waiting_join=0;

        zeng_online_rooms_list[i].streaming_enabled=0;

        int j;
        for (j=0;j<ZENG_ONLINE_DISPLAY_SLOTS;j++) {
            zeng_online_rooms_list[i].streaming_display_slots_memory[j]=NULL;
            zeng_online_rooms_list[i].streaming_display_slots_size[j]=0;
        }
        zeng_online_rooms_list[i].reading_streaming_display_count=0;

        zeng_online_rooms_list[i].streaming_audio_memory=NULL;
        zeng_online_rooms_list[i].streaming_audio_size=0;
        zeng_online_rooms_list[i].reading_streaming_audio_count=0;
        zeng_online_rooms_list[i].audio_streaming_id=0;

        z_atomic_reset(&zeng_online_rooms_list[i].mutex_reading_snapshot);
        z_atomic_reset(&zeng_online_rooms_list[i].semaphore_writing_snapshot);
        z_atomic_reset(&zeng_online_rooms_list[i].semaphore_events);
        z_atomic_reset(&zeng_online_rooms_list[i].semaphore_joined_users);
        z_atomic_reset(&zeng_online_rooms_list[i].semaphore_allowed_keys);

        z_atomic_reset(&zeng_online_rooms_list[i].mutex_reading_streaming_display);
        z_atomic_reset(&zeng_online_rooms_list[i].semaphore_writing_streaming_display);

        z_atomic_reset(&zeng_online_rooms_list[i].mutex_reading_streaming_audio);
        z_atomic_reset(&zeng_online_rooms_list[i].semaphore_writing_streaming_audio);

    }
}



void enable_zeng_online(void)
{
    DBG_PRINT_ZENG_ONLINE VERBOSE_INFO,"ZENG Online: Starting ZENG Online Server");
    zeng_online_enabled=1;
    //TODO: acciones adicionales al activarlo
}

void disable_zeng_online(void)
{
    DBG_PRINT_ZENG_ONLINE VERBOSE_INFO,"ZENG Online: Stopping ZENG Online Server");
    zeng_online_enabled=0;
    //TODO: acciones adicionales al desactivarlo
}

void zeng_online_set_room_name(int room,char *room_name)
{

    int i;

    //El nombre, excluyendo el 0 del final, y filtrando caracteres raros
    for (i=0;room_name[i] && i<ZENG_ONLINE_MAX_ROOM_NAME;i++) {
        z80_byte caracter=room_name[i];
        if (caracter<32 || caracter>126) caracter='?';

        zeng_online_rooms_list[room].name[i]=caracter;
    }

    //Y el 0 del final
    zeng_online_rooms_list[room].name[i]=0;

}


int zeng_online_get_last_used_room(void)
{

    int i;


    for (i=zeng_online_current_max_rooms;i>=0;i--) {
        if (zeng_online_rooms_list[i].created) return i;
    }

    return -1;


}

void zeng_online_assign_room_passwords(int room)
{

    int i;

    for (i=0;i<ZENG_ROOM_PASSWORD_LENGTH;i++) {
        //letras mayus de la A a la Z (26 caracteres)
        ay_randomize(0);

        int valor_random=randomize_noise[0];

        int letra=valor_random % 26;

        char caracter_password='A'+letra;

        zeng_online_rooms_list[room].user_password[i]=caracter_password;
    }

    for (i=0;i<ZENG_ROOM_PASSWORD_LENGTH;i++) {
        //letras mayus de la A a la Z (26 caracteres)
        ay_randomize(0);

        int valor_random=randomize_noise[0];

        int letra=valor_random % 26;

        char caracter_password='A'+letra;

        zeng_online_rooms_list[room].creator_password[i]=caracter_password;
    }

}

void zeng_online_create_room(int misocket,int room_number,char *room_name,int streaming_enabled)
{
    //comprobaciones
    if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
        escribir_socket_format(misocket,"ERROR. Room number beyond limit");
        return;
    }

    //Ver si esta libre
    if (zeng_online_rooms_list[room_number].created) {
        escribir_socket_format(misocket,"ERROR. Room is already created");
        return;
    }

    zeng_online_set_room_name(room_number,room_name);
    zeng_online_assign_room_passwords(room_number);

    //zeng_online_rooms_list[room_number].max_players=zeng_online_current_max_players_per_room;

    zeng_online_rooms_list[room_number].current_players=0;
    zeng_online_rooms_list[room_number].snapshot_memory=NULL;

    zeng_online_rooms_list[room_number].autojoin_enabled=0;

    zeng_online_rooms_list[room_number].streaming_enabled=streaming_enabled;

    zeng_online_rooms_list[room_number].broadcast_message_id=0;
    zeng_online_rooms_list[room_number].broadcast_message[0]=0;
    zeng_online_rooms_list[room_number].broadcast_messages_allowed=1;

    zeng_online_rooms_list[room_number].kicked_user[0]=0;

    //Inicializar lista de usuarios vacia
    int i;
    for (i=0;i<ZENG_ONLINE_MAX_PLAYERS_PER_ROOM;i++) {
        zeng_online_rooms_list[room_number].joined_users[i][0]=0;
        zeng_online_rooms_list[room_number].joined_users_uuid[i][0]=0;
    }

    //Inicializar perfiles de teclas
    for (i=0;i<ZOC_MAX_KEYS_PROFILES;i++) {
        zeng_online_rooms_list[room_number].allowed_keys[i][0]=0; //0 teclas
        zeng_online_rooms_list[room_number].allowed_keys_assigned[i][0]=0; //Asignado a nadie
    }

    //Prueba un perfil de usuario con teclas restringidas
    //strcpy(zeng_online_rooms_list[room_number].allowed_keys_assigned[0],"1697471703.603752");
    //zeng_online_rooms_list[room_number].allowed_keys[0][0]='a';
    //zeng_online_rooms_list[room_number].allowed_keys[0][1]='b';
    //zeng_online_rooms_list[room_number].allowed_keys[0][2]=0; //fin de lista de teclas


    zeng_online_rooms_list[room_number].created=1;

    //Retornar el creator password
    escribir_socket(misocket,zeng_online_rooms_list[room_number].creator_password);

}

void zeng_online_destroy_room(int misocket,int room_number)
{
    //comprobaciones
    if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
        escribir_socket_format(misocket,"ERROR. Room number beyond limit");
        return;
    }

    //Ver si esta libre
    if (zeng_online_rooms_list[room_number].created==0) {
        escribir_socket_format(misocket,"ERROR. Room is not created");
        return;
    }

    zeng_online_destroy_room_aux(room_number);


}

void zeng_online_parse_command(int misocket,int comando_argc,char **comando_argv,char *ip_source_address)
{
    //TODO: si el parse para un comando largo, como put-snapshot, fuese lento, habria que procesarlo diferente:
    //ir hasta el primer espacio, y no procesar los dos parametros

    if (comando_argc<1) {
        escribir_socket(misocket,"ERROR. Needs at least one parameter");
        return;
    }

    //Este comando_argc indicara los parametros del comando zengonline XXX, o sea,
    //zengonline enable : parametros 0
    //zengonline join 0: parametros 1
    comando_argc--;
    //printf("Parametros zeng online comando: %d\n",comando_argc);

    if (!strcmp(comando_argv[0],"is-enabled")) {
        escribir_socket_format(misocket,"%d",zeng_online_enabled);
    }

    else if (!strcmp(comando_argv[0],"enable")) {
        if (zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. Already enabled");
        }
        else {
            enable_zeng_online();
        }
    }

    else if (!strcmp(comando_argv[0],"disable")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. Already disabled");
        }
        else {
            disable_zeng_online();
        }
    }

    else if (!strcmp(comando_argv[0],"list-rooms")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        int i;

        escribir_socket(misocket,"N.  Created Autojoin Players Max Name\n");

        for (i=0;i<zeng_online_current_max_rooms;i++) {
            escribir_socket_format(misocket,"%3d %d       %d      %3d       %3d %s\n",
                i,
                zeng_online_rooms_list[i].created,
                zeng_online_rooms_list[i].autojoin_enabled,
                zeng_online_rooms_list[i].current_players,
                zeng_online_rooms_list[i].max_players,
                zeng_online_rooms_list[i].name
            );
        }
    }

    //create-room, cuando se crea desde menu, debe comprobar que no se retorna mensaje de ERROR, y/o mostrar el error al usuario
    else if (!strcmp(comando_argv[0],"create-room")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<3) {
            escribir_socket(misocket,"ERROR. Needs three parameters");
            return;
        }

        if (zeng_online_allow_room_creation_from_any_ip.v==0) {
            if (strcmp(ip_source_address,"127.0.0.1")) {
                //Realmente el mensaje seria: This server only allows localhost room creation
                //pero no queremos dar muchas pistas a un posible atacante
                escribir_socket(misocket,"ERROR. This server does not allow room creation");
                return;
            }
        }

        int room_number=parse_string_to_number(comando_argv[1]);
        int streaming_mode=parse_string_to_number(comando_argv[3]);
        zeng_online_create_room(misocket,room_number,comando_argv[2],streaming_mode);
    }



    //get-join-queue-size creator_pass n
    else if (!strcmp(comando_argv[0],"get-join-queue-size")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<2) {
            escribir_socket(misocket,"ERROR. Needs two parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar creator_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].creator_password)) {
            escribir_socket(misocket,"ERROR. Invalid creator password for that room");
            return;
        }


        escribir_socket_format(misocket,"%d",zeng_online_rooms_list[room_number].total_waiting_join);

    }

    //"get-join-first-element-queue creator_pass n  Gets the first element in join waiting queue on room n\n"
    else if (!strcmp(comando_argv[0],"get-join-first-element-queue")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<2) {
            escribir_socket(misocket,"ERROR. Needs two parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar creator_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].creator_password)) {
            escribir_socket(misocket,"ERROR. Invalid creator password for that room");
            return;
        }

        //si vacio, retornar "empty"
        if (zeng_online_rooms_list[room_number].total_waiting_join==0) {
            escribir_socket(misocket,"<empty>");
            return;
        }

        //bloqueo de esto
        zoc_begin_lock_joined_users(room_number);
        int indice_primero=zeng_online_rooms_list[room_number].index_waiting_join_first;
        char nickname[ZOC_MAX_NICKNAME_LENGTH+1];
        strcpy(nickname,zeng_online_rooms_list[room_number].join_waiting_list[indice_primero].nickname);
        //escribir_socket_format(misocket,"%s",zeng_online_rooms_list[room_number].join_waiting_list[indice_primero].nickname);

        //fin bloqueo
        zoc_end_lock_joined_users(room_number);

        escribir_socket_format(misocket,"%s",nickname);


    }

    //"authorize-join creator_pass n perm Authorize/deny permissions to client join to room n. perm are permissions, can be:\n"
    else if (!strcmp(comando_argv[0],"authorize-join")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<3) {
            escribir_socket(misocket,"ERROR. Needs three parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar creator_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].creator_password)) {
            escribir_socket(misocket,"ERROR. Invalid creator password for that room");
            return;
        }

        int id_authorization=zeng_online_rooms_list[room_number].index_waiting_join_first;
        int permissions_client=parse_string_to_number(comando_argv[3]);

        if (zeng_online_rooms_list[room_number].total_waiting_join==0) {
            escribir_socket(misocket,"ERROR. There's not any pending join authorization");
            return;
        }

        //bloqueo desde aqui
        zoc_begin_lock_joined_users(room_number);
        zeng_online_rooms_list[room_number].join_waiting_list[id_authorization].permissions=permissions_client;
        zeng_online_rooms_list[room_number].join_waiting_list[id_authorization].waiting=0;


        join_delete_first_element(room_number);
        zoc_end_lock_joined_users(room_number);

    }


    //set-max-players creator_pass n m   Define max-players (m) for room (n). Requires creator_pass of that room\n"
    else if (!strcmp(comando_argv[0],"set-max-players")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<3) {
            escribir_socket(misocket,"ERROR. Needs three parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].creator_password)) {
            escribir_socket(misocket,"ERROR. Invalid creator password for that room");
            return;
        }


        int max_players=parse_string_to_number(comando_argv[3]);

        if (max_players<1 || max_players>zeng_online_current_max_players_per_room) {
            escribir_socket(misocket,"ERROR. Max players beyond limit");
            return;
        }

        zeng_online_rooms_list[room_number].max_players=max_players;

    }

    //set-key-profile creator_pass n p k1 k2 k3 ...  Defines profile (p) keys for room n.\n"
    else if (!strcmp(comando_argv[0],"set-key-profile")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<3) {
            escribir_socket(misocket,"ERROR. Needs at least three parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].creator_password)) {
            escribir_socket(misocket,"ERROR. Invalid creator password for that room");
            return;
        }


        int profile_index=parse_string_to_number(comando_argv[3]);

        if (profile_index<0 || profile_index>=ZOC_MAX_KEYS_PROFILES) {
            escribir_socket(misocket,"ERROR. Profile index beyond limit");
            return;
        }

        int restantes_teclas=comando_argc-3;

        if (restantes_teclas>ZOC_MAX_KEYS_ITEMS) {
            escribir_socket(misocket,"ERROR. Too many keys for profile");
            return;
        }

        //zeng_online_rooms_list[room_number].max_players=max_players;
        int indice_tecla=4;


        int teclas_agregadas=0;

        zoc_begin_lock_allowed_keys(room_number);

        for (;restantes_teclas>0;restantes_teclas--,teclas_agregadas++,indice_tecla++) {
            zeng_online_rooms_list[room_number].allowed_keys[profile_index][teclas_agregadas]=parse_string_to_number(comando_argv[indice_tecla]);
            DBG_PRINT_ZENG_ONLINE VERBOSE_INFO,"ZENG Online: Adding %s to profile key %d",comando_argv[indice_tecla],profile_index);
        }

        //meter el 0 del final si el array no esta lleno
        if (teclas_agregadas<ZOC_MAX_KEYS_ITEMS) {
            zeng_online_rooms_list[room_number].allowed_keys[profile_index][teclas_agregadas]=0;
        }

        zoc_end_lock_allowed_keys(room_number);

    }

    //set-key-profile-assign creator_pass n p [uuid]
    else if (!strcmp(comando_argv[0],"set-key-profile-assign")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<3) {
            escribir_socket(misocket,"ERROR. Needs at least three parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].creator_password)) {
            escribir_socket(misocket,"ERROR. Invalid creator password for that room");
            return;
        }


        int profile_index=parse_string_to_number(comando_argv[3]);

        if (profile_index<0 || profile_index>=ZOC_MAX_KEYS_PROFILES) {
            escribir_socket(misocket,"ERROR. Profile index beyond limit");
            return;
        }

        zoc_begin_lock_allowed_keys(room_number);

        if (comando_argc==3) {
            //desasignar. uuid es cadena vacia
            zeng_online_rooms_list[room_number].allowed_keys_assigned[profile_index][0]=0;
        }

        else {
            strcpy(zeng_online_rooms_list[room_number].allowed_keys_assigned[profile_index],comando_argv[4]);
        }

        zoc_end_lock_allowed_keys(room_number);


    }

    //get-key-profile-assign creator_pass n p
    else if (!strcmp(comando_argv[0],"get-key-profile-assign")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<3) {
            escribir_socket(misocket,"ERROR. Needs three parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].creator_password)) {
            escribir_socket(misocket,"ERROR. Invalid creator password for that room");
            return;
        }


        int profile_index=parse_string_to_number(comando_argv[3]);

        if (profile_index<0 || profile_index>=ZOC_MAX_KEYS_PROFILES) {
            escribir_socket(misocket,"ERROR. Profile index beyond limit");
            return;
        }


        escribir_socket(misocket,zeng_online_rooms_list[room_number].allowed_keys_assigned[profile_index]);


    }

    //get-key-profile creator_pass n p Gets profile (p) keys for room n.\n"
    else if (!strcmp(comando_argv[0],"get-key-profile")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<3) {
            escribir_socket(misocket,"ERROR. Needs three parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].creator_password)) {
            escribir_socket(misocket,"ERROR. Invalid creator password for that room");
            return;
        }


        int profile_index=parse_string_to_number(comando_argv[3]);

        if (profile_index<0 || profile_index>=ZOC_MAX_KEYS_PROFILES) {
            escribir_socket(misocket,"ERROR. Profile index beyond limit");
            return;
        }

        int i;

        for (i=0;i<ZOC_MAX_KEYS_ITEMS && zeng_online_rooms_list[room_number].allowed_keys[profile_index][i];i++) {
            escribir_socket_format(misocket,"%d ",zeng_online_rooms_list[room_number].allowed_keys[profile_index][i]);
        }


    }

    //"set-autojoin creator_pass n p       Define permissions (p) for autojoin on room (n)
    else if (!strcmp(comando_argv[0],"set-autojoin")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<3) {
            escribir_socket(misocket,"ERROR. Needs three parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].creator_password)) {
            escribir_socket(misocket,"ERROR. Invalid creator password for that room");
            return;
        }


        int permissions=parse_string_to_number(comando_argv[3]);

        zeng_online_rooms_list[room_number].autojoin_enabled=1;
        zeng_online_rooms_list[room_number].autojoin_permissions=permissions;

    }

    //"set-allow-messages creator_pass n   Allows sending messages (allowed by default)\n"
    else if (!strcmp(comando_argv[0],"set-allow-messages")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<2) {
            escribir_socket(misocket,"ERROR. Needs two parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].creator_password)) {
            escribir_socket(misocket,"ERROR. Invalid creator password for that room");
            return;
        }


        zeng_online_rooms_list[room_number].broadcast_messages_allowed=1;

    }

    //"reset-allow-messages creator_pass n   Allows sending messages (allowed by default)\n"
    else if (!strcmp(comando_argv[0],"reset-allow-messages")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<2) {
            escribir_socket(misocket,"ERROR. Needs two parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].creator_password)) {
            escribir_socket(misocket,"ERROR. Invalid creator password for that room");
            return;
        }


        zeng_online_rooms_list[room_number].broadcast_messages_allowed=0;

    }

    //"streaming-is-enabled user_pass n             Tells if streaming mode is enabled or not on this room"
    else if (!strcmp(comando_argv[0],"streaming-is-enabled")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<2) {
            escribir_socket(misocket,"ERROR. Needs two parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].user_password)) {
            escribir_socket(misocket,"ERROR. Invalid user password for that room");
            return;
        }

        escribir_socket_format(misocket,"%d",zeng_online_rooms_list[room_number].streaming_enabled);

    }

    //"streaming-put-display creator_pass n s data
    else if (!strcmp(comando_argv[0],"streaming-put-display")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<4) {
            escribir_socket(misocket,"ERROR. Needs four parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].creator_password)) {
            escribir_socket(misocket,"ERROR. Invalid creator password for that room");
            return;
        }

        int slot=parse_string_to_number(comando_argv[3]);

        if (slot<0 || slot>=ZENG_ONLINE_DISPLAY_SLOTS) {
            escribir_socket_format(misocket,"ERROR. Slot number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].streaming_enabled) {
            escribir_socket(misocket,"ERROR. Streaming is not enabled for that room");
            return;
        }

        //longitud de la pantalla es la longitud del parametro snapshot /2 (porque viene en hexa)
        int longitud_pantalla=strlen(comando_argv[4])/2;

        if (longitud_pantalla<1) {
            escribir_socket(misocket,"ERROR. Received an empty display");
            return;
        }

        char *s=comando_argv[4];
        //int parametros_recibidos=0;

        z80_byte *buffer_destino;
        buffer_destino=malloc(longitud_pantalla);
        if (buffer_destino==NULL) cpu_panic("Can not allocate memory for streaming-put-display");


        z80_byte *destino=buffer_destino;
        while (*s) {
            *destino=(util_hex_nibble_to_byte(*s)<<4) | util_hex_nibble_to_byte(*(s+1));
            destino++;

            s++;
            if (*s) s++;
        }

        zengonline_streaming_put_display(room_number,buffer_destino,longitud_pantalla,slot);

        //printf("Putting display to slot %d\n",slot);

        free(buffer_destino);


    }


    //"streaming-get-display user_pass n  s             This command returns the streaming display from room n, returns ERROR if no display there. Requires user_pass\n"
    else if (!strcmp(comando_argv[0],"streaming-get-display")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<3) {
            escribir_socket(misocket,"ERROR. Needs three parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].user_password)) {
            escribir_socket(misocket,"ERROR. Invalid user password for that room");
            return;
        }

        int slot=parse_string_to_number(comando_argv[3]);

        if (slot<0 || slot>=ZENG_ONLINE_DISPLAY_SLOTS) {
            escribir_socket_format(misocket,"ERROR. Slot number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].streaming_enabled) {
            escribir_socket(misocket,"ERROR. Streaming is not enabled for that room");
            return;
        }

        if (!zeng_online_rooms_list[room_number].streaming_display_slots_size[slot]) {
            escribir_socket(misocket,"ERROR. There is no streaming display on this room");
            return;
        }



        z80_byte *puntero_display=util_malloc(ZRCP_GET_PUT_SNAPSHOT_MEM*2,"Can not allocate memory for streaming get display");

        //printf("get-display slot %d\n",slot);

        int longitud=zengonline_streaming_get_display(room_number,puntero_display,slot);

        //printf("display slot %d length %d\n",slot,longitud);


        int i;
        for (i=0;i<longitud;i++) {
            escribir_socket_format(misocket,"%02X",puntero_display[i]);
        }

        free(puntero_display);




    }


    //"streaming-put-audio creator_pass n data         Put a streaming audio on room n, requieres creator_pass for that room. Data must be hexadecimal characters without spaces\n"
    else if (!strcmp(comando_argv[0],"streaming-put-audio")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<3) {
            escribir_socket(misocket,"ERROR. Needs fothreeur parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].creator_password)) {
            escribir_socket(misocket,"ERROR. Invalid creator password for that room");
            return;
        }


        if (!zeng_online_rooms_list[room_number].streaming_enabled) {
            escribir_socket(misocket,"ERROR. Streaming is not enabled for that room");
            return;
        }

        //longitud de la pantalla es la longitud del parametro snapshot /2 (porque viene en hexa)
        int longitud_audio=strlen(comando_argv[3])/2;

        if (longitud_audio<1) {
            escribir_socket(misocket,"ERROR. Received an empty audio");
            return;
        }

        char *s=comando_argv[3];
        //int parametros_recibidos=0;

        z80_byte *buffer_destino;
        buffer_destino=util_malloc(longitud_audio,"Can not allocate memory for streaming-put-audio");


        z80_byte *destino=buffer_destino;
        while (*s) {
            *destino=(util_hex_nibble_to_byte(*s)<<4) | util_hex_nibble_to_byte(*(s+1));
            destino++;

            s++;
            if (*s) s++;
        }

        zengonline_streaming_put_audio(room_number,buffer_destino,longitud_audio);

        //printf("Putting audio to slot %d\n",slot);

        free(buffer_destino);


    }

    //"streaming-get-audio user_pass n                 This command returns the streaming audio from room n, returns ERROR if no audio there. Requires user_pass\n"
    //Nota: este comando no se usa , se usa streaming-get-audio-cont
    else if (!strcmp(comando_argv[0],"streaming-get-audio")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<2) {
            escribir_socket(misocket,"ERROR. Needs two parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].user_password)) {
            escribir_socket(misocket,"ERROR. Invalid user password for that room");
            return;
        }



        if (!zeng_online_rooms_list[room_number].streaming_enabled) {
            escribir_socket(misocket,"ERROR. Streaming is not enabled for that room");
            return;
        }

        if (!zeng_online_rooms_list[room_number].streaming_audio_size) {
            escribir_socket(misocket,"ERROR. There is no streaming audio on this room");
            return;
        }



        z80_byte *puntero_audio=util_malloc(ZRCP_GET_PUT_SNAPSHOT_MEM*2,"Can not allocate memory for streaming get audio");


        int longitud=zengonline_streaming_get_audio(room_number,puntero_audio);


        int i;
        for (i=0;i<longitud;i++) {
            escribir_socket_format(misocket,"%02X",puntero_audio[i]);
        }

        free(puntero_audio);

    }

    //"streaming-get-audio-cont user_pass n                 This command returns the streaming audio from room n, returns ERROR if no audio there. Requires user_pass\n"
    else if (!strcmp(comando_argv[0],"streaming-get-audio-cont")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<2) {
            escribir_socket(misocket,"ERROR. Needs two parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].user_password)) {
            escribir_socket(misocket,"ERROR. Invalid user password for that room");
            return;
        }



        if (!zeng_online_rooms_list[room_number].streaming_enabled) {
            escribir_socket(misocket,"ERROR. Streaming is not enabled for that room");
            return;
        }

        if (!zeng_online_rooms_list[room_number].streaming_audio_size) {
            escribir_socket(misocket,"ERROR. There is no streaming audio on this room");
            return;
        }



        z80_byte *puntero_audio=util_malloc(ZOC_STREAMING_AUDIO_BUFFER_SIZE,"Can not allocate memory for streaming get audio");

        int previous_id=-1;


        int errores_escribir=0;

        //Salir si falla x veces seguidas el envio de respuesta
        while (errores_escribir<30) {
            //printf("En bucle streaming-get-audio-cont %d\n",contador_segundo);
            if (zeng_online_rooms_list[room_number].audio_streaming_id!=previous_id) {
                //printf("Stream id ha cambiado\n");
                previous_id=zeng_online_rooms_list[room_number].audio_streaming_id;
                int longitud=zengonline_streaming_get_audio(room_number,puntero_audio);

                //printf("Antes escribir en socket\n");

                //Si el cliente se desconecta, con un leave room, se queda la operacion de escritura
                //congelada. Durante cuanto tiempo??

                //Si el cliente cierra ZEsarUX, el socket se corta y la operacion de escritura fallara

                int i;
                for (i=0;i<longitud;i++) {
                    escribir_socket_format(misocket,"%02X",puntero_audio[i]);
                }

                //Nota: solo vemos el error al escribir este \n
                //Se podria tambien ver el error del escribir_socket_format pero al final, si falla el socket,
                //fallan las dos escrituras, con comprobarlo aqui es suficiente
                int retorno_escribir=escribir_socket(misocket,"\n");

                if (retorno_escribir<=0) {
                    printf("Error returning data from command streaming-get-audio-cont. Retries: %d Source IP=%s\n",errores_escribir,ip_source_address);
                    errores_escribir++;

                    sleep(1);

                }

                else {
                    errores_escribir=0;
                }

                //printf("Despues escribir en socket\n");

            }

            else {
                //printf("Stream id NO ha cambiado\n");
                //TODO: parametro configurable
                usleep(1000); // (20 ms es un frame entero)
            }

        }

        printf("Exiting command streaming-get-audio-cont from server because the remote socket is not connected. Source IP=%s\n",ip_source_address);

        free(puntero_audio);


    }

    //"streaming-get-audio-id user_pass n              This command returns the last audio id from room n, returns ERROR if no audio there. Requires user_pass\n"
    else if (!strcmp(comando_argv[0],"streaming-get-audio-id")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<2) {
            escribir_socket(misocket,"ERROR. Needs two parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].user_password)) {
            escribir_socket(misocket,"ERROR. Invalid user password for that room");
            return;
        }

        if (!zeng_online_rooms_list[room_number].streaming_audio_size) {
            escribir_socket(misocket,"ERROR. There is no audio on this room");
            return;
        }



        escribir_socket_format(misocket,"%d",zeng_online_rooms_list[room_number].audio_streaming_id);



    }



    //kick creator_pass n uuid                        Kick user identified by uuid\n"
    else if (!strcmp(comando_argv[0],"kick")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<3) {
            escribir_socket(misocket,"ERROR. Needs three parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].creator_password)) {
            escribir_socket(misocket,"ERROR. Invalid creator password for that room");
            return;
        }


        strcpy(zeng_online_rooms_list[room_number].kicked_user,comando_argv[3]);

    }

       //send-message user_pass n nickname message
    else if (!strcmp(comando_argv[0],"send-message")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<4) {
            escribir_socket(misocket,"ERROR. Needs four parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].user_password)) {
            escribir_socket(misocket,"ERROR. Invalid user password for that room");
            return;
        }

        if (!zeng_online_rooms_list[room_number].broadcast_messages_allowed) {
            escribir_socket(misocket,"ERROR. Broadcast messages are not allowed in this room");
            return;
        }

        char broadcast_message[ZENG_ONLINE_MAX_BROADCAST_MESSAGE_SHOWN_LENGTH];

        //No hace falta indicar room number dado que solo se mostraran mensajes de nuestro room
        sprintf(broadcast_message,
            "Message from %s: %s",
            comando_argv[3],
            comando_argv[4]);
        zoc_send_broadcast_message(room_number,broadcast_message);


    }

    //"list-users user_pass n               Gets the list of joined users on room n\n"
    else if (!strcmp(comando_argv[0],"list-users")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<2) {
            escribir_socket(misocket,"ERROR. Needs two parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].user_password)) {
            escribir_socket(misocket,"ERROR. Invalid user password for that room");
            return;
        }

        int i;
        for (i=0;i<zeng_online_rooms_list[room_number].max_players;i++) {
            if (zeng_online_rooms_list[room_number].joined_users[i][0]) {
                escribir_socket_format(misocket,"%s\n",
                    zeng_online_rooms_list[room_number].joined_users[i]);
                escribir_socket_format(misocket,"%s\n",
                    zeng_online_rooms_list[room_number].joined_users_uuid[i]);
            }
        }


    }

    //get-kicked-user user_pass n                     This command returns the last kicked user, returning its uuid\n"
    else if (!strcmp(comando_argv[0],"get-kicked-user")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<2) {
            escribir_socket(misocket,"ERROR. Needs two parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].user_password)) {
            escribir_socket(misocket,"ERROR. Invalid user password for that room");
            return;
        }


        escribir_socket_format(misocket,"%s",zeng_online_rooms_list[room_number].kicked_user);

    }

    //"get-message-id user_pass n          Gets the broadcast message id from room\n"
    else if (!strcmp(comando_argv[0],"get-message-id")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<2) {
            escribir_socket(misocket,"ERROR. Needs two parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].user_password)) {
            escribir_socket(misocket,"ERROR. Invalid user password for that room");
            return;
        }

        escribir_socket_format(misocket,"%d",zeng_online_rooms_list[room_number].broadcast_message_id);

    }
    //"get-message user_pass n             Gets the broadcast message from room\n"
    else if (!strcmp(comando_argv[0],"get-message")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<2) {
            escribir_socket(misocket,"ERROR. Needs two parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].user_password)) {
            escribir_socket(misocket,"ERROR. Invalid user password for that room");
            return;
        }

        escribir_socket(misocket,zeng_online_rooms_list[room_number].broadcast_message);

    }


    //"send-keys user_pass n uuid key event nomenu   Simulates sending key press/release to room n.\n"
    else if (!strcmp(comando_argv[0],"send-keys")) {
        //printf("Server receives send-keys\n");
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<6) {
            escribir_socket(misocket,"ERROR. Needs six parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].user_password)) {
            escribir_socket(misocket,"ERROR. Invalid user password for that room");
            return;
        }


        //uuid key event nomenu

		int tecla=parse_string_to_number(comando_argv[4]);
		int event_type=parse_string_to_number(comando_argv[5]);
		int nomenu=parse_string_to_number(comando_argv[6]);

        if (zengonline_valid_event(room_number,comando_argv[3],tecla)) {
            //printf("Adding valid event tecla %d\n",tecla);
            zengonline_add_event(room_number,comando_argv[3],tecla,event_type,nomenu);
        }


    }

    //get-keys user_pass n
    else if (!strcmp(comando_argv[0],"get-keys")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<2) {
            escribir_socket(misocket,"ERROR. Needs two parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].user_password)) {
            escribir_socket(misocket,"ERROR. Invalid user password for that room");
            return;
        }

        //Siempre empiezo desde la posicion actual del buffer
        int indice_lectura=zeng_online_rooms_list[room_number].index_event;

        //TODO: ver posible manera de salir de aqui??
        while (1) {
            if (zeng_online_rooms_list[room_number].index_event==indice_lectura) {
                //Esperar algo.
                //TODO: parametro configurable
                usleep(1000); // (20 ms es un frame entero)
            }
            else {
                //Retornar evento de la lista
                //Returned format is: # uuid key event nomenu #". Los # inicial y final es para validar que se recibe bien
                int escritos=escribir_socket_format(misocket,"# %s %d %d %d #\n",
                    zeng_online_rooms_list[room_number].events[indice_lectura].uuid,
                    zeng_online_rooms_list[room_number].events[indice_lectura].tecla,
                    zeng_online_rooms_list[room_number].events[indice_lectura].pressrelease,
                    zeng_online_rooms_list[room_number].events[indice_lectura].nomenu
                );

                /*printf("Retorno key-keys: %s %d %d %d\n",
                    zeng_online_rooms_list[room_number].events[indice_lectura].uuid,
                    zeng_online_rooms_list[room_number].events[indice_lectura].tecla,
                    zeng_online_rooms_list[room_number].events[indice_lectura].pressrelease,
                    zeng_online_rooms_list[room_number].events[indice_lectura].nomenu
                );*/

                indice_lectura++;
                if (indice_lectura>=ZENG_ONLINE_MAX_EVENTS) {
                    indice_lectura=0;
                }

                //printf("Escritos socket: %d\n",escritos);
                if (escritos<0) {
                    DBG_PRINT_ZENG_ONLINE VERBOSE_DEBUG,"ZENG Online: Error returning zeng-online get-keys. Client connection may be closed");
                    return;
                }

            }
        }


    }

    //"get-snapshot user_pass n          Get a snapshot from room n, returns ERROR if no snapshot there. Requires user_pass\n"
    else if (!strcmp(comando_argv[0],"get-snapshot")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<2) {
            escribir_socket(misocket,"ERROR. Needs two parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].user_password)) {
            escribir_socket(misocket,"ERROR. Invalid user password for that room");
            return;
        }

        if (!zeng_online_rooms_list[room_number].snapshot_size) {
            escribir_socket(misocket,"ERROR. There is no snapshot on this room");
            return;
        }



        z80_byte *puntero_snapshot=util_malloc(ZRCP_GET_PUT_SNAPSHOT_MEM*2,"Can not allocate memory for get snapshot");

        int longitud=zengonline_get_snapshot(room_number,puntero_snapshot);


        int i;
        for (i=0;i<longitud;i++) {
            escribir_socket_format(misocket,"%02X",puntero_snapshot[i]);
        }

        free(puntero_snapshot);




    }

    else if (!strcmp(comando_argv[0],"get-snapshot-id")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<2) {
            escribir_socket(misocket,"ERROR. Needs two parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].user_password)) {
            escribir_socket(misocket,"ERROR. Invalid user password for that room");
            return;
        }

        if (!zeng_online_rooms_list[room_number].snapshot_size) {
            escribir_socket(misocket,"ERROR. There is no snapshot on this room");
            return;
        }



        escribir_socket_format(misocket,"%d",zeng_online_rooms_list[room_number].snapshot_id);





    }

    //"destroy-room creator_pass n         Destroys room n\n"
    else if (!strcmp(comando_argv[0],"destroy-room")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<2) {
            escribir_socket(misocket,"ERROR. Needs two parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].creator_password)) {
            escribir_socket(misocket,"ERROR. Invalid creator password for that room");
            return;
        }

        zeng_online_destroy_room(misocket,room_number);
    }

    else if (!strcmp(comando_argv[0],"reset-autojoin")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<2) {
            escribir_socket(misocket,"ERROR. Needs two parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].creator_password)) {
            escribir_socket(misocket,"ERROR. Invalid creator password for that room");
            return;
        }

        zeng_online_rooms_list[room_number].autojoin_enabled=0;
    }

    //"put-snapshot creator_pass n data
    else if (!strcmp(comando_argv[0],"put-snapshot")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<3) {
            escribir_socket(misocket,"ERROR. Needs three parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass. comando_argv[1]
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].creator_password)) {
            escribir_socket(misocket,"ERROR. Invalid creator password for that room");
            return;
        }

        //longitud del snapshot es la longitud del parametro snapshot /2 (porque viene en hexa)
        int longitud_snapshot=strlen(comando_argv[3])/2;

        if (longitud_snapshot<1) {
            escribir_socket(misocket,"ERROR. Received an empty snapshot");
            return;
        }

        char *s=comando_argv[3];
        //int parametros_recibidos=0;

        z80_byte *buffer_destino;
        buffer_destino=malloc(longitud_snapshot);
        if (buffer_destino==NULL) cpu_panic("Can not allocate memory for put-snapshot");

        //z80_byte valor;


        //Se usa un bucle mucho mas rapido que si se usase parse_string_to_number
        //tiempo mas bajo usando version "lenta" del bucle: 52 microsec
        //usando version rapida: 7 microsec


        /*while (*s) {
            char buffer_valor[4];
            buffer_valor[0]=s[0];
            buffer_valor[1]=s[1];
            buffer_valor[2]='H';
            buffer_valor[3]=0;
            //printf ("%s\n",buffer_valor);
            //TODO: quiza este parse como es continuo se puede acelerar de alguna manera
            valor=parse_string_to_number(buffer_valor);
            //printf ("valor: %d\n",valor);

            buffer_destino[parametros_recibidos++]=valor;
            //menu_debug_write_mapped_byte(direccion++,valor);

            s++;
            if (*s) s++;
        }*/

        z80_byte *destino=buffer_destino;
        while (*s) {
            *destino=(util_hex_nibble_to_byte(*s)<<4) | util_hex_nibble_to_byte(*(s+1));
            destino++;

            s++;
            if (*s) s++;
        }

        zengonline_put_snapshot(room_number,buffer_destino,longitud_snapshot);

        free(buffer_destino);


    }

    //leave n user_pass uuid
    //TODO: no hacemos nada con el nickname, solo mostrarlo en footer
    else if (!strcmp(comando_argv[0],"leave")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<3) {
            escribir_socket(misocket,"ERROR. Needs three parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[1]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass.
        if (strcmp(comando_argv[2],zeng_online_rooms_list[room_number].user_password)) {
            escribir_socket(misocket,"ERROR. Invalid user password for that room");
            return;
        }


        //TODO: seguro que hay que hacer mas cosas en el leave...


        //comando_argv[3] contiene el uuid
        char nickname_left[ZOC_MAX_NICKNAME_LENGTH+1];
        //Por si acaso no lo encuentra
        nickname_left[0]=0;
        zoc_del_user_to_joined_users(room_number,comando_argv[3],nickname_left);


        //Y notificarlo con un broadcast message
        char broadcast_message[ZENG_ONLINE_MAX_BROADCAST_MESSAGE_SHOWN_LENGTH];
        sprintf(broadcast_message,"Left %s",nickname_left);
        zoc_send_broadcast_message(room_number,broadcast_message);


    }

    //alive user_pass n uuid
    else if (!strcmp(comando_argv[0],"alive")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<3) {
            escribir_socket(misocket,"ERROR. Needs three parameters");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[2]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        //validar user_pass.
        if (strcmp(comando_argv[1],zeng_online_rooms_list[room_number].user_password)) {
            escribir_socket(misocket,"ERROR. Invalid user password for that room");
            return;
        }



        //comando_argv[3] contiene el uuid
        zeng_online_set_alive_user(room_number,comando_argv[3]);


    }

    //join n. Aunque el master requiere join tambien, no necesita el user_password que le retorna, pero debe leerlo
    //para quitar esa respuesta del socket
    // "join n nickname uuid [creator_pass]
    else if (!strcmp(comando_argv[0],"join")) {
        if (!zeng_online_enabled) {
            escribir_socket(misocket,"ERROR. ZENG Online is not enabled");
            return;
        }

        if (comando_argc<3) {
            escribir_socket(misocket,"ERROR. Needs three parameters at least");
            return;
        }

        int room_number=parse_string_to_number(comando_argv[1]);

        if (room_number<0 || room_number>=zeng_online_current_max_rooms) {
            escribir_socket_format(misocket,"ERROR. Room number beyond limit");
            return;
        }

        if (!zeng_online_rooms_list[room_number].created) {
            escribir_socket(misocket,"ERROR. Room is not created");
            return;
        }

        if (zeng_online_rooms_list[room_number].current_players >= zeng_online_rooms_list[room_number].max_players) {
            escribir_socket(misocket,"ERROR. Maximum players in that room reached");
            return;
        }

        int permissions;

        //Si tiene 4 parámetros, el cuarto es creator_pass para no necesitar autorización del master
        if (comando_argc>3) {
            if (strcmp(comando_argv[4],zeng_online_rooms_list[room_number].creator_password)) {
                escribir_socket(misocket,"ERROR. Invalid creator password for that room");
                return;
            }
            //Damos casi todos permisos al master, excepto read snapshot
            permissions=ZENG_ONLINE_PERMISSIONS_ALL_MASTER;

        }

        else {
            //Esperar hasta recibir autorización del master
            //Ver si hay autojoin
            if (zeng_online_rooms_list[room_number].autojoin_enabled) {
                DBG_PRINT_ZENG_ONLINE VERBOSE_DEBUG,"ZENG Online: Autojoin for room %d is enabled with permissions %d",room_number,
                zeng_online_rooms_list[room_number].autojoin_permissions);
                permissions=zeng_online_rooms_list[room_number].autojoin_permissions;

            }

            else {

                DBG_PRINT_ZENG_ONLINE VERBOSE_DEBUG,"ZENG Online: Waiting to receive authorization from master");
                int id_authorization=join_list_add_element(room_number,comando_argv[2]); //nickname agregado a la lista

                int timeout=60;
                while (zeng_online_rooms_list[room_number].join_waiting_list[id_authorization].waiting && timeout) {
                    DBG_PRINT_ZENG_ONLINE VERBOSE_DEBUG,"ZENG Online: Waiting authorization...");
                    sleep(1);
                    timeout--;
                }
                if (zeng_online_rooms_list[room_number].join_waiting_list[id_authorization].waiting) {
                    escribir_socket(misocket,"ERROR. Timeout waiting for client join authorization");
                    return;
                }

                permissions=zeng_online_rooms_list[room_number].join_waiting_list[id_authorization].permissions;

            }

            DBG_PRINT_ZENG_ONLINE VERBOSE_DEBUG,"ZENG Online: Permissions for this join: %d",permissions);
            //Si permisos 0 , denegado join
            if (permissions==0) {
                escribir_socket(misocket,"ERROR. You are not authorized to join");
                return;
            }


            //estos permisos ya es reponsabilidad de ZEsarUX hacerle caso desde las funciones cliente
            //el servidor no va a comprobar dichos permisos, seria demasiado lio (y logicamente esto es un pequeño problema de seguridad)
            //ZEsarUX desde el menu hace el join, obtiene los permisos que le retorna el servidor, y tiene que ser
            //fiel en recibir o no snapshot, en enviar o no teclas, etc


        }



        zoc_add_user_to_joined_users(room_number,comando_argv[2],comando_argv[3]);

        //Y retornamos el user_password, permissions y streaming mode
        escribir_socket_format(misocket,"%s %d %d",
            zeng_online_rooms_list[room_number].user_password,permissions,zeng_online_rooms_list[room_number].streaming_enabled);


        //Y notificarlo con un broadcast message
        char broadcast_message[ZENG_ONLINE_MAX_BROADCAST_MESSAGE_SHOWN_LENGTH];
        sprintf(broadcast_message,"Joined %s",comando_argv[2]);
        zoc_send_broadcast_message(room_number,broadcast_message);


    }

    //TODO comandos
    //
    //delete-room: para una room concreta, requiere creator_password.
    //leave: para una room concreta, requiere que este creada, requiere user_password. hay que asegurarse que el leave
    //       es de esta conexion, y no de una nueva. como controlar eso??? el leave decrementara el numero de jugadores conectados, logicamente

    else {
        escribir_socket(misocket,"ERROR. Invalid command for zeng-online");
        return;
    }
}


