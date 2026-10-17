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

// gxm_mesh.cpp -- md3 surfaces uploaded once per model: shared texcoords, one position and normal block per frame

#include "gxm_mesh.h"
#include "gxm_device.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

struct gxmMeshModel_s {
	struct gxmMeshModel_s	*next;
	const void				*key;
	SceUID					uid;
	unsigned int			bytes;
	int						numSurfaces;
	gxmMeshSurface_t		surfaces[1];	// numSurfaces; frames is NULL for a surface left to the CPU
};

static gxmMeshModel_t	*mesh_models;
static unsigned int		mesh_bytes;

static signed char MeshSnorm8( float n )
{
	if ( n <= -1.0f ) return -127;
	if ( n >= 1.0f ) return 127;
	return (signed char)( n * 127.0f + ( n < 0.0f ? -0.5f : 0.5f ) );
}

// true when every index and count of the surface can be drawn with 16-bit indexes
static int MeshSurfaceOk( const gxmMeshSource_t *s )
{
	if ( !s->surface || !s->xyzNormals || !s->st || !s->triangles
		|| s->numVerts <= 0 || s->numVerts >= 64000 || s->numTriangles <= 0 || s->numFrames <= 0 ) {
		return 0;
	}
	for ( int i = 0; i < s->numTriangles * 3; i++ ) {
		if ( s->triangles[i] < 0 || s->triangles[i] >= s->numVerts ) {
			return 0;
		}
	}
	return 1;
}

static unsigned int MeshSurfaceBytes( const gxmMeshSource_t *s )
{
	const unsigned int st = (unsigned int)s->numVerts * GXM_MESH_ST_STRIDE;
	const unsigned int frames = (unsigned int)s->numFrames * (unsigned int)s->numVerts * GXM_MESH_FRAME_STRIDE;
	const unsigned int indexes = ALIGN( (unsigned int)s->numTriangles * 3 * sizeof(unsigned short), 4 );
	return st + frames + indexes;
}

/*
================
MeshWriteFrames

Positions keep the file's 16-bit units; normals are decoded from latitude and longitude as LerpMeshVertexes does.
================
*/
static void MeshWriteFrames( const gxmMeshSource_t *s, unsigned char *dst )
{
	const float step = 6.2831853f / 256.0f;
	const short *src = s->xyzNormals;
	const int count = s->numFrames * s->numVerts;
	unsigned char out[GXM_MESH_FRAME_STRIDE];

	for ( int i = 0; i < count; i++, src += 4, dst += GXM_MESH_FRAME_STRIDE ) {
		short *xyz = (short *)( out + GXM_MESH_OFS_XYZ );
		xyz[0] = src[0];
		xyz[1] = src[1];
		xyz[2] = src[2];
		xyz[3] = 0;

		const unsigned int packed = (unsigned short)src[3];
		const float lat = (float)( ( packed >> 8 ) & 0xff ) * step;
		const float lng = (float)( packed & 0xff ) * step;
		signed char *n = (signed char *)( out + GXM_MESH_OFS_NORMAL );
		n[0] = MeshSnorm8( cosf( lat ) * sinf( lng ) );
		n[1] = MeshSnorm8( sinf( lat ) * sinf( lng ) );
		n[2] = MeshSnorm8( cosf( lng ) );
		n[3] = 0;

		memcpy( dst, out, GXM_MESH_FRAME_STRIDE );	// sequential writes into uncached memory, never read back
	}
}

gxmMeshModel_t *GXM_MeshFindModel( const void *key )
{
	for ( gxmMeshModel_t *m = mesh_models; m; m = m->next ) {
		if ( m->key == key ) {
			return m;
		}
	}
	return NULL;
}

/*
================
GXM_MeshBuildModel

One uncached LPDDR block per model: per surface its texcoords, its frames, then its 16-bit indexes.
================
*/
gxmMeshModel_t *GXM_MeshBuildModel( const void *key, const gxmMeshSource_t *surfaces, int numSurfaces )
{
	if ( !key || !surfaces || numSurfaces <= 0 ) {
		return NULL;
	}
	gxmMeshModel_t *existing = GXM_MeshFindModel( key );
	if ( existing ) {
		return existing;
	}

	unsigned int total = 0;
	int usable = 0;
	for ( int i = 0; i < numSurfaces; i++ ) {
		if ( MeshSurfaceOk( &surfaces[i] ) ) {
			total += MeshSurfaceBytes( &surfaces[i] );
			usable++;
		}
	}
	if ( !usable ) {
		return NULL;
	}

	gxmMeshModel_t *m = (gxmMeshModel_t *)calloc( 1, sizeof(gxmMeshModel_t) + ( numSurfaces - 1 ) * sizeof(gxmMeshSurface_t) );
	if ( !m ) {
		return NULL;
	}
	unsigned char *base = (unsigned char *)GXM_Alloc( SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
		total, 16, SCE_GXM_MEMORY_ATTRIB_READ, &m->uid );
	if ( !base ) {
		free( m );
		return NULL;
	}
	m->key = key;
	m->bytes = ALIGN( total, 4 * 1024 );
	m->numSurfaces = numSurfaces;

	unsigned char *dst = base;
	for ( int i = 0; i < numSurfaces; i++ ) {
		const gxmMeshSource_t *s = &surfaces[i];
		gxmMeshSurface_t *gs = &m->surfaces[i];
		gs->surface = s->surface;
		if ( !MeshSurfaceOk( s ) ) {
			continue;
		}

		memcpy( dst, s->st, (size_t)s->numVerts * GXM_MESH_ST_STRIDE );
		gs->st = dst;
		dst += s->numVerts * GXM_MESH_ST_STRIDE;

		MeshWriteFrames( s, dst );
		gs->frames = dst;
		dst += s->numFrames * s->numVerts * GXM_MESH_FRAME_STRIDE;

		unsigned short *idx = (unsigned short *)dst;
		const int ni = s->numTriangles * 3;
		for ( int k = 0; k < ni; k++ ) {
			idx[k] = (unsigned short)s->triangles[k];
		}
		gs->indexes = idx;
		dst += ALIGN( ni * sizeof(unsigned short), 4 );

		gs->numVerts = s->numVerts;
		gs->numIndexes = ni;
		gs->numFrames = s->numFrames;
	}

	mesh_bytes += m->bytes;
	m->next = mesh_models;
	mesh_models = m;
	return m;
}

const gxmMeshSurface_t *GXM_MeshModelSurface( const gxmMeshModel_t *model, const void *surface )
{
	if ( !model ) {
		return NULL;
	}
	for ( int i = 0; i < model->numSurfaces; i++ ) {
		const gxmMeshSurface_t *gs = &model->surfaces[i];
		if ( gs->surface == surface ) {
			return gs->frames ? gs : NULL;
		}
	}
	return NULL;
}

const void *GXM_MeshFrame( const gxmMeshSurface_t *surf, int frame )
{
	if ( !surf || frame < 0 || frame >= surf->numFrames ) {
		return NULL;
	}
	return surf->frames + (unsigned int)frame * (unsigned int)surf->numVerts * GXM_MESH_FRAME_STRIDE;
}

void GXM_MeshFreeModel( const void *key )
{
	for ( gxmMeshModel_t **link = &mesh_models; *link; link = &(*link)->next ) {
		gxmMeshModel_t *m = *link;
		if ( m->key == key ) {
			*link = m->next;
			mesh_bytes -= m->bytes;
			GXM_Free( m->uid );
			free( m );
			return;
		}
	}
}

unsigned int GXM_MeshResidentBytes( void )
{
	return mesh_bytes;
}
