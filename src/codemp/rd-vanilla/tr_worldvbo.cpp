/*
===========================================================================
Copyright (C) 2026, JK2VITA contributors

This file is part of JAVITA, a PS Vita port built on the OpenJK
source code.

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

// tr_worldvbo.cpp -- static world VBO (r_worldVBO): eligible opaque surfaces bake into a resident buffer, the rest draw via tess

#include "tr_local.h"
#ifdef USE_GXM_NATIVE
#include "rd-gxm/gxm_device.h"
#endif

void R_BindAnimatedImage( textureBundle_t *bundle );

#ifdef VITA

// interleaved vertex: xyz f3 @0, diffuse st f2 @12, lightmap st f2 @20, rgba u8 @28 -> 32 bytes.
typedef struct {
	float	xyz[3];
	float	st0[2];
	float	st1[2];
	byte	rgba[4];
} wvboVert_t;
// the backend's vertex program reads this exact layout
static_assert( sizeof(wvboVert_t) == 32 && offsetof(wvboVert_t, st0) == 12
	&& offsetof(wvboVert_t, st1) == 20 && offsetof(wvboVert_t, rgba) == 28,
	"wvboVert_t must match the gxm vertex stream" );
#define WVBO_STRIDE		((GLsizei)sizeof(wvboVert_t))
#define WVBO_OFS_XYZ	((const GLvoid *)0)
#define WVBO_OFS_ST0	((const GLvoid *)12)
#define WVBO_OFS_ST1	((const GLvoid *)20)
#define WVBO_OFS_RGBA	((const GLvoid *)28)

#ifdef USE_GXM_NATIVE
// one u16 index space covers the whole bake (gxm needs u16 indices below 64000); surfaces past it stay on tess
#define WVBO_MAX_VERTS	64000
#endif

typedef struct {
	const void	*surfData;	// == msurface_t.data; render-time lookup key
	shader_t	*shader;
	int			firstIndex;	// into the baked index array
	int			numIndexes;
} wvboSurf_t;

// kept outside tr/world_t, which are memset every map load
static struct {
#ifdef USE_GXM_NATIVE
	void		*data;		// GPU-mapped vertex buffer, read straight by the backend
	SceUID		uid;
#else
	GLuint		vbo;
#endif
	glIndex_t	*idx;		// malloc; baked indices stay CPU-side, staged per shader run
	wvboSurf_t	*surfs;		// malloc
	int			numSurfs;
	int			*hash;		// malloc; surfData -> surfs index, -1 empty
	int			hashSize;	// power of two
	qboolean	ready;
} s_wvbo;

// backend-thread batch state; a run stages indices and flushes as one draw
#define WVBO_STAGE_MAX 32768
static glIndex_t	s_wvboStage[WVBO_STAGE_MAX];
static int			s_wvboStaged;
static qboolean		s_wvboBatch;
static shader_t		*s_wvboBatchShader;
static int			s_wvboBatchFog;			// part of the batch key: one fog per run
static qboolean		s_wvboBatchFogArmed;	// this batch armed the global-fog uniform

static void WorldVbo_Flush( void )
{
	if ( !s_wvboStaged ) {
		return;
	}
#ifdef USE_GXM_NATIVE
	// colours are baked per vertex, so the stream colour is always live
	GXM_DrawStaticBuffer( s_wvbo.data, s_wvboStage, s_wvboStaged, 1 );
#else
	qglDrawElements( GL_TRIANGLES, s_wvboStaged, GL_INDEX_TYPE, s_wvboStage );
#endif
	backEnd.pc.c_wvboDraws++;
	s_wvboStaged = 0;
}

extern bool g_bRenderGlowingObjects;	// glow prepass draws per-stage subsets; VBO can't

#ifdef USE_GXM_NATIVE
// --- resident world (r_nativeWorld): surfaces uploaded once per map, every stage, light and fog pass drawn by GPU programs ---

extern int	skyboxportal;
extern int	drawskyboxportal;

static_assert( VERTEXSIZE == GXM_WORLD_POINT_FLOATS && VERTEX_LM == GXM_WORLD_POINT_LM
	&& VERTEX_COLOR == GXM_WORLD_POINT_COLOR, "face points must match the shared packer" );
static_assert( sizeof(drawVert_t) == sizeof(gxmWorldDrawVert_t), "drawVert_t must match the shared packer" );

// entity effects that change a stage beyond what its uniforms carry
#define NW_ENTITY_FX	( RF_DISINTEGRATE1 | RF_DISINTEGRATE2 | RF_VOLUMETRIC | RF_DISTORTION \
						| RF_FORCE_ENT_ALPHA | RF_RGB_TINT | RF_FIRST_PERSON | RF_FORCEPOST )

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
			*bits = f->dlightBits;
			return f->vboIndexes;
		}
	case SF_GRID:
		{
			const srfGridMesh_t *g = (const srfGridMesh_t *)surface;
			*group = g->vboGroup;
			*numIndexes = g->vboNumIndexes;
			*bits = g->dlightBits;
			return g->vboIndexes;
		}
	case SF_TRIANGLES:
		{
			const srfTriangles_t *t = (const srfTriangles_t *)surface;
			*group = t->vboGroup;
			*numIndexes = t->numIndexes;
			*bits = t->dlightBits;
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
				t[r][1] = m[r][0]; t[r][2] = -m[r][1];	// st is ( 0.5 + e.y, 0.5 - e.z )
				t[r][3] = 0.5f * ( m[r][0] + m[r][1] ) + m[r][2];
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
static shaderStage_t *NW_DlightStage( shader_t *sh, textureBundle_t **bundle )
{
	for ( int i = 0; i < sh->numUnfoggedPasses; i++ ) {
		shaderStage_t *s = &sh->stages[i];
		if ( s->stateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) {
			continue;
		}
		for ( int b = 0; b < NUM_TEXTURE_BUNDLES; b++ ) {
			textureBundle_t *tb = &s->bundle[b];
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
	textureBundle_t *dBundle = NULL;
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
			if ( dl->additive ) {
				GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
			} else {
				GL_State( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
			}
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

// debug views and portal-sky passes that only the tess path implements
static qboolean NW_TessOnlyView( void )
{
	return (qboolean)( g_bRenderGlowingObjects || r_lightmap->integer || r_fullbright->integer || r_showtris->integer
		|| r_shownormals->integer || r_debugSort->integer
		|| ( skyboxportal && ( backEnd.refdef.rdflags & RDF_SKYBOXPORTAL ) && !drawskyboxportal ) );
}

/*
===============
NW_Surface

Queues a resident surface on the batch; false leaves it to tess.
===============
*/
static qboolean NW_Surface( surfaceType_t *surface, shader_t *shader, int fogNum )
{
	if ( NW_TessOnlyView() ) {
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
static qboolean NW_Build( world_t *w )
{
	memset( nw_shaders, 0, sizeof( nw_shaders ) );
	if ( !r_nativeWorld || !r_nativeWorld->integer || !GXM_WorldReady() || !w || w->numsurfaces <= 0 ) {
		return qfalse;
	}

	const int count = w->numsurfaces;
	gxmWorldSurfInfo_t *info = (gxmWorldSurfInfo_t *)calloc( count, sizeof( gxmWorldSurfInfo_t ) );
	gxmWorldSlot_t *slots = (gxmWorldSlot_t *)calloc( count, sizeof( gxmWorldSlot_t ) );
	if ( !info || !slots ) {
		free( info );
		free( slots );
		return qfalse;
	}
	qboolean fogVolumes = qfalse, dlightFaces = qfalse;

	for ( int i = 0; i < count; i++ ) {
		const msurface_t *surf = &w->surfaces[i];
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

		if ( surf->fogIndex && surf->fogIndex == w->globalFog && surf->shader->fogPass ) {
			ns->fogMask |= ( 1 << 1 ) | ( 1 << 2 );
		} else {
			ns->fogMask |= ( 1 << 0 );
			if ( surf->fogIndex && surf->shader->fogPass ) {
				fogVolumes = qtrue;
			}
		}
	}

	if ( !GXM_WorldLayout( info, count, slots ) ) {
		free( slots );
		free( info );
		return qfalse;
	}

	for ( int i = 0; i < count; i++ ) {
		const gxmWorldSlot_t *slot = &slots[i];
		if ( slot->group < 0 ) {
			continue;
		}
		surfaceType_t *data = w->surfaces[i].data;
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
	free( slots );
	free( info );

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
		GXM_WorldPrepare( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL, 1, 0, 1 );
	}

	nw_active = qtrue;
	return qtrue;
}
#endif

static qboolean WorldVbo_Eligible( const shader_t *sh, int surfaceType )
{
	if ( surfaceType != SF_FACE && surfaceType != SF_GRID ) return qfalse;
	if ( !sh || sh->defaultShader || !sh->stages ) return qfalse;
	if ( sh->sky || ( sh->surfaceFlags & SURF_SKY ) ) return qfalse;
	if ( sh->polygonOffset || sh->numDeforms != 0 ) return qfalse;
	if ( sh->sort != SS_OPAQUE || sh->numUnfoggedPasses != 1 ) return qfalse;
	// only unstyled (style 0) surfaces bake; lightstyles animate per frame
	if ( sh->styles[0] != LS_NORMAL && sh->styles[0] < LS_UNUSED ) return qfalse;
	if ( sh->styles[1] < LS_UNUSED ) return qfalse;

	const shaderStage_t *st = &sh->stages[0];
	if ( !st->active ) return qfalse;
	if ( st->ss && st->ss->surfaceSpriteType ) return qfalse;
	if ( st->stateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS | GLS_ATEST_BITS ) ) return qfalse;

	switch ( st->rgbGen ) {
	case CGEN_IDENTITY: case CGEN_IDENTITY_LIGHTING: case CGEN_CONST:
		break;
	case CGEN_VERTEX: case CGEN_EXACT_VERTEX:
		if ( sh->lightmapIndex[0] == LIGHTMAP_BY_VERTEX ) return qfalse;	// per-frame styleColors
		break;
	default:
		return qfalse;	// waveform/entity/lightingDiffuse/fog/lightmapstyle are per-frame
	}
	switch ( st->alphaGen ) {
	case AGEN_IDENTITY: case AGEN_SKIP: case AGEN_CONST: case AGEN_VERTEX:
		break;
	default:
		return qfalse;
	}
	// only the st0=diffuse, st1=lightmap layout bakes
	const textureBundle_t *b0 = &st->bundle[0];
	if ( !b0->image || b0->isVideoMap || b0->numImageAnimations > 1 ) return qfalse;
	if ( b0->tcGen != TCGEN_TEXTURE || b0->isLightmap ) return qfalse;
	if ( b0->numTexMods != 0 ) return qfalse;	// scroll/turb/etc are per-frame
	const textureBundle_t *b1 = &st->bundle[1];
	if ( b1->image ) {
		if ( b1->isVideoMap || b1->numImageAnimations > 1 ) return qfalse;
		if ( b1->tcGen != TCGEN_LIGHTMAP || !b1->isLightmap ) return qfalse;
		if ( b1->numTexMods != 0 ) return qfalse;
	}
	return qtrue;
}

// ComputeColors for the eligible cgens; alpha bakes as 255 (eligible surfaces are opaque)
static void WorldVbo_BakeColor( const shaderStage_t *st, const byte *raw, byte *out )
{
	switch ( st->rgbGen ) {
	default:
	case CGEN_IDENTITY:
		out[0] = out[1] = out[2] = 255;
		break;
	case CGEN_IDENTITY_LIGHTING:
		out[0] = out[1] = out[2] = tr.identityLightByte;
		break;
	case CGEN_CONST:
		out[0] = st->constantColor[0]; out[1] = st->constantColor[1]; out[2] = st->constantColor[2];
		break;
	case CGEN_EXACT_VERTEX:
		out[0] = raw[0]; out[1] = raw[1]; out[2] = raw[2];
		break;
	case CGEN_VERTEX:
		if ( tr.identityLight == 1.0f ) {
			out[0] = raw[0]; out[1] = raw[1]; out[2] = raw[2];
		} else {
			out[0] = (byte)( raw[0] * tr.identityLight );
			out[1] = (byte)( raw[1] * tr.identityLight );
			out[2] = (byte)( raw[2] * tr.identityLight );
		}
		break;
	}
	out[3] = 255;
}

void R_FreeWorldVBO( void )
{
#ifdef USE_GXM_NATIVE
	NW_Free();
	if ( s_wvbo.data ) {
		// a queued scene may still reference the buffer, so wait the GPU out first
		GXM_Sync();
		GXM_Free( s_wvbo.uid );
		s_wvbo.data = NULL;
		s_wvbo.uid = 0;
	}
#else
	if ( s_wvbo.vbo ) {
		// re-point the vertex arrays off the buffer before deleting it
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		qglVertexPointer( 3, GL_FLOAT, 16, NULL );
		qglColorPointer( 4, GL_UNSIGNED_BYTE, 0, NULL );
		GL_SelectTexture( 1 );
		qglTexCoordPointer( 2, GL_FLOAT, 0, NULL );
		GL_SelectTexture( 0 );
		qglTexCoordPointer( 2, GL_FLOAT, 0, NULL );
	}
	if ( s_wvbo.vbo ) { glDeleteBuffers( 1, &s_wvbo.vbo ); s_wvbo.vbo = 0; }
#endif
	if ( s_wvbo.idx ) { free( s_wvbo.idx ); s_wvbo.idx = NULL; }
	if ( s_wvbo.surfs ) { free( s_wvbo.surfs ); s_wvbo.surfs = NULL; }
	if ( s_wvbo.hash )  { free( s_wvbo.hash );  s_wvbo.hash  = NULL; }
	s_wvbo.numSurfs = 0;
	s_wvbo.hashSize = 0;
	s_wvbo.ready = qfalse;
	s_wvboStaged = 0;
	s_wvboBatch = qfalse;
	s_wvboBatchShader = NULL;
	s_wvboBatchFog = 0;
	s_wvboBatchFogArmed = qfalse;
}

// count and fill passes select the same surfaces; runs on main with the render thread parked
void R_BuildWorldVBO( world_t *w )
{
	R_FreeWorldVBO();
#ifdef USE_GXM_NATIVE
	if ( r_nativeWorld && r_nativeWorld->integer ) {
		NW_Build( w );
		return;
	}
#endif
	if ( !r_worldVBO || !r_worldVBO->integer || !w || w->numsurfaces <= 0 ) return;

	// world model (bmodels[0]) only; inline bmodels render as entities
	const int surfFirst = w->bmodels ? (int)( w->bmodels[0].firstSurface - w->surfaces ) : 0;
	const int surfLast  = w->bmodels ? surfFirst + w->bmodels[0].numSurfaces : w->numsurfaces;

	int totalVerts = 0, totalIdx = 0, numEl = 0;
#ifdef USE_GXM_NATIVE
	int skippedU16 = 0;		// eligible surfaces the u16 index space has no room for
#endif
	for ( int i = surfFirst; i < surfLast; i++ ) {
		msurface_t *s = &w->surfaces[i];
		// a bounded fog volume needs the tess fog pass; the global fog is a uniform
		if ( ( s->fogIndex != 0 && s->fogIndex != w->globalFog ) || !s->data ) continue;
		surfaceType_t t = *(surfaceType_t *)s->data;
		if ( !WorldVbo_Eligible( s->shader, t ) ) continue;
		if ( t == SF_FACE ) {
			srfSurfaceFace_t *f = (srfSurfaceFace_t *)s->data;
			if ( f->numPoints <= 0 || f->numIndices <= 0 ) continue;
#ifdef USE_GXM_NATIVE
			if ( totalVerts + f->numPoints > WVBO_MAX_VERTS ) { skippedU16++; continue; }
#endif
			totalVerts += f->numPoints;
			totalIdx   += f->numIndices;
		} else {
			srfGridMesh_t *g = (srfGridMesh_t *)s->data;
			if ( g->width < 2 || g->height < 2 ) continue;
#ifdef USE_GXM_NATIVE
			if ( totalVerts + g->width * g->height > WVBO_MAX_VERTS ) { skippedU16++; continue; }
#endif
			totalVerts += g->width * g->height;
			totalIdx   += ( g->width - 1 ) * ( g->height - 1 ) * 6;
		}
		numEl++;
	}
	if ( numEl == 0 || totalVerts == 0 || totalIdx == 0 ) return;

	wvboVert_t *verts = (wvboVert_t *)malloc( (size_t)totalVerts * sizeof(wvboVert_t) );
	glIndex_t  *idx   = (glIndex_t *)malloc( (size_t)totalIdx * sizeof(glIndex_t) );
	s_wvbo.surfs      = (wvboSurf_t *)malloc( (size_t)numEl * sizeof(wvboSurf_t) );
	if ( !verts || !idx || !s_wvbo.surfs ) {
		free( verts ); free( idx ); free( s_wvbo.surfs ); s_wvbo.surfs = NULL;
		return;
	}

	int vCount = 0, iCount = 0, rCount = 0;
	for ( int i = surfFirst; i < surfLast; i++ ) {
		msurface_t *s = &w->surfaces[i];
		// a bounded fog volume needs the tess fog pass; the global fog is a uniform
		if ( ( s->fogIndex != 0 && s->fogIndex != w->globalFog ) || !s->data ) continue;
		surfaceType_t t = *(surfaceType_t *)s->data;
		if ( !WorldVbo_Eligible( s->shader, t ) ) continue;

		const shaderStage_t *st = &s->shader->stages[0];
		int baseVertex = vCount;
		int firstIndex = iCount;

		if ( t == SF_FACE ) {
			srfSurfaceFace_t *f = (srfSurfaceFace_t *)s->data;
			if ( f->numPoints <= 0 || f->numIndices <= 0 ) continue;
#ifdef USE_GXM_NATIVE
			if ( vCount + f->numPoints > WVBO_MAX_VERTS ) continue;	// must reject what the count pass rejected
#endif
			const float *pv = f->points[0];
			for ( int p = 0; p < f->numPoints; p++, pv += VERTEXSIZE ) {
				wvboVert_t *o = &verts[vCount++];
				o->xyz[0] = pv[0]; o->xyz[1] = pv[1]; o->xyz[2] = pv[2];
				o->st0[0] = pv[3]; o->st0[1] = pv[4];
				o->st1[0] = pv[VERTEX_LM]; o->st1[1] = pv[VERTEX_LM + 1];
				WorldVbo_BakeColor( st, (const byte *)&pv[VERTEX_COLOR], o->rgba );
			}
			const int *si = (const int *)( (const byte *)f + f->ofsIndices );
			for ( int k = 0; k < f->numIndices; k++ ) idx[iCount++] = (glIndex_t)( si[k] + baseVertex );
		} else {
			srfGridMesh_t *g = (srfGridMesh_t *)s->data;
			if ( g->width < 2 || g->height < 2 ) continue;
#ifdef USE_GXM_NATIVE
			if ( vCount + g->width * g->height > WVBO_MAX_VERTS ) continue;	// must reject what the count pass rejected
#endif
			const int W = g->width, H = g->height;
			for ( int n = 0; n < W * H; n++ ) {
				const drawVert_t *dv = &g->verts[n];
				wvboVert_t *o = &verts[vCount++];
				o->xyz[0] = dv->xyz[0]; o->xyz[1] = dv->xyz[1]; o->xyz[2] = dv->xyz[2];
				o->st0[0] = dv->st[0]; o->st0[1] = dv->st[1];
				o->st1[0] = dv->lightmap[0][0]; o->st1[1] = dv->lightmap[0][1];
				WorldVbo_BakeColor( st, dv->color[0], o->rgba );
			}
			for ( int r = 0; r < H - 1; r++ ) {
				for ( int c = 0; c < W - 1; c++ ) {
					int v1 = baseVertex + r * W + c + 1;
					int v2 = v1 - 1;
					int v3 = v2 + W;
					int v4 = v3 + 1;
					idx[iCount++] = v2; idx[iCount++] = v3; idx[iCount++] = v1;
					idx[iCount++] = v1; idx[iCount++] = v3; idx[iCount++] = v4;
				}
			}
		}

		wvboSurf_t *rec = &s_wvbo.surfs[rCount++];
		rec->surfData   = s->data;
		rec->shader     = s->shader;
		rec->firstIndex = firstIndex;
		rec->numIndexes = iCount - firstIndex;
	}
	s_wvbo.numSurfs = rCount;

	int hs = 16;
	while ( hs < rCount * 2 ) hs <<= 1;
	s_wvbo.hash = (int *)malloc( (size_t)hs * sizeof(int) );
	if ( !s_wvbo.hash ) {	// low-memory map load: bail rather than deref NULL
		free( verts ); free( idx );
		R_FreeWorldVBO();
		return;
	}
	s_wvbo.hashSize = hs;
	for ( int h = 0; h < hs; h++ ) s_wvbo.hash[h] = -1;
	for ( int r = 0; r < rCount; r++ ) {
		unsigned h = (unsigned)( ( (uintptr_t)s_wvbo.surfs[r].surfData >> 4 ) & ( hs - 1 ) );
		while ( s_wvbo.hash[h] != -1 ) h = ( h + 1 ) & ( hs - 1 );
		s_wvbo.hash[h] = r;
	}

	// the GPU reads the vertices in place, so they live in a GXM memblock
	s_wvbo.data = GXM_Alloc( SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
		(unsigned int)( vCount * sizeof(wvboVert_t) ), 4, SCE_GXM_MEMORY_ATTRIB_READ, &s_wvbo.uid );
	if ( !s_wvbo.data ) {
		free( verts ); free( idx );
		R_FreeWorldVBO();
		return;
	}
	memcpy( s_wvbo.data, verts, (size_t)vCount * sizeof(wvboVert_t) );

	free( verts );
	s_wvbo.idx = idx;	// indices stay CPU-side; staged per shader run at draw time
	s_wvbo.ready = qtrue;
	ri.Printf( PRINT_ALL, "r_worldVBO: baked %d surfaces (%d verts, %d indices, %d KB VBO)\n",
		rCount, vCount, iCount, (int)( ( vCount * sizeof(wvboVert_t) ) >> 10 ) );
	if ( skippedU16 ) {
		ri.Printf( PRINT_ALL, "r_worldVBO: %d surfaces past the %d-vertex u16 limit fall back to tess\n",
			skippedU16, WVBO_MAX_VERTS );
	}
}

static wvboSurf_t *WorldVbo_Lookup( const void *surfData )
{
	if ( !s_wvbo.ready || !s_wvbo.hash ) return NULL;
	unsigned h = (unsigned)( ( (uintptr_t)surfData >> 4 ) & ( s_wvbo.hashSize - 1 ) );
	for ( int probe = 0; probe < s_wvbo.hashSize; probe++ ) {
		int r = s_wvbo.hash[h];
		if ( r == -1 ) return NULL;
		if ( s_wvbo.surfs[r].surfData == surfData ) return &s_wvbo.surfs[r];
		h = ( h + 1 ) & ( s_wvbo.hashSize - 1 );
	}
	return NULL;
}

// DrawMultitextured's state with the vertex VBO bound at byte offsets; indices stay client-side
static void WorldVbo_SetupBatch( shader_t *sh, int fogNum )
{
	const shaderStage_t *st = &sh->stages[0];

	WorldVbo_Flush();	// stale staged indices must not draw with the new shader's state
	GL_Cull( sh->cullType );
#ifdef USE_GXM_NATIVE
	if ( s_wvboBatchFogArmed ) {	// the previous batch's fog must not carry into this one
		RB_GxmVolumeFogOff();
		s_wvboBatchFogArmed = qfalse;
	}
	// the backend reads the interleaved buffer directly, so only state and bindings move
	GL_State( st->stateBits );

	GL_SelectTexture( 0 );
	R_BindAnimatedImage( &st->bundle[0] );
	if ( st->bundle[1].image ) {
		GL_SelectTexture( 1 );
		GL_TexEnv( sh->multitextureEnv );
		R_BindAnimatedImage( &st->bundle[1] );
		GL_SelectTexture( 0 );
	}

	GXM_SetConstantColor( 1.0f, 1.0f, 1.0f, 1.0f );
	GXM_SetTexUnitCount( st->bundle[1].image ? 2 : 1 );
	GXM_SetStateBits( glState.glStateBits );
	s_wvboBatchFogArmed = RB_GxmVolumeFog( fogNum, sh );
#else
	glBindBuffer( GL_ARRAY_BUFFER, s_wvbo.vbo );

	qglEnableClientState( GL_VERTEX_ARRAY );
	qglVertexPointer( 3, GL_FLOAT, WVBO_STRIDE, WVBO_OFS_XYZ );
	qglEnableClientState( GL_COLOR_ARRAY );
	qglColorPointer( 4, GL_UNSIGNED_BYTE, WVBO_STRIDE, WVBO_OFS_RGBA );

	GL_State( st->stateBits );

	GL_SelectTexture( 0 );
	qglEnableClientState( GL_TEXTURE_COORD_ARRAY );
	qglTexCoordPointer( 2, GL_FLOAT, WVBO_STRIDE, WVBO_OFS_ST0 );
	R_BindAnimatedImage( &st->bundle[0] );

	if ( st->bundle[1].image ) {
		GL_SelectTexture( 1 );
		qglEnable( GL_TEXTURE_2D );
		qglEnableClientState( GL_TEXTURE_COORD_ARRAY );
		qglTexCoordPointer( 2, GL_FLOAT, WVBO_STRIDE, WVBO_OFS_ST1 );
		GL_TexEnv( sh->multitextureEnv );
		R_BindAnimatedImage( &st->bundle[1] );
	}
#endif

	s_wvboBatch = qtrue;
	s_wvboBatchShader = sh;
	s_wvboBatchFog = fogNum;
}

// ends the batch: flushes staged indices, disables tmu1 and unbinds the buffer for tess client arrays
void RB_EndWorldVBO( void )
{
#ifdef USE_GXM_NATIVE
	NW_Flush();
#endif
	if ( !s_wvboBatch ) return;
	WorldVbo_Flush();
#ifdef USE_GXM_NATIVE
	// the next tess draw picks its own unit count, but leave the shadow single-textured
	GXM_SetTexUnitCount( 1 );
	GL_SelectTexture( 0 );
	if ( s_wvboBatchFogArmed ) {
		RB_GxmVolumeFogOff();
	}
#else
	if ( s_wvboBatchShader && s_wvboBatchShader->stages[0].bundle[1].image ) {
		GL_SelectTexture( 1 );
		qglDisable( GL_TEXTURE_2D );
		GL_SelectTexture( 0 );
	}
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
#endif
	s_wvboBatch = qfalse;
	s_wvboBatchShader = NULL;
	s_wvboBatchFog = 0;
	s_wvboBatchFogArmed = qfalse;
}

// stages the surface into the current same-shader run; qfalse = draw via tess
qboolean RB_TryWorldVBO( void *surface, shader_t *shader, int fogNum, int dlighted, int entityNum )
{
#ifdef USE_GXM_NATIVE
	if ( nw_active ) {
		if ( NW_Surface( (surfaceType_t *)surface, shader, fogNum ) ) {
			return qtrue;
		}
		// blended surfaces keep their submission order across the tess and resident paths
		if ( nw_numRuns && shader->sort > SS_OPAQUE ) {
			NW_Flush();
		}
		return qfalse;
	}
#endif
	if ( !s_wvbo.ready || !r_worldVBO->integer
		|| entityNum != REFENTITYNUM_WORLD || dlighted || g_bRenderGlowingObjects
		|| r_lightmap->integer || r_showtris->integer || r_shownormals->integer || r_fullbright->integer ) {
		RB_EndWorldVBO();
		return qfalse;
	}
#ifdef USE_GXM_NATIVE
	// only the global fog rides the uniform; a bounded volume needs its own tess pass
	if ( fogNum && !RB_GxmVolumeFogPass( fogNum, shader ) ) {
		RB_EndWorldVBO();
		return qfalse;
	}
#else
	if ( fogNum ) {
		RB_EndWorldVBO();
		return qfalse;
	}
#endif
	wvboSurf_t *rec = WorldVbo_Lookup( surface );
	if ( !rec || rec->shader != shader ) {
		RB_EndWorldVBO();
		return qfalse;
	}
	if ( !s_wvboBatch || s_wvboBatchShader != shader || s_wvboBatchFog != fogNum ) {
		WorldVbo_SetupBatch( shader, fogNum );
	}
	if ( s_wvboStaged + rec->numIndexes > WVBO_STAGE_MAX ) {
		WorldVbo_Flush();
		if ( rec->numIndexes > WVBO_STAGE_MAX ) {	// oversized surface: draw straight from the bake
#ifdef USE_GXM_NATIVE
			GXM_DrawStaticBuffer( s_wvbo.data, s_wvbo.idx + rec->firstIndex, rec->numIndexes, 1 );
#else
			qglDrawElements( GL_TRIANGLES, rec->numIndexes, GL_INDEX_TYPE, s_wvbo.idx + rec->firstIndex );
#endif
			backEnd.pc.c_wvboDraws++;
			backEnd.pc.c_wvboSurfaces++;
			return qtrue;
		}
	}
	memcpy( s_wvboStage + s_wvboStaged, s_wvbo.idx + rec->firstIndex, (size_t)rec->numIndexes * sizeof(glIndex_t) );
	s_wvboStaged += rec->numIndexes;
	backEnd.pc.c_wvboSurfaces++;
	return qtrue;
}

#endif // VITA
