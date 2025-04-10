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
#include <stdlib.h>
#include <dirent.h>


#include "mmc.h"
#include "cpu.h"
#include "debug.h"
#include "utils.h"
#include "zxvision.h"
#include "menu_items.h"
#include "screen.h"
#include "divmmc.h"
#include "compileoptions.h"
#include "operaciones.h"
#include "zxmmcplus.h"


//valores temporales
//z80_byte mmc_csd[16]={1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};

//Pruebas originales para unit (45248/2/128) 5650 MB aprox. Usando archivo de pruebas pruebasorig.mmc de 64 MB
z80_byte mmc_csd[16]={11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11};

//Registro OCR
z80_byte mmc_ocr[5]={5,0,0,0,0};
//primer byte: R1: xxx0sss1
//sss: status: 010 data accepted
//Siguientes 4 bytes indican voltaje y algun status bit. Indicamos 0 y es voltaje 1.45V-1.50V
//Siguientes 4 bytes: (32 bits):
//Del 0 al 23: voltaje
//  0 1.45 - 1.50
//  1 1.50 - 1.55
//  2 1.55 - 1.60
//  3 1.60 - 1.65
//  4 1.65 - 1.70
//  5 1.7 - 1.8
//  6 1.8 - 1.9
//  7 1.9 - 2.0
//  8 2.0-2.1
//  9 2.1-2.2
// 10 2.2-2.3
// 11 2.3-2.4
// 12 2.4-2.5
// 13 2.5-2.6
// 14 2.6-2.7
// 15 2.7-2.8
// 16 2.8-2.9
// 17 2.9-3.0
// 18 3.0-3.1
// 19 3.1-3.2
// 20 3.2-3.3
// 21 3.3-3.4
// 22 3.4-3.5
// 23 3.5-3.6
// 24: Switching to 1.8 V Accepted
// 25:28: reserved
// 29: UHS-II Card Status
// 30: Card Capacity Status (CCS): (CCS = 0): must be Ver2.00 or later Standard Capacity SD Memory Card, (CCS = 1): must be Ver2.00 or later High Capacity SD Memory Card
// 31: Card power up status bit
//Nota: en teoria para tarjetas SDHC se deberia retornar array: z80_byte mmc_ocr[5]={5,0,0,0,2}
//      ya que el bit 30 (el de CCS) esta activo
//Nota2: para soportar SDHC en imagenes .mmc/.sd/.sdhc en Spectrum Next por ejemplo se deberia:
//1. Emular tarjeta de mas de 2 GB en ZEsarUX: creo que eso no funciona
//2: Retornar ese bit de CCS activo
//3: Setting --sd-enable-sdhc-addressing
//Se probó (05/12/2024) a activar el bit CCS pero Next no iniciaba el boot, probablemente
//por culpa de que no emulo correctamente tarjetas de mas de 2 GB, o quiza a que no gestiono correctamente el CMD8 SEND_IF_COND
//Nota3: Si que se soporta SDHC pero no con un archivo de imagen .mmc/.sd/.sdhc sino mediante mapeo desde esxdos
// (llamada DISK_FILEMAP) usada en Atic Atac por ejemplo



//valores temporales
/*
00h: manufacturer id
01-02h: oem/application id
03h-08h Manufacturers’s name in ascii
09h: product revision
0ah-0dh Card’s 32 bit serial number
0e: manufacturing date

• MDT
The manufacturing date is composed of two hexadecimal digits, four bits each, representing a two digits date
code m/y;
The “m” field, most significant nibble, is the month code. 1 = January.
The “y” field, least significant nibble, is the year code. 0 = 1997.
As an example, the binary value of the MDT field for production date “April 2000” will be: 0100 0011

Julio 2015 = 7 2015 = 7 18 = 0111   (18 no se puede meter en anyo dado que es mayor que 15)
metemos 7 15 = Julio 2012 = 0111 1111 = 127

0f: crc checksum+128
*/
//z80_byte mmc_cid[16]={1,1,1,'Z','E','s','a','r','U',1,1,1,1,1,127,128};

//ESXDOS para DivMMC muestra el oem/application id como dos primeras letras y luego
//el Manufacturers’s name in ascii como cinco letras (aunque son 6)
//Ponemos nosotros el oem/application id como "ZE" y el nombre como "sarUX "

z80_byte mmc_cid[16]={1,'Z','E','s','a','r','U','X',' ',1,1,1,1,1,127,128};



//Parametros enviados en operacion de escritura
z80_byte mmc_parameters_sent[10];

//A 0 cuando se ha recibido todos los valores correctos de csd
int mmc_csd_index=-1;
//A 0 cuando se ha recibido todos los valores correctos de cid
int mmc_cid_index=-1;

//A 0 cuando se ha recibido todos los valores correctos de ocr
int mmc_ocr_index=-1;

//A 0 cuando se ha recibido todos los valores correctos de read block
int mmc_read_index=-1;

//A 0 cuando se ha recibido todos los valores correctos de write block
int mmc_write_index=-1;

//Direcciones de inicio de lectura y escritura de datos
unsigned int mmc_write_address,mmc_read_address;

z80_bit mmc_enabled={0};

z80_byte mmc_last_command=0;

int mmc_index_command=0;

//TODO: este mmc_r1 necesita una aclaracion o mejora. Parece los bits CURRENT_STATE de R1 pero no es justo lo mismo
//Aqui lo pongo a 1 para indicar idle, a 0 para no idle. Pero esos bits de CURRENT_STATE son:
//0 = idle
//1 = ready
//2 = ident
// ....
z80_byte mmc_r1=0;

//64 MB
long long int mmc_size=64*1024*1024;

z80_byte *mmc_memory_pointer;

int mmc_flash_must_flush_to_disk=0;
char mmc_file_name[PATH_MAX]="";

