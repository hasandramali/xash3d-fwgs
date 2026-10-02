/*
vid_common.c - common vid component
Copyright (C) 2018 a1batross, Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "common.h"
#include "client.h"
#include "mod_local.h"
#include "input.h"
#include "vid_common.h"
#include "platform/platform.h"

static CVAR_DEFINE_AUTO( vid_mode, "0", FCVAR_RENDERINFO, "current video mode index (used only for storage)" );
static CVAR_DEFINE_AUTO( vid_rotate, "0", FCVAR_RENDERINFO|FCVAR_VIDRESTART, "screen rotation (0-3)" );
static CVAR_DEFINE_AUTO( vid_scale, "1.0", FCVAR_RENDERINFO|FCVAR_VIDRESTART, "pixel scale" );

CVAR_DEFINE_AUTO( vid_maximized, "0", FCVAR_RENDERINFO, "window maximized state, read-only" );
CVAR_DEFINE_AUTO( vid_blackcheck, "1", FCVAR_ARCHIVE, "check for wedged black presentation after video init and refresh once" );
CVAR_DEFINE( vid_fullscreen, "fullscreen", DEFAULT_FULLSCREEN, FCVAR_RENDERINFO|FCVAR_VIDRESTART, "fullscreen state (0 windowed, 1 fullscreen, 2 borderless)" );
CVAR_DEFINE( window_width, "width", "0", FCVAR_RENDERINFO|FCVAR_VIDRESTART, "screen width" );
CVAR_DEFINE( window_height, "height", "0", FCVAR_RENDERINFO|FCVAR_VIDRESTART, "screen height" );
CVAR_DEFINE( vid_width, "vid_width", "0", FCVAR_READ_ONLY, "actual window viewport size" );
CVAR_DEFINE( vid_height, "vid_height", "0", FCVAR_READ_ONLY, "actual window viewport size" );

glwstate_t	glw_state;

static void VID_ArmBlackCheck( void );
static void VID_BlackCheckResolve( void );

/*
=================
R_SaveVideoMode
=================
*/
void R_SaveVideoMode( int w, int h, int render_w, int render_h, qboolean maximized )
{
	string temp;

	if( !w || !h || !render_w || !render_h )
	{
		host.renderinfo_changed = false;
		return;
	}

	host.window_center_x = w / 2;
	host.window_center_y = h / 2;

	Q_snprintf( temp, sizeof( temp ), "%d", w );
	Cvar_DirectSet( &window_width, temp );

	Q_snprintf( temp, sizeof( temp ), "%d", h );
	Cvar_DirectSet( &window_height, temp );

	Q_snprintf( temp, sizeof( temp ), "%d", render_w );
	Cvar_FullSet( "vid_width", temp, vid_width.flags );

	Q_snprintf( temp, sizeof( temp ), "%d", render_h );
	Cvar_FullSet( "vid_height", temp, vid_width.flags  );

	Cvar_DirectSet( &vid_maximized, maximized ? "1" : "0" );
	
	// immediately drop changed state or we may trigger
	// video subsystem to reapply settings
	host.renderinfo_changed = false;

	refState.scale_x = (float)render_w / w;
	refState.scale_y = (float)render_h / h;

	if( refState.width == render_w && refState.height == render_h )
		return;

	refState.width = render_w;
	refState.height = render_h;

	// every successful mode set re-arms the black-presentation watchdog:
	// a fresh EGL binding can come up wedged (black screen with running
	// audio), and this is the same refresh a map load performs to clear it
	VID_ArmBlackCheck();

	// check for 4:3 or 5:4
	refState.wideScreen = render_w * 3 != render_h * 4 && render_w * 4 != render_h * 5;

	SCR_VidInit(); // tell client.dll that vid_mode has changed
}

/*
=================
VID_GetModeString
=================
*/
const char *VID_GetModeString( int vid_mode )
{
	if( vid_mode < 0 || vid_mode >= R_MaxVideoModes() )
		return NULL;

	vidmode_t *vidmode = R_GetVideoMode( vid_mode );
	if( !vidmode )
		return NULL;

	return vidmode->desc;
}

/*
==================
VID_ArmBlackCheck

Called on every successful video mode set. Forces non-blocking swaps so a
wedged present queue can't hang the main loop (which also stalls audio and
input), and arms a one-shot black-presentation check a couple seconds later.
The menu is never a pure-black fullscreen at that point, so an all-black
readback means presentation died and needs a refresh.
==================
*/
static qboolean vid_blackcheck_armed;
static qboolean vid_blackcheck_gaveup;
static double vid_blackcheck_time;
static int vid_blackcheck_hits;
static int vid_blackcheck_restarts;
static float vid_blackcheck_vsync = -1.0f;

