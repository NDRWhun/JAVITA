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

// World geometry is static, so its vertices live in GPU buffers built at load and
// only indices are written per frame. Eligible planar surfaces draw straight from
// there; everything else falls through to the unchanged tess path.

#include "../server/exe_headers.h"

#include "tr_local.h"
#ifdef USE_GXM_NATIVE
#include "../rd-gxm/gxm_device.h"
#endif

#ifdef VITA

// owned by tr_init.cpp
extern cvar_t	*r_worldVBO;
#ifdef USE_GXM_NATIVE
extern cvar_t	*r_gxmStats;
extern cvar_t	*r_gxmCullFlip;
#endif

#define WVBO_MAXGROUPS		64
#define WVBO_GROUPVERTS		65000					// u16 indices address one group
#define WVBO_STRIDE			32						// xyz(12) st(8) lm(8) rgba(4)
#define WVBO_OFS_ST			12
#define WVBO_OFS_LM			20
#define WVBO_OFS_RGBA		28

typedef struct {
#ifdef USE_GXM_NATIVE
	void	*data;					// GPU-mapped, read straight by sceGxmSetVertexStream
	SceUID	uid;
#else
	GLuint	vbo;
#endif
	float	*xyzMirror;				// cached copy for per-frame fog texcoords (vec4 stride)
	int		numVerts;
} wvboGroup_t;

static wvboGroup_t	wvbo_groups[WVBO_MAXGROUPS];
static int			wvbo_numGroups;
static int			wvbo_bytes;
static qboolean		wvbo_failed;

// per-frame index scratch; one draw flushes it
#define WVBO_MAXIDX	(1 << 16)
static glIndex_t	wvbo_idx[WVBO_MAXIDX];
static int			wvbo_numIdx;
static int			wvbo_curGroup = -1;
static int			wvbo_curFog = 0;
static float		*wvbo_fogSt;			// per-flush fog texcoords, batch-rebased

// per-frame, so the fallback reasons can be told apart
static int	wvbo_statBatches, wvbo_statSurfs, wvbo_statTris;
static int	wvbo_statNotResident, wvbo_statLit, wvbo_statGroupSplit, wvbo_statFull;
static int	wvbo_statStyle;

/*
===============
R_WorldVBO_Clear
===============
*/
void R_WorldVBO_Clear( void )
{
	GXM_Sync();
	for ( int i = 0; i < wvbo_numGroups; i++ ) {
#ifdef USE_GXM_NATIVE
		if ( wvbo_groups[i].data ) {
			GXM_Free( wvbo_groups[i].uid );
		}
#else
		if ( wvbo_groups[i].vbo ) {
			glDeleteBuffers( 1, &wvbo_groups[i].vbo );
		}
#endif
	}
	for ( int i = 0; i < wvbo_numGroups; i++ ) {
		if ( wvbo_groups[i].xyzMirror ) {
			R_Free( wvbo_groups[i].xyzMirror );
		}
	}
	if ( wvbo_fogSt ) {
		R_Free( wvbo_fogSt );
		wvbo_fogSt = NULL;
	}
	memset( wvbo_groups, 0, sizeof( wvbo_groups ) );
	wvbo_numGroups = 0;
	wvbo_bytes = 0;
	wvbo_failed = qfalse;
	wvbo_numIdx = 0;
	wvbo_curGroup = -1;
}

// A fresh GL context invalidates every buffer name. GXM memblocks are owned by the
// process rather than a context, so there is nothing to forget there.
void R_WorldVBO_ContextReset( void )
{
#ifndef USE_GXM_NATIVE
	memset( wvbo_groups, 0, sizeof( wvbo_groups ) );
	wvbo_numGroups = 0;
	wvbo_bytes = 0;
#endif
}

// A stage can be drawn from the buffer when both its coordinates and its colour
// are already in it: uv0 for the diffuse, uv1 for a collapsed lightmap, and
// either a constant tint or the stream colour.
// why a shader was turned away, tallied over one map build
enum { WVR_SKY, WVR_POLYOFFSET, WVR_SORT, WVR_DEFORM, WVR_PASSES,
       WVR_LMSTYLE, WVR_NOLIGHTMAP, WVR_STAGE, WVR_COUNT };