//Si se mapea un archivo a mmc desde esxdos
int mmc_filemap_from_esxdos=0;
//Y el nombre de ese archivo
char mmc_filemap_name[PATH_MAX]="";

//Si se habilita mapeo de direcciones de sector en vez de byte, para tarjetas SDHC por ejemplo
//De momento este setting no se autoguarda en archivo de configuración, que yo sepa solo
//hay un juego que lo necesita: Atic Atac
z80_bit mmc_sdhc_addressing={0};

z80_byte mmc_last_port_value_1f=0;


/*
Inicializar una mmc desde plus3e:

format to 0,4  (4 particiones)
new data "pruebas",16  (nueva particion nombre pruebas y tamanyo 16 MB)
move "c:" in "pruebas" (asignar unidad c a particion pruebas)

otros:
cat tab para ver contenido disco
*/




//0: primera tarjeta
//1: segunda tarjeta.

int mmc_card_selected=0;

//Decir que hay una segunda unidad mmc pero en realidad es clon de la primera
z80_bit mmc_mirror_second_card={0};

z80_bit mmc_write_protection={0};


//Si archivo mmc insertado es de tipo hdf
z80_bit mmc_file_inserted_hdf={0};

//Tamaño cabecera hdf
z80_int mmc_file_header_hdf_size;

//Puntero a cabecera hdf
z80_byte *mmc_file_header_hdf_pointer=NULL;

//Si cambios en escritura se hace flush a disco
z80_bit mmc_persistent_writes={1};


void mmc_footer_mmc_operating(void)
{

	generic_footertext_print_operating("MMC");
    watermark_tell_device_activity();

	//Y poner icono de mmc en inverso
	if (!zxdesktop_icon_mmc_inverse) {
		zxdesktop_icon_mmc_inverse=1;
		menu_draw_ext_desktop();
	}
}

char *mmc_get_file_name(void)
{
    if (mmc_filemap_from_esxdos) return mmc_filemap_name;
    else return mmc_file_name;
}





void mmc_flush_flash_to_disk(void)
{

	if (mmc_enabled.v==0) return;

        if (mmc_flash_must_flush_to_disk==0) {
                debug_printf (VERBOSE_DEBUG,"Trying to flush MMC to disk but no changes made");
                return;
        }

	if (mmc_persistent_writes.v==0) {
                debug_printf (VERBOSE_DEBUG,"Trying to flush MMC to disk but persistent writes disabled");
                return;
        }

        if (mmc_filemap_from_esxdos) {
            debug_printf(VERBOSE_DEBUG,"Do not flush changes when MMC is mapped from ESXDOS");
            return;
        }


        debug_printf (VERBOSE_INFO,"Flushing MMC to disk");


        FILE *ptr_mmcfile;

	debug_printf (VERBOSE_INFO,"Opening MMC File %s",mmc_file_name);
	ptr_mmcfile=fopen(mmc_file_name,"wb");



        long long int escritos=0;
        long long int size;
        size=mmc_size;





        if (ptr_mmcfile!=NULL) {

		//Si tiene cabecera hdf, grabarla
		if (mmc_file_inserted_hdf.v) {
			debug_printf (VERBOSE_DEBUG,"Writing hdf header");
			fwrite(mmc_file_header_hdf_pointer,1,mmc_file_header_hdf_size,ptr_mmcfile);
			debug_printf (VERBOSE_DEBUG,"Writing hdf data");
		}

                z80_byte *puntero;
                puntero=mmc_memory_pointer;

		//Justo antes del fwrite se pone flush a 0, porque si mientras esta el fwrite entra alguna operacion de escritura,
		//metera flush a 1
		mmc_flash_must_flush_to_disk=0;

                escritos=fwrite(puntero,1,size,ptr_mmcfile);

                fclose(ptr_mmcfile);


        }

        //printf ("ptr_mmcfile: %d\n",ptr_mmcfile);
        //printf ("escritos: %lld\n",escritos);

        if (escritos!=size || ptr_mmcfile==NULL) {
                debug_printf (VERBOSE_ERR,"Error writing to MMC file. Disabling write file operations");
		        mmc_persistent_writes.v=0;
        }

}


int mmc_read_file_to_memory(void)
{
  if (mmc_memory_pointer==NULL || mmc_enabled.v==0) {
    debug_printf(VERBOSE_ERR,"MMC is not enabled");
    return 1;
  }

  FILE *ptr_mmcfile;
  unsigned int leidos=0;

  debug_printf (VERBOSE_INFO,"Opening MMC File %s",mmc_get_file_name());

  ptr_mmcfile=fopen(mmc_get_file_name(),"rb");


  unsigned int bytes_a_leer=mmc_size;


  //mmc_file_inserted_hdf.v=1;


  if (ptr_mmcfile!=NULL) {

	//Si tiene cabecera hdf, ignorarla
	if (mmc_file_inserted_hdf.v) {
		fseek(ptr_mmcfile,mmc_file_header_hdf_size,SEEK_SET);
	}


        leidos=fread(mmc_memory_pointer,1,bytes_a_leer,ptr_mmcfile);
        fclose(ptr_mmcfile);
  }

  if (ptr_mmcfile==NULL) {
  debug_printf (VERBOSE_ERR,"Error opening mmc file");
  return 1;
  }

  if (leidos!=bytes_a_leer) {
  debug_printf (VERBOSE_ERR,"Error reading mmc. Asked: %ld Read: %d",bytes_a_leer,leidos);
  return 1;
  }

  return 0;

}

//Retorna 0 si ok
int mmc_read_file(void)
{

	//Si habia memoria asignada, desasignar
	if (mmc_memory_pointer!=NULL) free (mmc_memory_pointer);
	mmc_memory_pointer=NULL;


        mmc_memory_pointer=malloc(mmc_size);
        if (mmc_memory_pointer==NULL) {
                cpu_panic ("No enough memory for mmc emulation");
        }

        return mmc_read_file_to_memory();



}

