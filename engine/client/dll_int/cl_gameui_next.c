/*
cl_gameui_next.c - GameUI bridge for NextClient menus
Copyright (C) 2024

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This module bridges between xash3d's engine menu API (UI_FUNCTIONS)
and NextClient's GameUI DLL (CreateInterface / IGameUI).

When -gameui is used, the engine loads the GameUI library via
CreateInterface instead of the standard GetMenuAPI path.
This module handles:
  - GameUI DLL loading/unloading
  - IGameUI interface bridging to UI_FUNCTIONS
  - Factory functions for engine services
  - Incremental VGUI2 support
*/

#include "common.h"
#include "client.h"
#include "const.h"
#include "library.h"
#include "input.h"
#include "server.h"
#include "vid_common.h"
#include "dll_int/cl_gameui_next.h"

gameui_next_t gameuiNext;

/*
=====================
Forward declarations for bridge functions
=====================
*/
static int	GAME_EXPORT GameUI_VidInit( void );
static void	GAME_EXPORT GameUI_Init( void );
static void	GAME_EXPORT GameUI_Shutdown( void );
static void	GAME_EXPORT GameUI_Redraw( float flTime );
static void	GAME_EXPORT GameUI_KeyEvent( int key, int down );
static void	GAME_EXPORT GameUI_MouseMove( int x, int y );
static void	GAME_EXPORT GameUI_SetActiveMenu( int active );
static void	GAME_EXPORT GameUI_GetCursorPos( int *pos_x, int *pos_y );
static void	GAME_EXPORT GameUI_SetCursorPos( int pos_x, int pos_y );
static void	GAME_EXPORT GameUI_ShowCursor( int show );
static void	GAME_EXPORT GameUI_CharEvent( int key );
static int	GAME_EXPORT GameUI_MouseInRect( void );
static int	GAME_EXPORT GameUI_IsVisible( void );
static int	GAME_EXPORT GameUI_CreditsActive( void );
static void	GAME_EXPORT GameUI_FinalCredits( void );

/*
=====================
Bridge function table

These bridge xash3d's engine calls to IGameUI methods.
As VGUI2 support is incrementally added, these functions
will be populated with actual IGameUI calls.
=====================
*/
static UI_FUNCTIONS g_GameUIFuncs =
{
	GameUI_VidInit,
	GameUI_Init,
	GameUI_Shutdown,
	GameUI_Redraw,
	GameUI_KeyEvent,
	GameUI_MouseMove,
	GameUI_SetActiveMenu,
	NULL,			// pfnAddServerToList - handled by engine's direct net API
	GameUI_GetCursorPos,
	GameUI_SetCursorPos,
	GameUI_ShowCursor,
	GameUI_CharEvent,
	GameUI_MouseInRect,
	GameUI_IsVisible,
	GameUI_CreditsActive,
	GameUI_FinalCredits
};

/*
=====================
GameUI_CreateInterfaceFn (reference)

Factory providing engine interface access to GameUI DLL.
Not yet active - will be used when VGUI2 integration is implemented.

Required interfaces for full GameUI support:
  - FILESYSTEM_INTERFACE_VERSION  (filesystem)
  - VEngineVGui001                (VGUI2 panel system)
  - EngineSurface007              (surface rendering)
  - VEngineUIFuncs001              (UI functions)
  - GameClientExports001           (client exports)
=====================
*/
#if 0
static void *GameUI_CreateInterfaceFn( const char *pName, int *pReturnCode )
{
	if( pReturnCode ) *pReturnCode = 1;
	return NULL;
}
#endif

/*
=====================
GameUI_VidInit
=====================
*/
static int GameUI_VidInit( void )
{
	if( gameui.globals )
	{
		gameui.globals->scrWidth = refState.width;
		gameui.globals->scrHeight = refState.height;
	}

	Con_DPrintf( "GameUI: VidInit (%dx%d)\n", refState.width, refState.height );
	return 1;
}

/*
=====================
GameUI_Init

Called once after loading. Initializes gameui state.
=====================
*/
static void GameUI_Init( void )
{
	Con_Reportf( "GameUI: Init (bridge mode, VGUI2 integration in progress)\n" );

	if( gameui.globals )
	{
		gameui.globals->scrWidth = refState.width;
		gameui.globals->scrHeight = refState.height;
		gameui.globals->developer = host.allow_console;
	}

	gameuiNext.active = true;
}

/*
=====================
GameUI_Shutdown
=====================
*/
static void GameUI_Shutdown( void )
{
	gameuiNext.active = false;
	Con_Reportf( "GameUI: Shutdown\n" );
}

