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

#include <stdlib.h>
#include <stdio.h>

#include <time.h>
#include <sys/time.h>
#include <errno.h>


#include "cpu.h"
#include "debug.h"
#include "tape.h"
#include "audio.h"
#include "ay38912.h"
#include "screen.h"
#include "operaciones.h"
#include "snap.h"
#include "timer.h"
#include "zxvision.h"
#include "compileoptions.h"
#include "contend.h"
#include "ula.h"
#include "utils.h"
#include "realjoystick.h"
#include "chardetect.h"

#include "scrstdout.h"
#include "settings.h"

#include "snap_zsf.h"
#include "zeng.h"
#include "zeng_online_client.h"

z80_byte byte_leido_core_sam;



int core_sam_medio_scanline=0;

void core_sam_handle_interrupts(void)
{


    debug_fired_interrupt=1;

    z80_adjust_flags_interrupt_block_opcode();

    //printf ("Generada interrupcion Z80\n");

    //if (interrupcion_non_maskable_generada.v) printf ("generada nmi\n");

    //if (interrupts.v==1) {   //esto ya no se mira. si se ha producido interrupcion es porque estaba en ei o es una NMI
    //ver si esta en HALT
    if (z80_halt_signal.v) {
        z80_halt_signal.v=0;
        //reg_pc++;
    }



    if (interrupcion_non_maskable_generada.v) {
        debug_anota_retorno_step_nmi();
        //printf ("generada nmi\n");
        interrupcion_non_maskable_generada.v=0;


        //NMI wait 14 estados
        t_estados += 14;




        push_valor(reg_pc,PUSH_VALUE_TYPE_NON_MASKABLE_INTERRUPT);


        reg_r++;
        iff1.v=0;
        //printf ("Calling NMI with pc=0x%x\n",reg_pc);

        //Otros 6 estados
        t_estados += 6;

        //Total NMI: NMI WAIT 14 estados + NMI CALL 12 estados
        reg_pc= 0x66;

        //temp

        t_estados -=15;




    }



    //justo despues de EI no debe generar interrupcion
    //e interrupcion nmi tiene prioridad
    if (interrupcion_maskable_generada.v && byte_leido_core_sam!=251) {
        debug_anota_retorno_step_maskable();
        //Tratar interrupciones maskable
        interrupcion_maskable_generada.v=0;



        push_valor(reg_pc,PUSH_VALUE_TYPE_MASKABLE_INTERRUPT);

        reg_r++;




        //desactivar interrupciones al generar una
        iff1.v=iff2.v=0;


        if (im_mode==0 || im_mode==1) {
            cpu_common_jump_im01();
        }
        else {
        //IM 2.

            z80_int temp_i;
            z80_byte dir_l,dir_h;
            temp_i=get_im2_interrupt_vector();
            dir_l=peek_byte(temp_i++);
            dir_h=peek_byte(temp_i);
            reg_pc=value_8_to_16(dir_h,dir_l);
            t_estados += 7;


        }

    }



}

void core_sam_fin_frame_pantalla(void)
{


    //if (rainbow_enabled.v==1) t_scanline_next_fullborder();

    t_scanline=0;

    timer_get_elapsed_core_frame_post();

    //printf ("final scan lines. total: %d\n",screen_scanlines);
            //printf ("reset no inves\n");
    set_t_scanline_draw_zero();


    //Parche para maquinas que no generan 312 lineas, porque si enviamos menos sonido se escuchara un click al final
    //Es necesario que cada frame de pantalla contenga 312 bytes de sonido
    //Igualmente en la rutina de envio_audio se vuelve a comprobar que todo el sonido a enviar
    //este completo; esto es necesario para Z88


    int linea_estados=t_estados/screen_testados_linea;

    while (linea_estados<312) {
            audio_send_mono_sample(audio_valor_enviar_sonido);
            linea_estados++;
    }




    t_estados -=screen_testados_total;

    //Para paperboy, thelosttapesofalbion0 y otros que hacen letras en el border, para que no se desplacen en diagonal
    //t_estados=0;
    //->paperboy queda fijo. thelosttapesofalbion0 no se desplaza, sino que tiembla si no forzamos esto


    //Final de instrucciones ejecutadas en un frame de pantalla
    if (iff1.v==1) {
        interrupcion_maskable_generada.v=1;

    }


    cpu_loop_refresca_pantalla();

    vofile_send_frame(rainbow_buffer);


    siguiente_frame_pantalla();


    if (debug_registers) scr_debug_registers();

    contador_parpadeo--;
    //printf ("Parpadeo: %d estado: %d\n",contador_parpadeo,estado_parpadeo.v);
    if (!contador_parpadeo) {
            contador_parpadeo=16;
            toggle_flash_state();
    }


    if (!interrupcion_timer_generada.v) {
        //Llegado a final de frame pero aun no ha llegado interrupcion de timer. Esperemos...
        //printf ("no demasiado\n");
        esperando_tiempo_final_t_estados.v=1;
    }

    else {
        //Llegado a final de frame y ya ha llegado interrupcion de timer. No esperamos.... Hemos tardado demasiado
        //printf ("demasiado\n");
        esperando_tiempo_final_t_estados.v=0;
    }

    core_end_frame_check_zrcp_zeng_snap.v=1;




}