static int wvbo_reason[WVR_COUNT];
static const char *wvbo_reasonName[WVR_COUNT] = {
	"sky", "polygonOffset", "sort>opaque", "deform", "passes",
	"vertexStyle", "noLightmap", "stage"
};

static qboolean R_WorldVBO_StageEligible( const shaderStage_t *st )
{
	if ( !st->active ) {
		return qfalse;
	}
	if ( st->bundle[0].tcGen != TCGEN_TEXTURE || st->bundle[0].numTexMods ) {
		return qfalse;
	}
	if ( st->bundle[1].image
		&& ( st->bundle[1].tcGen != TCGEN_LIGHTMAP || st->bundle[1].numTexMods ) ) {
		return qfalse;
	}
	switch ( st->rgbGen ) {
	case CGEN_IDENTITY:
	case CGEN_IDENTITY_LIGHTING:
	case CGEN_EXACT_VERTEX:
	case CGEN_VERTEX:
		break;
	default:
		return qfalse;
	}
	// ParseStage rewrites identity to skip on script-authored stages
	return (qboolean)( st->alphaGen == AGEN_IDENTITY || st->alphaGen == AGEN_SKIP );
}

// Only the static per-vertex data the backend reads can come from the buffer;
// anything view- or time-dependent has to keep running through tess.
static qboolean R_WorldVBO_ShaderEligible( const shader_t *shader )
{
	if ( shader->sky ) {
		wvbo_reason[WVR_SKY]++;
		return qfalse;					// sky runs its own stage iterator, not the generic one
	}
	if ( shader->polygonOffset ) {
		wvbo_reason[WVR_POLYOFFSET]++;
		return qfalse;					// this path never sets GL_POLYGON_OFFSET_FILL
	}
	if ( shader->sort > SS_OPAQUE ) {
		wvbo_reason[WVR_SORT]++;
		return qfalse;					// blended surfaces need their draw order kept
	}
	if ( shader->numDeforms ) {
		wvbo_reason[WVR_DEFORM]++;
		return qfalse;					// deformVertexes rewrites xyz every frame
	}
	if ( shader->numUnfoggedPasses < 1 || shader->numUnfoggedPasses > MAX_SHADER_STAGES ) {
		wvbo_reason[WVR_PASSES]++;
		return qfalse;
	}
	if ( shader->lightmapIndex[0] == LIGHTMAP_BY_VERTEX ) {
		// the bake folds style 0 in; a second style stays dynamic and has no stage to reject
		if ( shader->styles[0] != LS_NORMAL || shader->styles[1] < LS_UNUSED ) {
			wvbo_reason[WVR_LMSTYLE]++;
			return qfalse;
		}
	} else if ( shader->lightmapIndex[0] < 0 ) {
		wvbo_reason[WVR_NOLIGHTMAP]++;
		return qfalse;
	}

	for ( int i = 0; i < shader->numUnfoggedPasses; i++ ) {
		if ( !R_WorldVBO_StageEligible( &shader->stages[i] ) ) {
			wvbo_reason[WVR_STAGE]++;
			return qfalse;
		}
	}
	return qtrue;
}

typedef struct {
	srfSurfaceFace_t	*face;
	int					shader;			// tr.shaders[] index; only adjacency matters
	qboolean			vertexLit;		// tess runs ComputeFinalVertexColor on these
} wvboEntry_t;

static int R_WorldVBO_CompareEntry( const void *a, const void *b )
{
	return ( (const wvboEntry_t *)a )->shader - ( (const wvboEntry_t *)b )->shader;
}

