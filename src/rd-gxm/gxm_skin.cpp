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

// gxm_skin.cpp -- ghoul2 surfaces uploaded once per model, bone palettes copied once per frame

#include "gxm_skin.h"
#include "gxm_device.h"
#include "gxm_texture.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// mdxmVertex_t as the file stores it
typedef struct {
	float			normal[3];
	float			xyz[3];
	unsigned int	packed;		// 31-30 weight count - 1, 27-20 weight high bits, 19-0 bone slots
	unsigned char	weightLow[4];
} skinFileVert_t;

static_assert( sizeof(skinFileVert_t) == 32, "skinFileVert_t must match mdxmVertex_t" );

struct gxmSkinModel_s {
	struct gxmSkinModel_s	*next;
	const void				*key;
	SceUID					uid;
	int						numLods;
	int						numSurfaces;
	gxmSkinSurface_t		surfaces[1];	// numLods * numSurfaces
};

static gxmSkinModel_t	*skin_models;

static int SkinVertWeightCount( const skinFileVert_t *v )
{
	return (int)( v->packed >> 30 ) + 1;
}

static int SkinVertSlot( const skinFileVert_t *v, int k )
{
	return (int)( ( v->packed >> ( 5 * k ) ) & 31 );
}

// the 10-bit weight the CPU skinner decodes
static float SkinVertWeight( const skinFileVert_t *v, int k )
{
	const int i = v->weightLow[k] | (int)( ( v->packed >> ( 12 + k * 2 ) ) & 0x300 );
	return (float)i * ( 1.0f / 1023.0f );
}

static unsigned short SkinUnorm16( float w )
{
	if ( w <= 0.0f ) return 0;
	if ( w >= 1.0f ) return 65535;
	return (unsigned short)( w * 65535.0f + 0.5f );
}

static signed char SkinSnorm8( float n )
{
	if ( n <= -1.0f ) return -127;
	if ( n >= 1.0f ) return 127;
	return (signed char)( n * 127.0f + ( n < 0.0f ? -0.5f : 0.5f ) );
}

/*
================
SkinClassify

Weight class of a surface, or 0 when its data cannot be drawn by the skin programs.
================
*/
static int SkinClassify( const gxmSkinSource_t *s, int *maxBone )
{
	if ( !s->verts || !s->texCoords || !s->triangles || !s->boneRefs
		|| s->numVerts <= 0 || s->numVerts >= 64000 || s->numTriangles <= 0 || s->numBoneRefs <= 0 ) {
		return 0;
	}
	int top = -1;
	for ( int r = 0; r < s->numBoneRefs; r++ ) {
		if ( s->boneRefs[r] < 0 || s->boneRefs[r] >= GXM_SKIN_MAX_BONES ) {
			return 0;
		}
		if ( s->boneRefs[r] > top ) {
			top = s->boneRefs[r];
		}
	}

	const skinFileVert_t *v = (const skinFileVert_t *)s->verts;
	int most = 1;
	for ( int i = 0; i < s->numVerts; i++ ) {
		const int n = SkinVertWeightCount( &v[i] );
		for ( int k = 0; k < n; k++ ) {
			if ( SkinVertSlot( &v[i], k ) >= s->numBoneRefs ) {
				return 0;
			}
		}
		if ( n > most ) {
			most = n;
		}
	}
	for ( int i = 0; i < s->numTriangles * 3; i++ ) {
		if ( s->triangles[i] < 0 || s->triangles[i] >= s->numVerts ) {
			return 0;
		}
	}

	*maxBone = top;
	return most >= 3 ? 4 : most;
}

/*
================
SkinWriteVerts

Packs a surface into its stride; weights reproduce the CPU skinner's, the last one taking the remainder.
================
*/
static void SkinWriteVerts( const gxmSkinSource_t *s, int weights, unsigned char *dst )
{
	const skinFileVert_t *v = (const skinFileVert_t *)s->verts;
	const int stride = GXM_SKIN_STRIDE( weights );
	unsigned char out[36];

	for ( int i = 0; i < s->numVerts; i++, dst += stride ) {
		const skinFileVert_t *fv = &v[i];
		const int n = SkinVertWeightCount( fv );

		float *f = (float *)out;
		f[0] = fv->xyz[0];
		f[1] = fv->xyz[1];
		f[2] = fv->xyz[2];
		f[3] = s->texCoords[i * 2 + 0];
		f[4] = s->texCoords[i * 2 + 1];

		signed char *nrm = (signed char *)( out + GXM_SKIN_OFS_NORMAL );
		nrm[0] = SkinSnorm8( fv->normal[0] );
		nrm[1] = SkinSnorm8( fv->normal[1] );
		nrm[2] = SkinSnorm8( fv->normal[2] );
		nrm[3] = 0;

		unsigned char *bones = out + GXM_SKIN_OFS_BONES;
		for ( int k = 0; k < 4; k++ ) {
			const int slot = SkinVertSlot( fv, k < n ? k : 0 );
			bones[k] = (unsigned char)( s->boneRefs[slot] * 3 );
		}

		unsigned short w[4] = { 65535, 0, 0, 0 };
		if ( n >= 2 ) {
			float total = 0.0f;
			for ( int k = 0; k < n - 1; k++ ) {
				const float wk = SkinVertWeight( fv, k );
				w[k] = SkinUnorm16( wk );
				total += wk;
			}
			w[n - 1] = SkinUnorm16( 1.0f - total );
		}
		memcpy( out + GXM_SKIN_OFS_WEIGHTS, w, sizeof(w) );

		memcpy( dst, out, stride );
	}
}

