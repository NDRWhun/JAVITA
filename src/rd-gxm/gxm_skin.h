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

// gxm_skin.h -- GPU-resident ghoul2 surface buffers, per-frame bone palettes and the skinned draw

#ifndef GXM_SKIN_H
#define GXM_SKIN_H

#ifdef __cplusplus
extern "C" {
#endif

// vertices store bone * 3 in a byte; skin_v.cg sizes its palette to match
#define GXM_SKIN_MAX_BONES		86

// vertex layout shared with skin_v.cg; the weights field only exists in the 2- and 4-weight strides
#define GXM_SKIN_OFS_XYZ		0		// f32 x3
#define GXM_SKIN_OFS_ST			12		// f32 x2
#define GXM_SKIN_OFS_NORMAL		20		// s8n x4
#define GXM_SKIN_OFS_BONES		24		// u8 x4, bone * 3
#define GXM_SKIN_OFS_WEIGHTS	28		// u16n x2 or x4
#define GXM_SKIN_STRIDE( w )	( ( w ) == 1 ? 28 : ( w ) == 2 ? 32 : 36 )

// one LOD surface as the GPU draws it; weights is 0 when the surface stays on the CPU path
typedef struct {
	const void				*vertices;
	const unsigned short	*indexes;
	int						numIndexes;
	unsigned char			weights;		// 1, 2 or 4: picks the vertex program and stride
	unsigned char			paletteBones;	// bones the model's surfaces address, so the palette upload size
} gxmSkinSurface_t;

// one ghoul2 mesh surface in the file's own layout
typedef struct {
	const void		*verts;			// mdxmVertex_t: normal f32x3, xyz f32x3, packed u32, weight bytes x4
	const float		*texCoords;		// st per vertex
	const int		*triangles;		// three indexes per triangle
	const int		*boneRefs;		// surface bone slot -> global bone
	int				numVerts;
	int				numTriangles;
	int				numBoneRefs;
} gxmSkinSource_t;

typedef struct gxmSkinModel_s gxmSkinModel_t;

// models are keyed by the loaded file image, which outlives a level when the model cache keeps it
gxmSkinModel_t			*GXM_SkinFindModel( const void *key );
// surfaces arrive LOD-major, numSurfaces per LOD; NULL when GPU memory is short
gxmSkinModel_t			*GXM_SkinBuildModel( const void *key, const gxmSkinSource_t *surfaces, int numLods, int numSurfaces );
const gxmSkinSurface_t	*GXM_SkinModelSurface( const gxmSkinModel_t *model, int lod, int surface );
// called when the file image is freed; the GPU block is released once in-flight frames retire
void					GXM_SkinFreeModel( const void *key );

// copies a bone snapshot of 3x4 matrices into this frame's ring once; NULL when the ring is full
const void				*GXM_SkinPalette( const void *bones, int numBones );

// one surface of a skinned stage draw
typedef struct {
	const void				*vertices;
	const unsigned short	*indexes;
	int						numIndexes;
	int						weights;
	const void				*palette;
} gxmSkinDraw_t;

// per-stage inputs; directions and positions are in the entity's model space
typedef struct {
	float	color[4];		// rgba multiplier, alpha used unless spec
	float	texMat[2][4];	// st or environment vector -> texcoord rows
	float	lightDir[4];
	float	ambient[4];		// 0..1
	float	directed[4];	// 0..1
	float	viewer[4];		// w=1 view origin, w=0 fixed direction
	float	envParams[4];	// x = viewer weight in the environment vector
	int		lit;			// rgb from the entity's diffuse light
	int		spec;			// alpha from lightingSpecular
	int		env;			// texcoords from the environment vector
} gxmSkinStage_t;

// true once every skinning program linked; the draw path lives with the rest in gxm_backend.cpp
int  GXM_SkinReady( void );

// draws with the current blend/depth/cull/fog state and texture unit 0, as GXM_DrawTess would
void GXM_DrawSkinned( const gxmSkinStage_t *stage, const gxmSkinDraw_t *draws, int numDraws );

#ifdef __cplusplus
}
#endif

#endif // GXM_SKIN_H
