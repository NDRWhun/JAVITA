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

#include "tr_local.h"

#ifdef USE_GXM_NATIVE
#include "rd-gxm/gxm_device.h"	// GXM_Sync
#endif


/*
=====================
R_PerformanceCounters
=====================
*/
void R_PerformanceCounters( void ) {
	if ( !r_speeds->integer ) {
		// clear the counters even if we aren't printing
		memset( &tr.pc, 0, sizeof( tr.pc ) );
		memset( &backEnd.pc, 0, sizeof( backEnd.pc ) );
		return;
	}

	if (r_speeds->integer == 1) {
		const float texSize = R_SumOfUsedImages( qfalse )/(8*1048576.0f)*(r_texturebits->integer?r_texturebits->integer:glConfig.colorBits);
		ri.Printf( PRINT_ALL,  "%i/%i shdrs/srfs %i leafs %i vrts %i/%i tris %.2fMB tex %.2f dc\n",
			backEnd.pc.c_shaders, backEnd.pc.c_surfaces, tr.pc.c_leafs, backEnd.pc.c_vertexes,
			backEnd.pc.c_indexes/3, backEnd.pc.c_totalIndexes/3,
			texSize, backEnd.pc.c_overDraw / (float)(glConfig.vidWidth * glConfig.vidHeight) );
	} else if (r_speeds->integer == 2) {
		ri.Printf( PRINT_ALL,  "(patch) %i sin %i sclip  %i sout %i bin %i bclip %i bout\n",
			tr.pc.c_sphere_cull_patch_in, tr.pc.c_sphere_cull_patch_clip, tr.pc.c_sphere_cull_patch_out,
			tr.pc.c_box_cull_patch_in, tr.pc.c_box_cull_patch_clip, tr.pc.c_box_cull_patch_out );
		ri.Printf( PRINT_ALL,  "(md3) %i sin %i sclip  %i sout %i bin %i bclip %i bout\n",
			tr.pc.c_sphere_cull_md3_in, tr.pc.c_sphere_cull_md3_clip, tr.pc.c_sphere_cull_md3_out,
			tr.pc.c_box_cull_md3_in, tr.pc.c_box_cull_md3_clip, tr.pc.c_box_cull_md3_out );
	} else if (r_speeds->integer == 3) {
		ri.Printf( PRINT_ALL,  "viewcluster: %i\n", tr.viewCluster );
	} else if (r_speeds->integer == 4) {
		if ( backEnd.pc.c_dlightVertexes ) {
			ri.Printf( PRINT_ALL,  "dlight srf:%i  culled:%i  verts:%i  tris:%i\n",
				tr.pc.c_dlightSurfaces, tr.pc.c_dlightSurfacesCulled,
				backEnd.pc.c_dlightVertexes, backEnd.pc.c_dlightIndexes / 3 );
		}
	}
	else if (r_speeds->integer == 5 )
	{
		ri.Printf( PRINT_ALL, "zFar: %.0f\n", tr.viewParms.zFar );
	}
	else if (r_speeds->integer == 6 )
	{
		ri.Printf( PRINT_ALL, "flare adds:%i tests:%i renders:%i\n",
			backEnd.pc.c_flareAdds, backEnd.pc.c_flareTests, backEnd.pc.c_flareRenders );
	}
	else if (r_speeds->integer == 7) {
		const float texSize = R_SumOfUsedImages(qtrue) / (1048576.0f);
		const float backBuff= glConfig.vidWidth * glConfig.vidHeight * glConfig.colorBits / (8.0f * 1024*1024);
		const float depthBuff= glConfig.vidWidth * glConfig.vidHeight * glConfig.depthBits / (8.0f * 1024*1024);
		const float stencilBuff= glConfig.vidWidth * glConfig.vidHeight * glConfig.stencilBits / (8.0f * 1024*1024);
		ri.Printf( PRINT_ALL,  "Tex MB %.2f + buffers %.2f MB = Total %.2fMB\n",
			texSize, backBuff*2+depthBuff+stencilBuff, texSize+backBuff*2+depthBuff+stencilBuff);
	}

	memset( &tr.pc, 0, sizeof( tr.pc ) );
	memset( &backEnd.pc, 0, sizeof( backEnd.pc ) );
}

