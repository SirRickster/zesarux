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

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cpu.h"
#include "hilow_datadrive_audio.h"
#include "hilow_datadrive.h"

long long int hilow_read_audio_tamanyo_archivo_audio;

//Puntero a los datos del audio leido
z80_byte *hilow_read_audio_read_hilow_memoria_audio;

//Puntero a los datos del archivo de salida ddh
z80_byte *hilow_read_audio_hilow_ddh=NULL;





int hilow_read_audio_modo_verbose=0;

int hilow_read_audio_modo_verbose_extra=0;

int hilow_read_audio_directo_a_pista=0;

int hilow_read_audio_ejecutar_sleep=0;

int hilow_read_audio_invertir_senyal=0;

int hilow_read_audio_autocorrect=0;


//bytes leidos dentro del sector. Solo identificativos para programa externo que usa esta funcion
int hilow_read_audio_lee_sector_bytes_leidos=0;


int hilow_read_audio_leer_cara_dos=0;

int hilow_read_audio_autoajustar_duracion_bits=0;

//Para descartar ruido
int hilow_read_audio_minimo_variacion=10;


//a 44100 Hz
#define HILOW_READ_AUDIO_LONGITUD_ONDA_INICIO_BITS 367
#define HILOW_READ_AUDIO_LONGITUD_ONDA_INICIO_BITS_FLANCO_BAJADA 180
#define HILOW_READ_AUDIO_LONGITUD_ONDA_INICIO_BITS_MARGEN 40

//los 5 bytes indicadores de sector
z80_byte hilow_read_audio_buffer_sector_five_byte[5];

//Leer los 5 bytes indicadores de sector
z80_byte hilow_read_audio_buffer_label[17];

z80_byte hilow_read_audio_buffer_result[HILOW_SECTOR_SIZE+1];


//Callback para cada byte de audio leido, poder llamar a una funcion externa
hilow_read_audio_callback_function hilow_read_audio_byteread_callback=NULL;


//Callback para cada byte de output generado, poder llamar a una funcion externa
hilow_read_audio_callback_function hilow_read_audio_byte_output_write_callback=NULL;

//Callback para cada bit de output generado, poder llamar a una funcion externa
hilow_read_audio_callback_function hilow_read_audio_bit_output_write_callback=NULL;

//Callback para senyal de sync detectada mas pequenya de lo normal
hilow_read_audio_callback_function hilow_read_audio_probably_sync_error_callback=NULL;

//Para indicar en que momento esta
int hilow_read_audio_current_phase=HILOW_READ_AUDIO_PHASE_NONE;

//Para guardar los bytes de final de sector
z80_byte hilow_read_audio_buffer_end_sector[HILOW_LONGITUD_FINAL_SECTOR];


void hilow_read_audio_reset_buffer_label(void)
{
    int i;

    for (i=0;i<17;i++) hilow_read_audio_buffer_label[i]=0;
}

void hilow_read_audio_reset_buffer_sector_five_byte(void)
{
    hilow_read_audio_buffer_sector_five_byte[0]=0;
    hilow_read_audio_buffer_sector_five_byte[1]=0;
    hilow_read_audio_buffer_sector_five_byte[2]=0;
    hilow_read_audio_buffer_sector_five_byte[3]=0;
    hilow_read_audio_buffer_sector_five_byte[4]=0;

}

void hilow_read_audio_pausa(int segundos)
{
    if (hilow_read_audio_ejecutar_sleep) sleep(segundos);
}

int hilow_read_audio_lee_byte_memoria(int posicion)
{
    if (posicion<0 || posicion>=hilow_read_audio_tamanyo_archivo_audio) {
        //TODO: mejorar esto, no finalizar sino retornar fin de memoria a la rutina que llama
        //printf("Out of range %d\n",posicion);
        return -1;
    }

    int valor=hilow_read_audio_read_hilow_memoria_audio[posicion];

    //Invertir
    if (hilow_read_audio_invertir_senyal) valor=255-valor;

    if (hilow_read_audio_byteread_callback!=NULL) {
        hilow_read_audio_byteread_callback(valor,posicion);
    }

    return valor;
}


