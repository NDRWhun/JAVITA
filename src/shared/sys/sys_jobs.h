/*
===========================================================================
Copyright (C) 2026, JAVITA contributors

This file is part of JAVITA, a PS Vita port built on the OpenJK
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

#pragma once

// fork-join job pool shared by SP and MP: the workers and the calling thread drain one indexed job list together

#define SYS_JOB_MAX_WORKERS	2							// one on core 0, one on CPU3 when the kernel allows it
#define SYS_THREAD_SLOTS	( 1 + SYS_JOB_MAX_WORKERS )	// slot 0 is the main thread, worker n runs as slot n + 1

typedef void ( *sysJobFn_t )( void *ctx, int index );

void	Sys_JobsInit( void );			// main thread, before anything reads Sys_ThreadSlot
void	Sys_JobsShutdown( void );		// joins and deletes the workers
int		Sys_JobWorkerCount( void );
void	Sys_SetThreadSlot( int slot );
void	Sys_RunJobs( sysJobFn_t fn, int count, void *ctx );	// runs fn( ctx, 0 .. count - 1 ) and returns when all are done

#ifdef VITA
#include <psp2/kernel/threadmgr.h>

extern int sys_threadSlotTlsOffset;		// byte offset of the slot word from the thread's TLS base register

// reads the calling thread's slot from its user TLS word; only the main thread and pool workers have one
static inline int Sys_ThreadSlot( void ) {
	unsigned int tls;
	__asm__( "mrc p15, 0, %0, c13, c0, 3" : "=r" ( tls ) );
	return *(const int *)( tls + sys_threadSlotTlsOffset );
}

// creates a helper thread on CPU3 when the pool got CPU3, otherwise on core 0
SceUID	Sys_CreateAuxThread( const char *name, SceKernelThreadEntry entry, int priority, int stackSize );
#else
extern thread_local int sys_threadSlot;

static inline int Sys_ThreadSlot( void ) {
	return sys_threadSlot;
}
#endif
