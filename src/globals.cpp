/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

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

//============================================================================
// JAVITA — static-build glue translation unit.
//
// JK2 SP is built here as a SINGLE statically-linked executable (the Vita has
// no dlopen, and SP has no QVM). On desktop, OpenJK builds three separate
// binaries — engine (openjo_sp), renderer (rdjosp-vanilla) and game
// (jospgame) — and each may carry its own definition of certain globals.
// Linked together into one ELF, those become "multiple definition" errors.
//
// This file is the single, intentional home for such globals: as Phase 2
// link errors surface a redundant definition, the duplicates are removed from
// their original TUs (left as `extern`) and the one true definition lives here.
// We never `#if 0` real engine/game code to dodge a collision.
//
// Kept deliberately minimal; grown only in response to real link diagnostics.
//============================================================================

#ifdef VITA
// Sizes the newlib heap — the engine's whole malloc budget (Z_Malloc/Hunk_Alloc are
// thin malloc wrappers), carved from the ~365 MiB USER partition before vitaGL takes
// its texture pool, so a bigger heap leaves vitaGL less. JKA's yavin1 peaks past the
// old 144 MiB (97 MiB zone + sound pool). A heavy yavin1b save peaks the zone at
// ~107 MiB; picmip 3 + r_texturebits 16 shrink vitaGL's texture pool so the freed
// USER-partition space goes to the heap: 224 MiB of headroom keeps the working set off
// the freeup-cascade cliff. The 16 MiB image-decode OOM that used to hit here was
// fragmentation (no contiguous hole), not size — it's fixed by the boot-reserved
// transient arena in z_memman_pc.cpp, which also pulls ~28 MiB of Upload32 workspace
// out of the zone, so this heap mostly carries the level/model/sound working set now.
unsigned int _newlib_heap_size_user = 224 * 1024 * 1024;

// VitaSDK reads this to size the main thread's stack. The default (~256 KB) is
// too small for rd-vanilla: R_SubdividePatchToGrid alone uses a ~332 KB stack
// frame (large local drawVert_t grids) during BSP load and overflows it, faulting
// with a data abort. 4 MB is plenty of headroom for the engine's deep call paths.
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;
#endif

