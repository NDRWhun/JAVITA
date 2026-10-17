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
#include "rd-gxm/gxm_device.h"
#endif

#ifdef VITA

// owned by tr_init.cpp
extern cvar_t	*r_worldVBO;
extern bool		g_bRenderGlowingObjects;
#ifdef USE_GXM_NATIVE
extern cvar_t	*r_gxmStats;
extern cvar_t	*r_gxmCullFlip;
#endif

#define WVBO_MAXGROUPS		64
#define WVBO_GROUPVERTS		64000					// gxm needs u16 indices below 64000
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

#ifdef USE_GXM_NATIVE
// --- resident world (r_nativeWorld): surfaces uploaded once per map, every stage, light and fog pass drawn by GPU programs ---

static_assert( VERTEXSIZE == GXM_WORLD_POINT_FLOATS && VERTEX_LM == GXM_WORLD_POINT_LM
	&& VERTEX_COLOR == GXM_WORLD_POINT_COLOR, "face points must match the shared packer" );
static_assert( sizeof(drawVert_t) == sizeof(gxmWorldDrawVert_t), "drawVert_t must match the shared packer" );

// entity effects that change a stage beyond what its uniforms carry
#define NW_ENTITY_FX	( RF_DISINTEGRATE1 | RF_DISINTEGRATE2 | RF_VOLUMETRIC | RF_DISTORTION \
						| RF_FORCE_ENT_ALPHA | RF_ALPHA_FADE | RF_RGB_TINT | RF_FIRST_PERSON )

#define NW_MAX_RUNS		4096
#define NW_MAX_SEGS		32

enum { NWS_UNKNOWN, NWS_NO, NWS_YES };

typedef struct {
	byte	state;		// NWS_*
	byte	styled;		// reads lightmap sets 1..3 or lights vertices with more than one style
	byte	fogAdjust;	// a stage fades its colours by the per-vertex fog density
	byte	fogMask;	// fog modes the map's surfaces need, for program creation at load
} nwShader_t;

static nwShader_t		nw_shaders[MAX_SHADERS];
static qboolean			nw_active;			// this map's world was built resident

// the batch: one shader, fog and entity, surfaces in submission order
static gxmWorldRun_t	nw_runs[NW_MAX_RUNS];
static const srfSurfaceFace_t	*nw_runFace[NW_MAX_RUNS];
static int				nw_runBits[NW_MAX_RUNS];
static int				nw_numRuns;
static shader_t			*nw_shader;
static int				nw_fog;
static const trRefEntity_t	*nw_entity;
static int				nw_dlightBits;
static int				nw_groupSpans;		// runs of one group in a row, which bounds the segments a gather makes

static const float nw_fogOverrideColors[GLFOGOVERRIDE_MAX][4] = {
	{ 0.0f, 0.0f, 0.0f, 1.0f },
	{ 0.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 1.0f, 1.0f, 1.0f },
};

// the lightmap set a bundle reads, -1 when it reads none
static int NW_BundleLightmap( const textureBundle_t *b )
{
	switch ( b->tcGen ) {
	case TCGEN_LIGHTMAP:	return 0;
	case TCGEN_LIGHTMAP1:	return 1;
	case TCGEN_LIGHTMAP2:	return 2;
	case TCGEN_LIGHTMAP3:	return 3;
	default:				return -1;
	}
}

static qboolean NW_BundleOk( const textureBundle_t *b, qboolean first )
{
	switch ( b->tcGen ) {
	case TCGEN_TEXTURE:
	case TCGEN_LIGHTMAP:
	case TCGEN_LIGHTMAP1:
	case TCGEN_LIGHTMAP2:
	case TCGEN_LIGHTMAP3:
	case TCGEN_VECTOR:
	case TCGEN_IDENTITY:
		break;
	case TCGEN_ENVIRONMENT_MAPPED:
		if ( !first ) {
			return qfalse;
		}
		break;
	default:
		return qfalse;
	}
	int turbs = 0;
	for ( int tm = 0; tm < b->numTexMods; tm++ ) {
		const texMod_t type = b->texMods[tm].type;
		if ( type == TMOD_NONE ) {
			break;
		}
		switch ( type ) {
		case TMOD_TURBULENT:
			turbs++;
			break;
		case TMOD_TRANSFORM:
		case TMOD_SCROLL:
		case TMOD_SCALE:
		case TMOD_STRETCH:
		case TMOD_ROTATE:
		case TMOD_ENTITY_TRANSLATE:
			break;
		default:
			return qfalse;
		}
	}
	return (qboolean)( turbs == 0 || ( turbs == 1 && first && b->tcGen != TCGEN_ENVIRONMENT_MAPPED ) );
}