//Devolver valor sin signo
int hilow_read_audio_get_absolute(int valor)
{
        if (valor<0) valor=-valor;

        return valor;
}

//Dice la duracion de una onda, asumiendo:
//subimos - bajamos - y empezamos a subir

int hilow_read_audio_duracion_onda(int posicion,int *duracion_flanco_bajada)
{


    z80_byte valor_anterior=hilow_read_audio_lee_byte_memoria(posicion);
    int direccion=+1;

    *duracion_flanco_bajada=0;

    //int duracion_variacion=0;

    int flag_menor=0;
    int flag_mayor=0;
    int posmarca;
    int inicio_bajada_pos;
    z80_byte vmarca;

    int pos_inicio=posicion;

    do {
        //printf("%d ",direccion);
        int valor_leido=hilow_read_audio_lee_byte_memoria(posicion);
        if (hilow_read_audio_modo_verbose_extra) printf("V%d ",valor_leido);
        if (valor_leido==-1) {
            //fin de archivo
            return -1;
        }


/*
La idea es ver, que cuando haya una variacion en el sentido contrario al que vamos, sea lo suficientemente mayor
como para interpretar que ahí hay un pico (si es abajo, un "valle" de onda, y si es arriba, una "cresta" de onda)
Si no es suficientemente mayor, esto evitar variaciones de ruido en la señal


flag_menor indica si se ha detectado un momento en que el pico va de bajada, cuando estamos subiendo
flag_mayor indica si se ha detectado un momento en que el pico va de subida, cuando estamos bajando
posmarca indica desde que posicion se ha detectado un pico
vmarca contiene el valor del sample de audio en el momento que se detecta un pico
Nota: flag_menor y flag_mayor podrian ser realmente la misma variable, pero se deja en dos separadas para que quede mas claro
lo que hace el algoritmo


Si subimos.
-si flag_menor=0 y si valor leido es menor indicar hemos leido un valor menor. flag_menor=1. apuntar posicion (posmarca). apuntar valor leido anterior (vmarca)

-si flag_menor=1 y valor leido>vmarca, resetear flag_menor
-si flag_menor=1 y valor leido<vmarca y hay mas de un umbral (hilow_read_audio_minimo_variacion) apuntar en inicio_bajada posicion posmarca. pasar a bajada


Si bajamos.
-si flag_mayor=0 y si valor leido es mayor indicar hemos leido un valor mayor. flag_mayor=1. apuntar posicion (posmarca). apuntar valor leido anterior (vmarca)

-si flag_mayor=1 y valor leido<vmarca, resetear flag_mayor
-si flag_mayor=1 y valor leido>vmarca y hay mas de un umbral (hilow_read_audio_minimo_variacion) retornar posmarca

*/

        //subimos
        if (direccion==+1) {
            //vemos si bajamos
            if (!flag_menor) {
                if (valor_leido<valor_anterior) {
                    flag_menor=1;

                    posmarca=posicion;
                    vmarca=valor_anterior;
                }
            }

            else {
                if (valor_leido>vmarca) flag_menor=0;

                if (valor_leido<vmarca) {
                    int delta=hilow_read_audio_get_absolute(valor_leido-vmarca);
                    if (delta>hilow_read_audio_minimo_variacion) {
                        inicio_bajada_pos=posmarca;
                        direccion=-1;
                    }
                }
            }
        }

        //bajamos
        else {
            //vemos si subimos
            if (!flag_mayor) {
                if (valor_leido>valor_anterior) {
                    flag_mayor=1;

                    posmarca=posicion;
                    vmarca=valor_anterior;
                }
            }

            else {
                if (valor_leido<vmarca) flag_mayor=0;

                if (valor_leido>vmarca) {
                    int delta=hilow_read_audio_get_absolute(valor_leido-vmarca);
                    if (delta>hilow_read_audio_minimo_variacion) {
                        (*duracion_flanco_bajada)=posmarca-inicio_bajada_pos;

                        int duracion=posmarca-pos_inicio;
                        return duracion;
                    }
                }
            }
        }

        posicion++;
        valor_anterior=valor_leido;


    } while (1);
}




