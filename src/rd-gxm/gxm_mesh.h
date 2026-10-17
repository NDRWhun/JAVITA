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

// gxm_mesh.h -- GPU-resident md3 surfaces: every frame uploaded once per model, blended and lit in the vertex program

#ifndef GXM_MESH_H
#define GXM_MESH_H

#include "gxm_skin.h"
#include "gxm_world.h"

#ifdef __cplusplus
extern "C" {
#endif

// per-frame vertex layout shared with mesh_v.cg; texcoords live in their own stream
#define GXM_MESH_OFS_XYZ		0		// s16 x3, the file's position in 1/64 units
#define GXM_MESH_OFS_NORMAL		8		// s8n x4
#define GXM_MESH_FRAME_STRIDE	12
#define GXM_MESH_ST_STRIDE		8		// f32 x2

// one md3 surface in the file's own layout
typedef struct {
	const void		*surface;		// the md3Surface_t, the key draws look the surface up by
	const short		*xyzNormals;	// md3XyzNormal_t per vertex per frame: xyz s16 x3, normal latitude and longitude bytes
	const float		*st;			// st per vertex
	const int		*triangles;		// three indexes per triangle
	int				numVerts;
	int				numTriangles;
	int				numFrames;
} gxmMeshSource_t;

// one surface as the GPU draws it
typedef struct {
	const void				*surface;
	const unsigned char		*frames;		// numFrames blocks of numVerts * GXM_MESH_FRAME_STRIDE
	const void				*st;
	const unsigned short	*indexes;
	int						numVerts;
	int						numIndexes;
	int						numFrames;
} gxmMeshSurface_t;

typedef struct gxmMeshModel_s gxmMeshModel_t;

// models are keyed by the loaded file image, which outlives a level when the model cache keeps it
gxmMeshModel_t			*GXM_MeshFindModel( const void *key );
// NULL when GPU memory is short or no surface can be drawn
gxmMeshModel_t			*GXM_MeshBuildModel( const void *key, const gxmMeshSource_t *surfaces, int numSurfaces );
// the resident copy of one of the model's surfaces, NULL when it stays on the CPU path
const gxmMeshSurface_t	*GXM_MeshModelSurface( const gxmMeshModel_t *model, const void *surface );
// called when the file image is freed; the GPU block is released once in-flight frames retire
void					GXM_MeshFreeModel( const void *key );
// bytes of GPU memory the resident models hold
unsigned int			GXM_MeshResidentBytes( void );

// the vertex block of one frame of a surface
const void				*GXM_MeshFrame( const gxmMeshSurface_t *surf, int frame );

// one surface of a mesh draw; oldFrame is only read when the draw blends
typedef struct {
	const void				*frame;
	const void				*oldFrame;
	const void				*st;
	const unsigned short	*indexes;
	int						numIndexes;
} gxmMeshDraw_t;

// true once every md3 program linked; the draw path lives with the rest in gxm_backend.cpp
int  GXM_MeshReady( void );

// draws with the current blend/depth/cull/fog state and texture unit 0, as GXM_DrawSkinned would; backlerp is the old frame's weight
void GXM_DrawMesh( const gxmSkinStage_t *stage, float backlerp, const gxmMeshDraw_t *draws, int numDraws );
// the fog volume pass over the same draws
void GXM_DrawMeshFog( const gxmWorldFog_t *fog, float backlerp, const gxmMeshDraw_t *draws, int numDraws );

#ifdef __cplusplus
}
#endif

#endif // GXM_MESH_H