#ifdef VITA
extern window_t window;	// tr_init.cpp
int activeBackEnd = 0;
int rendBackEnd = 0;
SceUID rend_mutex_in = -1;
SceUID rend_mutex_out = -1;
// init handshakes only; reusing rend_mutex_out would eat the frame-1 prime token
SceUID rend_init_done = -1;
// One-shot: set by main after WIN_CreateWindow (ordered by the handshake); the render
// thread's first wake runs the context init instead of a frame.
volatile qboolean pendingCtxInit = qfalse;
static SceUID rend_thid = -1;
static volatile qboolean rend_should_exit = qfalse;
static volatile int rend_handedBuffer = 0;	// index of the frame being handed off; written before Signal(in), read after Wait(in)

/*
Render-thread semaphore protocol (all created at 0; R = render thread, M = main):

  init:  M starts R -> R: WIN_LoadGL (vglInit on R), Signal(init) -> M: create window,
         pendingCtxInit=true, Signal(in) -> R: ctx init + clear-only first scene,
         Signal(init), Signal(out) <- this one unconsumed token is the frame-1 credit.
  frame: M: Wait(out), Signal(in), flip activeBackEnd/tessPtr -> R: Wait(in),
         RB_ExecuteRenderCommands, Signal(out), flip rendBackEnd.

  Every frame ends [in=0 out=1] with R parked on Wait(in), same as post-init, so
  no wakeup is ever lost. The drain (R_IssuePendingRenderCommands) does
  Wait(out)+Signal(out): blocks until R parks, leaves the token balance intact.
*/
extern "C" int sceGxmTransferFinish( void );	// GXM transfer-queue sync (SDK)

// Render backend thread: owns the vitaGL/GXM context (vglInit fires here).
extern void Sys_BootMark( const char *s );
static int renderThread( SceSize argc, void *argv ) {
	Sys_BootMark( "rt: vglinit" );
	ri.WIN_LoadGL();
	Sys_BootMark( "rt: vglinit done" );
	sceKernelSignalSema( rend_init_done, 1 );

	for ( ;; ) {
		sceKernelWaitSema( rend_mutex_in, 1, NULL );
		if ( rend_should_exit ) {
			break;
		}
		if ( pendingCtxInit ) {
			// One-shot context init on this thread. SDL's current-window TLS was set on
			// main; without MakeCurrent here every present from this thread no-ops.
			Sys_BootMark( "rt: ctx start" );
			ri.WIN_MakeCurrent();
			GL_SetDefaultState();
			// clear-only first scene: a draw before any completed scene GPU-faults
			qglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
			qglClear( GL_COLOR_BUFFER_BIT );
			ri.WIN_Present( &window );
			sceGxmTransferFinish();
			qglFinish();
#ifdef USE_GXM_NATIVE
			GXM_Sync();	// the first scene must be done before main starts uploading
#endif
			Sys_BootMark( "rt: ctx done" );
			pendingCtxInit = qfalse;
			sceKernelSignalSema( rend_init_done, 1 );	// release main from step-5 wait
			sceKernelSignalSema( rend_mutex_out, 1 );	// the single frame-1 prime
			continue;
		}
		rendBackEnd = rend_handedBuffer;	// adopt the handed index; mispairing is structurally impossible
		set_tessPtr( &tessArray[rendBackEnd] );
		RB_ExecuteRenderCommands( backEndDataPtr[rendBackEnd]->commands.cmds );
		sceKernelSignalSema( rend_mutex_out, 1 );
	}
	return sceKernelExitDeleteThread( 0 );
}

void R_StartRenderThread( void ) {
	if ( rend_thid >= 0 || !r_renderThread || !r_renderThread->integer ) {
		return;
	}
	rend_should_exit = qfalse;
	pendingCtxInit   = qfalse;
	rend_init_done = sceKernelCreateSema( "rend_init", 0, 0, 2, NULL );
	rend_mutex_in  = sceKernelCreateSema( "rend_in",   0, 0, 1, NULL );
	rend_mutex_out = sceKernelCreateSema( "rend_out",  0, 0, 1, NULL );
	// main/frontend on core 0, backend owns core 2 (core 3 is system-reserved)
	rend_thid = sceKernelCreateThread( "Renderer Thread", renderThread, 0x10000100, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL );
	sceKernelStartThread( rend_thid, 0, NULL );
}

qboolean R_OnRenderThread( void ) {
	return (qboolean)( rend_thid >= 0 && sceKernelGetThreadId() == rend_thid );
}