/*
===============
NW_Shader

Decides once per shader whether every stage maps onto the stage programs.
===============
*/
static nwShader_t *NW_Shader( const shader_t *sh )
{
	nwShader_t *ns = &nw_shaders[sh->index];
	if ( ns->state != NWS_UNKNOWN ) {
		return ns;
	}
	ns->state = NWS_NO;
	if ( sh->sky || sh->numDeforms || sh->entityMergable || sh->numUnfoggedPasses < 1 || !sh->stages
		|| sh == tr.distortionShader || sh == tr.shadowShader || sh == tr.projectionShadowShader ) {
		return ns;
	}
	byte styled = ( sh->lightmapIndex[0] == LIGHTMAP_BY_VERTEX && sh->styles[0] < LS_UNUSED && sh->styles[1] < LS_UNUSED );
	byte fogAdjust = 0;
	for ( int i = 0; i < sh->numUnfoggedPasses; i++ ) {
		const shaderStage_t *pStage = &sh->stages[i];
		if ( !pStage->active ) {
			break;
		}
		if ( pStage->ss && pStage->ss->surfaceSpriteType ) {
			return ns;
		}
		if ( !NW_BundleOk( &pStage->bundle[0], qtrue )
			|| ( pStage->bundle[1].image && !NW_BundleOk( &pStage->bundle[1], qfalse ) ) ) {
			return ns;
		}
		const int lm0 = NW_BundleLightmap( &pStage->bundle[0] );
		const int lm1 = pStage->bundle[1].image ? NW_BundleLightmap( &pStage->bundle[1] ) : -1;
		if ( lm0 >= 0 && lm1 >= 0 && lm0 != lm1 ) {
			return ns;
		}
		if ( lm0 > 0 || lm1 > 0 ) {
			styled = 1;
		}
		switch ( pStage->rgbGen ) {
		case CGEN_IDENTITY:
		case CGEN_IDENTITY_LIGHTING:
		case CGEN_CONST:
		case CGEN_ENTITY:
		case CGEN_ONE_MINUS_ENTITY:
		case CGEN_EXACT_VERTEX:
		case CGEN_VERTEX:
		case CGEN_ONE_MINUS_VERTEX:
		case CGEN_WAVEFORM:
		case CGEN_FOG:
		case CGEN_LIGHTMAPSTYLE:
			break;
		default:
			return ns;
		}
		switch ( pStage->alphaGen ) {
		case AGEN_SKIP:
		case AGEN_IDENTITY:
		case AGEN_CONST:
		case AGEN_WAVEFORM:
		case AGEN_ENTITY:
		case AGEN_ONE_MINUS_ENTITY:
		case AGEN_VERTEX:
		case AGEN_ONE_MINUS_VERTEX:
			break;
		default:
			return ns;
		}
		if ( pStage->adjustColorsForFog != ACFF_NONE ) {
			fogAdjust = 1;
		}
	}
	ns->styled = styled;
	ns->fogAdjust = fogAdjust;
	ns->state = NWS_YES;
	return ns;
}

// the dynamic light pass runs for this shader
static qboolean NW_Dlightable( const shader_t *sh )
{
	return (qboolean)( sh->sort <= SS_OPAQUE && !( sh->surfaceFlags & ( SURF_NODLIGHT | SURF_SKY ) ) );
}

