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
#include <stdio.h>
#include "sys/sys_public.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

// unbuffered boot-milestone trail; survives a hang + forced power-off
char g_lastBootMark[64] = "none";

// appends without claiming to be a milestone, so the watchdog cannot erase the real one
static void Sys_BootWrite( const char *s )
{
	static int first = 1;
	static SceUInt64 base, prev;
	if ( first ) {
		sceIoMkdir( "ux0:data/JAVITA", 0777 );
		base = prev = sceKernelGetProcessTimeWide();
	}
	SceUID fd = sceIoOpen( "ux0:data/JAVITA/mpboot.log",
		SCE_O_WRONLY | SCE_O_CREAT | ( first ? SCE_O_TRUNC : SCE_O_APPEND ), 0666 );
	if ( fd < 0 ) return;		// leave first set, so a later call still truncates
	first = 0;

	// absolute and since-previous, so a gap between marks is as visible as a slow mark
	const SceUInt64 now = sceKernelGetProcessTimeWide();
	char stamp[32];
	snprintf( stamp, sizeof( stamp ), "[%6u +%5u] ",
		(unsigned)( ( now - base ) / 1000 ), (unsigned)( ( now - prev ) / 1000 ) );
	prev = now;

	sceIoWrite( fd, stamp, strlen( stamp ) );
	sceIoWrite( fd, s, strlen( s ) );
	sceIoWrite( fd, "\n", 1 );
	sceIoClose( fd );
}

void Sys_BootMark( const char *s )
{
	Sys_BootWrite( s );
	strncpy( g_lastBootMark, s, sizeof( g_lastBootMark ) - 1 );
}

// microseconds and call counts for the load-path cost centres; racy by design,
// they only ever feed the trail
unsigned int g_profVmUs, g_profVmCalls;
unsigned int g_profShaderUs, g_profShaderCalls;
unsigned int g_profSoundUs, g_profSoundCalls;
unsigned int g_profModelUs, g_profModelCalls;
unsigned int g_profFsUs, g_profFsCalls, g_profFsKb;
unsigned int g_profImgUs, g_profImgCalls;

// everything the engine was asked to do, so whatever is left is module cpu
void Sys_ProfMark( const char *tag )
{
	char line[256];
	snprintf( line, sizeof( line ),
		"%s | vm %ums/%u shader %ums/%u img %ums/%u snd %ums/%u mdl %ums/%u fs %ums/%u %uKB",
		tag,
		g_profVmUs / 1000, g_profVmCalls,
		g_profShaderUs / 1000, g_profShaderCalls,
		g_profImgUs / 1000, g_profImgCalls,
		g_profSoundUs / 1000, g_profSoundCalls,
		g_profModelUs / 1000, g_profModelCalls,
		g_profFsUs / 1000, g_profFsCalls, g_profFsKb );
	Sys_BootWrite( line );
}

// main-loop stall watchdog: names any main-thread freeze in the trail
volatile unsigned int g_vitaMainTicks = 0;

static int Sys_StallWatchdog( SceSize argc, void *argv )
{
	unsigned int last = 0;
	int stalledFor = 0;
	for ( ;; ) {
		sceKernelDelayThread( 5 * 1000 * 1000 );
		const unsigned int now = g_vitaMainTicks;
		if ( now == last && now != 0 ) {
			stalledFor += 5;
			char msg[128];
			snprintf( msg, sizeof( msg ), "STALL %ds (last mark: %s)", stalledFor, g_lastBootMark );
			Sys_BootWrite( msg );
		} else {
			if ( stalledFor ) Sys_BootWrite( "stall recovered" );
			stalledFor = 0;
		}
		last = now;
	}
	return 0;
}

void Sys_StartStallWatchdog( void )
{
	SceUID t = sceKernelCreateThread( "stall_wd", Sys_StallWatchdog, 0x10000100, 0x4000, 0, 0, NULL );
	if ( t >= 0 ) sceKernelStartThread( t, 0, NULL );
}

#ifndef SP_GAME
// single player links its game code directly, so only MP has the isolated modules
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
#endif
