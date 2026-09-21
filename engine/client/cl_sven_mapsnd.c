/*
* =====================================================================================
*	cl_sven_mapsnd.c
*
*	Client-side prediction of map/entity sounds that Sven Co-op servers never
*	transmit, mirroring what the stock Sven client relies on the client DLL to
*	reproduce itself (its StartSound handler is a read-and-drop stub):
*
*	- melee material hits: svencoop/sound/materials.txt maps the texture under
*	  the crowbar strike to a debris material ("wood" -> debris/woodN.wav) that
*	  the server would have emitted via UTIL_EmitAmbientSound. This is a port of
*	  TEXTURETYPE_PlaySound from hlsdk dlls/sound.cpp (world-hit half).
*	- active looping ambient_generic: origin/message/spawnflags/vol/pitch come
 *	  from the map BSP entity lump; the client animates them locally (the
 *	  0bc76 baseline proves the server never sends the loop start). The
 *	  attenuation constant follows the radius spawnflags (hlsdk sound.cpp).
 *	- brush movers (func_door / func_button): move/stop sounds are annotated
 *	  onto the entity origin updates the server DOES stream, so travelling
 *	  doors and pressed buttons become audible without any server change.
 *	  func_door movesnd/stopsnd select the loop/landing file exactly like
 *	  hlsdk doors.cpp (doors/doormove1..10.wav, doors/doorstop1..8.wav).
 *	- wall chargers (func_recharge / func_healthcharger): while +use is held
 *	  near a recorded charger the first-contact chirp and the loop hum are
 *	  reproduced (hlsdk h_battery.cpp / healthkit.cpp).
 *
 *	Everything is gated by CL_SvenSoundActive() (current map soundcache loaded =
 *	Sven-like server), so vanilla GoldSrc sessions keep server-provided sounds
 *	untouched and there is no double playback with a vanilla StartSound path.
*
*	Foundations only: per-weapon voice prediction, charger hum and
*	scripted_sentence playback are the next layer to build on top of this file.
* =====================================================================================
*/
#include "common.h"
#include "client.h"
#include "sound.h"

qboolean CL_SvenSoundActive( void ); // from cl_parse.c

// ----------------------------------------------------------------------------
// material table (mirrors PM_LoadMaterials / PM_FindTextureType in
// hlsdk pm_shared/pm_shared.c: sound/materials.txt, max 2048 entries)
// ----------------------------------------------------------------------------
#define SVEN_MATERIALS_MAX	2048
#define SVEN_TEXNAMELEN	12	// CBTEXTURENAMEMAX - 1

static char	svenMatTypes[SVEN_MATERIALS_MAX];
static char	svenMatNames[SVEN_MATERIALS_MAX][SVEN_TEXNAMELEN + 1];
static int	svenMatOrder[SVEN_MATERIALS_MAX];
static int	svenMatCount;
static qboolean	svenMatBuilt;


static int SVEN_SortTextures( const void *a, const void *b )
{
	const int *ia = ( const int *)a, *ib = ( const int *)b;

	return strcmp( svenMatNames[*ia], svenMatNames[*ib] );
}

static void CL_SvenMaterialsInit( void )
{
	byte *buf;
	fs_offset_t size, pos;
	int n;
	char line[128];

	if( svenMatBuilt )
		return; // built once per process

	buf = FS_LoadFile( "sound/materials.txt", &size, false );
	if( !buf )
		return;

	for( pos = 0, n = 0; pos < size && n < SVEN_MATERIALS_MAX; )
	{
		fs_offset_t eol;
		char typechar = 0;

		for( eol = pos; eol < size && buf[eol] != '\n'; eol++ );

		if(( eol - pos ) < (fs_offset_t)sizeof( line ) && ( eol - pos ) >= 0 )
		{
			int len = ( int )( eol - pos );

			memcpy( line, &buf[pos], len );
			line[len] = '\0';
			pos = ( eol < size ) ? eol + 1 : size; // skip the newline too

			if( sscanf( line, " %c %12s", &typechar, svenMatNames[n] ) != 2 )
				continue;
			if( !isalpha(( unsigned char )typechar ))
				continue; // comment/blank
			svenMatTypes[n] = toupper(( unsigned char )typechar );
			svenMatOrder[n] = n;
			n++;
		}
		else pos = ( eol < size ) ? eol + 1 : size;
	}
	Mem_Free( buf );

	qsort( svenMatOrder, n, sizeof( svenMatOrder[0] ), SVEN_SortTextures );
	svenMatCount = n;
	svenMatBuilt = true;
	Con_DPrintf( "CL_SvenMaterialsInit: %d materials\n", n );
}