void mmc_get_sector_size(int *valor, z80_byte *valor_8_bits)
{

/*
Max capacity teniendo en cuenta sectores de 512 bytes:
(4096) * (512 ) * (512 ) = 1073741824 bytes = 1048576 kb = 1024 MB = 1 GB

Max capacity teniendo en cuenta sectores de (2^15)=32768 bytes:
(4096) * (512 ) * (32768 ) = 68719476736 bytes = 67108864 kb = 65536 MB = 64 GB
*/

	if (mmc_size<1073741824) {
		*valor=512;
		*valor_8_bits=9;
	}
	else {
		*valor=32768;
		*valor_8_bits=15;
	}

	debug_printf (VERBOSE_DEBUG,"mmc_size: %ld sector_size: %d (%d)",mmc_size,*valor_8_bits,*valor);
    //printf ("mmc_size: %ld sector_size: %d (%d)\n",mmc_size,*valor_8_bits,*valor);

}

void mmc_get_cmult(int *valor, z80_byte *valor_8_bits)
{
	//Valores fijos
	*valor_8_bits=7;
	*valor=512;

	debug_printf (VERBOSE_DEBUG,"mmc_size: %ld cmult: %d (%d)",mmc_size,*valor_8_bits,*valor);
}

int mmc_read_hdf_header(void)
{
	unsigned char buffer_lectura[1024];


        FILE *ptr_inputfile;
        ptr_inputfile=fopen(mmc_get_file_name(),"rb");

        if (ptr_inputfile==NULL) {
                debug_printf (VERBOSE_ERR,"Error opening %s",mmc_get_file_name());
                return 1;
        }



	// Leer offset a datos raw del byte de cabecera:
	//0x09 DOFS WORD Image data offset This is the absolute offset in the HDF file where the actual hard-disk data dump starts.
	//In HDF version 1.1 this is 0x216.

	//Leemos 10 bytes de la cabecera
        fread(buffer_lectura,1,10,ptr_inputfile);

	mmc_file_header_hdf_size=buffer_lectura[9]+256*buffer_lectura[10];

	//printf ("Offset to raw data: %d\n",offset_raw);


	//Leer desde el principio al buffer
	fseek(ptr_inputfile,0,SEEK_SET);



	//Si habia memoria asignada, desasignar
	if (mmc_file_header_hdf_pointer!=NULL) free (mmc_file_header_hdf_pointer);
	mmc_file_header_hdf_pointer=NULL;


        mmc_file_header_hdf_pointer=malloc(mmc_file_header_hdf_size);
        if (mmc_file_header_hdf_pointer==NULL) {
                cpu_panic ("No enough memory for mmc emulation");
        }


	unsigned int leidos=0;
	debug_printf (VERBOSE_DEBUG,"Reading %d bytes of hdf header",mmc_file_header_hdf_size);

          leidos=fread(mmc_file_header_hdf_pointer,1,mmc_file_header_hdf_size,ptr_inputfile);
          fclose(ptr_inputfile);

  if (leidos!=mmc_file_header_hdf_size) {
  debug_printf (VERBOSE_ERR,"Error reading mmc header. Asked: %ld Read: %d",mmc_file_header_hdf_size,leidos);
  return 1;
  }

  return 0;

}






void mmc_insert(void)
{

        //Si existe
        if (!si_existe_archivo(mmc_get_file_name())) {
                debug_printf (VERBOSE_ERR,"File %s does not exist",mmc_get_file_name());
                mmc_disable();
                return;
        }

	//Meter capacidad, etc en CSD
	//Cargar archivo en memoria
	if (mmc_enabled.v==0) {
		return;
	}

	mmc_size=get_file_size(mmc_get_file_name());
	debug_printf (VERBOSE_DEBUG,"mmc file size: %ld",mmc_size);

	//Gestionar si archivo es tipo hdf
	if (!util_compare_file_extension(mmc_get_file_name(),"hdf")) {
		debug_printf (VERBOSE_INFO,"File has hdf header");
		if (mmc_read_hdf_header()) {
			mmc_disable();
                	return;
		}
		mmc_size -=mmc_file_header_hdf_size;
		mmc_file_inserted_hdf.v=1;
	}

	else {
		mmc_file_inserted_hdf.v=0;
	}


	int sector_size;
	z80_byte sector_size_8_bits;
	mmc_get_sector_size(&sector_size,&sector_size_8_bits);

	int cmult;
	z80_byte cmult_8_bits;
	mmc_get_cmult(&cmult,&cmult_8_bits);

	unsigned int multiple=sector_size*cmult;

	//Tamanyo debe ser multiple de 256 KB, en caso de sectores de 512 bytes
	//O de 16 MB en el caso de sectores de 32768 byes (para tarjetas > 1 GB)
	long long int resultado=mmc_size/multiple;
	long long int multiplicado=resultado*multiple;

    //No mostramos esto como error por pantalla. Por ejemplo para mapeos desde esxdos con filemap no queremos que salte este error
	if (multiplicado!=mmc_size) {
		debug_printf (VERBOSE_INFO,"Warning. MMC file should be multiple of %d KB. Use at your own risk!",multiple/1024);
		//mmc_disable();
		//return;
	}

	if (mmc_read_file()) {
		mmc_disable();
		return;
	}


//Pruebas originales para unit (45248/2/128) 5650 MB aprox. Usando archivo de pruebas pruebasorig.mmc de 64 MB
//z80_byte mmc_csd[16]={11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11};
/*
Byte Locations:

06h,07h,08h : (contents AND 00000011 11111111b 11000000b) >> 6 = “Device size (C_Size)”

09h,0ah : (contents AND 00000011 10000000b) >> 7 = “Device size multiplier (C_Mult)”

05h : (contents AND 00001111b) = Sector size (“Read_BL_Len”)

When you have the 12 bit “C_Size”, 3 Bit “C_Mult” and 4 bit “Read_BL_Len” you need to follow the formula:

Capacity in bytes = (C_Size+1) * (2 ^ (C_Mult+2)) * (2 ^ Read_BL_Len)

(Note: The computed sector size (2 ^ Read_BL_len) is normally 512 bytes)


05h : (contents AND 00001111b) = Sector size (“Read_BL_Len”). 512 = 2^9
Medio byte. Bit superior y anteriores bytes (4 y 3) son "card command classes"

C_Mult sera como mucho 3 bits->7. (2 ^ (C_Mult+2)) = 2 ^ (7+2) = 2 ^ 9 = 512

C_Size es de 12 bits, que es: 4095

Max capacity teniendo en cuenta sectores de 512 bytes:
(4096) * (512 ) * (512 ) = 1073741824 bytes = 1048576 kb = 1024 MB = 1 GB

Max capacity teniendo en cuenta sectores de (2^15)=32768 bytes:
(4096) * (512 ) * (32768 ) = 68719476736 bytes = 67108864 kb = 65536 MB = 64 GB


*/

	mmc_csd[5]=sector_size_8_bits;

	//Si suponemos multiplicador 512: 512*512=256 KB. minimo tamanyo
	//Tamanyos multiples de 256 kb para tarjetas < 1 GB

	mmc_csd[9]=(cmult_8_bits>>1)&3;
	mmc_csd[0xa]=(cmult_8_bits<<7)&128;

	int device_size=mmc_size/multiple;

	debug_printf (VERBOSE_DEBUG,"device size: %d",device_size);

	//06h,07h,08h : (contents AND 00000011 11111111b 11000000b) >> 6 = “Device size (C_Size)”
	device_size=device_size<<6;
	mmc_csd[6]=(device_size>>16)&3;
	mmc_csd[7]=(device_size>>8)&255;
	mmc_csd[8]=device_size&(128+64);

}