// a resident surface's cached indexes, group and this frame's dynamic light bits; NULL when not resident
static const glIndex_t *NW_SurfaceIndexes( const surfaceType_t *surface, int *group, int *numIndexes, int *bits )
{
	switch ( *surface ) {
	case SF_FACE:
		{
			const srfSurfaceFace_t *f = (const srfSurfaceFace_t *)surface;
			*group = f->vboGroup;
			*numIndexes = f->numIndices;
			*bits = f->dlightBits[backEnd.smpFrame];
			return f->vboIndexes;
		}
	case SF_GRID:
		{
			const srfGridMesh_t *g = (const srfGridMesh_t *)surface;
			*group = g->vboGroup;
			*numIndexes = g->vboNumIndexes;
			*bits = g->dlightBits[backEnd.smpFrame];
			return g->vboIndexes;
		}
	case SF_TRIANGLES:
		{
			const srfTriangles_t *t = (const srfTriangles_t *)surface;
			*group = t->vboGroup;
			*numIndexes = t->numIndexes;
			*bits = t->dlightBits[backEnd.smpFrame];
			return t->vboIndexes;
		}
	default:
		return NULL;
	}
}

// ComputeFinalVertexColor for a white vertex of this shader: style colours summed for vertex-lit surfaces
static void NW_FinalWhite( const shader_t *sh, byte out[4] )
{
	out[0] = out[1] = out[2] = out[3] = 255;
	if ( sh->lightmapIndex[0] != LIGHTMAP_BY_VERTEX ) {
		return;
	}
	unsigned int sum[3] = { 0, 0, 0 };
	for ( int k = 0; k < MAXLIGHTMAPS && sh->styles[k] < LS_UNUSED; k++ ) {
		for ( int c = 0; c < 3; c++ ) {
			sum[c] += 255u * styleColors[sh->styles[k]][c];
		}
	}
	for ( int c = 0; c < 3; c++ ) {
		out[c] = (byte)( ( sum[c] >> 8 ) > 255 ? 255 : ( sum[c] >> 8 ) );
	}
}

// colour = vertex colour * scale + bias, fitted through the CPU generators at vertex colours 0 and f1
static void NW_StageColorFit( shaderStage_t *pStage, const byte f1[4], float scale[4], float bias[4] )
{
	static const byte f0[4] = { 0, 0, 0, 0 };
	byte c0[4], c1[4];
	RB_NativeStageColors( pStage, f0, f1, c0, c1 );
	for ( int i = 0; i < 4; i++ ) {
		scale[i] = ( (float)c1[i] - (float)c0[i] ) * ( 1.0f / 255.0f );
		bias[i] = (float)c0[i] * ( 1.0f / 255.0f );
	}
}

// one bundle's texcoord rows; returns the lightmap set it reads, -1 for none
static int NW_BundleRows( const textureBundle_t *b, int set, gxmWorldStage_t *st )
{
	float m[2][3], post[2][2], turb[2];
	if ( !RB_CalcTexMatrixSplit( b, m, post, turb ) ) {
		m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f;
		m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f;
		turb[0] = 0.0f;
	}
	float (*t)[4] = st->tex[set];
	const int lm = NW_BundleLightmap( b );
	for ( int r = 0; r < 2; r++ ) {
		switch ( b->tcGen ) {
		case TCGEN_TEXTURE:
			t[r][0] = m[r][0]; t[r][1] = m[r][1];
			t[r + 2][3] = m[r][2];
			break;
		case TCGEN_LIGHTMAP:
		case TCGEN_LIGHTMAP1:
		case TCGEN_LIGHTMAP2:
		case TCGEN_LIGHTMAP3:
			t[r][2] = m[r][0]; t[r][3] = m[r][1];
			t[r + 2][3] = m[r][2];
			break;
		case TCGEN_VECTOR:
			for ( int k = 0; k < 3; k++ ) {
				t[r + 2][k] = m[r][0] * b->tcGenVectors[0][k] + m[r][1] * b->tcGenVectors[1][k];
			}
			t[r + 2][3] = m[r][2];
			break;
		case TCGEN_ENVIRONMENT_MAPPED:
			if ( r_environmentMapping->integer ) {
				t[r][0] = m[r][0]; t[r][1] = m[r][1]; t[r][3] = m[r][2];	// st is the environment vector's xy
				st->tc0 = GXM_WORLD_TC_ENV;
				break;
			}
			t[r + 2][3] = m[r][2];
			break;
		default:
			t[r + 2][3] = m[r][2];
			break;
		}
	}
	if ( st->tc0 == GXM_WORLD_TC_ENV && set == 0 ) {
		VectorCopy( backEnd.ori.viewOrigin, st->viewer );
		st->viewer[3] = 1.0f;
	} else if ( set == 0 && turb[0] != 0.0f ) {
		st->tc0 = GXM_WORLD_TC_TURB;
		st->turb[0] = turb[0];
		st->turb[1] = turb[1];
		st->turbPost[0] = post[0][0]; st->turbPost[1] = post[0][1];
		st->turbPost[2] = post[1][0]; st->turbPost[3] = post[1][1];
	}
	return lm;
}

