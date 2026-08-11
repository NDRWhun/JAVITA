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

// A map's static props never move, so each instance is baked once into world space and
// drawn from a shared buffer. That drops the entity number from their sort key, which is
// the only thing stopping hundreds of copies of one model from batching into one draw.

#include "../server/exe_headers.h"

#include "tr_local.h"
#ifdef USE_GXM_NATIVE
#include "../rd-gxm/gxm_device.h"
#include "../rd-gxm/gxm_backend.h"
#endif

#ifdef VITA

extern cvar_t	*r_staticBatch;

#define SB_MAXGROUPS	16
#define SB_GROUPVERTS	65000					// u16 indices address one group
#define SB_STRIDE		32						// xyz(12) st(8) st2(8) rgba(4)
#define SB_OFS_ST		12
#define SB_OFS_ST2		20
#define SB_OFS_RGBA		28

#define SB_MAXINSTANCES	1024
#define SB_HASHSIZE		256

// Props bake as they come into view, so a group is allocated at full size and written
// in place. Only vertices no submitted index can reach yet are ever touched.
typedef struct {
#ifdef USE_GXM_NATIVE
	void	*data;
	SceUID	uid;
#else
	GLuint	vbo;
	byte	*shadow;				// GL needs a client copy to re-upload from
#endif
	int		numVerts;
} sbGroup_t;

typedef struct sbInstance_s {
	struct sbInstance_s	*next;
	qhandle_t			hModel;
	vec3_t				origin;
	vec3_t				axis[3];
	vec3_t				scale;
	int					numSurfs;
	srfStaticBatch_t	*surfs;
	shader_t			**shaders;
} sbInstance_t;

static sbGroup_t		sb_groups[SB_MAXGROUPS];
static int				sb_numGroups;
static int				sb_bytes;
static qboolean			sb_failed;

static sbInstance_t		*sb_hash[SB_HASHSIZE];
static int				sb_numInstances;

// per-frame index scratch; one draw flushes it
#define SB_MAXIDX	(1 << 16)
static glIndex_t	sb_idx[SB_MAXIDX];
static int			sb_numIdx;
static int			sb_curGroup = -1;

static int	sb_statBatches, sb_statSurfs, sb_statTris, sb_statInstances;
static int	sb_statFull, sb_statGroupSplit, sb_statLit, sb_statReject;

/*
===============
R_StaticBatch_Clear
===============
*/
void R_StaticBatch_Clear( void )
{
	GXM_Sync();
	for ( int i = 0; i < sb_numGroups; i++ ) {
#ifdef USE_GXM_NATIVE
		if ( sb_groups[i].data ) {
			GXM_Free( sb_groups[i].uid );
		}
#else
		if ( sb_groups[i].vbo ) {
			glDeleteBuffers( 1, &sb_groups[i].vbo );
		}
#endif
#ifndef USE_GXM_NATIVE
		if ( sb_groups[i].shadow ) {
			R_Free( sb_groups[i].shadow );
		}
#endif
	}
	memset( sb_groups, 0, sizeof( sb_groups ) );
	sb_numGroups = 0;
	sb_bytes = 0;
	sb_failed = qfalse;
	sb_numIdx = 0;
	sb_curGroup = -1;

	for ( int i = 0; i < SB_HASHSIZE; i++ ) {
		sbInstance_t *inst = sb_hash[i];
		while ( inst ) {
			sbInstance_t *next = inst->next;
			R_Free( inst->surfs );
			R_Free( inst->shaders );
			R_Free( inst );
			inst = next;
		}
		sb_hash[i] = NULL;
	}
	sb_numInstances = 0;
}

// GXM memblocks outlive a context, so only the GL build needs to forget its buffer names
void R_StaticBatch_ContextReset( void )
{
#ifndef USE_GXM_NATIVE
	R_StaticBatch_Clear();
#endif
}