void mmc_reset(void)
{
        //Resetear estado
        mmc_index_command=0;
        mmc_r1=0;

}


void mmc_enable(void)
{
        debug_printf (VERBOSE_INFO,"Enabling mmc");
        mmc_enabled.v=1;

	mmc_reset();

        mmc_memory_pointer=NULL;

        mmc_insert();


}

void mmc_disable(void)
{

	//Hacer flush si hay algun cambio
	mmc_flush_flash_to_disk();

	mmc_enabled.v=0;

	//Desactivar ZXMMC
	zxmmc_emulation.v=0;

	//Desactivar Divmmc ports
	divmmc_mmc_ports_disable();

        //Si habia memoria asignada, desasignar
        if (mmc_memory_pointer!=NULL) free (mmc_memory_pointer);
        mmc_memory_pointer=NULL;

}

int mmc_tarjeta_invalida_seleccionada=0;

//Card select
void mmc_cs(z80_byte value)
{
    mmc_last_port_value_1f=value;

	//Hay que ir a idle??
	mmc_r1=1;

	mmc_last_command=0;

	mmc_index_command=0;


	mmc_read_index=-1;
	mmc_write_index=-1;
	mmc_csd_index=-1;
	mmc_cid_index=-1;
	mmc_ocr_index=-1;

    //printf("mmc_cs value %02XH\n",value);

    mmc_tarjeta_invalida_seleccionada=0;

    //2 bit chip select register (D0 = MMC0; D1 = MMC1), active LOW
    //Al parecer residos soporta hasta 3 tarjetas, aunque la documentacion oficial dice 2
    z80_byte seleccion_tarjeta=value & 0x03;

    if (seleccion_tarjeta==0x02) mmc_card_selected=0;
    else if (seleccion_tarjeta==0x01 && mmc_mirror_second_card.v) {
        mmc_card_selected=1;
    }

    else {
        //no valida
        mmc_tarjeta_invalida_seleccionada=1;
        //por si acaso, aunque con mmc_tarjeta_invalida_seleccionada no se debe realizar ninguna accion
        mmc_card_selected=0;
    }

    //printf("Card selected: %d\n",mmc_card_selected);

	//debug_printf (VERBOSE_PARANOID,"Card selected: %d",mmc_card_selected);
}


//-1 si no aplica
int mmc_get_visualmem_position(unsigned int address)
{
#ifdef EMULATE_VISUALMEM
	if (mmc_size>0) {

		long long int address_l,mmc_size_l;

		address_l=address;
		mmc_size_l=mmc_size;

		// Necesario hacerlo asi porque son numeros de 64 bits y si no, no va bien
		// Basicamente ajustamos el valor de direccion al total de tamanyo de visualmem
		// Seria (address/mmc_size) * visualmem_size
		// la primera division es decimal, entre 0 y 1, por eso la realizo al final,
		// multiplico antes y luego divido, asi puedo usar numeros enteros y no necesito decimales
		long long int posicion_final=(address_l*VISUALMEM_MMC_BUFFER_SIZE);

		posicion_final /=mmc_size_l;

		//por si acaso
		if (posicion_final>=0 && posicion_final<VISUALMEM_MMC_BUFFER_SIZE) {
				return posicion_final;
				//printf ("add %d mmc_size %ld visualsize: %d final: %ld\n",address,mmc_size,VISUALMEM_MMC_BUFFER_SIZE,posicion_final);

		}
	}

#endif

	return -1;
}


void mmc_set_visualmem_read(unsigned int address)
{
#ifdef EMULATE_VISUALMEM
	int posicion_final=mmc_get_visualmem_position(address);
	if (posicion_final>=0) {
		set_visualmemmmc_read_buffer(posicion_final);
	}

#endif
}

void mmc_set_visualmem_write(unsigned int address)
{
#ifdef EMULATE_VISUALMEM
	int posicion_final=mmc_get_visualmem_position(address);
	if (posicion_final>=0) {
		set_visualmemmmc_write_buffer(posicion_final);
	}

#endif
}

