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

// gxm_world.cpp -- world vertices uploaded once per map into groups, indexes cached and gathered into the frame ring per draw

#include "gxm_world.h"
#include "gxm_device.h"
#include "gxm_texture.h"

#include <string.h>
#include <stdlib.h>

static_assert( sizeof(gxmWorldDrawVert_t) == 80, "gxmWorldDrawVert_t must match drawVert_t" );

// the most segments one gather produces
#define WORLD_MAX_SEGS		32

typedef struct {
	unsigned char	*data;
	SceUID			uid;
	int				numVerts;
	int				styled;
} worldGroup_t;

static worldGroup_t		*world_groups;
static int				world_numGroups;
static unsigned short	*world_indexes;

void GXM_LmAtlasRemap( int slot, float st[2] )
{
	const int n = slot & ( GXM_LMATLAS_SLOTS - 1 );
	const float inset = 0.5f / GXM_LMATLAS_LIGHTMAP;
	for ( int i = 0; i < 2; i++ ) {
		float v = st[i];
		if ( v < inset ) {
			v = inset;
		} else if ( v > 1.0f - inset ) {
			v = 1.0f - inset;
		}
		st[i] = v;
	}
	st[0] = ( (float)( n % GXM_LMATLAS_COLS ) + st[0] ) * ( 1.0f / GXM_LMATLAS_COLS );
	st[1] = ( (float)( n / GXM_LMATLAS_COLS ) + st[1] ) * ( 1.0f / GXM_LMATLAS_COLS );
}

int GXM_LmAtlasTexelOffset( int slot )
{
	const int n = slot & ( GXM_LMATLAS_SLOTS - 1 );
	return ( n / GXM_LMATLAS_COLS ) * GXM_LMATLAS_LIGHTMAP * GXM_LMATLAS_SIZE
		+ ( n % GXM_LMATLAS_COLS ) * GXM_LMATLAS_LIGHTMAP;
}

void GXM_WorldFree( void )
{
	for ( int g = 0; g < world_numGroups; g++ ) {
		if ( world_groups[g].data ) {
			GXM_Free( world_groups[g].uid );
		}
	}
	free( world_groups );
	free( world_indexes );
	world_groups = NULL;
	world_indexes = NULL;
	world_numGroups = 0;
}

int GXM_WorldGroupStyled( int group )
{
	return ( group >= 0 && group < world_numGroups && world_groups[group].styled ) ? 1 : 0;
}

// the surface list WorldCompareSurf orders
static const gxmWorldSurfInfo_t	*world_sortInfo;

static int WorldCompareSurf( const void *a, const void *b )
{
	const int ia = *(const int *)a, ib = *(const int *)b;
	const gxmWorldSurfInfo_t *sa = &world_sortInfo[ia], *sb = &world_sortInfo[ib];
	if ( sa->styled != sb->styled ) {
		return sa->styled - sb->styled;
	}
	if ( sa->key != sb->key ) {
		return ( sa->key < sb->key ) ? -1 : 1;
	}
	return ia - ib;
}

