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

extern "C" {
GetModuleAPIProc GetModuleAPI_game;
GetModuleAPIProc GetModuleAPI_cgame;
GetModuleAPIProc GetModuleAPI_ui;
}

GetModuleAPIProc *Sys_VitaStaticModuleAPI( const char *name )
{
	if ( strstr( name, "cgame" ) )	return GetModuleAPI_cgame;
	if ( strstr( name, "ui" ) )		return GetModuleAPI_ui;
	if ( strstr( name, "game" ) )	return GetModuleAPI_game;	// jampgame
	return NULL;
}