// everything a stage's programs read, evaluated once for this draw
static void NW_StageInputs( const shader_t *sh, const nwShader_t *ns, shaderStage_t *pStage, gxmWorldStage_t *st )
{
	memset( st, 0, sizeof( *st ) );

	byte white[4];
	NW_FinalWhite( sh, white );
	NW_StageColorFit( pStage, white, st->colorScale, st->colorBias );
	if ( ns->styled ) {
		static const byte full[4] = { 255, 255, 255, 255 };
		NW_StageColorFit( pStage, full, st->colorScaleStyled, st->colorBiasStyled );
		if ( sh->lightmapIndex[0] == LIGHTMAP_BY_VERTEX ) {
			for ( int k = 0; k < MAXLIGHTMAPS && sh->styles[k] < LS_UNUSED; k++ ) {
				for ( int c = 0; c < 3; c++ ) {
					st->style[k][c] = styleColors[sh->styles[k]][c] * ( 1.0f / 256.0f );
				}
			}
		} else {
			st->style[0][0] = st->style[0][1] = st->style[0][2] = 1.0f;
		}
		st->style[0][3] = 1.0f;
	} else {
		memcpy( st->colorScaleStyled, st->colorScale, sizeof( st->colorScale ) );
		memcpy( st->colorBiasStyled, st->colorBias, sizeof( st->colorBias ) );
	}

	st->nuv = pStage->bundle[1].image ? 2 : 1;
	int lm = NW_BundleRows( &pStage->bundle[0], 0, st );
	if ( st->nuv == 2 ) {
		const int lm1 = NW_BundleRows( &pStage->bundle[1], 1, st );
		if ( lm < 0 ) {
			lm = lm1;
		}
	}
	st->lmSelect[lm > 0 ? lm : 0] = 1.0f;
}

static void NW_DrawStages( shader_t *sh, const nwShader_t *ns, const gxmWorldSeg_t *segs, int numSegs, qboolean fogArmed )
{
	qboolean fogColorChanged = qfalse;
	const fog_t *fog = ( fogArmed && tr.world ) ? tr.world->fogs + nw_fog : NULL;

	for ( int stage = 0; stage < sh->numUnfoggedPasses; stage++ ) {
		shaderStage_t *pStage = &sh->stages[stage];
		if ( !pStage->active ) {
			break;
		}
		if ( pStage->ss && pStage->ss->surfaceSpriteType ) {
			continue;
		}
		if ( fog ) {
			if ( pStage->mGLFogColorOverride ) {
				GXM_SetFogColor( nw_fogOverrideColors[pStage->mGLFogColorOverride] );
				fogColorChanged = qtrue;
			} else if ( fogColorChanged ) {
				GXM_SetFogColor( fog->parms.color );
				fogColorChanged = qfalse;
			}
		}

		gxmWorldStage_t st;
		NW_StageInputs( sh, ns, pStage, &st );

		GL_SelectTexture( 0 );
		R_BindAnimatedImage( &pStage->bundle[0] );
		if ( pStage->bundle[1].image ) {
			GL_SelectTexture( 1 );
			GL_TexEnv( sh->multitextureEnv );
			R_BindAnimatedImage( &pStage->bundle[1] );
			GL_SelectTexture( 0 );
		}
		GL_State( pStage->stateBits );
		GXM_SetTexUnitCount( st.nuv );
		GXM_SetStateBits( glState.glStateBits );
		GXM_DrawWorldStage( &st, segs, numSegs );
	}
	if ( fogColorChanged ) {
		GXM_SetFogColor( fog->parms.color );
	}
	GXM_SetTexUnitCount( 1 );
}