/*
================
GXM_WorldLayout

Plain and styled surfaces get separate groups; within a kind, surfaces of one key stay adjacent so a shader rarely spans two groups.
================
*/
int GXM_WorldLayout( const gxmWorldSurfInfo_t *info, int count, gxmWorldSlot_t *slots )
{
	GXM_WorldFree();
	if ( !info || !slots || count <= 0 ) {
		return 0;
	}

	int *order = (int *)malloc( count * sizeof(int) );
	if ( !order ) {
		return 0;
	}
	int numPlaced = 0;
	unsigned int totalIndexes = 0;
	for ( int i = 0; i < count; i++ ) {
		slots[i].group = -1;
		slots[i].firstVert = 0;
		slots[i].indexes = NULL;
		slots[i].verts = NULL;
		if ( info[i].numVerts <= 0 || info[i].numVerts > GXM_WORLD_GROUP_VERTS || info[i].numIndexes <= 0 ) {
			continue;
		}
		order[numPlaced++] = i;
		totalIndexes += (unsigned int)info[i].numIndexes;
	}
	world_sortInfo = info;
	qsort( order, numPlaced, sizeof(int), WorldCompareSurf );
	world_sortInfo = NULL;

	// assign groups in packing order
	int maxGroups = 0, groupVerts = 0, groupStyled = -1;
	for ( int o = 0; o < numPlaced; o++ ) {
		const gxmWorldSurfInfo_t *s = &info[order[o]];
		if ( groupStyled != s->styled || groupVerts + s->numVerts > GXM_WORLD_GROUP_VERTS ) {
			maxGroups++;
			groupVerts = 0;
			groupStyled = s->styled;
		}
		slots[order[o]].group = maxGroups - 1;
		slots[order[o]].firstVert = groupVerts;
		groupVerts += s->numVerts;
	}

	world_groups = (worldGroup_t *)calloc( maxGroups ? maxGroups : 1, sizeof(worldGroup_t) );
	world_indexes = (unsigned short *)malloc( ( totalIndexes ? totalIndexes : 1 ) * sizeof(unsigned short) );
	if ( !world_groups || !world_indexes ) {
		free( order );
		GXM_WorldFree();
		return 0;
	}
	world_numGroups = maxGroups;
	for ( int o = 0; o < numPlaced; o++ ) {
		const gxmWorldSurfInfo_t *s = &info[order[o]];
		worldGroup_t *g = &world_groups[slots[order[o]].group];
		g->numVerts += s->numVerts;
		g->styled = s->styled;
	}

	for ( int gi = 0; gi < world_numGroups; gi++ ) {
		worldGroup_t *g = &world_groups[gi];
		const int stride = g->styled ? GXM_WORLD_STYLED_STRIDE : GXM_WORLD_STRIDE;
		g->data = (unsigned char *)GXM_Alloc( SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
			(unsigned int)( g->numVerts * stride ), 16, SCE_GXM_MEMORY_ATTRIB_READ, &g->uid );
		if ( !g->data ) {
			free( order );
			GXM_WorldFree();
			for ( int i = 0; i < count; i++ ) {
				slots[i].group = -1;
			}
			return 0;
		}
	}

	unsigned short *idx = world_indexes;
	for ( int o = 0; o < numPlaced; o++ ) {
		gxmWorldSlot_t *slot = &slots[order[o]];
		const worldGroup_t *g = &world_groups[slot->group];
		const int stride = g->styled ? GXM_WORLD_STYLED_STRIDE : GXM_WORLD_STRIDE;
		slot->verts = g->data + slot->firstVert * stride;
		slot->indexes = idx;
		idx += info[order[o]].numIndexes;
	}

	free( order );
	return 1;
}

static signed char WorldSnorm8( float n )
{
	if ( n <= -1.0f ) return -127;
	if ( n >= 1.0f ) return 127;
	return (signed char)( n * 127.0f + ( n < 0.0f ? -0.5f : 0.5f ) );
}

// one vertex in the group's layout; lm holds four st pairs with lmStride floats between them
static void WorldWriteVert( unsigned char *dst, int styled, const float *xyz, const float *st,
							const float *lm, int lmStride, const float *normal, const unsigned char *colors )
{
	unsigned char out[GXM_WORLD_STYLED_STRIDE];
	float *f = (float *)out;
	f[0] = xyz[0];
	f[1] = xyz[1];
	f[2] = xyz[2];
	f[3] = st[0];
	f[4] = st[1];
	f[5] = lm[0];
	f[6] = lm[1];
	signed char *n = (signed char *)( out + GXM_WORLD_OFS_NORMAL );
	n[0] = WorldSnorm8( normal[0] );
	n[1] = WorldSnorm8( normal[1] );
	n[2] = WorldSnorm8( normal[2] );
	n[3] = 0;
	memcpy( out + GXM_WORLD_OFS_COLOR, colors, 4 );
	if ( styled ) {
		float *l = (float *)( out + GXM_WORLD_OFS_LM1 );
		for ( int k = 1; k < 4; k++ ) {
			l[( k - 1 ) * 2 + 0] = lm[k * lmStride + 0];
			l[( k - 1 ) * 2 + 1] = lm[k * lmStride + 1];
		}
		memcpy( out + GXM_WORLD_OFS_COLOR1, colors + 4, 12 );
	}
	memcpy( dst, out, styled ? GXM_WORLD_STYLED_STRIDE : GXM_WORLD_STRIDE );	// sequential writes into uncached memory, never read back
}

static void WorldWriteIndexes( const gxmWorldSlot_t *slot, const int *indexes, int numIndexes )
{
	for ( int i = 0; i < numIndexes; i++ ) {
		slot->indexes[i] = (unsigned short)( slot->firstVert + indexes[i] );
	}
}

void GXM_WorldPackPoints( const gxmWorldSlot_t *slot, int styled, const float *points, int numPoints,
						  const float normal[3], const int *indexes, int numIndexes )
{
	if ( !slot || slot->group < 0 ) {
		return;
	}
	const int stride = styled ? GXM_WORLD_STYLED_STRIDE : GXM_WORLD_STRIDE;
	for ( int i = 0; i < numPoints; i++ ) {
		const float *p = points + i * GXM_WORLD_POINT_FLOATS;
		WorldWriteVert( slot->verts + i * stride, styled, p, p + 3, p + GXM_WORLD_POINT_LM, 2, normal,
			(const unsigned char *)( p + GXM_WORLD_POINT_COLOR ) );
	}
	WorldWriteIndexes( slot, indexes, numIndexes );
}