#define VID_BLACKCHECK_DELAY	2.0	// seconds after video init before checking
#define VID_BLACKCHECK_TIMEOUT	30.0	// give up waiting (e.g. stuck states)
#define VID_BLACKCHECK_MAXRESTARTS	3	// don't loop video restarts forever
#define VID_BLACKCHECK_SIZE	32	// center sample block, pixels

typedef void (*PFNGLREADBUFFER)( int mode );
typedef void (*PFNGLREADPIXELS)( int x, int y, int w, int h, unsigned int format, unsigned int type, void *pixels );

#define GL_FRONT		0x0404
#define GL_BACK		0x0405
#define GL_RGBA		0x1908
#define GL_UNSIGNED_BYTE	0x1401

static byte vid_blackcheck_px[VID_BLACKCHECK_SIZE * VID_BLACKCHECK_SIZE * 4];

static static void VID_BlackCheckResolve( void )
{
	// restore the user's vsync through the normal path
	if( vid_blackcheck_vsync >= 0.0f )
	{
		Cvar_DirectSetValue( &gl_vsync, vid_blackcheck_vsync );
		SetBits( gl_vsync.flags, FCVAR_CHANGED );
		vid_blackcheck_vsync = -1.0f;
	}

	vid_blackcheck_armed = false;
	vid_blackcheck_hits = 0;
}

static void VID_ArmBlackCheck( void )
{
	if( vid_blackcheck_gaveup )
		return;

	vid_blackcheck_armed = true;
	vid_blackcheck_time = host.realtime;
	vid_blackcheck_hits = 0;

	// don't let the very first swaps block forever on a not-yet-ready
	// EGL queue; tearing for a couple seconds of menu is invisible
	if( vid_blackcheck_vsync < 0.0f )
		vid_blackcheck_vsync = gl_vsync.value;

	Cvar_DirectSetValue( &gl_vsync, 0.0f );
	SetBits( gl_vsync.flags, FCVAR_CHANGED );
}

static void VID_BlackCheck( void )
{
	PFNGLREADBUFFER pglReadBuffer;
	PFNGLREADPIXELS pglReadPixels;
	int w, h, x0, y0, i;
	qboolean allblack = true;

	if( !vid_blackcheck_armed || !vid_blackcheck.value )
		return;

	// in game the 3D view owns the screen; never touch a running session
	if( cls.state == ca_active )
	{
		VID_BlackCheckResolve();
		return;
	}

	if( host.realtime - vid_blackcheck_time < VID_BLACKCHECK_DELAY )
		return;

	if( host.realtime - vid_blackcheck_time > VID_BLACKCHECK_TIMEOUT )
	{
		VID_BlackCheckResolve();
		return;
	}

	pglReadBuffer = (PFNGLREADBUFFER)GL_GetProcAddress( "glReadBuffer" );
	pglReadPixels = (PFNGLREADPIXELS)GL_GetProcAddress( "glReadPixels" );

	if( !pglReadBuffer || !pglReadPixels )
	{
		Con_DPrintf( S_WARN "%s: no readback procs, skipping check\n", __func__ );
		VID_BlackCheckResolve();
		return;
	}

	w = (int)vid_width.value;
	h = (int)vid_height.value;

	if( w < VID_BLACKCHECK_SIZE || h < VID_BLACKCHECK_SIZE )
		return; // no valid viewport yet, try again next frame

	// sample the presented (front) frame center: menu, console, plaque and
	// loading text all paint non-black pixels here once running
	x0 = ( w - VID_BLACKCHECK_SIZE ) / 2;
	y0 = ( h - VID_BLACKCHECK_SIZE ) / 2;

	pglReadBuffer( GL_FRONT );
	pglReadPixels( x0, y0, VID_BLACKCHECK_SIZE, VID_BLACKCHECK_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, vid_blackcheck_px );
	pglReadBuffer( GL_BACK );

	for( i = 0; i < (int)sizeof( vid_blackcheck_px ); i += 4 )
	{
		if( vid_blackcheck_px[i] || vid_blackcheck_px[i + 1] || vid_blackcheck_px[i + 2] )
		{
			allblack = false;
			break;
		}
	}

	if( !allblack )
	{
		Con_DPrintf( "%s: presentation alive, vsync restored\n", __func__ );
		VID_BlackCheckResolve();
		return;
	}

	if( ++vid_blackcheck_hits < 2 )
		return; // require two consecutive black frames

	vid_blackcheck_hits = 0;

	if( ++vid_blackcheck_restarts > VID_BLACKCHECK_MAXRESTARTS )
	{
		Con_Printf( S_ERROR "%s: presentation still black after %d refreshes, giving up (set %s 0 to silence)\n",
			__func__, VID_BLACKCHECK_MAXRESTARTS, vid_blackcheck.name );
		vid_blackcheck_gaveup = true;
		VID_BlackCheckResolve();
		return;
	}

	Con_Printf( S_WARN "%s: black presentation detected %ds after video init, refreshing video (attempt %d/%d)\n",
		__func__, (int)VID_BLACKCHECK_DELAY, vid_blackcheck_restarts, VID_BLACKCHECK_MAXRESTARTS );

	// validated recovery path: recreates the window/EGL binding, the same
	// thing a map load does when it clears this state. Runs at the top of
	// the next frame via the existing renderinfo check below.
	VID_BlackCheckResolve();
	host.renderinfo_changed = true;
}