static unsigned int R_StaticBatch_Hash( qhandle_t hModel, const vec3_t origin )
{
	unsigned int h = (unsigned int)hModel * 2654435761u;
	for ( int i = 0; i < 3; i++ ) {
		h ^= *(const unsigned int *)&origin[i];
		h *= 16777619u;
	}
	return h & ( SB_HASHSIZE - 1 );
}

/*
===============
R_StaticBatch_ShaderEligible

Anything whose vertices are not fully determined at bake time has to keep its own
entity, so only plain diffuse lighting and constant colour are taken.
===============
*/
static qboolean R_StaticBatch_ShaderEligible( const shader_t *shader )
{
	if ( !shader || shader->defaultShader ) {
		return qfalse;
	}
	if ( shader->numDeforms || shader->sky || shader->polygonOffset ) {
		return qfalse;
	}
	if ( shader->sort > SS_OPAQUE ) {
		return qfalse;					// blended geometry needs its per-entity depth order
	}
	if ( shader->numUnfoggedPasses < 1 || shader->numUnfoggedPasses > 2 ) {
		return qfalse;
	}
	for ( int i = 0; i < shader->numUnfoggedPasses; i++ ) {
		const shaderStage_t *st = &shader->stages[i];
		if ( !st->active ) {
			return qfalse;
		}
		if ( st->bundle[0].numTexMods || st->bundle[1].numTexMods ) {
			return qfalse;				// scrolling coordinates are recomputed every frame
		}
		if ( st->bundle[0].isVideoMap || st->bundle[1].image ) {
			return qfalse;
		}
		if ( st->bundle[0].numImageAnimations > 1 ) {
			return qfalse;
		}
		if ( st->alphaGen != AGEN_IDENTITY && st->alphaGen != AGEN_SKIP ) {
			return qfalse;
		}
		switch ( st->rgbGen ) {
		case CGEN_IDENTITY:
		case CGEN_IDENTITY_LIGHTING:
		case CGEN_LIGHTING_DIFFUSE:
			break;
		default:
			return qfalse;				// waveforms and entity colour are per-frame
		}
	}
	return qtrue;
}

// Vertex colour is baked exactly as RB_CalcDiffuseColor would compute it, using the
// model-space normal and the entity-space light direction it is paired with.
static void R_StaticBatch_VertexColor( const trRefEntity_t *ent, const vec3_t normal,
									   qboolean diffuse, byte *out )
{
	if ( !diffuse ) {
		out[0] = out[1] = out[2] = (byte)( tr.identityLight * 255 );
		out[3] = 255;
		return;
	}

	const float incoming = DotProduct( normal, ent->lightDir );
	if ( incoming <= 0 ) {
		*(int *)out = ent->ambientLightInt;
		return;
	}
	for ( int i = 0; i < 3; i++ ) {
		int j = Q_ftol( ent->ambientLight[i] + incoming * ent->directedLight[i] );
		if ( j > 255 ) {
			j = 255;
		}
		out[i] = (byte)j;
	}
	out[3] = 255;
}

// Returns a group with room for numVerts, opening one if the current is full.
static int R_StaticBatch_GroupFor( int numVerts )
{
	if ( numVerts > SB_GROUPVERTS ) {
		return -1;
	}
	if ( sb_numGroups && sb_groups[sb_numGroups - 1].numVerts + numVerts <= SB_GROUPVERTS ) {
		return sb_numGroups - 1;
	}
	if ( sb_numGroups >= SB_MAXGROUPS ) {
		return -1;
	}

	// Props bake mid-frame from the frontend, so the render thread has to be parked before
	// mapping gpu memory -- R_BuildWorldVBO takes the same precaution at load time.
	R_IssuePendingRenderCommands();

	sbGroup_t *g = &sb_groups[sb_numGroups];
	const int bytes = SB_GROUPVERTS * SB_STRIDE;
#ifdef USE_GXM_NATIVE
	SceUID uid = 0;
	void *gpu = GXM_Alloc( SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
		bytes, 4, SCE_GXM_MEMORY_ATTRIB_READ, &uid );
	if ( !gpu ) {
		sb_failed = qtrue;
		return -1;
	}
	g->data = gpu;
	g->uid = uid;
#else
	g->shadow = (byte *)R_Malloc( bytes, TAG_MODEL_MD3, qfalse );
	if ( !g->shadow ) {
		sb_failed = qtrue;
		return -1;
	}
	glGenBuffers( 1, &g->vbo );
	if ( !g->vbo ) {
		R_Free( g->shadow );
		g->shadow = NULL;
		sb_failed = qtrue;
		return -1;
	}
	glBindBuffer( GL_ARRAY_BUFFER, g->vbo );
	glBufferData( GL_ARRAY_BUFFER, bytes, NULL, GL_STATIC_DRAW );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
#endif
	g->numVerts = 0;
	sb_bytes += bytes;
	return sb_numGroups++;
}