void core_sam_fin_scanline(void)
{

//printf ("%d\n",t_estados);
//if (t_estados>69000) printf ("t_scanline casi final: %d\n",t_scanline);



    //TODO. detector de sonido en beeper provoca, que cuando salta, el output de sonido es extraño,
    //cuando se combina con chip AY. forzar a no desactivarlo nunca
    reset_beeper_silence_detection_counter();

    audio_valor_enviar_sonido=0;


    audio_valor_enviar_sonido +=da_output_ay();



    if (beeper_enabled.v) {

        //Sam coupe no se oye bien real beeper, creo que porque el array de valores del puerto FEH tiene limite
        //CURRENT_BEEPER_ARRAY_LENGTH, y eso es 256 , y sam coupe tiene aproximadamente 384 estados por linea
        //lo cual es mayor de 256 y provoca que no se escriba todo el array
        //Habria que probar si aumentando CURRENT_BEEPER_ARRAY_LENGTH se oye bien
        //De todas maneras lo mejor es hacer que Sam Coupe de momento no use real beeper, como si no estuviera habilitado

        audio_valor_enviar_sonido += value_beeper;

        /*

        if (beeper_real_enabled==0) {
            audio_valor_enviar_sonido += value_beeper;
        }

        else {
            audio_valor_enviar_sonido += get_value_beeper_sum_array();
            beeper_new_line();
        }

        */
    }

    //printf ("Sonido: %d\n",audio_valor_enviar_sonido);

    int leer_cinta_real=0;

    if (realtape_inserted.v && realtape_playing.v) leer_cinta_real=1;

    if (audio_can_record_input()) {
        if (audio_is_recording_input) {
            leer_cinta_real=1;
        }
    }

    if (leer_cinta_real) {
        realtape_get_byte();
        if (realtape_loading_sound.v) {
            reset_silence_detection_counter();
            audio_valor_enviar_sonido /=2;
            audio_valor_enviar_sonido += get_realtape_last_value()/2;
            //Sonido alterado cuando top speed
            if (timer_condicion_top_speed() ) audio_valor_enviar_sonido=audio_change_top_speed_sound(audio_valor_enviar_sonido);
        }
    }

    //Ajustar volumen
    if (audiovolume!=100) {
        audio_valor_enviar_sonido=audio_adjust_volume(audio_valor_enviar_sonido);
    }

    audio_send_mono_sample(audio_valor_enviar_sonido);

    ay_chip_siguiente_ciclo();


    //final de linea


    //copiamos contenido linea y border a buffer rainbow
    if (rainbow_enabled.v==1) {
        screen_store_scanline_rainbow_solo_border();
        screen_store_scanline_rainbow_solo_display();

        //t_scanline_next_border();

    }

    t_scanline_next_line();

    //se supone que hemos ejecutado todas las instrucciones posibles de toda la pantalla. refrescar pantalla y
    //esperar para ver si se ha generado una interrupcion 1/50

    if (t_estados>=screen_testados_total) {

        core_sam_fin_frame_pantalla();


    }



}

