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

// gxm_world.h -- resident world geometry, the lightmap atlas, and the stage, dynamic light and fog draws over it

#ifndef GXM_WORLD_H
#define GXM_WORLD_H

#ifdef __cplusplus
extern "C" {
#endif

// vertices per group, so 16-bit indexes stay below 64000
#define GXM_WORLD_GROUP_VERTS	64000

// vertex layouts shared with stage_v.cg, dlight_v.cg and fogpass_v.cg
#define GXM_WORLD_OFS_XYZ		0		// f32 x3
#define GXM_WORLD_OFS_ST		12		// f32 x2
#define GXM_WORLD_OFS_LM		20		// f32 x2, style 0 lightmap
#define GXM_WORLD_OFS_NORMAL	28		// s8n x4
#define GXM_WORLD_OFS_COLOR		32		// u8n x4, style 0 colour
#define GXM_WORLD_STRIDE		36
#define GXM_WORLD_OFS_LM1		36		// f32 x2 for each of styles 1..3
#define GXM_WORLD_OFS_COLOR1	60		// u8n x4 for each of styles 1..3
#define GXM_WORLD_STYLED_STRIDE	72

// a BSP face point as both renderers store it: xyz, st, four lightmap st, four packed colours, final colour
#define GXM_WORLD_POINT_FLOATS	18
#define GXM_WORLD_POINT_LM		5
#define GXM_WORLD_POINT_COLOR	13

// drawVert_t of both renderers
typedef struct {
	float			xyz[3];
	float			st[2];
	float			lightmap[4][2];
	float			normal[3];
	unsigned char	color[4][4];
} gxmWorldDrawVert_t;

// lightmaps of 128x128 packed 4x4 into 512x512 pages
#define GXM_LMATLAS_LIGHTMAP	128
#define GXM_LMATLAS_SIZE		512
#define GXM_LMATLAS_COLS		( GXM_LMATLAS_SIZE / GXM_LMATLAS_LIGHTMAP )
#define GXM_LMATLAS_SLOTS		( GXM_LMATLAS_COLS * GXM_LMATLAS_COLS )

// rescales a lightmap st into the page slot it was packed at, clamped half a texel in as GL_CLAMP sampled it
void GXM_LmAtlasRemap( int slot, float st[2] );
// texel index of the slot's top-left corner within its page
int  GXM_LmAtlasTexelOffset( int slot );

// one surface to place; equal keys are packed next to each other
typedef struct {
	int		numVerts;
	int		numIndexes;
	int		styled;
	int		key;
} gxmWorldSurfInfo_t;

// where a placed surface lives; group is -1 when it did not fit
typedef struct {
	int				group;
	int				firstVert;
	unsigned short	*indexes;		// cached copy, rebased to the group
	unsigned char	*verts;			// GPU memory for its vertices, written once
} gxmWorldSlot_t;

// packs every surface into groups and allocates them; false when GPU memory ran out and nothing was kept
int  GXM_WorldLayout( const gxmWorldSurfInfo_t *info, int count, gxmWorldSlot_t *slots );
// releases the groups and the index copies; the caller has waited the GPU out
void GXM_WorldFree( void );
// true when the group uses the styled layout
int  GXM_WorldGroupStyled( int group );

// fill a placed surface; grids are kept at full resolution and triangulated as RB_SurfaceGrid does
void GXM_WorldPackPoints( const gxmWorldSlot_t *slot, int styled, const float *points, int numPoints,
						  const float normal[3], const int *indexes, int numIndexes );
void GXM_WorldPackGrid( const gxmWorldSlot_t *slot, int styled, const gxmWorldDrawVert_t *verts, int width, int height );
void GXM_WorldPackVerts( const gxmWorldSlot_t *slot, int styled, const gxmWorldDrawVert_t *verts, int numVerts,
						 const int *indexes, int numIndexes );

// one surface's cached indexes queued for a draw
typedef struct {
	const unsigned short	*indexes;
	int						numIndexes;
	int						group;
} gxmWorldRun_t;

// one draw over a group: vertices resident, indexes in this frame's ring
typedef struct {
	const void				*verts;
	const unsigned short	*indexes;
	int						numIndexes;
	int						styled;
} gxmWorldSeg_t;

// copies queued runs into the ring as one index run per group, or per adjacent group change when order must hold; 0 when the ring is full
int  GXM_WorldGather( const gxmWorldRun_t *runs, int numRuns, int keepOrder, gxmWorldSeg_t *segs, int maxSegs );

#define GXM_WORLD_TC_LINEAR		0
#define GXM_WORLD_TC_ENV		1
#define GXM_WORLD_TC_TURB		2

// per-draw inputs of one shader stage
typedef struct {
	float	colorScale[4];			// plain layout: colour = vertex colour * scale + bias
	float	colorBias[4];
	float	colorScaleStyled[4];	// styled layout: colour = summed style colour * scale + bias
	float	colorBiasStyled[4];
	float	style[4][4];			// styled layout: per-style colour multipliers
	float	lmSelect[4];			// styled layout: weight of each lightmap set
	float	tex[2][4][4];			// per set: s and t over ( st, lightmap st ) or ( e, 1 ), then s and t over ( xyz, 1 )
	float	viewer[4];				// environment: view origin in model space
	float	turb[4];				// turbulence: amplitude, phase in turns
	float	turbPost[4];			// turbulence: linear part of the texmods applied after it
	int		tc0;					// GXM_WORLD_TC_* for set 0
	int		nuv;					// texcoord sets: 1, or 2 with the second texture unit
} gxmWorldStage_t;

// one dynamic light over planar surfaces; diffuse adds the surface texture on unit 0
typedef struct {
	float	origin[3];
	float	radius;
	float	color[3];
	int		diffuse;
} gxmWorldLight_t;

// a fog volume pass, the per-view vectors of RB_CalcFogTexCoords
typedef struct {
	float	color[4];
	float	distance[4];
	float	depth[4];
	float	eyeT;
	int		eyeOutside;
} gxmWorldFog_t;

// true once every world program linked
int  GXM_WorldReady( void );
// creates the fragment instances a stage will need, before the first frame; fogMask bit n = fog mode n
void GXM_WorldPrepare( unsigned int stateBits, int ntex, int env, int fogMask );

// draw with the current state bits, texture units, texture env and fog, as GXM_DrawTess would
void GXM_DrawWorldStage( const gxmWorldStage_t *stage, const gxmWorldSeg_t *segs, int numSegs );
void GXM_DrawWorldLight( const gxmWorldLight_t *light, const gxmWorldSeg_t *segs, int numSegs );
void GXM_DrawWorldFog( const gxmWorldFog_t *fog, const gxmWorldSeg_t *segs, int numSegs );

#ifdef __cplusplus
}
#endif

#endif // GXM_WORLD_H