z80_byte mmc_read_byte_memory(unsigned int address)
{

	//no se ha asignado memoria
	if (mmc_memory_pointer==NULL) return 0xff;

	if (address>=mmc_size) {
		debug_printf (VERBOSE_DEBUG,"Error. Trying to read beyond mmc. Size: %ld Asked: %u",mmc_size,address);
		return 0;
	}
	else {
        mmc_set_visualmem_read(address);

        //printf("leyendo byte de direccion %XH: %02XH. PC=%XH\n",address,mmc_memory_pointer[address],reg_pc);
		return mmc_memory_pointer[address];
	}
}

void mmc_write_byte_memory(unsigned int address,z80_byte value)
{

	//no se ha asignado memoria
	if (mmc_memory_pointer==NULL) return;

        if (address>=mmc_size) {
                debug_printf (VERBOSE_ERR,"Error. Trying to write beyond mmc. Size: %ld Asked: %u. Disabling MMC",mmc_size,address);
		mmc_disable();
		return;
	}

	if (mmc_write_protection.v) return;

	mmc_memory_pointer[address]=value;
	mmc_flash_must_flush_to_disk=1;

	mmc_set_visualmem_write(address);
}


//z80_byte temporal_pruebas_stop_transmission=0;



//Lectura de valor de la controladora mmc
z80_byte mmc_read(void)
{
	if (mmc_enabled.v==0) return 0xFF;

	//Si seleccionada tarjeta invalida, volver sin mas
	if (mmc_tarjeta_invalida_seleccionada) return 0;

	mmc_footer_mmc_operating();

	z80_byte value;

	//Si no esta en idle
	if ((mmc_r1&1)==0) {
		return mmc_r1;
	}

    //printf("MMC read. mmc_last_command=%02XH\n",mmc_last_command);

	//Actuar segun mmc_last_command
	switch (mmc_last_command) {

		case 0x00:
			//Viene de un cs
			if (MACHINE_IS_TBBLUE) return 0xFF; //Temporal. Sin esto no puede cargar el config.ini
			return 0;
		break;

		case 0x40:
			//Devuelve ok
			debug_printf (VERBOSE_PARANOID,"MMC Read command GO_IDLE_STATE");
			return 1;
		break;

		/* Este comando lo usa +3e / NextOS. Ya sea que le devuelva error como si no, sigue reintenando
		y acaba haciendo timeout y arranca la rom
		Quiza esto ya sucederia en un entorno real: este comando lo soportan las tarjetas SD pero no las MMC,
		por lo que deduzco que en un entorno real con MMC, también hace monton de reintentos y acaba haciendo timeout
		*/
		//0x48=CMD8=SEND_IF_COND. For only SDC V2. Check voltage range.
		//Parece que es de deteccion de MMC/SD
		case 0x48:
			debug_printf (VERBOSE_DEBUG,"MMC Read command CMD8 SEND_IF_COND unhandled");

			//mmc_r1 |=4; //Devolver error

            //Parche para poder actualizar desde la bios de zxuno
            if (MACHINE_IS_ZXUNO) {
                //CMD8, SEND_IF_COND (send interface condition), is used to check whether the card is
                //first generation or Version 2.00 (or later). If the card is of first generation,
                //it will respond with R1 with bit 2 set (illegal command).
                return 4;
            }

            else return 0;
		break;

		case 0x49:
			debug_printf (VERBOSE_PARANOID,"MMC Read command SEND_CSD");
			if (mmc_csd_index>=0) {
				//valor primero, byte ncr time
				if (mmc_csd_index==0) {
					//printf ("retornando ncr\n");
					value=0xff;
				}

				//valor segundo, command response 0
				if (mmc_csd_index==1) {
					//printf ("retornando command response\n");
					value=0;
				}


				//Valor tercero feh
				if (mmc_csd_index==2) {
					//printf ("retornando feh\n");
					value=0xFE;
				}

				//Indice de 3-18, array csd
				if (mmc_csd_index>=3 && mmc_csd_index<=18) {
					//printf ("retornando valor csd indice: %d\n",mmc_csd_index-3);
					value=mmc_csd[mmc_csd_index-3];
				}

				//CRC. A FFh
				if (mmc_csd_index==19 || mmc_csd_index==20) {
					//printf ("retornando CRC\n");
					value=0xFF;
				}

				//Si final
				mmc_csd_index++;
				if (mmc_csd_index==21) mmc_csd_index=-1;
				return value;
			}

			//Que devolvemos si aun no se ha enviado todo el comando 49?
			else {
				return 0xFF;
			}
		break;

		case 0x4A:
			debug_printf (VERBOSE_PARANOID,"MMC Read command SEND_CID");
                        if (mmc_cid_index>=0) {
                                //valor primero, byte ncr time
                                if (mmc_cid_index==0) {
                                        //printf ("retornando ncr\n");
                                        value=0xff;
                                }

                                //valor segundo, command response 0
                                if (mmc_cid_index==1) {
                                        //printf ("retornando command response\n");
                                        value=0;
                                }

                                //Valor tercero feh
                                if (mmc_cid_index==2) {
                                        //printf ("retornando feh\n");
                                        value=0xFE;
                                }

                                //Indice de 3-18, array csd
                                if (mmc_cid_index>=3 && mmc_cid_index<=18) {
                                        //printf ("retornando valor csd indice: %d\n",mmc_cid_index-3);
                                        value=mmc_cid[mmc_cid_index-3];
                                }

                                //CRC. A FFh
                                if (mmc_cid_index==19 || mmc_cid_index==20) {
                                        //printf ("retornando CRC\n");
                                        value=0xFF;
                                }

                                //Si final
                                mmc_cid_index++;
                                if (mmc_cid_index==21) mmc_cid_index=-1;
                                return value;
                        }

                        //Que devolvemos si aun no se ha enviado todo el comando 49?
                        else {
                                return 0xFF;
                        }
                break;


                case 0x4C:
                        //debug_printf (VERBOSE_PARANOID,"MMC Read command STOP_TRANSMISSION. PC=%d A=%d BC=%d",reg_pc,reg_a,reg_bc);



                        return 1;
                break;



		case 0x51:
			if (mmc_read_index>=0) {

				//Debug vario
				if (mmc_read_index>=3 && mmc_read_index<=514) {
					debug_printf (VERBOSE_PARANOID,"MMC Read command READ_SINGLE_BLOCK. Adress=%XH Index=%d PC=%d",
					mmc_read_address+mmc_read_index-3,mmc_read_index,reg_pc);
				}
				else debug_printf (VERBOSE_PARANOID,"MMC Read command READ_SINGLE_BLOCK. Index=%d PC=%d",mmc_read_index,reg_pc);

                                //valor primero, byte ncr time
                                if (mmc_read_index==0) value=0xff;

                                //valor segundo, command response 0
                                if (mmc_read_index==1) value=0;

                                //Valor tercero feh
                                if (mmc_read_index==2) value=0xFE;

                                //Indice de 3-514, sector
                                if (mmc_read_index>=3 && mmc_read_index<=514) {
					value=mmc_read_byte_memory(mmc_read_address+mmc_read_index-3);
					//printf ("Retornando byte numero %d con contenido 0x%02X ('%c')\n",mmc_read_index-3,value,
					//(value>=32 && value<=127 ? value : '?') );
				}

                                //CRC. A FFh
                                /*
                                Nota: no estoy seguro viendo mi codigo si este crc se pretende que sean 1 byte o 2
                                Porque tal y como está ahora, será 1 byte de crc. Si vemos sobre mmc_read_index:
                                mmc_read_index=514. ultimo byte de datos
                                mmc_read_index=515. se asigna value=crc=255. Se retorna ese value. Al incrementarse mmc_read_index pasa a ser 516 y luego a -1
                                mmc_read_index=516. se asigna value=crc=255. Pero aqui NO se llega nunca pues mmc_read_index pasa de 515 a 516 y a -1 de golpe
                                */
                                if (mmc_read_index==515 || mmc_read_index==516) value=0xFF;

                                //Si final
                                mmc_read_index++;
                                if (mmc_read_index==516) mmc_read_index=-1;


                                return value;

                        }

                        //Que devolvemos si aun no se ha enviado todo el comando 51?
                        else {
				debug_printf (VERBOSE_PARANOID,"MMC Read command READ_SINGLE_BLOCK. Index<0. Returning FFH. PC=%d",reg_pc);

                                return 0xFF;
                        }
                break;

		//Este comando solo testeado en el arranque del Next. Tambien lo usa el Atic Atac
		case 0x52:
			if (mmc_read_index>=0) {

                //printf("READ_MULTIPLE_BLOCK. mmc_read_index: %d\n",mmc_read_index);

				//Debug vario
                /*
				if (mmc_read_index>=3 && mmc_read_index<=514) {
					debug_printf (VERBOSE_PARANOID,"MMC Read command READ_MULTIPLE_BLOCK. Adress=%XH Index=%d PC=%d A=%d BC=%d",
					    mmc_read_address+mmc_read_index-3,mmc_read_index,reg_pc,reg_a,reg_bc);
				}
				else debug_printf (VERBOSE_PARANOID,"MMC Read command READ_MULTIPLE_BLOCK. Index=%d PC=%d",mmc_read_index,reg_pc);
                */

                //valor primero, byte ncr time
                if (mmc_read_index==0) {
                    value=0xff;
                    //sleep(1);
                    //TODO: atic atac requiere que el primer byte no sea ff, no entiendo del todo por que
                    //aqui de momento lo hacemos cuando se habilita SDHC, lo cual creo que no es correcto
                    //Habria que revisar la respuesta a este comando en la especificación a ver que sucede
                    //En cambio con el Next Boot este valor tiene que ser FF el primero
                    if (mmc_sdhc_addressing.v) value=0xfe;
                }

                //valor segundo, command response 0
                if (mmc_read_index==1) value=0;

                //Valor tercero feh
                if (mmc_read_index==2) value=0xFE;

                //Indice de 3-514, sector
                if (mmc_read_index>=3 && mmc_read_index<=514) {
                    value=mmc_read_byte_memory(mmc_read_address+mmc_read_index-3);
                    //printf ("Retornando byte numero %d con contenido 0x%02X ('%c')\n",mmc_read_index-3,value,
                    //    (value>=32 && value<=127 ? value : '?') );
                }

                //cuando se han leido los 512 bytes, ir al siguiente sector
                if (mmc_read_index==514) mmc_read_index=515;

				/*


                                //CRC. A FFh
                                if (mmc_read_index==515 || mmc_read_index==516) value=0xFF;

				*/

                //Si final
                mmc_read_index++;
                if (mmc_read_index==516) mmc_read_index=-1;


                //Siguiente bloque a leer
                if (mmc_read_index==-1) {
                    mmc_read_index=0;
                    mmc_read_address +=512;
                    //debug_printf (VERBOSE_PARANOID,"MMC: After read 512 bytes on READ_MULTIPLE_BLOCK. Jumping to next Block Read. mmc_read_address=%XH",mmc_read_address);
                }

                return value;

            }

            //Que devolvemos si aun no se ha enviado todo el comando 52?
            else {
				debug_printf (VERBOSE_PARANOID,"MMC Read command READ_MULTIPLE_BLOCK. Index<0. Returning FFH. PC=%d",reg_pc);

                return 0xFF;
            }
        break;

                case 0x58:
			debug_printf (VERBOSE_PARANOID,"MMC Read command WRITE_BLOCK");
                        if (mmc_write_index>=0) {
				//printf ("leyendo status codigo 0x58 con mmc_write_index: %d\n",mmc_write_index);

                                //valor primero, byte ncr time
                                if (mmc_write_index==0) value=0xff;

                                //valor segundo, command response 0
                                if (mmc_write_index==1) value=0;


				//valores siguientes
				if (mmc_write_index==2) value=0xff;
				if (mmc_write_index==3) value=0xff;

				//xxx0sss1
				//sss: status:
				//010 data accepted
				if (mmc_write_index==4) value=4+1;


				if (mmc_write_index>=5) value=4+1;

				mmc_write_index++;

                                return value;

                        }

                        //Que devolvemos si aun no se ha enviado todo el comando 51?
                        else {
                                return 0xFF;
                        }
                break;

                case 0x7A:
			debug_printf (VERBOSE_PARANOID,"MMC Read command READ_OCR");
                        if (mmc_ocr_index>=0) {
                                //valor primero, byte ncr time
                                if (mmc_ocr_index==0) {
                                        //printf ("retornando ncr\n");
                                        value=0xff;
                                }

                                //valor segundo, command response 0
                                if (mmc_ocr_index==1) {
                                        //printf ("retornando command response\n");
                                        value=0;
                                }

                                //Indice de 2-6, array ocr
                                if (mmc_ocr_index>=2 && mmc_ocr_index<=6) {
                                        //printf ("retornando valor ocr indice: %d\n",mmc_ocr_index-2);
                                        value=mmc_ocr[mmc_ocr_index-2];
                                }

                                //CRC. A FFh
                                if (mmc_ocr_index==7 || mmc_ocr_index==8) {
                                        //printf ("retornando CRC\n");
                                        value=0xFF;
                                }

                                //Si final
                                mmc_ocr_index++;
                                if (mmc_ocr_index==9) mmc_ocr_index=-1;
                                return value;
                        }

                        //Que devolvemos si aun no se ha enviado todo el comando?
                        else {
                                return 0xFF;
                        }
              break;


		default:
			debug_printf (VERBOSE_DEBUG,"Reading parameter for MMC unknown command 0x%02X",mmc_last_command);

            //printf ("Reading parameter for MMC unknown command 0x%02X\n",mmc_last_command);
                break;

	}

	return 0;
}