// where a group's vertices are written during a bake
static byte *R_StaticBatch_GroupVerts( sbGroup_t *g )
{
#ifdef USE_GXM_NATIVE
	return (byte *)g->data;
#else
	return g->shadow;
#endif
}

/*
===============
R_StaticBatch_Bake

Packs one instance's surfaces into world space. NULL means it has to keep the
per-entity path, and that answer is cached by the caller.
===============
*/
static sbInstance_t *R_StaticBatch_Bake( trRefEntity_t *ent, md3Header_t *header )
{
	md3Surface_t	*surface;
	int				i, v;

	// Vet the whole model before writing anything: a bail-out halfway through would
	// leave orphaned vertices in the group. Every surface also has to fit one group,
	// or the instance would end up split across two buffers and two draws.
	int totalVerts = 0;
	surface = (md3Surface_t *)( (byte *)header + header->ofsSurfaces );
	for ( i = 0; i < header->numSurfaces; i++ ) {
		shader_t *shader = tr.defaultShader;
		if ( surface->numShaders > 0 ) {
			const md3Shader_t *md3Shader = (const md3Shader_t *)( (byte *)surface + surface->ofsShaders );
			shader = tr.shaders[ md3Shader->shaderIndex ];
		}
		if ( !R_StaticBatch_ShaderEligible( shader ) ) {
			return NULL;
		}
		totalVerts += surface->numVerts;
		surface = (md3Surface_t *)( (byte *)surface + surface->ofsEnd );
	}
	if ( !totalVerts || totalVerts > SB_GROUPVERTS ) {
		return NULL;
	}

	const int group = R_StaticBatch_GroupFor( totalVerts );
	if ( group < 0 ) {
		return NULL;
	}

	sbInstance_t *inst = (sbInstance_t *)R_Malloc( sizeof( sbInstance_t ), TAG_MODEL_MD3, qtrue );
	inst->surfs   = (srfStaticBatch_t *)R_Malloc( header->numSurfaces * sizeof( srfStaticBatch_t ), TAG_MODEL_MD3, qtrue );
	inst->shaders = (shader_t **)R_Malloc( header->numSurfaces * sizeof( shader_t * ), TAG_MODEL_MD3, qtrue );
	inst->hModel  = ent->e.hModel;
	VectorCopy( ent->e.origin, inst->origin );
	VectorCopy( ent->e.axis[0], inst->axis[0] );
	VectorCopy( ent->e.axis[1], inst->axis[1] );
	VectorCopy( ent->e.axis[2], inst->axis[2] );
	VectorCopy( ent->e.modelScale, inst->scale );

	sbGroup_t *g = &sb_groups[group];
	byte *groupVerts = R_StaticBatch_GroupVerts( g );
	const int firstVert = g->numVerts;
	surface = (md3Surface_t *)( (byte *)header + header->ofsSurfaces );

	for ( i = 0; i < header->numSurfaces; i++ )
	{
		shader_t *shader = tr.defaultShader;
		if ( surface->numShaders > 0 ) {
			md3Shader_t *md3Shader = (md3Shader_t *)( (byte *)surface + surface->ofsShaders );
			shader = tr.shaders[ md3Shader->shaderIndex ];
		}

		const qboolean diffuse = (qboolean)( shader->stages[0].rgbGen == CGEN_LIGHTING_DIFFUSE );
		const md3XyzNormal_t *xyz = (const md3XyzNormal_t *)( (byte *)surface + surface->ofsXyzNormals );
		const float *st = (const float *)( (byte *)surface + surface->ofsSt );
		const int base = g->numVerts;

		for ( v = 0; v < surface->numVerts; v++, xyz++ )
		{
			vec3_t	local, normal;
			byte	*dst = groupVerts + ( base + v ) * SB_STRIDE;

			local[0] = xyz->xyz[0] * MD3_XYZ_SCALE * inst->scale[0];
			local[1] = xyz->xyz[1] * MD3_XYZ_SCALE * inst->scale[1];
			local[2] = xyz->xyz[2] * MD3_XYZ_SCALE * inst->scale[2];

			const float lat = ( xyz->normal >> 8 ) & 0xff;
			const float lng = ( xyz->normal & 0xff );
			const float rlat = lat * ( 2.0f * M_PI ) / 255.0f;
			const float rlng = lng * ( 2.0f * M_PI ) / 255.0f;
			normal[0] = cos( rlat ) * sin( rlng );
			normal[1] = sin( rlat ) * sin( rlng );
			normal[2] = cos( rlng );

			float *out = (float *)dst;
			out[0] = inst->origin[0] + local[0] * inst->axis[0][0]
				   + local[1] * inst->axis[1][0] + local[2] * inst->axis[2][0];
			out[1] = inst->origin[1] + local[0] * inst->axis[0][1]
				   + local[1] * inst->axis[1][1] + local[2] * inst->axis[2][1];
			out[2] = inst->origin[2] + local[0] * inst->axis[0][2]
				   + local[1] * inst->axis[1][2] + local[2] * inst->axis[2][2];

			memcpy( dst + SB_OFS_ST,  &st[v * 2], 2 * sizeof( float ) );
			memcpy( dst + SB_OFS_ST2, &st[v * 2], 2 * sizeof( float ) );
			R_StaticBatch_VertexColor( ent, normal, diffuse, dst + SB_OFS_RGBA );
		}

		const int *tri = (const int *)( (byte *)surface + surface->ofsTriangles );
		const int numIndexes = surface->numTriangles * 3;
		glIndex_t *idx = (glIndex_t *)R_Malloc( numIndexes * sizeof( glIndex_t ), TAG_MODEL_MD3, qfalse );
		for ( int n = 0; n < numIndexes; n++ ) {
			idx[n] = (glIndex_t)( base + tri[n] );
		}

		inst->surfs[i].surfaceType = SF_STATICBATCH;
		inst->surfs[i].group       = group;
		inst->surfs[i].numIndexes  = numIndexes;
		inst->surfs[i].indexes     = idx;
		inst->shaders[i]           = shader;

		g->numVerts += surface->numVerts;
		surface = (md3Surface_t *)( (byte *)surface + surface->ofsEnd );
	}

#ifndef USE_GXM_NATIVE
	// GXM wrote straight into the mapped buffer; GL has to push the new range
	glBindBuffer( GL_ARRAY_BUFFER, g->vbo );
	glBufferSubData( GL_ARRAY_BUFFER, firstVert * SB_STRIDE,
		( g->numVerts - firstVert ) * SB_STRIDE, groupVerts + firstVert * SB_STRIDE );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
#endif

	inst->numSurfs = header->numSurfaces;
	sb_numInstances++;
	return inst;
}