// texture material type for a hit texture, 'C' (concrete) on miss.
// Mirrors hlsdk PM_CatagorizeTextureType's name preprocessing.
char CL_SvenTextureMaterial( const char *name )
{
	char buf[SVEN_TEXNAMELEN + 1];
	int left, right;

	if( !svenMatBuilt )
		CL_SvenMaterialsInit();
	if( !svenMatCount )
		return 'C';

	if( name && ( *name == '-' || *name == '+' ))
		name += 2;
	if( name && ( *name == '{' || *name == '!' || *name == '~' || *name == ' ' ))
		name++;
	if( !name || !name[0] )
		return 'C';

	Q_strncpy( buf, name, sizeof( buf ));
	buf[SVEN_TEXNAMELEN] = '\0';

	// binary search over the sorted 12-char names
	left = 0;
	right = svenMatCount - 1;
	while( left <= right )
	{
		int pivot = ( left + right ) / 2;
		int val = Q_strnicmp( buf, svenMatNames[svenMatOrder[pivot]], SVEN_TEXNAMELEN );

		if( val == 0 )
			return svenMatTypes[svenMatOrder[pivot]];
		if( val > 0 )
			left = pivot + 1;
		else
			right = pivot - 1;
	}

	return 'C';
}

// ----------------------------------------------------------------------------
// melee material hit (port of TEXTURETYPE_PlaySound, the world-hit half)
// ----------------------------------------------------------------------------
// Plays the material debris/tile sound at the impact point, exactly like
// hlsdk's UTIL_EmitAmbientSound( ENT(0), endpos, rgsz[rand], fvol, attn ).
// When 'raw' is NULL the trace is re-run via PM_CL_TraceTexture against the
// physical entity that was hit (world=0 or the brush door/func_wall).
void CL_SvenPlayTextureHit( vec3_t start, vec3_t end, int physent, const char *raw )
{
	const char *rgsz[4];
	const char *snd;
	const char *texname = NULL;
	char type;
	float fvol, fattn = 0.5f;
	int cnt = 0;
	sound_t handle;

	if( !CL_SvenSoundActive())
		return; // vanilla server already tells us everything

	type = 'C';
	if( raw )
	{
		texname = raw;
		type = CL_SvenTextureMaterial( raw );
	}
	else if( clgame.pmove )
	{
		texname = PM_CL_TraceTexture( physent, start, end );
		type = CL_SvenTextureMaterial( texname );
	}

	switch( type )
	{
	// concrete and metal carry no extra debris: the crowbar strike itself
	// (cbar_hit1) is the whole sound on cement/metal, exactly as the user
	// specified (metal-only, no spray/splash filler on those materials).
	default:
	case 'C':
	case 'M':	return;
	case 'D':	fvol = 0.9f;	fattn = 0.5f;
				rgsz[0] = "player/pl_dirt1.wav"; rgsz[1] = "player/pl_dirt2.wav"; rgsz[2] = "player/pl_dirt3.wav"; cnt = 3; break;
	case 'V':	fvol = 0.5f;	fattn = 0.5f;
				rgsz[0] = "player/pl_duct1.wav"; rgsz[1] = "player/pl_duct1.wav"; cnt = 2; break;
	case 'G':	fvol = 0.9f;	fattn = 0.5f;
				rgsz[0] = "player/pl_grate1.wav"; rgsz[1] = "player/pl_grate4.wav"; cnt = 2; break;
	case 'T':	fvol = 0.8f;	fattn = 0.5f;
				rgsz[0] = "player/pl_tile1.wav"; rgsz[1] = "player/pl_tile3.wav"; rgsz[2] = "player/pl_tile2.wav"; rgsz[3] = "player/pl_tile4.wav"; cnt = 4; break;
	case 'S':	fvol = 0.9f;	fattn = 0.5f;
				rgsz[0] = "player/pl_slosh1.wav"; rgsz[1] = "player/pl_slosh3.wav"; rgsz[2] = "player/pl_slosh2.wav"; rgsz[3] = "player/pl_slosh4.wav"; cnt = 4; break;
	case 'W':	fvol = 0.9f;	fattn = 0.4f;
				rgsz[0] = "debris/wood1.wav"; rgsz[1] = "debris/wood2.wav"; rgsz[2] = "debris/wood3.wav"; cnt = 3; break;
	case 'Y':
	case 'P':	fvol = 0.8f;	fattn = 0.4f;
				rgsz[0] = "debris/glass1.wav"; rgsz[1] = "debris/glass2.wav"; rgsz[2] = "debris/glass3.wav"; cnt = 3; break;
	case 'F':	return; // crowbar already makes the hit sound on the body
	}

	// computers throw a random spark (hlsdk: buttons/spark5|6)
	if( type == 'P' && COM_RandomLong( 0, 1 ))
	{
		snd = COM_RandomLong( 0, 1 ) ? "buttons/spark6.wav" : "buttons/spark5.wav";
		handle = S_RegisterSound( snd );
		if( handle )
			S_AmbientSound( end, 0, handle, COM_RandomFloat( 0.7f, 1.0f ), 0.5f, 100, 0 );
	}

	snd = rgsz[COM_RandomLong( 0, cnt - 1 )];
	handle = S_RegisterSound( snd );
	if( handle )
		S_AmbientSound( end, 0, handle, fvol, fattn, COM_RandomLong( 96, 111 ), 0 );
}