int hilow_read_audio_buscar_onda_inicio_bits(int posicion)
{


    int duracion_flanco_bajada;
    int duracion;

    do {

        duracion=hilow_read_audio_duracion_onda(posicion,&duracion_flanco_bajada);

        //printf("duracion %d flanco bajada %d pos_antes %d\n",duracion,duracion_flanco_bajada,posicion);

        if (duracion_flanco_bajada>=HILOW_READ_AUDIO_LONGITUD_ONDA_INICIO_BITS_FLANCO_BAJADA-HILOW_READ_AUDIO_LONGITUD_ONDA_INICIO_BITS_MARGEN &&
            duracion_flanco_bajada<=HILOW_READ_AUDIO_LONGITUD_ONDA_INICIO_BITS_FLANCO_BAJADA+HILOW_READ_AUDIO_LONGITUD_ONDA_INICIO_BITS_MARGEN) {
                //TODO: puede ser -1??
                return posicion+duracion;
            }

        if (duracion!=-1) posicion +=duracion;

    } while (posicion!=-1 && duracion!=-1);

    return -1;
}

int hilow_read_audio_buscar_dos_sync_bits(int posicion)
{


    //Buscar dos marcas consecutivas primero

    do {

    if (hilow_read_audio_modo_verbose) {
        printf("\nPosition before searching start bits sync wave %d\n",posicion);
        hilow_read_audio_pausa(2);
    }
    posicion=hilow_read_audio_buscar_onda_inicio_bits(posicion);
    if (posicion==-1) {
        if (hilow_read_audio_modo_verbose) printf("\nEnd of file trying to read first start bits sync wave\n");
        return -1;
    }
    //Estamos al final de la primera


    //printf("despues 1\n");


    int posicion0=posicion;

    if (hilow_read_audio_modo_verbose) {
        printf("\nPosition after first start bits sync wave %d\n",posicion);
        hilow_read_audio_pausa(2);
    }



    //int final_posicion;

    //Antes la segunda onda no se detectaba bien. Descomentar estas lineas si vuelve a suceder
    //final_posicion=posicion+HILOW_READ_AUDIO_LONGITUD_ONDA_INICIO_BITS;
    //if (hilow_read_audio_modo_verbose) printf("final posicion dos ondas sincronismo: %d\n",final_posicion);
    //return final_posicion;





    posicion=hilow_read_audio_buscar_onda_inicio_bits(posicion);
    if (posicion==-1) return -1;


    if (hilow_read_audio_modo_verbose) {
        printf("\nPosition after second start bits sync wave %d\n",posicion);
        hilow_read_audio_pausa(2);
    }

    //Estamos al final de la segunda



    //Ver si la segunda acaba en donde acaba la primera + el tiempo de onda
    int delta=posicion-posicion0;

    if (hilow_read_audio_modo_verbose) printf("delta %d expected %d\n",delta,HILOW_READ_AUDIO_LONGITUD_ONDA_INICIO_BITS);

    //hilow_read_audio_pausa(3);

    if (delta>=HILOW_READ_AUDIO_LONGITUD_ONDA_INICIO_BITS-HILOW_READ_AUDIO_LONGITUD_ONDA_INICIO_BITS_MARGEN &&
            delta<=HILOW_READ_AUDIO_LONGITUD_ONDA_INICIO_BITS+HILOW_READ_AUDIO_LONGITUD_ONDA_INICIO_BITS_MARGEN)
        {
        if (hilow_read_audio_modo_verbose) {
            printf("\n---Two consecutive sync at %d---\n",posicion);
            hilow_read_audio_pausa(5);
        }

        return posicion;
    }

    else {
        if (hilow_read_audio_modo_verbose) {
            printf("\n---There is NOT two consecutive sync at %d---\n",posicion);
            hilow_read_audio_pausa(5);
        }
    }

    } while (posicion!=-1);

    return posicion;
}