// the stage the CPU light pass pairs with the light texture: textured, untransformed and opaque
static const shaderStage_t *NW_DlightStage( const shader_t *sh, const textureBundle_t **bundle )
{
	for ( int i = 0; i < sh->numUnfoggedPasses; i++ ) {
		const shaderStage_t *s = &sh->stages[i];
		if ( s->stateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) {
			continue;
		}
		for ( int b = 0; b < NUM_TEXTURE_BUNDLES; b++ ) {
			const textureBundle_t *tb = &s->bundle[b];
			if ( tb->image && !tb->isLightmap && !tb->numTexMods
				&& tb->tcGen != TCGEN_ENVIRONMENT_MAPPED && tb->tcGen != TCGEN_FOG ) {
				*bundle = tb;
				return s;
			}
		}
	}
	return NULL;
}

// one additive pass per light over the faces it reaches, as ProjectDlightTexture2 draws them
static void NW_DrawDlights( shader_t *sh )
{
	const textureBundle_t *dBundle = NULL;
	const shaderStage_t *dStage = NW_DlightStage( sh, &dBundle );
	static gxmWorldRun_t lightRuns[NW_MAX_RUNS];

	for ( int l = 0; l < backEnd.refdef.num_dlights; l++ ) {
		if ( !( nw_dlightBits & ( 1 << l ) ) ) {
			continue;
		}
		const dlight_t *dl = &backEnd.refdef.dlights[l];
		int numLightRuns = 0;
		for ( int r = 0; r < nw_numRuns; r++ ) {
			const srfSurfaceFace_t *f = nw_runFace[r];
			if ( !f || !( nw_runBits[r] & ( 1 << l ) ) ) {
				continue;
			}
			const float h = DotProduct( f->plane.normal, dl->transformed ) - f->plane.dist;
			if ( h <= 0.0f || h >= dl->radius ) {
				continue;
			}
			lightRuns[numLightRuns++] = nw_runs[r];
		}
		if ( !numLightRuns ) {
			continue;
		}
		gxmWorldSeg_t segs[NW_MAX_SEGS];
		const int numSegs = GXM_WorldGather( lightRuns, numLightRuns, 0, segs, NW_MAX_SEGS );
		if ( !numSegs ) {
			continue;
		}

		gxmWorldLight_t light;
		VectorCopy( dl->transformed, light.origin );
		light.radius = dl->radius;
		VectorCopy( dl->color, light.color );
		light.diffuse = dStage ? 1 : 0;

		GL_SelectTexture( 0 );
		if ( dStage ) {
			R_BindAnimatedImage( dBundle );
			GL_SelectTexture( 1 );
			GL_Bind( tr.dlightImage );
			GL_TexEnv( GL_MODULATE );
			GL_SelectTexture( 0 );
			GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
			GXM_SetTexUnitCount( 2 );
		} else {
			GL_Bind( tr.dlightImage );
			GL_State( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
			GXM_SetTexUnitCount( 1 );
		}
		GXM_SetStateBits( glState.glStateBits );
		GXM_DrawWorldLight( &light, segs, numSegs );
	}
	GXM_SetTexUnitCount( 1 );
}

// the fog volume pass with RB_CalcFogTexCoords' per-view vectors
static void NW_DrawFogPass( const shader_t *sh, const gxmWorldSeg_t *segs, int numSegs )
{
	gxmWorldFog_t wf;
	RB_CalcFogPassInputs( nw_fog, &wf );

	GL_SelectTexture( 0 );
	GL_Bind( tr.fogImage );
	if ( sh->fogPass == FP_EQUAL ) {
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL );
	} else {
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
	}
	GXM_SetTexUnitCount( 1 );
	GXM_SetStateBits( glState.glStateBits );
	GXM_DrawWorldFog( &wf, segs, numSegs );
}