// ----------------------------------------------------------------------------
// map BSP entity scan: ambient_generic + func_button 'sounds' selector
// (fact from server.dll: "sounds" = atoi(N), NEVER networked, table below)
// + func_door movesnd/stopsnd (selector unanswered yet, keep origin for when
// the client copy of the door table arrives)
// ----------------------------------------------------------------------------
#define SVEN_AMBIENTS_MAX	64
#define SVEN_ENTSND_MAX		256
#define SVEN_MOVER_RANGE	700.0f
#define SVEN_DOOR_MATCH	256.0f	// rest-origin match radius for door records
#define SVEN_CHARGER_RANGE	256.0f
#define SVEN_MOVE_EPS		0.35f
#define SVEN_SMALLBRUSH	64.0f

// func_button "sounds" -> wav (verified statically in server.dll image:
// buttons/button1..11 + latch/switch/lever, selector at m_sounds).
static const char *Sven_ButtonSound( int sounds )
{
	switch( sounds )
	{
	case 1:  return "buttons/button1.wav";
	case 2:  return "buttons/button2.wav";
	case 3:  return "buttons/button3.wav";
	case 4:  return "buttons/button4.wav";
	case 5:  return "buttons/button5.wav";
	case 6:  return "buttons/button6.wav";
	case 7:  return "buttons/button7.wav";
	case 8:  return "buttons/button8.wav";
	case 9:  return "buttons/button9.wav";
	case 10: return "buttons/button10.wav";
	case 11: return "buttons/button11.wav";
	case 12: return "buttons/latchlocked1.wav";
	case 13: return "buttons/latchunlocked1.wav";
	case 14: return "buttons/lightswitch2.wav";
	case 15: return "buttons/lever1.wav";
	case 16: return "buttons/lever2.wav";
	case 17: return "buttons/lever3.wav";
	case 18: return "buttons/lever4.wav";
	case 19: return "buttons/lever5.wav";
	default: return NULL; // sounds=0 or unset: mapper/death default
	}
}