void GXM_WorldPackGrid( const gxmWorldSlot_t *slot, int styled, const gxmWorldDrawVert_t *verts, int width, int height )
{
	if ( !slot || slot->group < 0 ) {
		return;
	}
	const int stride = styled ? GXM_WORLD_STYLED_STRIDE : GXM_WORLD_STRIDE;
	for ( int i = 0; i < width * height; i++ ) {
		const gxmWorldDrawVert_t *dv = &verts[i];
		WorldWriteVert( slot->verts + i * stride, styled, dv->xyz, dv->st, &dv->lightmap[0][0], 2, dv->normal, &dv->color[0][0] );
	}
	unsigned short *dst = slot->indexes;
	for ( int r = 0; r < height - 1; r++ ) {
		for ( int c = 0; c < width - 1; c++ ) {
			const int v1 = slot->firstVert + r * width + c + 1;
			const int v2 = v1 - 1;
			const int v3 = v2 + width;
			const int v4 = v3 + 1;
			*dst++ = (unsigned short)v2;
			*dst++ = (unsigned short)v3;
			*dst++ = (unsigned short)v1;
			*dst++ = (unsigned short)v1;
			*dst++ = (unsigned short)v3;
			*dst++ = (unsigned short)v4;
		}
	}
}

void GXM_WorldPackVerts( const gxmWorldSlot_t *slot, int styled, const gxmWorldDrawVert_t *verts, int numVerts,
						 const int *indexes, int numIndexes )
{
	if ( !slot || slot->group < 0 ) {
		return;
	}
	const int stride = styled ? GXM_WORLD_STYLED_STRIDE : GXM_WORLD_STRIDE;
	for ( int i = 0; i < numVerts; i++ ) {
		const gxmWorldDrawVert_t *dv = &verts[i];
		WorldWriteVert( slot->verts + i * stride, styled, dv->xyz, dv->st, &dv->lightmap[0][0], 2, dv->normal, &dv->color[0][0] );
	}
	WorldWriteIndexes( slot, indexes, numIndexes );
}

/*
================
GXM_WorldGather

Opaque runs merge per group; ordered runs only merge while the group stays the same.
================
*/
int GXM_WorldGather( const gxmWorldRun_t *runs, int numRuns, int keepOrder, gxmWorldSeg_t *segs, int maxSegs )
{
	int segGroup[WORLD_MAX_SEGS];
	int segCount[WORLD_MAX_SEGS];
	unsigned short *cursor[WORLD_MAX_SEGS];
	int numSegs = 0;

	if ( maxSegs > WORLD_MAX_SEGS ) {
		maxSegs = WORLD_MAX_SEGS;
	}
	for ( int i = 0; i < numRuns; i++ ) {
		const int g = runs[i].group;
		if ( g < 0 || g >= world_numGroups || runs[i].numIndexes <= 0 ) {
			continue;
		}
		int s = -1;
		if ( keepOrder ) {
			if ( numSegs && segGroup[numSegs - 1] == g ) {
				s = numSegs - 1;
			}
		} else {
			for ( int k = 0; k < numSegs; k++ ) {
				if ( segGroup[k] == g ) {
					s = k;
					break;
				}
			}
		}
		if ( s < 0 ) {
			if ( numSegs >= maxSegs ) {
				return 0;
			}
			s = numSegs++;
			segGroup[s] = g;
			segCount[s] = 0;
		}
		segCount[s] += runs[i].numIndexes;
	}

	for ( int s = 0; s < numSegs; s++ ) {
		cursor[s] = (unsigned short *)GXM_RingAlloc( (unsigned int)segCount[s] * sizeof(unsigned short), 4 );
		if ( !cursor[s] ) {
			return 0;
		}
		segs[s].verts = world_groups[segGroup[s]].data;
		segs[s].styled = world_groups[segGroup[s]].styled;
		segs[s].indexes = cursor[s];
		segs[s].numIndexes = segCount[s];
	}

	// the same walk again, now writing; sequential stores into uncached memory
	int cur = -1;
	for ( int i = 0; i < numRuns; i++ ) {
		const int g = runs[i].group;
		if ( g < 0 || g >= world_numGroups || runs[i].numIndexes <= 0 ) {
			continue;
		}
		if ( keepOrder ) {
			if ( cur < 0 || segGroup[cur] != g ) {
				cur++;
			}
		} else {
			cur = 0;
			while ( segGroup[cur] != g ) {
				cur++;
			}
		}
		memcpy( cursor[cur], runs[i].indexes, runs[i].numIndexes * sizeof(unsigned short) );
		cursor[cur] += runs[i].numIndexes;
	}
	return numSegs;
}