/*
===============
NW_Flush

Draws the batch: the stages in order, then the dynamic lights and the fog volume pass, as RB_StageIteratorGeneric does.
===============
*/
static void NW_Flush( void )
{
	if ( !nw_numRuns ) {
		return;
	}
	shader_t *sh = nw_shader;
	const nwShader_t *ns = NW_Shader( sh );
	gxmWorldSeg_t segs[NW_MAX_SEGS];
	const int numSegs = GXM_WorldGather( nw_runs, nw_numRuns, sh->sort > SS_OPAQUE, segs, NW_MAX_SEGS );

	if ( numSegs ) {
		GL_Cull( sh->cullType );
		if ( sh->polygonOffset ) {
			GXM_SetDepthBias( r_offsetFactor->value, r_offsetUnits->value );
		}

		const qboolean fogArmed = RB_GxmVolumeFog( nw_fog, sh );
		NW_DrawStages( sh, ns, segs, numSegs, fogArmed );
		if ( fogArmed ) {
			RB_GxmVolumeFogOff();
		}

		if ( nw_dlightBits && NW_Dlightable( sh ) ) {
			NW_DrawDlights( sh );
		}
		if ( tr.world && ( nw_fog != tr.world->globalFog || r_drawfog->value != 2 ) && r_drawfog->value
			&& nw_fog && sh->fogPass ) {
			NW_DrawFogPass( sh, segs, numSegs );
		}

		if ( sh->polygonOffset ) {
			GXM_SetDepthBias( 0.0f, 0.0f );
		}
		GL_SelectTexture( 0 );
		backEnd.pc.c_wvboDraws += numSegs;
	}

	nw_numRuns = 0;
	nw_groupSpans = 0;
	nw_dlightBits = 0;
	nw_shader = NULL;
}

// debug views that only the tess path implements
static qboolean NW_DebugView( void )
{
	return (qboolean)( g_bRenderGlowingObjects || r_lightmap->integer || r_fullbright->integer || r_showtris->integer
		|| r_shownormals->integer || r_debugSort->integer || r_debugStyle->integer >= 0 );
}

/*
===============
NW_Surface

Queues a resident surface on the batch; false leaves it to tess.
===============
*/
static qboolean NW_Surface( surfaceType_t *surface, shader_t *shader, int fogNum )
{
	if ( NW_DebugView() ) {
		NW_Flush();
		return qfalse;
	}
	const trRefEntity_t *ent = backEnd.currentEntity;
	if ( ent != &tr.worldEntity && ( ent->e.renderfx & NW_ENTITY_FX ) ) {
		return qfalse;
	}
	int group = -1, numIndexes = 0, bits = 0;
	const glIndex_t *indexes = NW_SurfaceIndexes( surface, &group, &numIndexes, &bits );
	if ( !indexes || group < 0 || numIndexes <= 0 ) {
		return qfalse;
	}
	const nwShader_t *ns = NW_Shader( shader );
	if ( ns->state != NWS_YES || ( fogNum && ns->fogAdjust ) || ( ns->styled && !GXM_WorldGroupStyled( group ) ) ) {
		return qfalse;
	}
	if ( bits && NW_Dlightable( shader ) && ( *surface != SF_FACE || r_dlightStyle->integer <= 0 ) ) {
		return qfalse;
	}

	const int newSpan = ( !nw_numRuns || nw_runs[nw_numRuns - 1].group != group ) ? 1 : 0;
	if ( nw_numRuns && ( nw_shader != shader || nw_fog != fogNum || nw_entity != ent || nw_numRuns >= NW_MAX_RUNS
		|| nw_groupSpans + newSpan > NW_MAX_SEGS ) ) {
		NW_Flush();
	}
	nw_groupSpans += nw_numRuns ? newSpan : 1;
	// blended surfaces keep their submission order across the tess and resident paths
	if ( shader->sort > SS_OPAQUE && tess.numIndexes ) {
		RB_EndSurface();
		RB_BeginSurface( shader, fogNum );
	}

	nw_shader = shader;
	nw_fog = fogNum;
	nw_entity = ent;
	nw_runs[nw_numRuns].indexes = (const unsigned short *)indexes;
	nw_runs[nw_numRuns].numIndexes = numIndexes;
	nw_runs[nw_numRuns].group = group;
	nw_runFace[nw_numRuns] = ( *surface == SF_FACE ) ? (const srfSurfaceFace_t *)surface : NULL;
	nw_runBits[nw_numRuns] = bits;
	nw_numRuns++;
	nw_dlightBits |= bits;
	backEnd.pc.c_wvboSurfaces++;
	return qtrue;
}