void hilow_read_audio_print_mostrar_ids_sector(void)
{
    int i;

    for (i=0;i<5;i++) {
        printf("%02XH ",hilow_read_audio_buffer_sector_five_byte[i]);
    }

    printf("\n");

}


int hilow_read_audio_esperar_inicio_sincronismo(int posicion)
{

    z80_byte valor_anterior=hilow_read_audio_lee_byte_memoria(posicion);
    do {
        //printf("%d ",direccion);
        int valor_leido=hilow_read_audio_lee_byte_memoria(posicion);
        if (valor_leido==-1) {
            //fin de archivo
            return -1;
        }
        int delta=valor_leido-valor_anterior;
        if (delta<0) delta=-delta;

        if (delta>1) return posicion;

        valor_anterior=valor_leido;
        posicion++;
    } while(1);
}



//Retorna posicion
int hilow_read_audio_lee_byte(int posicion,z80_byte *byte_salida)
{
    //Averiguar primero duracion pulso
    /*

    Marca inicio byte: 71 samples. Flanco bajada: 40
    Bit a 1: 56 samples -> 0.788 de la marca. 1.4 del flanco de bajada
             28 samples flanco bajada. 0.7 del flanco de bajada

    Bit a 0: 37 samples -> 0.521 de la marca. 0.925 del flanco de bajada
             11 samples flanco bajada. 0.275 del flanco de bajada
    */

   //Saltar zona de señal plana hasta que realmente empieza el sincronismo
   /*printf("pos antes inicio sync: %d\n",posicion);
   posicion=hilow_read_audio_esperar_inicio_sincronismo(posicion);
   printf("pos en inicio sync: %d\n",posicion);

   if (posicion==-1) {
       //fin
       return -1;
   }
   */

   z80_byte byte_final=0;

    int duracion_flanco_bajada;
    int duracion_sincronismo_byte;

    //Solo indicar el primer error
    int error_en_sync_mostrado=0;

do {
    duracion_sincronismo_byte=hilow_read_audio_duracion_onda(posicion,&duracion_flanco_bajada);

   if (duracion_sincronismo_byte==-1) {
       //fin
       return -1;
   }
   posicion +=duracion_sincronismo_byte;


#define MINIMO_SYNC_BYTE 45

    if (!error_en_sync_mostrado) {

        if (duracion_sincronismo_byte<MINIMO_SYNC_BYTE) {



            if (hilow_read_audio_autocorrect) {
                if (hilow_read_audio_modo_verbose) printf("Lenght of S_START_BYTE signal is smaller than expected: lenght: %d in position %d. Autocorrecting\n",duracion_sincronismo_byte,posicion);
                //Si eso sucede, situarnos en la siguiente senyal de sync byte

            }
            else {
                if (hilow_read_audio_modo_verbose) printf("Lenght of S_START_BYTE signal is smaller than expected: lenght: %d in position %d. You should enable autocorrect\n",duracion_sincronismo_byte,posicion);
            }

            if (hilow_read_audio_probably_sync_error_callback!=NULL) {
                    hilow_read_audio_probably_sync_error_callback(duracion_sincronismo_byte,posicion);
            }




            error_en_sync_mostrado=1;
        }
    }

} while (hilow_read_audio_autocorrect && duracion_sincronismo_byte<MINIMO_SYNC_BYTE);


   if (hilow_read_audio_modo_verbose_extra) printf("\nEnd wave sync bits: pos: %d\n",posicion);

   //int duracion_uno=(duracion_sincronismo_byte*79)/100;
   //int duracion_cero=(duracion_sincronismo_byte*40)/100;

   //int duracion_uno=(duracion_flanco_bajada*140)/100;
   //int duracion_cero=(duracion_flanco_bajada*92)/100;

   int duracion_uno;
   int duracion_cero;

   //duraciones bits 0 y 1 fijas
    duracion_uno=28;
    duracion_cero=11;

    if (hilow_read_audio_autoajustar_duracion_bits) {
        duracion_uno=(duracion_flanco_bajada*70)/100;
        duracion_cero=(duracion_flanco_bajada*27)/100;
    }

   //int duracion_uno=(duracion_sincronismo_byte*80)/100;
   //int duracion_cero=(duracion_sincronismo_byte*40)/100;
   //int duracion_cero=duracion_uno/2;



   int diferencia_cero_uno=duracion_uno-duracion_cero;

   int dif_umbral=(diferencia_cero_uno)/2;


   //Umbrales entre uno y otro
   int umbral_cero_uno=duracion_cero+dif_umbral;


   //printf("Sync %d Bajada %d Zero %d One %d Umbral %d\n",duracion_sincronismo_byte,duracion_flanco_bajada,duracion_cero,duracion_uno,umbral_cero_uno);

   int i;

   for (i=0;i<8;i++) {

       byte_final=byte_final<<1;

       int duracion_flanco_bajada;
       int duracion_bit=hilow_read_audio_duracion_onda(posicion,&duracion_flanco_bajada);
       //printf("L%d ",duracion_bit);
       //printf("L%d ",duracion_flanco_bajada);
       if (duracion_bit==-1) {
           //fin
           *byte_salida=byte_final;
           //printf("\n");

           return -1;
       }
       posicion +=duracion_bit;

        //if (duracion_bit<umbral_cero_uno) {
        if (duracion_flanco_bajada<umbral_cero_uno) {
            //Es un 0
            if (hilow_read_audio_modo_verbose_extra) printf(" -0- ");

            if (hilow_read_audio_bit_output_write_callback!=NULL) hilow_read_audio_bit_output_write_callback(0,posicion);
        }
        else {
            //Es un 1
            byte_final |=1;
            if (hilow_read_audio_modo_verbose_extra) printf(" -1- ");
            if (hilow_read_audio_bit_output_write_callback!=NULL) hilow_read_audio_bit_output_write_callback(1,posicion);
        }
        //printf("\n");

       //if (i!=7) byte_final=byte_final<<1;
    }

    if (hilow_read_audio_modo_verbose_extra) printf("\nfinal byte: %02XH\n",byte_final);
   *byte_salida=byte_final;

    if (hilow_read_audio_byte_output_write_callback!=NULL) hilow_read_audio_byte_output_write_callback(byte_final,posicion);

   return posicion;
}