/*
===============
R_BuildWorldVBO

Packs eligible planar surfaces into GPU vertex buffers once per map.
===============
*/
void R_BuildWorldVBO( world_t &worldData )
{
	if ( !r_worldVBO || !r_worldVBO->integer || wvbo_failed ) {
		return;
	}

	const int numSurfaces = worldData.numsurfaces;
	if ( numSurfaces <= 0 ) {
		return;
	}

	// the buffers below are GL work issued from the loading thread
	R_IssuePendingRenderCommands();
	R_WorldVBO_Clear();

	// Gather eligible surfaces and pack them in material order. The backend batches a
	// run of one shader into one draw only while the run stays inside one buffer, so
	// packing in surface order would scatter a shader across groups and split it.
	wvboEntry_t *list = (wvboEntry_t *)R_Malloc( numSurfaces * sizeof( wvboEntry_t ),
												TAG_TEMP_WORKSPACE, qfalse );
	int numEligible = 0;
	int rejNotFace = 0, rejShader = 0;
	memset( wvbo_reason, 0, sizeof( wvbo_reason ) );

	for ( int i = 0; i < numSurfaces; i++ )
	{
		msurface_t *surf = &worldData.surfaces[i];
		if ( *surf->data != SF_FACE ) {
			rejNotFace++;			// patch or trisoup: a surface type thing, not a shader thing
			continue;
		}
		if ( !R_WorldVBO_ShaderEligible( surf->shader ) ) {
			rejShader++;
			continue;
		}
		list[numEligible].face  = (srfSurfaceFace_t *)surf->data;
		list[numEligible].shader = surf->shader->index;
		list[numEligible].vertexLit = (qboolean)( surf->shader->lightmapIndex[0] == LIGHTMAP_BY_VERTEX );
		numEligible++;
	}

	qsort( list, numEligible, sizeof( wvboEntry_t ), R_WorldVBO_CompareEntry );

	byte *verts = (byte *)R_Malloc( WVBO_GROUPVERTS * WVBO_STRIDE, TAG_TEMP_WORKSPACE, qfalse );
	int groupVerts = 0;
	int resident = 0, totalVerts = 0, totalIdx = 0, splitShaders = 0;
	int lastShader = -1;

	for ( int i = 0; i <= numEligible; i++ )
	{
		srfSurfaceFace_t *face = ( i < numEligible ) ? list[i].face : NULL;

		// a shader that outgrows one buffer still has to split, so count it
		if ( face && list[i].shader != lastShader ) {
			lastShader = list[i].shader;
		} else if ( face && groupVerts + face->numPoints > WVBO_GROUPVERTS ) {
			splitShaders++;
		}

		// close only when this surface cannot fit, or the list ran out
		if ( groupVerts && ( i == numEligible
			|| ( face && groupVerts + face->numPoints > WVBO_GROUPVERTS ) ) )
		{
			if ( wvbo_numGroups >= WVBO_MAXGROUPS ) {
				break;
			}
#ifdef USE_GXM_NATIVE
			SceUID uid = 0;
			void *gpu = GXM_Alloc( SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
				groupVerts * WVBO_STRIDE, 4, SCE_GXM_MEMORY_ATTRIB_READ, &uid );
			if ( !gpu ) {
				wvbo_failed = qtrue;
				break;
			}
			memcpy( gpu, verts, groupVerts * WVBO_STRIDE );

			wvbo_groups[wvbo_numGroups].data = gpu;
			wvbo_groups[wvbo_numGroups].uid  = uid;
			// positions stay CPU-side too: the fog overlay computes texcoords from them
			{
				float *mir = (float *)R_Malloc( groupVerts * 4 * sizeof( float ), TAG_MODEL_MD3, qfalse );
				for ( int mv = 0; mv < groupVerts; mv++ ) {
					memcpy( &mir[mv * 4], verts + mv * WVBO_STRIDE, 3 * sizeof( float ) );
					mir[mv * 4 + 3] = 1.0f;
				}
				wvbo_groups[wvbo_numGroups].xyzMirror = mir;
			}
#else
			GLuint id = 0;
			glGenBuffers( 1, &id );
			if ( !id ) {
				wvbo_failed = qtrue;
				break;
			}
			glBindBuffer( GL_ARRAY_BUFFER, id );
			glBufferData( GL_ARRAY_BUFFER, groupVerts * WVBO_STRIDE, verts, GL_STATIC_DRAW );
			glBindBuffer( GL_ARRAY_BUFFER, 0 );

			wvbo_groups[wvbo_numGroups].vbo = id;
#endif
			wvbo_groups[wvbo_numGroups].numVerts = groupVerts;
			wvbo_bytes += groupVerts * WVBO_STRIDE;
			wvbo_numGroups++;
			groupVerts = 0;
		}

		// re-test against the group that is current now, which may be a fresh one
		if ( !face || groupVerts + face->numPoints > WVBO_GROUPVERTS ) {
			continue;
		}

		const int base = groupVerts;
		const qboolean vertexLit = list[i].vertexLit;
		for ( int v = 0; v < face->numPoints; v++ )
		{
			const float	*p = face->points[v];
			byte		*dst = verts + ( base + v ) * WVBO_STRIDE;

			memcpy( dst, p, 3 * sizeof( float ) );					// xyz
			memcpy( dst + WVBO_OFS_ST, &p[3], 2 * sizeof( float ) );	// diffuse st
			memcpy( dst + WVBO_OFS_LM, &p[VERTEX_LM], 2 * sizeof( float ) );
			// reproduce ComputeFinalVertexColor: style 0 is white and r_fullbright is latched
			if ( vertexLit ) {
				const byte	*c = (const byte *)&p[VERTEX_COLOR];
				byte		*o = dst + WVBO_OFS_RGBA;
				if ( r_fullbright->integer ) {
					o[0] = o[1] = o[2] = 255;
				} else {
					o[0] = (byte)( ( c[0] * 255 ) >> 8 );
					o[1] = (byte)( ( c[1] * 255 ) >> 8 );
					o[2] = (byte)( ( c[2] * 255 ) >> 8 );
				}
				o[3] = c[3];
			} else {
				*(unsigned int *)( dst + WVBO_OFS_RGBA ) = *(const unsigned int *)&p[VERTEX_COLOR];
			}
		}
		groupVerts += face->numPoints;

		// pre-offset the indices so a frame only memcpys them
		const int *src = (const int *)( (byte *)face + face->ofsIndices );
		glIndex_t *dst = (glIndex_t *)R_Hunk_Alloc( face->numIndices * sizeof( glIndex_t ), qfalse );
		for ( int n = 0; n < face->numIndices; n++ ) {
			dst[n] = (glIndex_t)( base + src[n] );
		}

		face->vboGroup = wvbo_numGroups;	// the group being filled
		face->vboIndexes = dst;
		resident++;
		totalVerts += face->numPoints;
		totalIdx += face->numIndices;
	}

	R_Free( verts );
	R_Free( list );

	for ( int r = 0; r < WVR_COUNT; r++ ) {
		if ( wvbo_reason[r] ) {
			ri.Printf( PRINT_ALL, "world VBO: reject %-16s %i\n", wvbo_reasonName[r], wvbo_reason[r] );
		}
	}
	ri.Printf( PRINT_ALL, "world VBO: rejected %i non-planar, %i by shader\n", rejNotFace, rejShader );
	ri.Printf( PRINT_ALL, "world VBO: %i/%i surfaces resident, %i verts %i tris, %i groups, %i split, %.2f MB\n",
		resident, numSurfaces, totalVerts, totalIdx / 3, wvbo_numGroups, splitShaders,
		wvbo_bytes / (1024.0f * 1024.0f) );
}