static void NW_Free( void )
{
	if ( nw_active ) {
		GXM_Sync();
		GXM_WorldFree();
	}
	nw_active = qfalse;
	nw_numRuns = 0;
	nw_groupSpans = 0;
	nw_dlightBits = 0;
	nw_shader = NULL;
}

// GL_TexEnv's mode as the fragment program's env index
static int NW_TexEnvIndex( int glEnv )
{
	if ( glEnv == GL_ADD ) {
		return GXM_TEXENV_ADD;
	}
	if ( glEnv == GL_REPLACE ) {
		return GXM_TEXENV_REPLACE;
	}
	return GXM_TEXENV_MODULATE;
}

/*
===============
NW_Build

Uploads every surface whose shader the stage programs express, then creates the fragment instances the map's stages need.
===============
*/
static qboolean NW_Build( world_t &worldData )
{
	memset( nw_shaders, 0, sizeof( nw_shaders ) );
	if ( !r_nativeWorld || !r_nativeWorld->integer || !GXM_WorldReady() || worldData.numsurfaces <= 0 ) {
		return qfalse;
	}

	const int count = worldData.numsurfaces;
	gxmWorldSurfInfo_t *info = (gxmWorldSurfInfo_t *)R_Malloc( count * sizeof( gxmWorldSurfInfo_t ), TAG_TEMP_WORKSPACE, qtrue );
	gxmWorldSlot_t *slots = (gxmWorldSlot_t *)R_Malloc( count * sizeof( gxmWorldSlot_t ), TAG_TEMP_WORKSPACE, qtrue );
	qboolean fogVolumes = qfalse, dlightFaces = qfalse;

	for ( int i = 0; i < count; i++ ) {
		const msurface_t *surf = &worldData.surfaces[i];
		if ( !surf->data || !surf->shader ) {
			continue;
		}
		nwShader_t *ns = NW_Shader( surf->shader );
		if ( ns->state != NWS_YES ) {
			continue;
		}
		switch ( *surf->data ) {
		case SF_FACE:
			{
				const srfSurfaceFace_t *f = (const srfSurfaceFace_t *)surf->data;
				info[i].numVerts = f->numPoints;
				info[i].numIndexes = f->numIndices;
				dlightFaces = (qboolean)( dlightFaces || NW_Dlightable( surf->shader ) );
			}
			break;
		case SF_GRID:
			{
				const srfGridMesh_t *g = (const srfGridMesh_t *)surf->data;
				if ( g->width >= 2 && g->height >= 2 ) {
					info[i].numVerts = g->width * g->height;
					info[i].numIndexes = ( g->width - 1 ) * ( g->height - 1 ) * 6;
				}
			}
			break;
		case SF_TRIANGLES:
			{
				const srfTriangles_t *t = (const srfTriangles_t *)surf->data;
				info[i].numVerts = t->numVerts;
				info[i].numIndexes = t->numIndexes;
			}
			break;
		default:
			break;
		}
		if ( !info[i].numVerts ) {
			continue;
		}
		info[i].styled = ns->styled;
		info[i].key = surf->shader->index;

		if ( surf->fogIndex && surf->fogIndex == worldData.globalFog && surf->shader->fogPass ) {
			ns->fogMask |= ( 1 << 1 ) | ( 1 << 2 );
		} else {
			ns->fogMask |= ( 1 << 0 );
			if ( surf->fogIndex && surf->shader->fogPass ) {
				fogVolumes = qtrue;
			}
		}
	}

	if ( !GXM_WorldLayout( info, count, slots ) ) {
		R_Free( slots );
		R_Free( info );
		return qfalse;
	}

	for ( int i = 0; i < count; i++ ) {
		const gxmWorldSlot_t *slot = &slots[i];
		if ( slot->group < 0 ) {
			continue;
		}
		surfaceType_t *data = worldData.surfaces[i].data;
		switch ( *data ) {
		case SF_FACE:
			{
				srfSurfaceFace_t *f = (srfSurfaceFace_t *)data;
				GXM_WorldPackPoints( slot, info[i].styled, f->points[0], f->numPoints, f->plane.normal,
					(const int *)( (byte *)f + f->ofsIndices ), f->numIndices );
				f->vboGroup = slot->group;
				f->vboIndexes = (glIndex_t *)slot->indexes;
			}
			break;
		case SF_GRID:
			{
				srfGridMesh_t *g = (srfGridMesh_t *)data;
				GXM_WorldPackGrid( slot, info[i].styled, (const gxmWorldDrawVert_t *)g->verts, g->width, g->height );
				g->vboGroup = slot->group;
				g->vboNumIndexes = info[i].numIndexes;
				g->vboIndexes = (glIndex_t *)slot->indexes;
			}
			break;
		case SF_TRIANGLES:
			{
				srfTriangles_t *t = (srfTriangles_t *)data;
				GXM_WorldPackVerts( slot, info[i].styled, (const gxmWorldDrawVert_t *)t->verts, t->numVerts,
					t->indexes, t->numIndexes );
				t->vboGroup = slot->group;
				t->vboIndexes = (glIndex_t *)slot->indexes;
			}
			break;
		default:
			break;
		}
	}
	R_Free( slots );
	R_Free( info );

	for ( int s = 0; s < tr.numShaders; s++ ) {
		const shader_t *sh = tr.shaders[s];
		const nwShader_t *ns = &nw_shaders[sh->index];
		if ( ns->state != NWS_YES || !ns->fogMask ) {
			continue;
		}
		for ( int i = 0; i < sh->numUnfoggedPasses && sh->stages[i].active; i++ ) {
			const shaderStage_t *pStage = &sh->stages[i];
			GXM_WorldPrepare( pStage->stateBits, pStage->bundle[1].image ? 2 : 1,
				NW_TexEnvIndex( sh->multitextureEnv ), ns->fogMask );
		}
	}
	if ( fogVolumes ) {
		GXM_WorldPrepare( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA, 1, 0, 1 );
	}
	if ( dlightFaces && r_dynamiclight->integer && r_dlightStyle->integer > 0 ) {
		GXM_WorldPrepare( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL, 2, GXM_TEXENV_MODULATE, 1 );
		GXM_WorldPrepare( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL, 1, 0, 1 );
	}

	nw_active = qtrue;
	return qtrue;
}
#endif

