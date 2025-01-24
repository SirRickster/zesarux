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

#ifndef SNAP_ZSF_H
#define SNAP_ZSF_H

//Importante NO alterar los ID existentes o se rompera compatibilidad hacia atras del formato ZSF
//Siempre agregar, nunca modificar ni borrar
#define ZSF_NOOP_ID 0
#define ZSF_MACHINEID 1
#define ZSF_Z80_REGS_ID 2
#define ZSF_MOTO_REGS_ID 3
#define ZSF_RAMBLOCK 4
#define ZSF_SPEC128_MEMCONF 5
#define ZSF_SPEC128_RAMBLOCK 6
#define ZSF_AYCHIP 7
#define ZSF_ULA 8
#define ZSF_ULAPLUS 9
#define ZSF_ZXUNO_RAMBLOCK 10
#define ZSF_ZXUNO_CONF 11
#define ZSF_ZX8081_CONF 12
#define ZSF_ZXEVO_NVRAM 13
#define ZSF_TSCONF_RAMBLOCK 14
#define ZSF_TSCONF_CONF 15
#define ZSF_DIVIFACE_CONF 16
#define ZSF_DIVIFACE_MEM 17
#define ZSF_CPC_RAMBLOCK 18
#define ZSF_CPC_CONF 19
#define ZSF_PENTAGON_CONF 20
#define ZSF_TBBLUE_RAMBLOCK 21
#define ZSF_TBBLUE_CONF 22
#define ZSF_TBBLUE_PALETTES 23
#define ZSF_TBBLUE_SPRITES 24
#define ZSF_TIMEX 25
#define ZSF_MSX_MEMBLOCK 26
#define ZSF_MSX_CONF 27
#define ZSF_VDP_9918A_VRAM 28
#define ZSF_GENERIC_LINEAR_MEM 29
#define ZSF_VDP_9918A_CONF 30
#define ZSF_SNCHIP 31
#define ZSF_SVI_CONF 32
#define ZSF_DATETIME 33
#define ZSF_QL_RAMBLOCK 34
#define ZSF_QL_CONF 35
#define ZSF_SMS_ROMBLOCK 36
#define ZSF_SMS_RAMBLOCK 37
#define ZSF_SMS_CONF 38
#define ZSF_SMS_CRAM 39
#define ZSF_ACE_CONF 40
#define ZSF_Z88_MEMBLOCK 41
#define ZSF_Z88_CONF 42
#define ZSF_Z80_HALT_STATE 43
#define ZSF_TIMEX_DOCK_ROM 44
#define ZSF_MK14_REGS_ID 45
#define ZSF_MK14_MEMBLOCK 46
#define ZSF_MK14_LEDS 47
#define ZSF_CHROME_RAMBLOCK 48
#define ZSF_PRISM_CONF 49
#define ZSF_PRISM_RAMBLOCK 50
#define ZSF_PRISM_VRAMBLOCK 51
#define ZSF_CHLOE_HOME_RAMBLOCK 52
#define ZSF_CHLOE_EX_RAMBLOCK 53
#define ZSF_CHLOE_DOCK_RAMBLOCK 54
#define ZSF_SAM_COUPE_CONF 55
#define ZSF_SAM_COUPE_RAMBLOCK 56
#define ZSF_PCW_CONF 57
#define ZSF_PCW_RAMBLOCK 58
#define ZSF_COMMON_ROMBLOCK 59
#define ZSF_CREATOR 60
#define ZSF_FLASH_STATE 61
#define ZSF_KEY_PORTS_SPECTRUM_STATE 62
#define ZSF_ZOC_ETC 63
#define ZSF_I8049_AUDIO 64
#define ZSF_KEY_PORTS_QL_STATE 65
#define ZSF_KEY_PORTS_CPC_STATE 66
#define ZSF_KEY_PORTS_MSX_STATE 67
#define ZSF_KEY_PORTS_SVI_STATE 68
#define ZSF_KEY_PORTS_PCW_STATE 69
#define ZSF_KEY_PORTS_SAM_STATE 70
#define ZSF_KEY_PORTS_Z88_STATE 71
#define ZSF_KEY_PORTS_MK14_STATE 72
#define ZSF_TBBLUE_CLIPWINDOWS 73
#define ZSF_DATAGEAR_DMA 74
#define ZSF_TEXT_NOTE 75

//Id maximo de nombres
#define MAX_ZSF_BLOCK_ID_NAMES 75

//Importante NO alterar los ID existentes o se rompera compatibilidad hacia atras del formato ZSF
//Siempre agregar, nunca modificar ni borrar


#define MAX_ZSF_SNAPSHOT_SIZE 1024*1024*16




#define SNAP_ZSF_NOTE_LENGTH 100



extern void load_zsf_snapshot(char *filename);
extern void save_zsf_snapshot(char *filename);

extern char *zsf_get_block_id_name(int block_id);

extern char zsf_magic_header[];

extern int zsf_force_uncompressed;

extern void save_zsf_snapshot_file_mem(char *filename,z80_byte *destination_memory,int *longitud_total,int from_zeng_online);
extern void load_zsf_snapshot_file_mem(char *filename,z80_byte *origin_memory,int longitud_memoria,int load_fast_mode,int from_zeng_online);

extern void load_zsf_snapshot_block_data_addr(z80_byte *block_data,z80_byte *destino,int block_lenght, int longitud_original,int si_comprimido);

extern z80_byte *pending_zrcp_put_snapshot_buffer_destino;
extern int pending_zrcp_put_snapshot_longitud;
extern void check_pending_zrcp_put_snapshot(void);

extern char snap_zsf_note_save[];
extern char snap_zsf_note_loaded[];

#endif
