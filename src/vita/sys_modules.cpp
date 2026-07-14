/*
===========================================================================
Copyright (C) 2026, JAMPVITA contributors

This file is part of JAMPVITA, a PS Vita port built on the OpenJK
source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// Static module dispatch. game/cgame/ui are each partially linked (ld -r) with
// every symbol localized except GetModuleAPI renamed per module (objcopy), so
// one ELF carries three modules whose bg_* copies can't collide.

#include <string.h>
#include "sys/sys_public.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

// unbuffered boot-milestone trail; survives a hang + forced power-off
void Sys_BootMark( const char *s )
{
	static int first = 1;
	if ( first ) {
		sceIoMkdir( "ux0:data/JAVITA", 0777 );
	}
	SceUID fd = sceIoOpen( "ux0:data/JAVITA/mpboot.log",
		SCE_O_WRONLY | SCE_O_CREAT | ( first ? SCE_O_TRUNC : SCE_O_APPEND ), 0666 );
	first = 0;
	if ( fd < 0 ) return;
	sceIoWrite( fd, s, strlen( s ) );
	sceIoWrite( fd, "\n", 1 );
	sceIoClose( fd );
}

extern "C" {
GetModuleAPIProc GetModuleAPI_game;
GetModuleAPIProc GetModuleAPI_cgame;
GetModuleAPIProc GetModuleAPI_ui;
}

GetModuleAPIProc *Sys_VitaStaticModuleAPI( const char *name )
{
	Sys_BootMark( name );
	if ( strstr( name, "cgame" ) )	return GetModuleAPI_cgame;
	if ( strstr( name, "ui" ) )		return GetModuleAPI_ui;
	if ( strstr( name, "game" ) )	return GetModuleAPI_game;	// jampgame
	return NULL;
}