/*
==================
VID_CheckChanges

check vid modes and fullscreen
==================
*/
void VID_CheckChanges( void )
{
	// black-presentation watchdog runs here (pre-render: the front buffer
	// still holds the last presented frame); a pending restart takes
	// precedence so we never sample mid-reinit
	if( !host.renderinfo_changed )
		VID_BlackCheck();

	if( FBitSet( cl_allow_levelshots.flags, FCVAR_CHANGED ))
	{
		//GL_FreeTexture( cls.loadingBar );
		SCR_RegisterTextures(); // reload 'lambda' image
		ClearBits( cl_allow_levelshots.flags, FCVAR_CHANGED );
	}

	if( host.renderinfo_changed )
	{
		if( VID_SetMode( ))
		{
			SCR_VidInit(); // tell the client.dll what vid_mode has changed
		}
		else
		{
			Sys_Error( "Can't re-initialize video subsystem\n" );
		}
		host.renderinfo_changed = false;
	}
}

/*
===============
VID_SetDisplayTransform

notify ref dll about screen transformations
===============
*/
void VID_SetDisplayTransform( int *render_w, int *render_h )
{
	VID_SetDisplayTransformScale( render_w, render_h, vid_scale.value, vid_scale.value );
}

void VID_SetDisplayTransformScale( int *render_w, int *render_h, float scale_x, float scale_y )
{
	uint rotate = vid_rotate.value;

	if( rotate < REF_ROTATE_NONE || rotate > REF_ROTATE_CCW )
		rotate = REF_ROTATE_NONE;

	if( ref.dllFuncs.R_SetDisplayTransform( rotate, 0, 0, scale_x, scale_y ))
	{
		if( rotate & 1 )
		{
			int swap = *render_w;

			*render_w = *render_h;
			*render_h = swap;
		}

		//*render_h /= vid_scale.value;
		//*render_w /= vid_scale.value;

		ref.rotation = rotate;
	}
	else
	{
		Con_Printf( S_WARN "failed to setup screen transform\n" );

		ref.rotation = REF_ROTATE_NONE;
	}
}

static void VID_Mode_f( void )
{
	int w, h;

	switch( Cmd_Argc() )
	{
	case 2:
	{
		vidmode_t *vidmode = R_GetVideoMode( Q_atoi( Cmd_Argv( 1 )) );
		if( !vidmode )
		{
			Con_Printf( S_ERROR "unable to set mode, backend returned null\n" );
			return;
		}

		w = vidmode->width;
		h = vidmode->height;
		break;
	}
	case 3:
	{
		w = Q_atoi( Cmd_Argv( 1 ));
		h = Q_atoi( Cmd_Argv( 2 ));
		break;
	}
	default:
		Msg( S_USAGE "vid_mode <modenum>|<width height>\n" );
		return;
	}

	R_ChangeDisplaySettings( w, h, bound( 0, vid_fullscreen.value, WINDOW_MODE_COUNT - 1 ));
}

void VID_Init( void )
{
	// system screen width and height (don't suppose for change from console at all)
	Cvar_RegisterVariable( &window_width );
	Cvar_RegisterVariable( &window_height );

	Cvar_RegisterVariable( &vid_mode );
	Cvar_RegisterVariable( &vid_rotate );
	Cvar_RegisterVariable( &vid_scale );
	Cvar_RegisterVariable( &vid_fullscreen );
	Cvar_RegisterVariable( &vid_maximized );
	Cvar_RegisterVariable( &vid_blackcheck );
	Cvar_RegisterVariable( &vid_width );
	Cvar_RegisterVariable( &vid_height );
	Cvar_Get( "_window_xpos", "-1", FCVAR_RENDERINFO, "deprecated cvar" );
	Cvar_Get( "_window_ypos", "-1", FCVAR_RENDERINFO, "deprecated cvar" );

	// a1ba: planned to be named vid_mode for compability
	// but supported mode list is filled by backends, so numbers are not portable any more
	Cmd_AddRestrictedCommand( "vid_setmode", VID_Mode_f, "display video mode" );
	Cmd_AddCommand( "vid_info", VID_Info_f, "show vid component info" );

	V_Init(); // init gamma
	R_Init(); // init renderer
}
