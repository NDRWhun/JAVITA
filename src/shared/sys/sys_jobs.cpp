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

#include "qcommon/qcommon.h"
#include "sys_jobs.h"

#ifdef VITA
#include <psp2/kernel/cpu.h>

#define SYS_TLS_KEY_THREADSLOT	0x8A			// user TLS key next to newlib's reent key 0x89
#define SYS_JOB_PRIORITY		0x10000101		// one step below snd_mix, so the mixer preempts a worker on core 0
#define SYS_JOB_STACK			0x10000

int sys_threadSlotTlsOffset;

static SceUID			s_jobThid[SYS_JOB_MAX_WORKERS] = { -1, -1 };
static SceUID			s_jobWake[SYS_JOB_MAX_WORKERS] = { -1, -1 };
static unsigned int		s_jobMask[SYS_JOB_MAX_WORKERS];
static SceUID			s_jobDone = -1;
static int				s_jobWorkers;
static unsigned int		s_auxMask = SCE_KERNEL_CPU_MASK_USER_0;
static volatile int		s_jobQuit;
static volatile int		s_jobBusy;

static sysJobFn_t		s_jobFn;
static void				*s_jobCtx;
static int				s_jobCount;
static volatile int		s_jobNext;

static inline unsigned int Sys_TlsBase( void ) {
	unsigned int tls;
	__asm__( "mrc p15, 0, %0, c13, c0, 3" : "=r" ( tls ) );
	return tls;
}

void Sys_SetThreadSlot( int slot ) {
	*(int *)( Sys_TlsBase() + sys_threadSlotTlsOffset ) = slot;
}

int Sys_JobWorkerCount( void ) {
	return s_jobWorkers;
}

static void Sys_DrainJobs( void ) {
	const sysJobFn_t fn = s_jobFn;
	void *ctx = s_jobCtx;
	const int count = s_jobCount;

	for ( ;; ) {
		const int i = __atomic_fetch_add( &s_jobNext, 1, __ATOMIC_RELAXED );
		if ( i >= count ) {
			break;
		}
		fn( ctx, i );
	}
}

static int Sys_JobWorker( SceSize argc, void *argv ) {
	const int worker = *(const int *)argv;
	const SceUID wake = s_jobWake[worker];

	Sys_SetThreadSlot( worker + 1 );
	for ( ;; ) {
		sceKernelWaitSema( wake, 1, NULL );
		if ( s_jobQuit ) {
			break;
		}
		__atomic_thread_fence( __ATOMIC_ACQUIRE );
		Sys_DrainJobs();
		__atomic_thread_fence( __ATOMIC_RELEASE );
		sceKernelSignalSema( s_jobDone, 1 );
	}
	// plain exit; Sys_JobsShutdown joins and deletes the thread
	return sceKernelExitThread( 0 );
}

// CPU3 needs a kernel plugin; either the create or the affinity change may be the call it unlocks
static SceUID Sys_CreateCpu3Thread( const char *name, SceKernelThreadEntry entry, int priority, int stackSize, int *err ) {
	SceUID thid = sceKernelCreateThread( name, entry, priority, stackSize, 0, SCE_KERNEL_CPU_MASK_SYSTEM, NULL );
	if ( thid >= 0 ) {
		return thid;
	}
	*err = thid;
	thid = sceKernelCreateThread( name, entry, priority, stackSize, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL );
	if ( thid < 0 ) {
		return thid;
	}
	const int res = sceKernelChangeThreadCpuAffinityMask( thid, SCE_KERNEL_CPU_MASK_SYSTEM );
	if ( res < 0 ) {
		sceKernelDeleteThread( thid );
		*err = res;
		return res;
	}
	return thid;
}