gxmSkinModel_t *GXM_SkinFindModel( const void *key )
{
	for ( gxmSkinModel_t *m = skin_models; m; m = m->next ) {
		if ( m->key == key ) {
			return m;
		}
	}
	return NULL;
}

/*
================
GXM_SkinBuildModel

One uncached LPDDR block per model, vertices first, then 16-bit indexes; CDRAM is left to textures.
================
*/
gxmSkinModel_t *GXM_SkinBuildModel( const void *key, const gxmSkinSource_t *surfaces, int numLods, int numSurfaces )
{
	if ( !key || numLods <= 0 || numSurfaces <= 0 ) {
		return NULL;
	}
	gxmSkinModel_t *existing = GXM_SkinFindModel( key );
	if ( existing ) {
		return existing;
	}

	const int count = numLods * numSurfaces;
	gxmSkinModel_t *m = (gxmSkinModel_t *)calloc( 1, sizeof(gxmSkinModel_t) + ( count - 1 ) * sizeof(gxmSkinSurface_t) );
	if ( !m ) {
		return NULL;
	}
	m->key = key;
	m->uid = -1;
	m->numLods = numLods;
	m->numSurfaces = numSurfaces;

	unsigned int vertBytes = 0, indexBytes = 0;
	int topBone = -1;
	for ( int i = 0; i < count; i++ ) {
		int maxBone = -1;
		const int w = SkinClassify( &surfaces[i], &maxBone );
		m->surfaces[i].weights = (unsigned char)w;
		if ( !w ) {
			continue;
		}
		if ( maxBone > topBone ) {
			topBone = maxBone;
		}
		vertBytes += (unsigned int)( surfaces[i].numVerts * GXM_SKIN_STRIDE( w ) );
		indexBytes += (unsigned int)( surfaces[i].numTriangles * 3 * sizeof(unsigned short) );
		indexBytes = ALIGN( indexBytes, 4 );
	}

	if ( vertBytes ) {
		unsigned char *base = (unsigned char *)GXM_Alloc( SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
			vertBytes + indexBytes, 16, SCE_GXM_MEMORY_ATTRIB_READ, &m->uid );
		if ( !base ) {
			free( m );
			return NULL;
		}

		unsigned char *vdst = base;
		unsigned short *idst = (unsigned short *)( base + vertBytes );
		for ( int i = 0; i < count; i++ ) {
			gxmSkinSurface_t *gs = &m->surfaces[i];
			const gxmSkinSource_t *s = &surfaces[i];
			if ( !gs->weights ) {
				continue;
			}
			SkinWriteVerts( s, gs->weights, vdst );
			gs->vertices = vdst;
			vdst += s->numVerts * GXM_SKIN_STRIDE( gs->weights );

			const int ni = s->numTriangles * 3;
			for ( int k = 0; k < ni; k++ ) {
				idst[k] = (unsigned short)s->triangles[k];
			}
			gs->indexes = idst;
			gs->numIndexes = ni;
			idst += ( ni + 1 ) & ~1;	// keeps the next run word aligned
		}
		for ( int i = 0; i < count; i++ ) {
			if ( m->surfaces[i].weights ) {
				m->surfaces[i].paletteBones = (unsigned char)( topBone + 1 );
			}
		}
	}

	m->next = skin_models;
	skin_models = m;
	return m;
}

const gxmSkinSurface_t *GXM_SkinModelSurface( const gxmSkinModel_t *model, int lod, int surface )
{
	if ( !model || lod < 0 || lod >= model->numLods || surface < 0 || surface >= model->numSurfaces ) {
		return NULL;
	}
	const gxmSkinSurface_t *gs = &model->surfaces[lod * model->numSurfaces + surface];
	return gs->weights ? gs : NULL;
}

void GXM_SkinFreeModel( const void *key )
{
	for ( gxmSkinModel_t **link = &skin_models; *link; link = &(*link)->next ) {
		gxmSkinModel_t *m = *link;
		if ( m->key == key ) {
			*link = m->next;
			if ( m->uid >= 0 ) {
				GXM_Free( m->uid );
			}
			free( m );
			return;
		}
	}
}

// snapshot -> ring copy, valid for the frame it was made in
#define SKIN_PALETTE_CACHE	64

typedef struct {
	const void		*bones;
	const void		*gpu;
	unsigned int	serial;
} skinPaletteSlot_t;

static skinPaletteSlot_t	skin_palettes[SKIN_PALETTE_CACHE];

const void *GXM_SkinPalette( const void *bones, int numBones )
{
	if ( !bones || numBones <= 0 || numBones > GXM_SKIN_MAX_BONES ) {
		return NULL;
	}
	const unsigned int serial = GXM_RingFrameSerial();
	skinPaletteSlot_t *slot = &skin_palettes[ ( (uintptr_t)bones >> 6 ) & ( SKIN_PALETTE_CACHE - 1 ) ];
	if ( slot->bones == bones && slot->serial == serial && slot->gpu ) {
		return slot->gpu;
	}

	const unsigned int bytes = (unsigned int)numBones * 48;	// three float4 rows per bone
	void *gpu = GXM_RingAlloc( bytes, 16 );
	if ( !gpu ) {
		return NULL;
	}
	memcpy( gpu, bones, bytes );	// sequential writes into uncached memory, never read back

	slot->bones = bones;
	slot->gpu = gpu;
	slot->serial = serial;
	return gpu;
}
