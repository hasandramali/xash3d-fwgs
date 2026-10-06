/*
bootmap.c -- embedded background boot map (_xashnull)

The launcher always passes +map_background _xashnull so the renderer
is fed from the first seconds (black-screen guard on some devices), but
the BSP is not part of the distributed game data on fresh installs. The
stock 2.6KB map (worldspawn + one spawn, zero render lumps, pure black)
is embedded into the engine binary (bootmap_blob.c) and extracted to the
write dir on first use, together with the tiny _xashnull_load.cfg boot
script ("wait;disconnect") that drops back to the menu once the
background has settled (see Host_ServerFrame). Present files are never
touched: existing maps/_xashnull.bsp or _xashnull_load.cfg always win.
*/
#include "common.h"

extern const unsigned char g_bootmap_xashnull[];
extern const unsigned int g_bootmap_xashnull_len;
extern const unsigned char g_bootmap_xashnull_cfg[];
extern const unsigned int g_bootmap_xashnull_cfg_len;

static qboolean BootMap_EnsureFile( const char *path, const unsigned char *data, unsigned int len )
{
	// Already on disk (dev data, a previous extract, or a user-modified
	// copy): never overwrite, whatever its size.
	if( FS_FileExists( path, false ))
		return true;

	Con_Printf( "BootMap: extracting embedded %s (%u bytes)...\n", path, len );

	if( !FS_WriteFile( path, data, len ))
	{
		Con_Printf( S_ERROR "BootMap: failed to write %s (storage read-only?)\n", path );
		return false;
	}

	return FS_FileExists( path, false );
}

qboolean BootMap_Ensure( const char *mapname )
{
	// Not our map: nothing to do (still counts as "ensured" for the caller).
	if( Q_stricmp( mapname, BOOTMAP_NAME ))
		return true;

	if( !BootMap_EnsureFile( BOOTMAP_PATH, g_bootmap_xashnull, g_bootmap_xashnull_len ))
		return false;

	// The boot script is optional: if it can't be written, the background
	// simply stays up (renderer still kicked) instead of dropping to menu.
	BootMap_EnsureFile( BOOTMAP_LOADCFG, g_bootmap_xashnull_cfg, g_bootmap_xashnull_cfg_len );

	return true;
}