typedef struct
{
	vec3_t	origin;
	char	message[128];
	int	spawnflags;
	int	vol;		// "vol" keyvalue, hi-nibble 0-10 scale (precached cap)
	int	pitch;		// "pitch" keyvalue (0-255, default 100)
	float	attn;		// derived from the radius spawnflags
	qboolean activeLoop;
	sound_t	handle;
	qboolean started;
} sven_ambient_t;

typedef struct
{
	vec3_t	origin;
	int	sounds;		// func_button "sounds" selector (m_sounds)
} sven_button_t;

typedef struct
{
	vec3_t	origin;
	int	movesnd;	// func_door movesnd selector -> doors/doormoveN.wav
	int	stopsnd;	// func_door stopsnd selector -> doors/doorstopN.wav
} sven_door_t;

// func_recharge (suit) & func_healthcharger wall chargers: the server links
// the hum loop to the map entity, which never arrives on the client.
typedef struct
{
	vec3_t	origin;
	qboolean isSuit;	// false = health charger
	float	checkAt;
	qboolean onedone;	// first-contact chirp already played
	qboolean humming;	// hum loop currently running (our channel)
} sven_charger_t;

static sven_ambient_t	svenAmbients[SVEN_AMBIENTS_MAX];
static int		svenAmbientsCount;
static sven_button_t	svenButtons[SVEN_ENTSND_MAX];
static int		svenButtonsCount;
static sven_door_t	svenDoors[SVEN_ENTSND_MAX];
static int		svenDoorsCount;
static sven_charger_t	svenChargers[SVEN_ENTSND_MAX];
static int		svenChargersCount;
static vec3_t		*svenMoverLast;	// per-entity last origin
static byte		*svenMoverMove;	// per-entity moving/idle flag
static byte		*svenMoverKnown;	// per-entity first-sight flag
static sound_t		*svenMoverLoop;	// per-entity active travelloop handle
static int		svenMoverCap;
static char		svenMapSndMap[MAX_QPATH];

static void CL_SvenMapSoundsInit( void );

static int CL_SvenButtonByOrigin( const vec3_t origin, float range )
{
	int i, best = -1;
	float bestdist = range;

	for( i = 0; i < svenButtonsCount; i++ )
	{
		float d = VectorDistance( origin, svenButtons[i].origin );

		if( d < bestdist )
		{
			bestdist = d;
			best = i;
		}
	}
	return best;
}

// nearest recorded func_door by world origin (the door sits at its recorded
// origin while at rest, its travel loop slot is on the same still point).
static int CL_SvenDoorByOrigin( const vec3_t origin, float range )
{
	int i, best = -1;
	float bestdist = range;

	for( i = 0; i < svenDoorsCount; i++ )
	{
		float d = VectorDistance( origin, svenDoors[i].origin );

		if( d < bestdist )
		{
			bestdist = d;
			best = i;
		}
	}
	return best;
}

// func_door selects its travel/arrival sounds with movesnd/stopsnd, the
// exact hlsdk doors.cpp table -> doors/doormove1..10.wav & doorstop1..8.wav,
// 0 selects silence (common/null.wav). svencoop ships the identical files.
static const char *Sven_DoorMoveSound( int movesnd )
{
	switch( movesnd )
	{
	case 1:  return "doors/doormove1.wav";
	case 2:  return "doors/doormove2.wav";
	case 3:  return "doors/doormove3.wav";
	case 4:  return "doors/doormove4.wav";
	case 5:  return "doors/doormove5.wav";
	case 6:  return "doors/doormove6.wav";
	case 7:  return "doors/doormove7.wav";
	case 8:  return "doors/doormove8.wav";
	case 9:  return "doors/doormove9.wav";
	case 10: return "doors/doormove10.wav";
	default: return NULL; // movesnd=0 or unset: silent door
	}
}

