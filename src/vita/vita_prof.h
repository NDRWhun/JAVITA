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

// vita_prof.h -- load-path cost accounting for the boot trail

#ifndef VITA_PROF_H
#define VITA_PROF_H

#ifdef VITA

#include <psp2/kernel/processmgr.h>

extern unsigned int g_profVmUs, g_profVmCalls;
extern unsigned int g_profShaderUs, g_profShaderCalls;
extern unsigned int g_profSoundUs, g_profSoundCalls;
extern unsigned int g_profModelUs, g_profModelCalls;
extern unsigned int g_profFsUs, g_profFsCalls, g_profFsKb;
extern unsigned int g_profImgUs, g_profImgCalls;

void Sys_ProfMark( const char *tag );

// charges the enclosing scope to a counter pair, whatever path it returns by
struct VitaProfScope {
	SceUInt64		t0;
	unsigned int	*us;
	unsigned int	*calls;
	VitaProfScope( unsigned int *pUs, unsigned int *pCalls )
		: t0( sceKernelGetProcessTimeWide() ), us( pUs ), calls( pCalls ) {}
	~VitaProfScope() {
		*us += (unsigned int)( sceKernelGetProcessTimeWide() - t0 );
		(*calls)++;
	}
};

#define VITA_PROF( name )	VitaProfScope vitaProf_( &g_prof##name##Us, &g_prof##name##Calls )

#else
#define VITA_PROF( name )	do {} while(0)
#endif

#endif // VITA_PROF_H