// what the batcher managed this frame, and where the rest went
void R_WorldVBO_Stats( char *out, int outSize )
{
	Com_sprintf( out, outSize,
		"WVBO: draws=%d surfs=%d tris=%d | tess: noresident=%d lit=%d split=%d full=%d style=%d\n",
		wvbo_statBatches, wvbo_statSurfs, wvbo_statTris,
		wvbo_statNotResident, wvbo_statLit, wvbo_statGroupSplit, wvbo_statFull, wvbo_statStyle );

	wvbo_statBatches = wvbo_statSurfs = wvbo_statTris = 0;
	wvbo_statNotResident = wvbo_statLit = wvbo_statGroupSplit = wvbo_statFull = wvbo_statStyle = 0;
}

/*
===============
R_WorldVBO_Surface

Appends a resident surface's indices. False means it has to go through tess.
===============
*/
qboolean R_WorldVBO_Surface( const srfSurfaceFace_t *face, const shader_t *shader, int fogNum, int dlighted )
{
	if ( !r_worldVBO || !r_worldVBO->integer || wvbo_failed ) {
		return qfalse;
	}
	if ( !face->vboIndexes ) {
		wvbo_statNotResident++;
		return qfalse;
	}
	// style 0 is white in every shipped map, but bail rather than draw a stale bake
	if ( shader && shader->lightmapIndex[0] == LIGHTMAP_BY_VERTEX
		&& ( styleColors[LS_NORMAL][0] != 255 || styleColors[LS_NORMAL][1] != 255
			|| styleColors[LS_NORMAL][2] != 255 ) ) {
		wvbo_statStyle++;
		return qfalse;
	}
	if ( dlighted || face->dlightBits[backEnd.smpFrame] ) {
		wvbo_statLit++;
		return qfalse;					// the dlight pass only exists on the tess path
	}
	if ( wvbo_numIdx == 0 ) {
		wvbo_curFog = ( fogNum && tr.world ) ? fogNum : 0;
	} else if ( fogNum != wvbo_curFog ) {
		return qfalse;					// mixed fog in one batch would mis-fog the overlay
	}
	if ( wvbo_numIdx + face->numIndices > WVBO_MAXIDX ) {
		wvbo_statFull++;
		return qfalse;						// scratch full; tess takes the remainder
	}
	if ( wvbo_curGroup >= 0 && wvbo_curGroup != face->vboGroup ) {
		wvbo_statGroupSplit++;
		return qfalse;						// a different buffer, so a different draw
	}

	wvbo_curGroup = face->vboGroup;
	wvbo_statSurfs++;
	backEnd.pc.c_wvboSurfaces++;
	memcpy( wvbo_idx + wvbo_numIdx, face->vboIndexes, face->numIndices * sizeof( glIndex_t ) );
	wvbo_numIdx += face->numIndices;
	return qtrue;
}