// vid_restart teardown: wake the render thread out of its Wait(in) with the exit
// flag set, join it, delete the semaphores so a later R_Init re-creates the thread.
void R_StopRenderThread( void ) {
	if ( rend_thid < 0 ) {
		return;
	}
	rend_should_exit = qtrue;
	sceKernelSignalSema( rend_mutex_in, 1 );
	sceKernelWaitThreadEnd( rend_thid, NULL, NULL );
	if ( rend_init_done >= 0 ) { sceKernelDeleteSema( rend_init_done ); rend_init_done = -1; }
	if ( rend_mutex_in  >= 0 ) { sceKernelDeleteSema( rend_mutex_in );  rend_mutex_in  = -1; }
	if ( rend_mutex_out >= 0 ) { sceKernelDeleteSema( rend_mutex_out ); rend_mutex_out = -1; }
	rend_thid = -1;
}
#endif

/*
====================
R_IssueRenderCommands
====================
*/
void R_IssueRenderCommands( qboolean runPerformanceCounters ) {
	renderCommandList_t	*cmdList;

	cmdList = &backEndData->commands;

	// add an end-of-list command
	byteAlias_t *ba = (byteAlias_t *)&cmdList->cmds[cmdList->used];
	ba->ui = RC_END_OF_LIST;

	// clear it out, in case this is a sync and not a buffer flip
	cmdList->used = 0;

#ifdef VITA
	if ( r_renderThread && r_renderThread->integer ) {
		// hand the frame to the render thread, flip the frontend to the other buffer
		sceKernelWaitSema( rend_mutex_out, 1, NULL );
		// backend parked between Wait(out) and Signal(in): its counters are stable here
		if ( runPerformanceCounters ) {
			R_PerformanceCounters();
		}
		rend_handedBuffer = activeBackEnd;
		sceKernelSignalSema( rend_mutex_in, 1 );
		activeBackEnd = !activeBackEnd;
		backEndData = backEndDataPtr[activeBackEnd];
		set_tessPtr( &tessArray[activeBackEnd] );
		return;
	}
#endif

	// at this point, the back end thread is idle, so it is ok
	// to look at it's performance counters
	if ( runPerformanceCounters ) {
		R_PerformanceCounters();
	}

	// actually start the commands going
	if ( !r_skipBackEnd->integer ) {
		// let it start on the new batch
		RB_ExecuteRenderCommands( cmdList->cmds );
	}
}


/*
====================
R_IssuePendingRenderCommands

Issue any pending commands and wait for them to complete.
====================
*/
void R_IssuePendingRenderCommands( void ) {
	if ( !tr.registered ) {
		return;
	}
	R_IssueRenderCommands( qfalse );

#ifdef VITA
	// The hand-off above is asynchronous; wait until the backend is parked so
	// main-thread GL can't race the GXM context. Wait(out)+Signal(out) blocks
	// until "done" and leaves the token balance unchanged.
	if ( r_renderThread && r_renderThread->integer ) {
		sceKernelWaitSema( rend_mutex_out, 1, NULL );
		sceKernelSignalSema( rend_mutex_out, 1 );
	}
#endif
}

/*
============
R_GetCommandBufferReserved

make sure there is enough command space
============
*/
static void *R_GetCommandBufferReserved( int bytes, int reservedBytes ) {
	renderCommandList_t	*cmdList;

	cmdList = &backEndData->commands;
	bytes = PAD(bytes, sizeof(void *));

	// always leave room for the end of list command
	if ( cmdList->used + bytes + sizeof( int ) + reservedBytes > MAX_RENDER_COMMANDS ) {
		if ( bytes > MAX_RENDER_COMMANDS - (int)sizeof( int ) ) {
			ri.Error( ERR_FATAL, "R_GetCommandBuffer: bad size %i", bytes );
		}
		// if we run out of room, just start dropping commands
		return NULL;
	}

	cmdList->used += bytes;

	return cmdList->cmds + cmdList->used - bytes;
}

/*
============
R_GetCommandBuffer

make sure there is enough command space
============
*/
void *R_GetCommandBuffer( int bytes ) {
	return R_GetCommandBufferReserved( bytes, PAD( sizeof( swapBuffersCommand_t ), sizeof(void *) ) );
}


/*
=============
R_AddDrawSurfCmd

=============
*/
void	R_AddDrawSurfCmd( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	drawSurfsCommand_t	*cmd;

	cmd = (drawSurfsCommand_t *) R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_DRAW_SURFS;

	cmd->drawSurfs = drawSurfs;
	cmd->numDrawSurfs = numDrawSurfs;

	cmd->refdef = tr.refdef;
	cmd->viewParms = tr.viewParms;

#ifdef VITA
	// Render-thread mode: snapshot this view's Ghoul2 bone matrices NOW, on the
	// frontend, while the bone caches still hold this frame's skeletons.
	if ( r_renderThread && r_renderThread->integer ) {
		RB_PrepGhoulSkinMT( drawSurfs, numDrawSurfs );
	}
#endif
}