int hilow_read_audio_buscar_inicio_sector(int posicion)
{
    //Buscar 3 veces las dos marcas consecutivas de inicio de bits
    int i;

    hilow_read_audio_current_phase=HILOW_READ_AUDIO_PHASE_SEARCHING_SECTOR_MARKS;


    if (!hilow_read_audio_leer_cara_dos) {
        if (hilow_read_audio_modo_verbose) {
            printf("\n---Searching first pair of sync marks at %d\n",posicion);
            hilow_read_audio_pausa(2);
        }
        posicion=hilow_read_audio_buscar_dos_sync_bits(posicion);
        if (posicion==-1) return -1;

        hilow_read_audio_current_phase=HILOW_READ_AUDIO_PHASE_READING_SECTOR_MARKS;

        for (i=0;i<5;i++) {
            z80_byte byte_leido;
            posicion=hilow_read_audio_lee_byte(posicion,&byte_leido);
            if (posicion==-1) return -1;
            hilow_read_audio_buffer_sector_five_byte[i]=byte_leido;
        }

        if (hilow_read_audio_modo_verbose) {
            printf("5 bytes id sector: ");

            hilow_read_audio_print_mostrar_ids_sector();
        }

        //hilow_read_audio_pausa(3);
    }

    hilow_read_audio_current_phase=HILOW_READ_AUDIO_PHASE_SEARCHING_SECTOR_LABEL;

    if (hilow_read_audio_modo_verbose) {
        printf("\n---Searching second pair of sync marks at %d\n",posicion);
        hilow_read_audio_pausa(2);
    }
    posicion=hilow_read_audio_buscar_dos_sync_bits(posicion);
    if (posicion==-1) return -1;

    //Leer el label del sector

    hilow_read_audio_current_phase=HILOW_READ_AUDIO_PHASE_READING_SECTOR_LABEL;

    for (i=0;i<17;i++) {
        z80_byte byte_leido;
        posicion=hilow_read_audio_lee_byte(posicion,&byte_leido);
        if (posicion==-1) return -1;
        hilow_read_audio_buffer_label[i]=byte_leido;
    }

    if (hilow_read_audio_modo_verbose) {

        printf("17 bytes of label: ");

        for (i=0;i<17;i++) {
            z80_byte byte_leido=hilow_read_audio_buffer_label[i];
            printf("%c",(byte_leido>=32 && byte_leido<=126 ? byte_leido : '.'));
        }

        printf("\n");

        printf("label in hexa: ");

        for (i=0;i<17;i++) {
            z80_byte byte_leido=hilow_read_audio_buffer_label[i];
            printf("%02X ",byte_leido);
        }

        printf("\n");

    }

    hilow_read_audio_current_phase=HILOW_READ_AUDIO_PHASE_SEARCHING_SECTOR_DATA;

    //hilow_read_audio_pausa(3);

    if (hilow_read_audio_modo_verbose) {
        printf("\n---Searching third pair of sync marks at %d\n",posicion);
        hilow_read_audio_pausa(2);
    }
    posicion=hilow_read_audio_buscar_dos_sync_bits(posicion);
    //printf("despues hilow_read_audio_buscar_dos_sync_bits\n");

    if (posicion==-1) return -1;
    //hilow_read_audio_pausa(2);

    hilow_read_audio_current_phase=HILOW_READ_AUDIO_PHASE_READING_SECTOR_DATA;

    return posicion;
}