/*
=====================
GameUI_Redraw

Called every frame to render the menu.
When VGUI2 is implemented, this will call IGameUI::RunFrame().
=====================
*/
static void GameUI_Redraw( float flTime )
{
	if( !gameuiNext.active )
		return;

	// TODO: Call IGameUI::RunFrame() when VGUI2 bridge is ready
	// For now, gameui draws nothing until VGUI2 surface is implemented
	// This is the key integration point for VGUI2 rendering
}

/*
=====================
GameUI_KeyEvent
=====================
*/
static void GameUI_KeyEvent( int key, int down )
{
	if( !gameuiNext.active )
		return;

	// TODO: Route to VGUI2 key handling
	// VGUI2 expects: surface()->AppHandler( event )
}

/*
=====================
GameUI_MouseMove
=====================
*/
static void GameUI_MouseMove( int x, int y )
{
	if( !gameuiNext.active )
		return;

	// TODO: Route to VGUI2 mouse handling
}

/*
=====================
GameUI_SetActiveMenu

Show or hide the game menu.
=====================
*/
static void GameUI_SetActiveMenu( int active )
{
	gameuiNext.active = ( active != 0 );

	// TODO: Call IGameUI::ActivateGameUI() / HideGameUI()
	if( active )
	{
		Con_DPrintf( "GameUI: menu activated\n" );
	}
	else
	{
		Con_DPrintf( "GameUI: menu hidden\n" );
	}
}

static void GameUI_GetCursorPos( int *pos_x, int *pos_y )
{
	if( pos_x ) *pos_x = 0;
	if( pos_y ) *pos_y = 0;
}

static void GameUI_SetCursorPos( int pos_x, int pos_y )
{
	// TODO: Set VGUI2 cursor via Platform_SetCursorType
}

static void GameUI_ShowCursor( int show )
{
	Platform_SetCursorType( show ? dc_user : dc_none );
}

static void GameUI_CharEvent( int key )
{
	// TODO: Route to VGUI2 text input
}

static int GameUI_MouseInRect( void )
{
	return 1; // always in rect for now
}

static int GameUI_IsVisible( void )
{
	return gameuiNext.active ? 1 : 0;
}

static int GameUI_CreditsActive( void )
{
	return 0;
}

static void GameUI_FinalCredits( void )
{
	// TODO: Show credits via gameui
}

