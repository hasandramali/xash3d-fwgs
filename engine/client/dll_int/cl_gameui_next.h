/*
cl_gameui_next.h - GameUI bridge for NextClient menus
Copyright (C) 2024

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#ifndef CL_GAMEUI_NEXT_H
#define CL_GAMEUI_NEXT_H

#include "common.h"
#include "library.h"

//
// GameUI bridge state
//
typedef struct gameui_next_s
{
	qboolean	active;		// is GameUI active
	void		*hInstance;	// GameUI DLL handle

	// Bridge function pointers (wraps IGameUI methods into UI_FUNCTIONS)
	int		(*pfnVidInit)( void );
	void	(*pfnInit)( void );
	void	(*pfnShutdown)( void );
	void	(*pfnRedraw)( float flTime );
	void	(*pfnKeyEvent)( int key, int down );
	void	(*pfnMouseMove)( int x, int y );
	void	(*pfnSetActiveMenu)( int active );
	void	(*pfnGetCursorPos)( int *pos_x, int *pos_y );
	void	(*pfnSetCursorPos)( int pos_x, int pos_y );
	void	(*pfnShowCursor)( int show );
	void	(*pfnCharEvent)( int key );
	int	(*pfnMouseInRect)( void );
	int	(*pfnIsVisible)( void );
	int	(*pfnCreditsActive)( void );
	void	(*pfnFinalCredits)( void );
} gameui_next_t;

extern gameui_next_t gameuiNext;

qboolean GameUI_LoadProgs( void );
void GameUI_UnloadProgs( void );
qboolean GameUI_IsActive( void );

#endif//CL_GAMEUI_NEXT_H
