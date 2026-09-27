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

// the watchdog thread and main both write, and they share the stamp origin
static volatile unsigned char s_trailLock = 0;

// appends without claiming to be a milestone, so the watchdog cannot erase the real one
static void Sys_BootWrite( const char *s )
{
	static int first = 1;
	static SceUInt64 base, prev;
	while ( __atomic_test_and_set( &s_trailLock, __ATOMIC_ACQUIRE ) ) {
		sceKernelDelayThread( 100 );
	}
	if ( first ) {
		sceIoMkdir( "ux0:data/JAVITA", 0777 );
		base = prev = sceKernelGetProcessTimeWide();
	}
	SceUID fd = sceIoOpen( "ux0:data/JAVITA/mpboot.log",
		SCE_O_WRONLY | SCE_O_CREAT | ( first ? SCE_O_TRUNC : SCE_O_APPEND ), 0666 );
	if ( fd < 0 ) {				// leave first set, so a later call still truncates
		__atomic_clear( &s_trailLock, __ATOMIC_RELEASE );
		return;
	}
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
	__atomic_clear( &s_trailLock, __ATOMIC_RELEASE );
}

void Sys_BootMark( const char *s )
{
	Sys_BootWrite( s );
	strncpy( g_lastBootMark, s, sizeof( g_lastBootMark ) - 1 );
}

// main-loop stall watchdog: names any main-thread freeze in the trail
volatile unsigned int g_vitaMainTicks = 0;
static volatile int s_wdQuit;
static SceUID s_wdThid = -1;

static int Sys_StallWatchdog( SceSize argc, void *argv )
{
	unsigned int last = 0;
	int stalledFor = 0;
	while ( !s_wdQuit ) {
		// sliced, so a quit is noticed promptly instead of up to five seconds later
		for ( int i = 0; i < 20 && !s_wdQuit; i++ ) {
			sceKernelDelayThread( 250 * 1000 );
		}
		if ( s_wdQuit ) {
			break;
		}
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
	return sceKernelExitThread( 0 );
}

void Sys_StartStallWatchdog( void )
{
	s_wdQuit = 0;
	s_wdThid = sceKernelCreateThread( "stall_wd", Sys_StallWatchdog, 0x10000100, 0x4000, 0, 0, NULL );
	if ( s_wdThid >= 0 ) sceKernelStartThread( s_wdThid, 0, NULL );
}

// a thread still writing to the card blocks process exit, so it is joined before teardown
void Sys_StopStallWatchdog( void )
{
	if ( s_wdThid < 0 ) {
		return;
	}
	s_wdQuit = 1;
	SceUInt tmo = 3 * 1000 * 1000;
	sceKernelWaitThreadEnd( s_wdThid, NULL, &tmo );
	sceKernelDeleteThread( s_wdThid );
	s_wdThid = -1;
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
