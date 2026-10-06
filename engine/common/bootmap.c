/*
bootmap.c -- embedded background boot map (_server_start)

The launcher always passes +map_background _server_start so the renderer
is fed from the first seconds (black-screen guard on some devices), but
the BSP is not part of the distributed game data on fresh installs. To
avoid searching the device for it, the stock 10KB map is embedded into
the engine binary (bootmap_blob.c) and extracted to the write dir on
first use. Present files are never touched: a mapper's own
maps/_server_start.bsp always wins.
*/
#include "common.h"

extern const unsigned char g_bootmap_server_start[];
extern const unsigned int g_bootmap_server_start_len;

#define BOOTMAP_NAME	"_server_start"
#define BOOTMAP_PATH	"maps/_server_start.bsp"

qboolean BootMap_Ensure( const char *mapname )
{
	// Not our map: nothing to do (still counts as "ensured" for the caller).
	if( Q_stricmp( mapname, BOOTMAP_NAME ))
		return true;

	// Already on disk (dev data, a previous extract, or a mapper's own
	// version): never overwrite, whatever its size.
	if( FS_FileExists( BOOTMAP_PATH, false ))
		return true;

	Con_Printf( "BootMap: extracting embedded %s (%u bytes)...\n", BOOTMAP_PATH, g_bootmap_server_start_len );

	if( !FS_WriteFile( BOOTMAP_PATH, g_bootmap_server_start, g_bootmap_server_start_len ))
	{
		Con_Printf( S_ERROR "BootMap: failed to write %s (storage read-only?)\n", BOOTMAP_PATH );
		return false;
	}

	return FS_FileExists( BOOTMAP_PATH, false );
}