static const char *Sven_DoorStopSound( int stopsnd )
{
	switch( stopsnd )
	{
	case 1:  return "doors/doorstop1.wav";
	case 2:  return "doors/doorstop2.wav";
	case 3:  return "doors/doorstop3.wav";
	case 4:  return "doors/doorstop4.wav";
	case 5:  return "doors/doorstop5.wav";
	case 6:  return "doors/doorstop6.wav";
	case 7:  return "doors/doorstop7.wav";
	case 8:  return "doors/doorstop8.wav";
	default: return NULL; // stopsnd=0 or unset: silent landing
	}
}

// func_recharge (suit) chap + hum sequence (hlsdk h_battery.cpp):
// first contact -> items/suitchargeok1.wav once, then the loop hum.
static const char *Sven_ChargerChirp( const sven_charger_t *c )
{
	return c->isSuit ? "items/suitchargeok1.wav" : "items/medshot4.wav";
}

static const char *Sven_ChargerHum( const sven_charger_t *c )
{
	return c->isSuit ? "items/suitcharge1.wav" : "items/medcharge4.wav";
}

// ambient_generic attenuation follows the radius spawnflags exactly like
// hlsdk sound.cpp Spawn(): EVERYWHERE=1 -> ATTN_NONE, SMALLRADIUS=2 ->
// ATTN_IDLE, MEDIUMRADIUS=4 -> ATTN_STATIC, LARGERADIUS=8 -> ATTN_NORM,
// no radius bit -> ATTN_STATIC default.
static float Sven_AmbientAttn( int sflags )
{
	if( sflags & 1 ) return ATTN_NONE;
	if( sflags & 2 ) return ATTN_IDLE;
	if( sflags & 8 ) return ATTN_NORM;
	return ATTN_STATIC; // explicit MEDIUMRADIUS(4) and no-bit default
}

