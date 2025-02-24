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

#ifndef COMMON_SDL_H
#define COMMON_SDL_H

#include "cpu.h"

extern z80_bit audiosdl_inicializado;
extern z80_bit scrsdl_inicializado;


extern int commonsdl_init(void);
extern void commonsdl_end(void);
extern int commonsdl_init_timer(void);
extern void commonsdl_stop_timer(void);

#endif