//Sector desde 0,1,2,3,4.... 7b,7c, 82,83,84
//Ya viene restado sector-1
int hilow_read_audio_get_offset_sector(int sector)
{

    int offset_final=sector*HILOW_SECTOR_SIZE;


    return offset_final;


}


void hilow_read_audio_write_sector_to_memory(int sector)
{
    if (hilow_read_audio_modo_verbose) printf("Saving sector %d to memory\n",sector);

    //Copiar a memoria ddh
    //int offset_destino=sector*HILOW_SECTOR_SIZE;

    int offset_destino=hilow_read_audio_get_offset_sector(sector);

    if (hilow_read_audio_modo_verbose) printf("Destination offset to file ddh: %d\n",offset_destino);

    //printf("puntero: %p\n",hilow_read_audio_hilow_ddh);
    /*for (i=0;i<HILOW_SECTOR_SIZE;i++) {
        printf("%d\n",i);
        hilow_ddh[offset_destino+i]=hilow_read_audio_buffer_result[i+1];
    }*/

    //por si acaso sector fuera de rango
    if (sector<0) {
        if (hilow_read_audio_modo_verbose) printf("Out of range sector\n");
    }

    else {
        memcpy(&hilow_read_audio_hilow_ddh[offset_destino],&hilow_read_audio_buffer_result[1],HILOW_SECTOR_SIZE);
    }
}