static void CL_SvenMapSoundsInit( void )
{
	// new map (or first call): drop everything and re-read the entity lump
	if( !svenMapSndMap[0] || Q_strcmp( svenMapSndMap, clgame.mapname ))
	{
		svenAmbientsCount = 0;
		memset( svenAmbients, 0, sizeof( svenAmbients ));
		svenButtonsCount = 0;
		svenDoorsCount = 0;
		svenChargersCount = 0;
		Q_strncpy( svenMapSndMap, clgame.mapname, sizeof( svenMapSndMap ));
	}

	// lazy per-entity buffers, sized to the connection's entity limit
	if( !svenMoverCap || svenMoverCap < clgame.maxEntities )
	{
		if( svenMoverLast )
			Mem_Free( svenMoverLast );
		if( svenMoverMove )
			Mem_Free( svenMoverMove );
		if( svenMoverKnown )
			Mem_Free( svenMoverKnown );
		if( svenMoverLoop )
			Mem_Free( svenMoverLoop );

		svenMoverCap = clgame.maxEntities;
		svenMoverLast = ( vec3_t *)Mem_Malloc( cls.mempool, svenMoverCap * sizeof( vec3_t ));
		svenMoverMove = ( byte *)Mem_Malloc( cls.mempool, svenMoverCap );
		svenMoverKnown = ( byte *)Mem_Malloc( cls.mempool, svenMoverCap );
		svenMoverLoop = ( sound_t *)Mem_Malloc( cls.mempool, svenMoverCap * sizeof( sound_t ));
		memset( svenMoverMove, 0, svenMoverCap );
		memset( svenMoverKnown, 0, svenMoverCap );
		memset( svenMoverLoop, 0, svenMoverCap * sizeof( sound_t ));
	}

	if( !cl.worldmodel || !cl.worldmodel->entities )
		return;

	// walk the world entity lump with the same COM_ParseFile scanner as
	// Mod_LoadEntities, collecting ambient_generic + moveable brush records.
	{
		char token[MAX_TOKEN];
		char keyname[64];
		char *pfile = ( char *)cl.worldmodel->entities;

		while(( pfile = COM_ParseFile( pfile, token, sizeof( token ))) != NULL )
		{
			if( token[0] != '{' )
				break;

			char cls[128] = "";
			char msg[128] = "";
			vec3_t org = { 0.0f, 0.0f, 0.0f };
			int sflags = 0, sounds = 0, movesnd = 0, stopsnd = 0;
			int vol = 0, pitch = 0;

			while( 1 )
			{
				if(( pfile = COM_ParseFile( pfile, token, sizeof( token ))) == NULL )
					break;
				if( token[0] == '}' )
					break;
				Q_strncpy( keyname, token, sizeof( keyname ));
				if(( pfile = COM_ParseFile( pfile, token, sizeof( token ))) == NULL )
					break;
				if( token[0] == '}' )
					break;

				if( !Q_stricmp( keyname, "classname" ))
					Q_strncpy( cls, token, sizeof( cls ));
				else if( !Q_stricmp( keyname, "message" ))
					Q_strncpy( msg, token, sizeof( msg ));
				else if( !Q_stricmp( keyname, "spawnflags" ))
					sflags = Q_atoi( token );
				else if( !Q_stricmp( keyname, "sounds" ))
					sounds = Q_atoi( token );
				else if( !Q_stricmp( keyname, "movesnd" ))
					movesnd = Q_atoi( token );
				else if( !Q_stricmp( keyname, "stopsnd" ))
					stopsnd = Q_atoi( token );
				else if( !Q_stricmp( keyname, "vol" ) || !Q_stricmp( keyname, "volume" ))
					vol = Q_atoi( token );
				else if( !Q_stricmp( keyname, "pitch" ))
					pitch = Q_atoi( token );
				else if( !Q_stricmp( keyname, "origin" ))
					sscanf( token, "%f %f %f", &org[0], &org[1], &org[2] );
			}

			if( !Q_stricmp( cls, "ambient_generic" ) && msg[0] && svenAmbientsCount < SVEN_AMBIENTS_MAX )
			{
				sven_ambient_t *a = &svenAmbients[svenAmbientsCount];

				VectorCopy( org, a->origin );
				Q_strncpy( a->message, msg, sizeof( a->message ));
				a->spawnflags = sflags;
				// SF_AMBIENT_START_SILENT=16, SF_AMBIENT_NOT_LOOPING=32:
				// active forever-loops start at map start; one-shot start
				// sounds play once; use-triggered ones stay server-side.
				a->activeLoop = !( sflags & ( 16 | 32 ));
				// "vol" runs on a 0-10 scale in these maps ("warn1" uses 7);
				// a fvol of clamp(vol,1,10)/10 reaches ~full at 10 and at the
				// classic full-scale values (100/255) without over-driving.
				a->vol = vol;
				a->pitch = pitch ? pitch : 100;
				a->attn = Sven_AmbientAttn( sflags );
				svenAmbientsCount++;
			}
			else if( !Q_stricmp( cls, "func_button" ) && svenButtonsCount < SVEN_ENTSND_MAX )
			{
				sven_button_t *b = &svenButtons[svenButtonsCount];

				VectorCopy( org, b->origin );
				b->sounds = sounds;
				svenButtonsCount++;
			}
			else if(( !Q_stricmp( cls, "func_door" ) || !Q_stricmp( cls, "func_door_rotating" ))
				&& svenDoorsCount < SVEN_ENTSND_MAX )
			{
				sven_door_t *d = &svenDoors[svenDoorsCount];

				VectorCopy( org, d->origin );
				d->movesnd = movesnd;
				d->stopsnd = stopsnd;
				svenDoorsCount++;
			}
			else if(( !Q_stricmp( cls, "func_recharge" ) || !Q_stricmp( cls, "func_healthcharger" ))
				&& svenChargersCount < SVEN_ENTSND_MAX )
			{
				sven_charger_t *c = &svenChargers[svenChargersCount];

				VectorCopy( org, c->origin );
				c->isSuit = !Q_stricmp( cls, "func_recharge" );
				c->checkAt = 0.0f;
				svenChargersCount++;
			}
		}
	}
}