unsigned int mmc_retorna_dir_32bit(z80_byte a,z80_byte b,z80_byte c,z80_byte d)
{
	unsigned int resultado=a*16777216+b*65536+c*256+d;

    if (mmc_sdhc_addressing.v) resultado *=512;

	return resultado;
}


//Escritura a la tarjeta MMC
void mmc_write(z80_byte value)
{

	if (mmc_enabled.v==0) {
        //printf("MMC no habilitada\n");
        return;
    }

        //Si seleccionada tarjeta invalida, volver sin mas
        if (mmc_tarjeta_invalida_seleccionada) {
            return;
        }

	mmc_footer_mmc_operating();


	if (mmc_index_command==0) {
        //printf("MMC Se recibe comando %d (%02XH)\n",value,value);
		//Se recibe comando
		mmc_last_command=value;
		mmc_index_command++;
	}

	else {
        //printf("MMC Se recibe parametro de comando %02XH\n",mmc_last_command);
		//Se recibe parametro de comando
		//Actuar segun mmc_last_command
		switch (mmc_last_command) {
			//GO_IDLE_STATE
			case 0x40:
				debug_printf (VERBOSE_PARANOID,"MMC Write command GO_IDLE_STATE");
				if (mmc_index_command==5) {
					//Estado idle
					mmc_r1=1;
					//Reseteamos indice
					mmc_index_command=0;
				}
				else mmc_index_command++;
			break;

			/* Este comando lo usa +3e / NextOS. Ya sea que le devuelva error como si no, sigue reintenando
			y acaba haciendo timeout y arranca la rom
			Quiza esto ya sucederia en un entorno real: este comando lo soportan las tarjetas SD pero no las MMC,
			por lo que deduzco que en un entorno real con MMC, también hace monton de reintentos y acaba haciendo timeout
			*/
			//0x48=CMD8=SEND_IF_COND. For only SDC V2. Check voltage range.
			//Parece que es de deteccion de MMC/SD
			case 0x48:
				debug_printf (VERBOSE_DEBUG,"MMC Write command CMD8 SEND_IF_COND unhandled");
				//mmc_r1 |=4; //devolver error
			break;



			//SEND_CSD
			case 0x49:
				debug_printf (VERBOSE_PARANOID,"MMC Write command SEND_CSD");
				if (mmc_index_command==5) {
					//Ya se pueden enviar valores csd
					mmc_csd_index=0;
					//Reseteamos indice
                                        mmc_index_command=0;
				}
				else mmc_index_command++;
			break;

                        //SEND_CID
                        case 0x4a:
				debug_printf (VERBOSE_PARANOID,"MMC Write command SEND_CID");
                                //5 valores envia
                                if (mmc_index_command==5) {
                                        //Ya se pueden enviar valores csd
                                        mmc_cid_index=0;
                                        //Reseteamos indice
                                        mmc_index_command=0;
                                }
                                else mmc_index_command++;
                        break;


                case 0x4C:
                    //debug_printf (VERBOSE_PARANOID,"MMC Write command STOP_TRANSMISSION");


			        if (mmc_index_command==5) {
                        //Estado idle
                        mmc_r1=1;
                        //Reseteamos indice
                        mmc_index_command=0;
                    }
                    else mmc_index_command++;
                break;

            break;



			//READ_SINGLE_BLOCK
			case 0x51:
				debug_printf (VERBOSE_PARANOID,"MMC Write command READ_SINGLE_BLOCK");
				mmc_parameters_sent[mmc_index_command-1]=value;
				mmc_index_command++;


				//5 valores y el ff del final
				if (mmc_index_command==6) {
                                        //Reseteamos indice
                                        mmc_index_command=0;

					//Devolvemos datos
					//printf ("Reading byte at address 0x%02X 0x%02X 0x%02X 0x%02X\n",mmc_parameters_sent[0],mmc_parameters_sent[1],
					//	mmc_parameters_sent[2],mmc_parameters_sent[3]);

					unsigned int direccion=mmc_retorna_dir_32bit(mmc_parameters_sent[0],mmc_parameters_sent[1],
                                                mmc_parameters_sent[2],mmc_parameters_sent[3]);
					//printf ("Direccion: 0x%X\n",direccion);
					mmc_read_address=direccion;




					mmc_read_index=0;
                                }
                        break;

			//READ MULTIPLE BLOCK
			case 0x52:
                                //debug_printf (VERBOSE_PARANOID,"MMC Write command READ_MULTIPLE_BLOCK");
                                mmc_parameters_sent[mmc_index_command-1]=value;
                                mmc_index_command++;


                                //5 valores y el ff del final
                                if (mmc_index_command==6) {
                                        //Reseteamos indice
                                        mmc_index_command=0;

                                        //Devolvemos datos
                                        //printf ("Reading byte at address 0x%02X 0x%02X 0x%02X 0x%02X\n",mmc_parameters_sent[0],mmc_parameters_sent[1],
                                        //      mmc_parameters_sent[2],mmc_parameters_sent[3]);

                                        unsigned int direccion=mmc_retorna_dir_32bit(mmc_parameters_sent[0],mmc_parameters_sent[1],
                                                mmc_parameters_sent[2],mmc_parameters_sent[3]);

                                        //printf ("Direccion: 0x%X\n",direccion);
                                        //printf ("MMC Write command READ_MULTIPLE_BLOCK. Address: %XH\n",direccion);
                                        mmc_read_address=direccion;
                                        //debug_printf (VERBOSE_PARANOID,"MMC Write command READ_MULTIPLE_BLOCK. Address: %XH",direccion);



                                        mmc_read_index=0;
                                }
                        break;


			//WRITE_BLOCK
                        case 0x58:
				#define INICIO_WRITE_BLOCK_OFFSET 5
				debug_printf (VERBOSE_PARANOID,"MMC Write command WRITE_BLOCK");
				if (mmc_index_command<5) {
	                                mmc_parameters_sent[mmc_index_command-1]=value;
				}


                                if (mmc_index_command==INICIO_WRITE_BLOCK_OFFSET) {
                                        //Reseteamos indice
                                        //mmc_index_command=0;

                                        //Devolvemos datos
                                        //printf ("Writing sector at address 0x%02X 0x%02X 0x%02X 0x%02X\n",mmc_parameters_sent[0],mmc_parameters_sent[1],
                                          //      mmc_parameters_sent[2],mmc_parameters_sent[3]);

					unsigned int direccion=mmc_retorna_dir_32bit(mmc_parameters_sent[0],mmc_parameters_sent[1],
                                                mmc_parameters_sent[2],mmc_parameters_sent[3]);
					mmc_write_address=direccion;

					//printf ("Direccion: 0x%X\n",direccion);


                                        mmc_write_index=0;
                                }

				//if (mmc_index_command==INICIO_WRITE_BLOCK_OFFSET) printf ("recibido byte gap\n");
				//if (mmc_index_command==INICIO_WRITE_BLOCK_OFFSET+1) printf ("recibido data token\n");

				if (mmc_index_command>=INICIO_WRITE_BLOCK_OFFSET+2 && mmc_index_command<=INICIO_WRITE_BLOCK_OFFSET+2+512-1) {
					//printf ("Escribiendo byte numero %d valor: %d %c\n",mmc_index_command-(INICIO_WRITE_BLOCK_OFFSET+2),value,
					//	(value>=31 && value<=127 ? value : '.'));

					mmc_write_byte_memory(mmc_write_address+mmc_index_command-(INICIO_WRITE_BLOCK_OFFSET+2) , value);
				}


				mmc_index_command++;
				if (mmc_index_command==INICIO_WRITE_BLOCK_OFFSET+2+512) {
					//printf ("byte final de escritura\n");
					//mmc_index_command=0;
				}
                        break;


			//CMD58. READ_OCR
			//Parameters: None
			//Response: R3, OCR
			//Aunque en divmmc, si quitamos este comando no cambia nada
			//En cambio si lo implementamos mal (por ejemplo devolviedo diferentes valores de retorno) la tarjeta no la detecta
			case 0x7A:
				debug_printf (VERBOSE_PARANOID,"MMC Write command READ_OCR");
                                //5 valores envia
                                if (mmc_index_command==5) {
                                        //Ya se pueden enviar valores ocr
                                        mmc_ocr_index=0;
                                        //Reseteamos indice
                                        mmc_index_command=0;
                                }
                                else mmc_index_command++;
                        break;





			default:
				debug_printf (VERBOSE_DEBUG,"Received parameter for MMC unknown command 0x%02X",mmc_last_command);
				/*
Similarly, if an illegal command has been received, a card shall not change its state, shall not response and shall set the ILLEGAL_COMMAND error bit in the status register.
				*/
				//mmc_r1 |=4;
			break;

		}
	}

}


void zxmmc_write_port(z80_byte puerto_l,z80_byte value)
{

    if (puerto_l==0x1f) mmc_cs(value);
    if (puerto_l==0x3f) mmc_write(value);

}

z80_byte zxmmc_read_port(z80_byte puerto_l)
{

    //printf ("Puerto ZXMMC Read: 0x%02x\n",puerto_l);
    if (puerto_l==0x3f) {
        z80_byte valor_leido=mmc_read();
        //printf ("Valor leido: %d\n",valor_leido);
        return valor_leido;
    }

    return 255;


}