/*
=============
RE_SetColor

Passing NULL will set the color to white
=============
*/
void	RE_SetColor( const float *rgba ) {
	setColorCommand_t	*cmd;

	if ( !tr.registered ) {
		return;
	}
	cmd = (setColorCommand_t *) R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_SET_COLOR;
	if ( !rgba ) {
		rgba = colorWhite;
	}
	cmd->color[0] = rgba[0];
	cmd->color[1] = rgba[1];
	cmd->color[2] = rgba[2];
	cmd->color[3] = rgba[3];

}


/*
=============
RE_StretchPic
=============
*/
void RE_StretchPic ( float x, float y, float w, float h,
					  float s1, float t1, float s2, float t2, qhandle_t hShader ) {
	stretchPicCommand_t	*cmd;

	if ( !tr.registered ) {
		return;
	}
	cmd = (stretchPicCommand_t *) R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_STRETCH_PIC;
	cmd->shader = R_GetShaderByHandle( hShader );
	cmd->x = x;
	cmd->y = y;
	cmd->w = w;
	cmd->h = h;
	cmd->s1 = s1;
	cmd->t1 = t1;
	cmd->s2 = s2;
	cmd->t2 = t2;
}

/*
=============
RE_RotatePic
=============
*/
void RE_RotatePic ( float x, float y, float w, float h,
					  float s1, float t1, float s2, float t2,float a, qhandle_t hShader ) {
	rotatePicCommand_t	*cmd;

	if (!tr.registered) {
		return;
	}
	cmd = (rotatePicCommand_t *) R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_ROTATE_PIC;
	cmd->shader = R_GetShaderByHandle( hShader );
	cmd->x = x;
	cmd->y = y;
	cmd->w = w;
	cmd->h = h;
	cmd->s1 = s1;
	cmd->t1 = t1;
	cmd->s2 = s2;
	cmd->t2 = t2;
	cmd->a = a;
}

/*
=============
RE_RotatePic2
=============
*/
void RE_RotatePic2 ( float x, float y, float w, float h,
					  float s1, float t1, float s2, float t2,float a, qhandle_t hShader ) {
	rotatePicCommand_t	*cmd;

	if (!tr.registered) {
		return;
	}

	cmd = (rotatePicCommand_t *) R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_ROTATE_PIC2;
	cmd->shader = R_GetShaderByHandle( hShader );
	cmd->x = x;
	cmd->y = y;
	cmd->w = w;
	cmd->h = h;
	cmd->s1 = s1;
	cmd->t1 = t1;
	cmd->s2 = s2;
	cmd->t2 = t2;
	cmd->a = a;
}

extern void R_CacheWorldEffects( void );	// tr_WorldEffects.cpp

void RE_RenderWorldEffects(void)
{
	drawBufferCommand_t	*cmd;

	if (!tr.registered) {
		return;
	}
	R_CacheWorldEffects();

	cmd = (drawBufferCommand_t *)R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_WORLD_EFFECTS;
}

void RE_RenderAutoMap(void)
{
	drawBufferCommand_t	*cmd;

	if (!tr.registered) {
		return;
	}
	cmd = (drawBufferCommand_t *)R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd )
	{
		return;
	}
	cmd->commandId = RC_AUTO_MAP;
}