// per-frame map sound prediction. Called each ClientFrame after movement
// prediction; only active on Sven-like servers (soundcache loaded).
void CL_SvenPredictMapSounds( void )
{
	int i;

	if( !CL_SvenSoundActive())
		return;

	if( !svenMoverCap || Q_strcmp( svenMapSndMap, clgame.mapname ))
		CL_SvenMapSoundsInit();
	if( !svenMoverCap )
		return;

	// ambient loops start once when their slot has never started its sound
	for( i = 0; i < svenAmbientsCount; i++ )
	{
		sven_ambient_t *a = &svenAmbients[i];

		if( !a->activeLoop || a->started )
			continue;
		if( a->message[0] == '!' )
			continue; // sentence, not a wav: future layer

		a->handle = S_RegisterSound( a->message );
		if( a->handle )
		{
			float fvol = bound( 0.0f, ( a->vol ? a->vol : 10 ) / 10.0f, 1.0f );

			S_AmbientSound( a->origin, 0, a->handle, fvol, a->attn, a->pitch, 0 );
			a->started = true;
			Con_DPrintf( "SVEN-AMB: started loop '%s' vol=%.2f attn=%.2f pitch=%d\n",
				a->message, fvol, a->attn, a->pitch );
		}
	}

	// wall chargers: while +use is held near a recorded charger, reproduce
	// the first-contact chirp then hang the charging hum on a loop slot
	// (mirrors hlsdk CRecharge::Off / CHealthCharger::Off). The hum stop on
	// release is distance-independent so walking off can't strand a loop.
	{
		qboolean useflag = ( cl.cmd.buttons & IN_USE ) != 0;

		for( i = 0; i < svenChargersCount; i++ )
		{
			sven_charger_t *c = &svenChargers[i];
			sound_t chirp, hum;

			if( useflag )
			{
				if( VectorDistance( c->origin, cl.simorg ) > SVEN_CHARGER_RANGE )
					continue;

				if( !c->humming )
				{
					if( !c->onedone )
					{
						chirp = S_RegisterSound( Sven_ChargerChirp( c ));
						if( chirp )
							S_AmbientSound( c->origin, 0, chirp, c->isSuit ? 0.85f : 1.0f, ATTN_NORM, 100, 0 );
						c->onedone = true;
						c->checkAt = 0.5f + cl.time;
					}
					if( c->checkAt && cl.time >= c->checkAt )
					{
						hum = S_RegisterSound( Sven_ChargerHum( c ));
						if( hum )
						{
							S_AmbientSound( c->origin, 0, hum, c->isSuit ? 0.85f : 1.0f, ATTN_NORM, 100, 0 );
							c->humming = true;
							c->checkAt = 0.0f;
							Con_DPrintf( "SVEN-CHG: hum on '%s'\n", Sven_ChargerHum( c ));
						}
					}
				}
			}
			else if( c->humming || ( c->onedone && cl.time >= c->checkAt ))
			{
				// release (walked off or let go): cut the loop, forget chirp
				if( c->humming )
				{
					hum = S_RegisterSound( Sven_ChargerHum( c ));
					if( hum )
						S_AmbientSound( c->origin, 0, hum, 0, 0, 0, SND_STOP );
				}
				c->humming = false;
				c->onedone = false;
				c->checkAt = 0.0f;
				Con_DPrintf( "SVEN-CHG: hum off\n" );
			}
		}
	}

	// brush mover detection: annotate travel sounds onto the entity origins
	// the server streams (doors/buttons move, so their position updates).
	for( i = 1; i < clgame.maxEntities; i++ )
	{
		cl_entity_t *ent = &clgame.entities[i];
		vec3_t delta;
		float dist, diag;
		qboolean moving, hasbox, isbutton;

		if( i == ( cl.playernum + 1 ))
			continue;
		if( !ent->model || ent->model->type != mod_brush )
			continue;
		if( !ent->curstate.solid && ent->curstate.messagenum != cl.parsecount )
			continue;

		VectorSubtract( ent->curstate.origin, svenMoverLast[i], delta );
		dist = VectorLength( delta );
		moving = ( dist > SVEN_MOVE_EPS );

		if( svenMoverKnown[i] && ( moving != svenMoverMove[i] ) && ( dist > SVEN_MOVE_EPS || !moving ))
		{
			const char *snd = NULL;
			sound_t handle;
			int di = -1;

			// bbox may not be transmitted for movers; treat lack of it as a
			// door (large movers) instead of a phantom tiny "button"
			hasbox = !VectorIsNull( ent->curstate.maxs ) && !VectorIsNull( ent->curstate.mins );
			diag = hasbox ? ( VectorLength( ent->curstate.maxs ) - VectorLength( ent->curstate.mins )) : 0.0f;
			isbutton = hasbox && diag < SVEN_SMALLBRUSH;

			// match the mover to its recorded func_door by its rest origin
			// (at rest the brush sits exactly on the recorded origin)
			if( !isbutton )
				di = CL_SvenDoorByOrigin( ent->curstate.origin, SVEN_DOOR_MATCH );

			if( VectorDistance( ent->curstate.origin, cl.simorg ) < SVEN_MOVER_RANGE )
			{
				if( moving && isbutton )
				{
					// press: match against the map's func_button record -> its
					// own "sounds" selector (m_sounds), exact server table
					int bi = CL_SvenButtonByOrigin( ent->curstate.origin, SVEN_SMALLBRUSH );

					snd = ( bi >= 0 ) ? Sven_ButtonSound( svenButtons[bi].sounds ) : NULL;
					if( !snd ) snd = "buttons/button1.wav"; // default button press
				}
				else if( moving )
				{
					// travel: the door's movesnd selector picks the loop file
					// (0 => silent door, no loop at all)
					snd = ( di >= 0 ) ? Sven_DoorMoveSound( svenDoors[di].movesnd ) : NULL;
					if( !snd ) snd = "doors/doormove1.wav";
				}
				else if( !isbutton )
				{
					// landing: silence the running travel loop, then dent the
					// arrival; stopsnd=0 (or a silent travel) still allows the
					// default landing sound unless stopsnd names silence
					if( svenMoverLoop[i] )
					{
						handle = svenMoverLoop[i];
						S_AmbientSound( ent->curstate.origin, i, handle, 0, 0, 0, SND_STOP );
						svenMoverLoop[i] = 0;
					}
					snd = NULL;
					if( di >= 0 )
						snd = Sven_DoorStopSound( svenDoors[di].stopsnd );
					if( !snd ) snd = "doors/doorstop1.wav";
				}
				// button release stays silent

				if( snd )
				{
					handle = S_RegisterSound( snd );
					if( handle )
					{
						if( moving && !isbutton )
							svenMoverLoop[i] = handle; // remember for the stop
						S_AmbientSound( ent->curstate.origin, i, handle, 1.0f, ATTN_NORM, 100, 0 );
					}
				}
			}
		}

		VectorCopy( ent->curstate.origin, svenMoverLast[i] );
		svenMoverMove[i] = moving ? 1 : 0;
		svenMoverKnown[i] = 1;
	}
}