void core_sam_ciclo_fetch(void)
{


    //core_spectrum_store_rainbow_current_atributes();



#ifdef DEBUG_SECOND_TRAP_STDOUT

    //Para poder debugar rutina que imprima texto. Util para aventuras conversacionales
    //hay que definir este DEBUG_SECOND_TRAP_STDOUT manualmente en compileoptions.h despues de ejecutar el configure

	scr_stdout_debug_print_char_routine();

#endif


    contend_read( reg_pc, 4 );
    byte_leido_core_sam=fetch_opcode();




#ifdef EMULATE_CPU_STATS
    util_stats_increment_counter(stats_codsinpr,byte_leido_core_sam);
#endif

    //Si la cpu está detenida por señal HALT, reemplazar opcode por NOP
    if (z80_halt_signal.v) {
        byte_leido_core_sam=0;
    }
    else {
        reg_pc++;
    }

    reg_r++;

            z80_no_ejecutado_block_opcodes();
            codsinpr[byte_leido_core_sam]  () ;

    //printf ("t_estados:%d\n",t_estados);



}

//bucle principal de ejecucion de la cpu de sam coupe
void cpu_core_loop_sam(void)
{

    debug_get_t_stados_parcial_pre();

    timer_check_interrupt();


    if (chardetect_detect_char_enabled.v) chardetect_detect_char();
    if (chardetect_printchar_enabled.v) chardetect_printchar();


    //Gestionar autoload
    gestionar_autoload_sam();



    if (esperando_tiempo_final_t_estados.v==0) {

        core_sam_ciclo_fetch();

    }


    //A mitad de scanline
    //Hacemos cosas como leer sample de audio de cable externo, pues leemos a 31200 hz (el doble de lo que seria cada scanline)
    if (!core_sam_medio_scanline) {
        int estados_en_linea=t_estados % screen_testados_linea;
        if (estados_en_linea>screen_testados_linea/2) {
            //printf("mitad scanline. %5d %5d\n",estados_en_linea,t_estados);
            //Indicamos que ya hemos pasado el medio scanline
            core_sam_medio_scanline=1;
            if (audio_can_record_input()) {
                if (audio_is_recording_input) {
                    //En este caso simplemente leemos el valor que luego el core lo interpreta en el puerto EAR
                    //En cambio no alimentamos con ese valor el buffer de sonido que permite escuchar el sonido de cable externo,
                    //no hace falta complicarse tanto
                    //digamos que de esos 31200 hz, 1 de cada dos samples no lo escuchamos, aunque por el puerto EAR se interpretan los dos
                    audio_read_sample_audio_input();
                    realtape_last_value=audio_last_record_input_sample;
                    //return;
                }
            }
        }
    }

    //ejecutar esto al final de cada una de las scanlines (312)
    //esto implica que al final del frame de pantalla habremos enviado 312 bytes de sonido


    //A final de cada scanline
    if ( (t_estados/screen_testados_linea)>t_scanline  ) {
        core_sam_fin_scanline();

        //Indicamos que no hemos pasado el medio scanline
        core_sam_medio_scanline=0;
    }

    if (esperando_tiempo_final_t_estados.v) {
        timer_pause_waiting_end_frame();
    }



		//Interrupcion de 1/50s. mapa teclas activas y joystick
    if (interrupcion_fifty_generada.v) {
        interrupcion_fifty_generada.v=0;

        //y de momento actualizamos tablas de teclado segun tecla leida
        //printf ("Actualizamos tablas teclado %d ", temp_veces_actualiza_teclas++);
        scr_actualiza_tablas_teclado();


        //lectura de joystick
        realjoystick_main();

        //printf ("temp conta fifty: %d\n",tempcontafifty++);
    }


		//Interrupcion de procesador y marca final de frame
    if (interrupcion_timer_generada.v) {
        //printf ("Generada interrupcion timer\n");
        interrupcion_timer_generada.v=0;
        esperando_tiempo_final_t_estados.v=0;
        interlaced_numero_frame++;
        //printf ("%d\n",interlaced_numero_frame);

        //Para calcular lo que se tarda en ejecutar todo un frame
        timer_get_elapsed_core_frame_pre();
    }


    //Interrupcion de cpu. gestion im0/1/2. Esto se hace al final de cada frame en spectrum o al cambio de bit6 de R en zx80/81
    if (interrupcion_maskable_generada.v || interrupcion_non_maskable_generada.v) {

        core_sam_handle_interrupts();


    }

	//Fin gestion interrupciones

	//Aplicar snapshot pendiente de ZRCP y ZENG envio snapshots. Despues de haber gestionado interrupciones
	if (core_end_frame_check_zrcp_zeng_snap.v) {
		core_end_frame_check_zrcp_zeng_snap.v=0;
		check_pending_zrcp_put_snapshot();
		zeng_send_snapshot_if_needed();

        zeng_online_client_end_frame_from_core_functions();
	}

    debug_get_t_stados_parcial_post();

}