/*
===============
R_WorldVBO_Flush

Draws whatever the current batch accumulated.
===============
*/
// The fog volume's density at each vertex, drawn over the resident batch the same
// way RB_FogPass covers a tess batch. The global fog is already carried by the
// fragment programs' fog uniform, so only bounded volumes project the fog image.
static void R_WorldVBO_FogOverlay( void )
{
	fog_t	*fog = tr.world->fogs + wvbo_curFog;

#ifndef JK2_MODE
	if ( ( wvbo_curFog == tr.world->globalFog || wvbo_curFog == tr.world->numfogs )
		&& r_drawfog->value == 2 ) {
		return;
	}
#endif

	const float *mir = wvbo_groups[wvbo_curGroup].xyzMirror;
	if ( !mir ) {
		return;
	}
	if ( !wvbo_fogSt ) {
		wvbo_fogSt = (float *)R_Malloc( WVBO_GROUPVERTS * 2 * sizeof( float ), TAG_MODEL_MD3, qfalse );
	}

	// the referenced span; the draw rebases onto it so the ring copy stays tight
	int minV = wvbo_idx[0], maxV = wvbo_idx[0];
	for ( int i = 1; i < wvbo_numIdx; i++ ) {
		const int v = wvbo_idx[i];
		if ( v < minV ) minV = v;
		if ( v > maxV ) maxV = v;
	}
	for ( int i = 0; i < wvbo_numIdx; i++ ) {
		wvbo_idx[i] = (glIndex_t)( wvbo_idx[i] - minV );
	}
	const int numV = maxV - minV + 1;

	// same construction as RB_CalcFogTexCoords; world orientation is current here
	vec3_t	localVec;
	vec4_t	fogDistanceVector, fogDepthVector;
	float	eyeT;

	VectorSubtract( backEnd.ori.origin, backEnd.viewParms.ori.origin, localVec );
	fogDistanceVector[0] = -backEnd.ori.modelMatrix[2];
	fogDistanceVector[1] = -backEnd.ori.modelMatrix[6];
	fogDistanceVector[2] = -backEnd.ori.modelMatrix[10];
	fogDistanceVector[3] = DotProduct( localVec, backEnd.viewParms.ori.axis[0] );
	fogDistanceVector[0] *= fog->tcScale;
	fogDistanceVector[1] *= fog->tcScale;
	fogDistanceVector[2] *= fog->tcScale;
	fogDistanceVector[3] *= fog->tcScale;
	fogDistanceVector[3] += 1.0f / 512.0f;

	if ( fog->hasSurface ) {
		fogDepthVector[0] = fog->surface[0] * backEnd.ori.axis[0][0] +
			fog->surface[1] * backEnd.ori.axis[0][1] + fog->surface[2] * backEnd.ori.axis[0][2];
		fogDepthVector[1] = fog->surface[0] * backEnd.ori.axis[1][0] +
			fog->surface[1] * backEnd.ori.axis[1][1] + fog->surface[2] * backEnd.ori.axis[1][2];
		fogDepthVector[2] = fog->surface[0] * backEnd.ori.axis[2][0] +
			fog->surface[1] * backEnd.ori.axis[2][1] + fog->surface[2] * backEnd.ori.axis[2][2];
		fogDepthVector[3] = -fog->surface[3] + DotProduct( backEnd.ori.origin, fog->surface );
		eyeT = DotProduct( backEnd.ori.viewOrigin, fogDepthVector ) + fogDepthVector[3];
	} else {
		eyeT = 1.0f;
		fogDepthVector[0] = fogDepthVector[1] = fogDepthVector[2] = 0.0f;
		fogDepthVector[3] = 1.0f;
	}
	const qboolean eyeOutside = (qboolean)( eyeT < 0.0f );

	for ( int i = 0; i < numV; i++ ) {
		const float *v = &mir[( minV + i ) * 4];
		float sc = DotProduct( v, fogDistanceVector ) + fogDistanceVector[3];
		float tc = DotProduct( v, fogDepthVector ) + fogDepthVector[3];
		if ( eyeOutside ) {
			if ( tc < 1.0f ) {
				tc = 1.0f / 32.0f;
			} else {
				tc = 1.0f / 32.0f + ( 30.0f / 32.0f ) * tc / ( tc - eyeT );
			}
			sc *= tc;
		} else if ( tc < 1.0f ) {
			tc = 1.0f / 32.0f;
		} else {
			tc = 31.0f / 32.0f;
		}
		wvbo_fogSt[i * 2 + 0] = sc;
		wvbo_fogSt[i * 2 + 1] = tc;
	}

#ifdef USE_GXM_NATIVE
	GL_Bind( tr.fogImage );
	GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL );
	GXM_SetTexUnitCount( 1 );
	GXM_SetConstantColor( fog->parms.color[0], fog->parms.color[1], fog->parms.color[2], 1.0f );
	GXM_SetVertexArrays( &mir[minV * 4], wvbo_fogSt, NULL, NULL );
	GXM_SetStateBits( glState.glStateBits );
	GXM_DrawTess( wvbo_numIdx, wvbo_idx, numV );
	GXM_SetConstantColor( 1.0f, 1.0f, 1.0f, 1.0f );
	wvbo_statBatches++;
	backEnd.pc.c_wvboDraws++;