int hilow_read_audio_warn_if_sector_mismatch(int sector)
{

    if (sector<1) return 1;

    //Cara A
    if (!hilow_read_audio_leer_cara_dos) {

        if (sector!=hilow_read_audio_buffer_sector_five_byte[1] || sector!=hilow_read_audio_buffer_sector_five_byte[2] ||
            sector!=hilow_read_audio_buffer_sector_five_byte[3] || sector!=hilow_read_audio_buffer_sector_five_byte[4]) {

            return 1;

        }

        //O si sector >127
        if (sector & 128) return 1;

    }

    else {
        //Cara B
        //O si sector <128
        if (!(sector & 128)) return 1;
    }

    return 0;
}


int hilow_read_audio_lee_sector(int posicion,int *total_bytes_leidos,int *p_sector)
{
    int total=HILOW_SECTOR_SIZE+1; //2049; //2049; //byte de numero de sector + 2048 del sector
    //int posicion=0;



    int i;

    *total_bytes_leidos=0;

    for (i=0;i<total && posicion!=-1;i++,(*total_bytes_leidos)++) {
        hilow_read_audio_lee_sector_bytes_leidos=i;
        //printf("\nPos %d %d\n",i,posicion);
        z80_byte byte_leido;

        posicion=hilow_read_audio_lee_byte(posicion,&byte_leido);
        if (posicion!=-1) {
            //printf("Byte leido: %d (%02XH) (%c)\n",byte_leido,byte_leido,(byte_leido>=32 && byte_leido<=126 ? byte_leido : '.') );
        }

        hilow_read_audio_buffer_result[i]=byte_leido;
    }


    if (*total_bytes_leidos==0) {
        if (hilow_read_audio_modo_verbose) printf("Zero bytes read\n");
        return posicion;
    }

    int sector=hilow_read_audio_buffer_result[0];

    //printf("Sector: %d\n",sector);

    //Leer otros 28 bytes adicionales de final de sector
    //Creo que solo son realmente 27, aunque leo 1 de mas porque me da una pista de si se han desplazado bits en la lectura


     for (i=0;i<HILOW_LONGITUD_FINAL_SECTOR && posicion!=-1;i++) {
        z80_byte byte_leido;

        posicion=hilow_read_audio_lee_byte(posicion,&byte_leido);
        if (posicion!=-1) {
            //printf("Byte leido: %d (%02XH) (%c)\n",byte_leido,byte_leido,(byte_leido>=32 && byte_leido<=126 ? byte_leido : '.') );
        }

        hilow_read_audio_buffer_end_sector[i]=byte_leido;
    }

    if (hilow_read_audio_modo_verbose) {
        printf("End sector bytes:\n");

        for (i=0;i<HILOW_LONGITUD_FINAL_SECTOR;i++) {
            z80_byte byte_leido=hilow_read_audio_buffer_end_sector[i];
            printf("%02X ",byte_leido);
        }

        printf("\n");
    }


    *p_sector=sector;

    return posicion;


}



long long int hilow_read_audio_get_file_size(char *nombre)
{
        struct stat buf_stat;

                if (stat(nombre, &buf_stat)!=0) {
                        printf("Unable to get status of file %s\n",nombre);
                        return 0;
                }

                else {
                        //printf ("file size: %ld\n",buf_stat.st_size);
                        return buf_stat.st_size;
                }
}


void hilow_read_audio_espejar_sonido(z80_byte *puntero,int tamanyo)
{

    int mitad=tamanyo/2;
    int indice_final=tamanyo-1;

    int i;

    z80_byte byte_izquierda,byte_derecha;

    for (i=0;i<mitad;i++,indice_final--) {
        byte_izquierda=puntero[i];
        byte_derecha=puntero[indice_final];

        //intercambiar
        puntero[i]=byte_derecha;
        puntero[indice_final]=byte_izquierda;

    }
}