void Sys_JobsInit( void ) {
	if ( s_jobDone >= 0 ) {
		return;
	}

	void *slotWord = sceKernelGetTLSAddr( SYS_TLS_KEY_THREADSLOT );
	if ( !slotWord ) {
		Sys_Error( "job pool: TLS key 0x%X unavailable", SYS_TLS_KEY_THREADSLOT );
	}
	sys_threadSlotTlsOffset = (int)( (unsigned int)slotWord - Sys_TlsBase() );
	Sys_SetThreadSlot( 0 );

	s_jobDone = sceKernelCreateSema( "job_done", 0, 0, SYS_JOB_MAX_WORKERS, NULL );
	if ( s_jobDone < 0 ) {
		Com_Printf( "job pool: no workers (semaphore 0x%08X)\n", (unsigned int)s_jobDone );
		return;
	}

	static const char *const wakeNames[SYS_JOB_MAX_WORKERS] = { "job_wake0", "job_wake1" };
	static const char *const thNames[SYS_JOB_MAX_WORKERS] = { "job0", "job1" };
	static int workerIndex[SYS_JOB_MAX_WORKERS];
	int cpu3Err = 0;

	for ( int w = 0; w < SYS_JOB_MAX_WORKERS; w++ ) {
		const int n = s_jobWorkers;
		s_jobWake[n] = sceKernelCreateSema( wakeNames[n], 0, 0, 1, NULL );
		if ( s_jobWake[n] < 0 ) {
			if ( w == 1 ) cpu3Err = s_jobWake[n];
			s_jobWake[n] = -1;
			continue;
		}

		SceUID thid;
		if ( w == 0 ) {
			thid = sceKernelCreateThread( thNames[n], Sys_JobWorker, SYS_JOB_PRIORITY, SYS_JOB_STACK, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL );
		} else {
			thid = Sys_CreateCpu3Thread( thNames[n], Sys_JobWorker, SYS_JOB_PRIORITY, SYS_JOB_STACK, &cpu3Err );
		}
		if ( thid < 0 ) {
			if ( w == 1 && !cpu3Err ) cpu3Err = thid;
			sceKernelDeleteSema( s_jobWake[n] );
			s_jobWake[n] = -1;
			continue;
		}

		s_jobThid[n] = thid;
		s_jobMask[n] = ( w == 0 ) ? SCE_KERNEL_CPU_MASK_USER_0 : SCE_KERNEL_CPU_MASK_SYSTEM;
		workerIndex[n] = n;
		sceKernelStartThread( thid, sizeof( workerIndex[n] ), &workerIndex[n] );
		s_jobWorkers++;
		if ( w == 1 ) {
			s_auxMask = SCE_KERNEL_CPU_MASK_SYSTEM;
		}
	}

	char cores[64] = "";
	for ( int n = 0; n < s_jobWorkers; n++ ) {
		Q_strcat( cores, sizeof( cores ), n ? ", " : "" );
		Q_strcat( cores, sizeof( cores ), s_jobMask[n] == SCE_KERNEL_CPU_MASK_SYSTEM ? "CPU3" : "core 0" );
	}
	if ( s_auxMask == SCE_KERNEL_CPU_MASK_SYSTEM ) {
		Com_Printf( "job pool: %d worker(s) on %s\n", s_jobWorkers, s_jobWorkers ? cores : "none" );
	} else {
		Com_Printf( "job pool: %d worker(s) on %s; CPU3 unavailable (0x%08X)\n", s_jobWorkers, s_jobWorkers ? cores : "none", (unsigned int)cpu3Err );
	}
}

void Sys_JobsShutdown( void ) {
	if ( s_jobDone < 0 ) {
		return;
	}

	const SceUID self = sceKernelGetThreadId();
	s_jobQuit = 1;
	__atomic_thread_fence( __ATOMIC_SEQ_CST );
	for ( int n = 0; n < s_jobWorkers; n++ ) {
		sceKernelSignalSema( s_jobWake[n], 1 );
	}
	for ( int n = 0; n < s_jobWorkers; n++ ) {
		if ( s_jobThid[n] != self ) {
			SceUInt tmo = 3 * 1000 * 1000;
			sceKernelWaitThreadEnd( s_jobThid[n], NULL, &tmo );
			sceKernelDeleteThread( s_jobThid[n] );
		}
		sceKernelDeleteSema( s_jobWake[n] );
		s_jobThid[n] = -1;
		s_jobWake[n] = -1;
	}
	sceKernelDeleteSema( s_jobDone );
	s_jobDone = -1;
	s_jobWorkers = 0;
	s_jobQuit = 0;
	s_auxMask = SCE_KERNEL_CPU_MASK_USER_0;
}

SceUID Sys_CreateAuxThread( const char *name, SceKernelThreadEntry entry, int priority, int stackSize ) {
	SceUID thid = sceKernelCreateThread( name, entry, priority, stackSize, 0, s_auxMask, NULL );
	if ( thid < 0 && s_auxMask != SCE_KERNEL_CPU_MASK_USER_0 ) {
		thid = sceKernelCreateThread( name, entry, priority, stackSize, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL );
	}
	return thid;
}

// waits for the woken workers even when a job throws out of Sys_RunJobs on the calling thread
struct sysJobJoin_t {
	int helpers;
	~sysJobJoin_t() {
		sceKernelWaitSema( s_jobDone, helpers, NULL );
		__atomic_thread_fence( __ATOMIC_ACQUIRE );
		__atomic_store_n( &s_jobBusy, 0, __ATOMIC_RELEASE );
	}
};

void Sys_RunJobs( sysJobFn_t fn, int count, void *ctx ) {
	if ( count <= 0 ) {
		return;
	}

	const int helpers = ( s_jobWorkers < count - 1 ) ? s_jobWorkers : count - 1;
	// one fork-join at a time: a nested or concurrent call runs its jobs inline
	if ( helpers <= 0 || __atomic_exchange_n( &s_jobBusy, 1, __ATOMIC_ACQUIRE ) ) {
		for ( int i = 0; i < count; i++ ) {
			fn( ctx, i );
		}
		return;
	}

	s_jobFn = fn;
	s_jobCtx = ctx;
	s_jobCount = count;
	s_jobNext = 0;
	__atomic_thread_fence( __ATOMIC_RELEASE );
	for ( int n = 0; n < helpers; n++ ) {
		sceKernelSignalSema( s_jobWake[n], 1 );
	}

	sysJobJoin_t join = { helpers };
	Sys_DrainJobs();
}

#else // !VITA

thread_local int sys_threadSlot;

void Sys_JobsInit( void ) {
	sys_threadSlot = 0;
}

void Sys_JobsShutdown( void ) {
}

int Sys_JobWorkerCount( void ) {
	return 0;
}

void Sys_SetThreadSlot( int slot ) {
	sys_threadSlot = slot;
}

void Sys_RunJobs( sysJobFn_t fn, int count, void *ctx ) {
	for ( int i = 0; i < count; i++ ) {
		fn( ctx, i );
	}
}

#endif // VITA