/*
====================
RE_BeginFrame

If running in stereo, RE_BeginFrame will be called twice
for each RE_EndFrame
====================
*/
void RE_BeginFrame( stereoFrame_t stereoFrame ) {
	drawBufferCommand_t	*cmd = NULL;

	if ( !tr.registered ) {
		return;
	}
	glState.finishCalled = qfalse;

	tr.frameCount++;
	tr.frameSceneNum = 0;

	//
	// do overdraw measurement
	//
	if ( r_measureOverdraw->integer )
	{
		if ( glConfig.stencilBits < 4 )
		{
			ri.Printf( PRINT_ALL, "Warning: not enough stencil bits to measure overdraw: %d\n", glConfig.stencilBits );
			ri.Cvar_Set( "r_measureOverdraw", "0" );
			r_measureOverdraw->modified = qfalse;
		}
		else if ( r_shadows->integer == 2 )
		{
			ri.Printf( PRINT_ALL, "Warning: stencil shadows and overdraw measurement are mutually exclusive\n" );
			ri.Cvar_Set( "r_measureOverdraw", "0" );
			r_measureOverdraw->modified = qfalse;
		}
		else
		{
			R_IssuePendingRenderCommands();
			qglEnable( GL_STENCIL_TEST );
			qglStencilMask( ~0U );
			qglClearStencil( 0U );
			qglStencilFunc( GL_ALWAYS, 0U, ~0U );
			qglStencilOp( GL_KEEP, GL_INCR, GL_INCR );
		}
		r_measureOverdraw->modified = qfalse;
	}
	else
	{
		// this is only reached if it was on and is now off
		if ( r_measureOverdraw->modified ) {
			R_IssuePendingRenderCommands();
			qglDisable( GL_STENCIL_TEST );
		}
		r_measureOverdraw->modified = qfalse;
	}

	//
	// texturemode stuff
	//
	if ( r_textureMode->modified || r_ext_texture_filter_anisotropic->modified) {
		R_IssuePendingRenderCommands();
		GL_TextureMode( r_textureMode->string );
		r_textureMode->modified = qfalse;
		r_ext_texture_filter_anisotropic->modified = qfalse;
	}

	//
	// gamma stuff
	//
	if ( r_gamma->modified ) {
		r_gamma->modified = qfalse;

		R_IssuePendingRenderCommands();
		R_SetColorMappings();
		R_SetGammaCorrectionLUT();
	}

	// check for errors
	if ( !r_ignoreGLErrors->integer ) {
		R_IssuePendingRenderCommands();

		GLenum err = qglGetError();
		if ( err != GL_NO_ERROR ) {
			Com_Error( ERR_FATAL, "RE_BeginFrame() - glGetError() failed (0x%x)!\n", err );
		}
	}

	//
	// draw buffer stuff
	//
	cmd = (drawBufferCommand_t *) R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_DRAW_BUFFER;

	if ( glConfig.stereoEnabled ) {
		if ( stereoFrame == STEREO_LEFT ) {
			cmd->buffer = (int)GL_BACK_LEFT;
		} else if ( stereoFrame == STEREO_RIGHT ) {
			cmd->buffer = (int)GL_BACK_RIGHT;
		} else {
			Com_Error( ERR_FATAL, "RE_BeginFrame: Stereo is enabled, but stereoFrame was %i", stereoFrame );
		}
	} else {
		if ( stereoFrame != STEREO_CENTER ) {
			Com_Error( ERR_FATAL, "RE_BeginFrame: Stereo is disabled, but stereoFrame was %i", stereoFrame );
		}
//		if ( !Q_stricmp( r_drawBuffer->string, "GL_FRONT" ) ) {
//			cmd->buffer = (int)GL_FRONT;
//		} else
		{
			cmd->buffer = (int)GL_BACK;
		}
	}
}


/*
=============
RE_EndFrame

Returns the number of msec spent in the back end
=============
*/
void RE_EndFrame( int *frontEndMsec, int *backEndMsec ) {
	swapBuffersCommand_t	*cmd;

	if ( !tr.registered ) {
		return;
	}
	cmd = (swapBuffersCommand_t *) R_GetCommandBufferReserved( sizeof( *cmd ), 0 );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_SWAP_BUFFERS;

#ifdef VITA
	static qboolean s_frame1 = qfalse;
	if ( !s_frame1 ) { s_frame1 = qtrue; Sys_BootMark( "frame1" ); }
#endif
	R_IssueRenderCommands( qtrue );

	// use the other buffers next frame, because another CPU
	// may still be rendering into the current ones
	R_InitNextFrame();

	if ( frontEndMsec ) {
		*frontEndMsec = tr.frontEndMsec;
	}
	tr.frontEndMsec = 0;
	if ( backEndMsec ) {
		*backEndMsec = backEnd.pc.msec;
	}
	backEnd.pc.msec = 0;
}

/*
=============
RE_TakeVideoFrame
=============
*/
void RE_TakeVideoFrame( int width, int height, byte *captureBuffer, byte *encodeBuffer, qboolean motionJpeg )
{
	videoFrameCommand_t *cmd;

	if ( !tr.registered )
		return;

	cmd = (videoFrameCommand_t *)R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd )
		return;

	cmd->commandId = RC_VIDEOFRAME;

	cmd->width = width;
	cmd->height = height;
	cmd->captureBuffer = captureBuffer;
	cmd->encodeBuffer = encodeBuffer;
	cmd->motionJpeg = motionJpeg;
}