/*
=====================
GameUI_LoadProgs

Load the GameUI DLL via CreateInterface and set up the bridge.

Loading strategy:
  1. Use COM_GetCommonLibraryPath to resolve library path
     (respects -menulib override, defaults to @GameUI)
  2. Load the shared library
  3. Get the exported CreateInterface function
  4. Request IGameUI (GameUI007) interface
  5. Populate gameui.dllFuncs with bridge functions
  6. Initialize basic engine-menn state

Returns true on success, false on failure.
=====================
*/
qboolean GameUI_LoadProgs( void )
{
	typedef void *(*CreateInterfaceFn)( const char *pName, int *pReturnCode );
	CreateInterfaceFn pfnCreateInterface = NULL;
	void *pGameUI = NULL;
	string dllpath;
	int retCode;

	if( gameuiNext.hInstance )
		GameUI_UnloadProgs();

	// Ensure we start clean
	memset( &gameuiNext, 0, sizeof( gameuiNext ));

	// Resolve library path (default: @GameUI → <gamedir>/GameUI.so)
	COM_GetCommonLibraryPath( LIBRARY_GAMEUI, dllpath, sizeof( dllpath ));
	Con_Reportf( "GameUI: loading from %s\n", dllpath );

	// Load the DLL
	if(!( gameuiNext.hInstance = COM_LoadLibrary( dllpath, false, false )))
	{
		// Fallback: try loading from engine directory with standard naming
		string fallback = OS_LIB_PREFIX "GameUI." OS_LIB_EXT;
		Con_Printf( S_WARN "GameUI: primary path failed, trying: %s\n", fallback );
		FS_AllowDirectPaths( true );
		gameuiNext.hInstance = COM_LoadLibrary( fallback, false, true );
		FS_AllowDirectPaths( false );

		if( !gameuiNext.hInstance )
		{
			Con_Printf( S_ERROR "GameUI: can't load GameUI DLL\n" );
			return false;
		}
	}

	// Try GetMenuAPI first (for gameui DLLs that also export the standard API)
	{
		typedef int (*MENUAPI)( UI_FUNCTIONS *, ui_enginefuncs_t *, ui_globalvars_t * );
		MENUAPI GetMenuAPI = (MENUAPI)COM_GetProcAddress( gameuiNext.hInstance, "GetMenuAPI" );
		if( GetMenuAPI )
		{
			Con_Reportf( "GameUI: using GetMenuAPI (dual-mode DLL)\n" );
			gameui.hInstance = gameuiNext.hInstance;

			if( !gameui.mempool )
				gameui.mempool = Mem_AllocPool( "GameUI Pool" );

			// globals already set by UI_LoadProgs caller
			return true; // caller will call GetMenuAPI and complete setup
		}
	}

	// Get CreateInterface
	pfnCreateInterface = (CreateInterfaceFn)COM_GetProcAddress( gameuiNext.hInstance, "CreateInterface" );
	if( !pfnCreateInterface )
	{
		pfnCreateInterface = (CreateInterfaceFn)COM_GetProcAddress( gameuiNext.hInstance, "CreateInterfaceFn" );
	}

	if( !pfnCreateInterface )
	{
		Con_Printf( S_ERROR "GameUI: CreateInterface not found in DLL\n" );
		COM_FreeLibrary( gameuiNext.hInstance );
		gameuiNext.hInstance = NULL;
		return false;
	}

	// Get IGameUI interface (GameUI007)
	retCode = 0;
	pGameUI = pfnCreateInterface( "GameUI007", &retCode );
	if( !pGameUI )
	{
		Con_Printf( S_ERROR "GameUI: IGameUI (GameUI007) not found (retCode=%d)\n", retCode );
		Con_Printf( "GameUI: ensure the DLL exports GameUI007 via CreateInterface\n" );
		COM_FreeLibrary( gameuiNext.hInstance );
		gameuiNext.hInstance = NULL;
		return false;
	}

	// ---- Bridge setup via CreateInterface ----

	// Setup memory pool for gameui
	if( !gameui.mempool )
		gameui.mempool = Mem_AllocPool( "GameUI Pool" );

	// Set engine's gameui.hInstance so all existing checks work
	gameui.hInstance = gameuiNext.hInstance;

	// Setup globals
	if( !gameui.globals )
	{
		static ui_globalvars_t gpGlobals;
		gameui.globals = &gpGlobals;
	}

	// Populate engine's function table with bridge callbacks
	memcpy( &gameui.dllFuncs, &g_GameUIFuncs, sizeof( UI_FUNCTIONS ));

	memset( &gameui.dllFuncs2, 0, sizeof( gameui.dllFuncs2 ));
	gameui.use_extended_api = false;

	// Setup game info
	if( FI && FI->GameInfo )
	{
		Q_strncpy( gameui.gameInfo.gamefolder, FI->GameInfo->gamefolder, sizeof( gameui.gameInfo.gamefolder ));
		Q_strncpy( gameui.gameInfo.title, FI->GameInfo->title, sizeof( gameui.gameInfo.title ));
		gameui.gameInfo.gi_version = GAMEINFO_VERSION;

		if( FI->GameInfo->startmap[0] )
			Q_strncpy( gameui.gameInfo.startmap, FI->GameInfo->startmap, sizeof( gameui.gameInfo.startmap ));
	}

	// Update globals
	gameui.globals->scrWidth = refState.width;
	gameui.globals->scrHeight = refState.height;
	gameui.globals->developer = host.allow_console;
	gameui.globals->time = host.realtime;
	gameui.globals->frametime = host.realframetime;

	Con_Reportf( "GameUI: loaded successfully (CreateInterface bridge, IGameUI=%p)\n", pGameUI );
	return true;
}

/*
=====================
GameUI_UnloadProgs

Unload the GameUI DLL and clean up.
=====================
*/
void GameUI_UnloadProgs( void )
{
	if( !gameuiNext.hInstance && !gameui.hInstance )
		return;

	// Call shutdown via bridge
	if( gameui.dllFuncs.pfnShutdown )
		gameui.dllFuncs.pfnShutdown();

	// Unload the GameUI DLL
	if( gameuiNext.hInstance )
	{
		COM_FreeLibrary( gameuiNext.hInstance );
		gameuiNext.hInstance = NULL;
	}

	// Clean up pool if we own it
	if( gameui.mempool )
	{
		Mem_FreePool( &gameui.mempool );
		gameui.mempool = 0;
	}

	memset( &gameuiNext, 0, sizeof( gameuiNext ));
	Cvar_FullSet( "host_gameuiloaded", "0", FCVAR_READ_ONLY );

	Con_Reportf( "GameUI: unloaded\n" );
}

/*
=====================
GameUI_IsActive

Check if GameUI bridge is currently active.
=====================
*/
qboolean GameUI_IsActive( void )
{
	return ( gameuiNext.hInstance != NULL );
}