/*
===============
R_StaticBatch_AddEntity

True means the instance was queued as world-sorted batch surfaces and the caller
must not add its own. Anything unbaked keeps the stock per-entity path.
===============
*/
qboolean R_StaticBatch_AddEntity( trRefEntity_t *ent, md3Header_t *header, int fogNum )
{
	if ( !r_staticBatch || !r_staticBatch->integer || sb_failed ) {
		return qfalse;
	}
	if ( !( ent->e.renderfx & RF_STATIC_BATCH ) ) {
		return qfalse;
	}
	// LA goggles force a fog index onto every surface, and a batch surface that
	// decomposes with fog would be skipped rather than drawn
	if ( tr.refdef.doLAGoggles ) {
		return qfalse;
	}
	// a baked instance carries no entity, so anything per-entity has to stay behind
	if ( fogNum || ent->needDlights || ent->e.customShader || ent->e.customSkin
		|| ent->e.skinNum || ent->e.frame || ent->e.oldframe ) {
		sb_statReject++;
		return qfalse;
	}
	if ( header->numFrames != 1 || header->numSurfaces < 1 ) {
		sb_statReject++;
		return qfalse;
	}

	const unsigned int h = R_StaticBatch_Hash( ent->e.hModel, ent->e.origin );
	sbInstance_t *inst = sb_hash[h];
	while ( inst ) {
		if ( inst->hModel == ent->e.hModel
			&& VectorCompare( inst->origin, ent->e.origin )
			&& VectorCompare( inst->axis[0], ent->e.axis[0] )
			&& VectorCompare( inst->axis[1], ent->e.axis[1] )
			&& VectorCompare( inst->axis[2], ent->e.axis[2] ) ) {
			break;
		}
		inst = inst->next;
	}

	if ( !inst ) {
		if ( sb_numInstances >= SB_MAXINSTANCES ) {
			return qfalse;
		}
		inst = R_StaticBatch_Bake( ent, header );
		if ( !inst ) {
			// remember the refusal, or an ineligible prop re-walks its model every frame
			inst = (sbInstance_t *)R_Malloc( sizeof( sbInstance_t ), TAG_MODEL_MD3, qtrue );
			inst->hModel = ent->e.hModel;
			VectorCopy( ent->e.origin, inst->origin );
			VectorCopy( ent->e.axis[0], inst->axis[0] );
			VectorCopy( ent->e.axis[1], inst->axis[1] );
			VectorCopy( ent->e.axis[2], inst->axis[2] );
			inst->numSurfs = 0;
			sb_numInstances++;
		}
		inst->next = sb_hash[h];
		sb_hash[h] = inst;
	}

	if ( !inst->numSurfs ) {
		sb_statReject++;
		return qfalse;
	}

	// the whole point: drop the entity from the sort key so copies batch together
	const unsigned oldShifted = tr.shiftedEntityNum;
	tr.shiftedEntityNum = REFENTITYNUM_WORLD << QSORT_REFENTITYNUM_SHIFT;
	for ( int i = 0; i < inst->numSurfs; i++ ) {
		R_AddDrawSurf( (surfaceType_t *)&inst->surfs[i], inst->shaders[i], 0, qfalse );
	}
	tr.shiftedEntityNum = oldShifted;

	sb_statInstances++;
	return qtrue;
}