#endif
}

void R_WorldVBO_Flush( shader_t *shader )
{
	if ( !wvbo_numIdx || wvbo_curGroup < 0 || wvbo_curGroup >= wvbo_numGroups ) {
		wvbo_numIdx = 0;
		wvbo_curGroup = -1;
		return;
	}

	const shaderStage_t *st = &shader->stages[0];

	wvbo_statTris += wvbo_numIdx / 3;
	GL_Cull( shader->cullType );

#ifdef USE_GXM_NATIVE
	// every stage reads the same resident vertices; only bindings and state move
	for ( int i = 0; i < shader->numUnfoggedPasses; i++ ) {
		const shaderStage_t *ps = &shader->stages[i];
		const int ntex = ps->bundle[1].image ? 2 : 1;
		const int vcol = ( ps->rgbGen == CGEN_EXACT_VERTEX || ps->rgbGen == CGEN_VERTEX );
		const float lit = ( ps->rgbGen == CGEN_IDENTITY_LIGHTING || ps->rgbGen == CGEN_VERTEX )
						  ? tr.identityLight : 1.0f;

		GL_State( ps->stateBits );
		GL_SelectTexture( 0 );
		R_BindAnimatedImage( &ps->bundle[0] );
		if ( ntex == 2 ) {
			GL_SelectTexture( 1 );
			GL_TexEnv( r_lightmap->integer ? GL_REPLACE : shader->multitextureEnv );
			R_BindAnimatedImage( &ps->bundle[1] );
			GL_SelectTexture( 0 );
		}

		GXM_SetConstantColor( lit, lit, lit, 1.0f );
		GXM_SetTexUnitCount( ntex );
		GXM_SetStateBits( glState.glStateBits );
		GXM_DrawStaticBuffer( wvbo_groups[wvbo_curGroup].data, wvbo_idx, wvbo_numIdx, vcol );
		wvbo_statBatches++;
		backEnd.pc.c_wvboDraws++;
	}
	GXM_SetTexUnitCount( 1 );
	GXM_SetConstantColor( 1.0f, 1.0f, 1.0f, 1.0f );
	GL_SelectTexture( 0 );

	if ( wvbo_curFog && tr.world && r_drawfog->value && shader->fogPass ) {
		R_WorldVBO_FogOverlay();
	}

	wvbo_numIdx = 0;
	wvbo_curFog = 0;
	wvbo_curGroup = -1;
	return;
#else
	GL_State( st->stateBits );
	wvbo_statBatches++;
	const GLuint vbo = wvbo_groups[wvbo_curGroup].vbo;

	glBindBuffer( GL_ARRAY_BUFFER, vbo );

	qglEnableClientState( GL_VERTEX_ARRAY );
	qglVertexPointer( 3, GL_FLOAT, WVBO_STRIDE, (void *)0 );

	GL_SelectTexture( 0 );
	qglEnable( GL_TEXTURE_2D );
	R_BindAnimatedImage( &st->bundle[0] );
	qglEnableClientState( GL_TEXTURE_COORD_ARRAY );
	qglTexCoordPointer( 2, GL_FLOAT, WVBO_STRIDE, (void *)WVBO_OFS_ST );

	GL_SelectTexture( 1 );
	qglEnable( GL_TEXTURE_2D );
	GL_TexEnv( r_lightmap->integer ? GL_REPLACE : shader->multitextureEnv );
	R_BindAnimatedImage( &st->bundle[1] );
	qglEnableClientState( GL_TEXTURE_COORD_ARRAY );
	qglTexCoordPointer( 2, GL_FLOAT, WVBO_STRIDE, (void *)WVBO_OFS_LM );

	// the gate only lets constant-colour stages through
	qglDisableClientState( GL_COLOR_ARRAY );
	if ( st->rgbGen == CGEN_IDENTITY_LIGHTING ) {
		qglColor4f( tr.identityLight, tr.identityLight, tr.identityLight, 1.0f );
	} else {
		qglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	}

	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	qglDrawRangeElements( GL_TRIANGLES, 0, wvbo_groups[wvbo_curGroup].numVerts - 1,
		wvbo_numIdx, GL_INDEX_TYPE, wvbo_idx );

	// unit 1 has to go back off, or the next single-texture pass (the sky) draws
	// through the lightmap still bound here
	qglDisableClientState( GL_TEXTURE_COORD_ARRAY );
	qglDisable( GL_TEXTURE_2D );

	GL_SelectTexture( 0 );
	qglDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	wvbo_numIdx = 0;
	wvbo_curGroup = -1;
#endif
}

#endif	// VITA