/*
===============
R_WorldVBO_Clear
===============
*/
void R_WorldVBO_Clear( void )
{
#ifdef USE_GXM_NATIVE
	NW_Free();
#endif
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
#ifdef USE_GXM_NATIVE
	if ( r_nativeWorld && r_nativeWorld->integer ) {
		R_IssuePendingRenderCommands();
		R_WorldVBO_Clear();
		NW_Build( worldData );
		return;
	}
#endif
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
R_WorldSurface

Routes a draw surface to the resident world or the static world VBO; false sends it to tess.
===============
*/
qboolean R_WorldSurface( surfaceType_t *surface, shader_t *shader, int fogNum, int dlighted )
{
#ifdef USE_GXM_NATIVE
	if ( nw_active ) {
		if ( NW_Surface( surface, shader, fogNum ) ) {
			return qtrue;
		}
		// blended surfaces keep their submission order across the tess and resident paths
		if ( nw_numRuns && shader->sort > SS_OPAQUE ) {
			NW_Flush();
		}
		return qfalse;
	}
#endif
	if ( *surface == SF_FACE ) {
		return R_WorldVBO_Surface( (const srfSurfaceFace_t *)surface, shader, fogNum, dlighted );
	}
	return qfalse;
}

/*
===============
R_WorldVBO_Flush

Draws whatever the current batch accumulated.
===============
*/
// A bounded volume's density per vertex, layered over the batch like RB_FogPass.
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
#ifdef USE_GXM_NATIVE
	NW_Flush();
#endif
	if ( !wvbo_numIdx || wvbo_curGroup < 0 || wvbo_curGroup >= wvbo_numGroups ) {
		wvbo_numIdx = 0;
		wvbo_curFog = 0;
		wvbo_curGroup = -1;
		return;
	}

	const shaderStage_t *st = &shader->stages[0];

	wvbo_statTris += wvbo_numIdx / 3;
	GL_Cull( shader->cullType );

#ifdef USE_GXM_NATIVE
	// resident geometry misses the tess path, so the global fog uniform is armed here
	const qboolean fogArmed = RB_GxmVolumeFog( wvbo_curFog, shader );

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

	if ( fogArmed ) {
		RB_GxmVolumeFogOff();
	}

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