void R_StaticBatch_Flush( shader_t *shader );

/*
===============
R_StaticBatch_Surface

Appends a batch surface's indices. False means it has to go through tess.
===============
*/
qboolean R_StaticBatch_Surface( const srfStaticBatch_t *surf, shader_t *shader,
								int fogNum, int dlighted )
{
	if ( fogNum || dlighted ) {
		sb_statLit++;
		return qfalse;					// the frontend keeps these on the entity path
	}

	// The stock path for these surfaces is a skip, so nothing here may refuse the
	// geometry: a boundary closes the current batch and starts a new one instead.
	if ( sb_curGroup >= 0 && sb_curGroup != surf->group ) {
		sb_statGroupSplit++;
		R_StaticBatch_Flush( shader );
	}
	if ( sb_numIdx + surf->numIndexes > SB_MAXIDX ) {
		sb_statFull++;
		R_StaticBatch_Flush( shader );
	}

	sb_curGroup = surf->group;
	sb_statSurfs++;
	memcpy( sb_idx + sb_numIdx, surf->indexes, surf->numIndexes * sizeof( glIndex_t ) );
	sb_numIdx += surf->numIndexes;
	return qtrue;
}

/*
===============
R_StaticBatch_Flush

Draws whatever the current batch accumulated.
===============
*/
void R_StaticBatch_Flush( shader_t *shader )
{
	if ( !sb_numIdx || sb_curGroup < 0 || sb_curGroup >= sb_numGroups || !shader ) {
		sb_numIdx = 0;
		sb_curGroup = -1;
		return;
	}

	sb_statTris += sb_numIdx / 3;
	GL_Cull( shader->cullType );

#ifdef USE_GXM_NATIVE
	for ( int i = 0; i < shader->numUnfoggedPasses; i++ ) {
		const shaderStage_t *ps = &shader->stages[i];
		const int vcol = ( ps->rgbGen == CGEN_LIGHTING_DIFFUSE );
		const float lit = ( ps->rgbGen == CGEN_IDENTITY_LIGHTING ) ? tr.identityLight : 1.0f;

		GL_State( ps->stateBits );
		GL_SelectTexture( 0 );
		R_BindAnimatedImage( &ps->bundle[0] );

		GXM_SetConstantColor( lit, lit, lit, 1.0f );
		GXM_SetTexUnitCount( 1 );
		GXM_SetStateBits( glState.glStateBits );
		GXM_DrawStaticBuffer( sb_groups[sb_curGroup].data, sb_idx, sb_numIdx, vcol );
		sb_statBatches++;
	}
	GXM_SetConstantColor( 1.0f, 1.0f, 1.0f, 1.0f );
#else
	const shaderStage_t *st = &shader->stages[0];
	GL_State( st->stateBits );
	sb_statBatches++;

	glBindBuffer( GL_ARRAY_BUFFER, sb_groups[sb_curGroup].vbo );
	qglEnableClientState( GL_VERTEX_ARRAY );
	qglVertexPointer( 3, GL_FLOAT, SB_STRIDE, (void *)0 );

	GL_SelectTexture( 0 );
	qglEnable( GL_TEXTURE_2D );
	R_BindAnimatedImage( &st->bundle[0] );
	qglEnableClientState( GL_TEXTURE_COORD_ARRAY );
	qglTexCoordPointer( 2, GL_FLOAT, SB_STRIDE, (void *)SB_OFS_ST );

	if ( st->rgbGen == CGEN_LIGHTING_DIFFUSE ) {
		qglEnableClientState( GL_COLOR_ARRAY );
		qglColorPointer( 4, GL_UNSIGNED_BYTE, SB_STRIDE, (void *)SB_OFS_RGBA );
	} else {
		qglDisableClientState( GL_COLOR_ARRAY );
		qglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	}

	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	qglDrawRangeElements( GL_TRIANGLES, 0, sb_groups[sb_curGroup].numVerts - 1,
		sb_numIdx, GL_INDEX_TYPE, sb_idx );

	qglDisableClientState( GL_COLOR_ARRAY );
	qglDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
#endif

	sb_numIdx = 0;
	sb_curGroup = -1;
}

// what the batcher managed this frame, and where the rest went
void R_StaticBatch_Stats( char *out, int outSize )
{
	Com_sprintf( out, outSize,
		"SBATCH: draws=%d surfs=%d tris=%d inst=%d | cached=%d groups=%d %.2fMB | tess: lit=%d split=%d full=%d reject=%d\n",
		sb_statBatches, sb_statSurfs, sb_statTris, sb_statInstances,
		sb_numInstances, sb_numGroups, sb_bytes / ( 1024.0f * 1024.0f ),
		sb_statLit, sb_statGroupSplit, sb_statFull, sb_statReject );

	sb_statBatches = sb_statSurfs = sb_statTris = sb_statInstances = 0;
	sb_statLit = sb_statGroupSplit = sb_statFull = sb_statReject = 0;
}

#endif	// VITA
