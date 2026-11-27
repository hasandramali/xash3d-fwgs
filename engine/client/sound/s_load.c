/*
s_load.c - sounds managment
Copyright (C) 2007 Uncle Mike

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
#include "sound.h"
#include "sven_sound.h"
#include "platform/platform.h"

// during registration it is possible to have more sounds
// than could actually be referenced during gameplay,
// because we don't want to free anything until we are
// sure we won't need it.
#define MAX_SFX      8192
#define MAX_SFX_HASH (MAX_SFX/4)

// sounds up to this size are decoded before long tracks while preloading,
// so short gameplay samples are ready first
#define S_PRELOAD_SMALL_FILE ( 256 * 1024 )

static int      s_numSfx = 0;
static sfx_t    s_knownSfx[MAX_SFX];
static char     *s_fullSfxNames[MAX_SFX];
static sfx_t    *s_sfxHashList[MAX_SFX_HASH];
static qboolean s_registering = false;

// decoded sound cache accounting and LRU bookkeeping
static size_t   s_cachedBytes;            // decoded PCM held by s_knownSfx
static size_t   s_sfxBytes[MAX_SFX];      // decoded size of each sfx slot
static double   s_sfxLastUse[MAX_SFX];    // LRU stamp of each sfx slot
static double   s_sfxUseClock;            // monotonic LRU stamp source
static byte     s_sfxInUse[MAX_SFX];      // scratch flags for the eviction pass

// frame-budgeted preload set up by S_EndRegistration
static qboolean s_preloadPending;         // a preload pass is waiting for frames
static int      s_preloadCursor;          // current slot of the preload scan
static qboolean s_preloadLongTracks;      // second scan: long tracks only
static qboolean s_preloadDone;            // preload pass finished
static qboolean s_preloadBudgetHit;       // eager loading budget already reached

#define SENTENCE_INDEX -99999 // unique sentence index
static string   s_sentenceImmediateName;	// keep dummy sentence name

/*
=================
S_SoundList_f
=================
*/
void S_SoundList_f( void )
{
	sfx_t	*sfx;
	int	i, totalSfx = 0;
	int	totalSize = 0;

	for( i = 0, sfx = s_knownSfx; i < s_numSfx; i++, sfx++ )
	{
		if( !sfx->name[0] )
			continue;

		wavdata_t *sc = sfx->cache;
		if( sc )
		{
			totalSize += sc->size;

			if( FBitSet( sc->flags, SOUND_LOOPED ))
				Con_Printf( "L" );
			else
				Con_Printf( " " );

			if( sfx->name[0] == '*' || !Q_strncmp( sfx->name, DEFAULT_SOUNDPATH, sizeof( DEFAULT_SOUNDPATH ) - 1 ))
				Con_Printf( " (%2db) %s : %s\n", sc->width * 8, Q_memprint( sc->size ), sfx->name );
			else Con_Printf( " (%2db) %s : " DEFAULT_SOUNDPATH "%s\n", sc->width * 8, Q_memprint( sc->size ), sfx->name );
			totalSfx++;
		}
	}

	Con_Printf( "-------------------------------------------\n" );
	Con_Printf( "%i total sounds\n", totalSfx );
	Con_Printf( "%s total memory\n", Q_memprint( totalSize ));
	Con_Printf( "\n" );
}

/*
=================
S_CreateDefaultSound
=================
*/
static wavdata_t *S_CreateDefaultSound( void )
{
	uint samples = SOUND_DMA_SPEED;
	uint channels = 1;
	uint width = 2;
	size_t size = samples * width * channels;

	wavdata_t *sc = Mem_Calloc( sndpool, sizeof( wavdata_t ) + size );

	sc->width = width;
	sc->channels = channels;
	sc->rate = SOUND_DMA_SPEED;
	sc->samples = samples;
	sc->size = size;

	return sc;
}

/*
=================
S_AccountSound

remember how much decoded PCM a sfx slot holds and mark it as used now
=================
*/
static void S_AccountSound( sfx_t *sfx )
{
	int index;
	size_t bytes;

	if( sfx < s_knownSfx || sfx >= s_knownSfx + MAX_SFX )
		return;

	index = sfx - s_knownSfx;
	bytes = sfx->cache ? sizeof( wavdata_t ) + sfx->cache->size : 0;

	if( s_sfxBytes[index] )
		s_cachedBytes -= Q_min( s_sfxBytes[index], s_cachedBytes );

	s_sfxBytes[index] = bytes;
	s_cachedBytes += bytes;
	s_sfxLastUse[index] = ++s_sfxUseClock;
}

/*
=================
S_ForgetSound

stop accounting cached PCM without touching the cache itself
=================
*/
static void S_ForgetSound( sfx_t *sfx )
{
	int index;

	if( sfx < s_knownSfx || sfx >= s_knownSfx + MAX_SFX )
		return;

	index = sfx - s_knownSfx;

	if( s_sfxBytes[index] )
		s_cachedBytes -= Q_min( s_sfxBytes[index], s_cachedBytes );

	s_sfxBytes[index] = 0;
	s_sfxLastUse[index] = 0;
}

/*
=================
S_UncacheSound

drop the decoded PCM of a sound but keep its registration, so it can be
loaded again from disk when it is needed
=================
*/
void S_UncacheSound( sfx_t *sfx )
{
	if( !sfx || !sfx->cache )
		return;

	S_ForgetSound( sfx );
	FS_FreeSound( sfx->cache );
	sfx->cache = NULL;
}

/*
=================
S_SoundCacheSize

amount of decoded PCM currently held by the sound cache
=================
*/
size_t S_SoundCacheSize( void )
{
	return s_cachedBytes;
}

/*
=================
S_LoadSound
=================
*/
const char *S_SoundName( const sfx_t *sfx )
{
	int index = sfx - s_knownSfx;
	return s_fullSfxNames[index] ? s_fullSfxNames[index] : sfx->name;
}

static wavdata_t *S_LoadSoundInternal( sfx_t *sfx, qboolean allowDefault )
{
	wavdata_t	*sc = NULL;

	if( !sfx ) return NULL;

	// see if still in memory
	if( sfx->cache )
	{
		// a cache hit is also a use: keep the LRU order fresh
		s_sfxLastUse[sfx - s_knownSfx] = ++s_sfxUseClock;
		return sfx->cache;
	}

	if( COM_StringEmptyOrNULL( sfx->name ))
		return NULL;

	// load it from disk
	const char *name = S_SoundName( sfx );
	if( Q_stricmp( name, "*default" ))
	{
		// load it from disk
		if( s_warn_late_precache.value > 0 && cls.state == ca_active )
			Con_Printf( S_WARN "%s: late precache of %s\n", __func__, sfx->name );

		if( name[0] == '*' )
			sc = FS_LoadSound( name + 1, NULL, 0 );
		else
			sc = FS_LoadSound( name, NULL, 0 );
	}

	if( !sc )
	{
		if( !allowDefault ) return NULL;
		sc = S_CreateDefaultSound();
	}

	if( sc->rate < SOUND_11k ) // some bad sounds
		Sound_Process( &sc, SOUND_11k, sc->width, sc->channels, SOUND_RESAMPLE );
	else if( sc->rate > SOUND_11k && sc->rate < SOUND_22k ) // some bad sounds
		Sound_Process( &sc, SOUND_22k, sc->width, sc->channels, SOUND_RESAMPLE );
	else if( sc->rate > SOUND_22k && sc->rate != SOUND_44k ) // some bad sounds
		Sound_Process( &sc, SOUND_44k, sc->width, sc->channels, SOUND_RESAMPLE );

	sfx->cache = sc;
	S_AccountSound( sfx );

	return sfx->cache;
}

wavdata_t *S_LoadSound( sfx_t *sfx )
{
	return S_LoadSoundInternal( sfx, true );
}

/*
==================
S_FindName

==================
*/
sfx_t *S_FindName( const char *pname, qboolean *pfInCache )
{
	sfx_t	*sfx;
	uint	i;
	char	name[SVEN_SOUND_PATH];

	if( COM_StringEmptyOrNULL( pname ) || !snd.initialized )
		return NULL;

	if( Q_strlen( pname ) >= sizeof( name ))
		return NULL;

	Q_strncpy( name, pname, sizeof( name ));
	COM_FixSlashes( name );

	// see if already loaded
	uint hash = COM_HashKey( name, MAX_SFX_HASH );
	for( sfx = s_sfxHashList[hash]; sfx; sfx = sfx->hashNext )
	{
		if( !Q_strcmp( S_SoundName( sfx ), name ))
		{
			if( pfInCache )
			{
				// indicate whether or not sound is currently in the cache.
				*pfInCache = ( sfx->cache != NULL ) ? true : false;
			}
			// prolonge registration
			sfx->servercount = cl.servercount;
			return sfx;
		}
	}

	// find a free sfx slot spot
	for( i = 0, sfx = s_knownSfx; i < s_numSfx; i++, sfx++)
		if( !sfx->name[0] ) break; // free spot

	if( i == s_numSfx )
	{
		if( s_numSfx == MAX_SFX )
			return NULL;
		s_numSfx++;
	}

	sfx = &s_knownSfx[i];
	memset( sfx, 0, sizeof( *sfx ));
	if( pfInCache ) *pfInCache = false;
	Q_strncpy( sfx->name, name, sizeof( sfx->name ));
	if( Q_strlen( name ) >= sizeof( sfx->name ))
	{
		s_fullSfxNames[i] = Mem_Malloc( sndpool, Q_strlen( name ) + 1 );
		Q_strncpy( s_fullSfxNames[i], name, Q_strlen( name ) + 1 );
	}
	sfx->servercount = cl.servercount;
	sfx->hashValue = hash;

	// link it in
	sfx->hashNext = s_sfxHashList[sfx->hashValue];
	s_sfxHashList[sfx->hashValue] = sfx;

	return sfx;
}

/*
==================
S_FreeSound
==================
*/
void S_FreeSound( sfx_t *sfx )
{
	if( !sfx || !sfx->name[0] )
		return;

	// de-link it from the hash tree
	sfx_t **prev = &s_sfxHashList[sfx->hashValue];
	while( 1 )
	{
		sfx_t *hashSfx = *prev;
		if( !hashSfx )
			break;

		if( hashSfx == sfx )
		{
			*prev = hashSfx->hashNext;
			break;
		}
		prev = &hashSfx->hashNext;
	}

	if( clgame.soundFuncs.pfnS_FreeSound )
	{
		clgame.soundFuncs.pfnS_FreeSound( sfx, sfx - s_knownSfx );
		S_ForgetSound( sfx );
	}
	else
	{
		S_UncacheSound( sfx );
	}
	if( s_fullSfxNames[sfx - s_knownSfx] )
		Mem_Free( s_fullSfxNames[sfx - s_knownSfx] );
	s_fullSfxNames[sfx - s_knownSfx] = NULL;
	memset( sfx, 0, sizeof( *sfx ));
}

/*
=====================
S_BeginRegistration

=====================
*/
void S_BeginRegistration( void )
{
	snd.have_ambient_sfx = false;

	// check for automatic ambient sounds
	for( int i = 0; i < NUM_AMBIENTS; i++ )
	{
		if( !GI->ambientsound[i][0] )
			continue;	// empty slot

		snd.ambient_sfx[i] = S_RegisterSound( GI->ambientsound[i] );
		if( snd.ambient_sfx[i] )
			snd.have_ambient_sfx = true; // allow auto-ambients
	}

	s_registering = true;
}

/*
=====================
S_EndRegistration

=====================
*/
void S_EndRegistration( void )
{
	sfx_t	*sfx;
	int	i;

	if( !s_registering || !snd.initialized )
		return;

	// free any sounds not from this registration sequence
	for( i = 0, sfx = s_knownSfx; i < s_numSfx; i++, sfx++ )
	{
		if( !sfx->name[0] || !Q_stricmp( sfx->name, "*default" ))
			continue; // don't release default sound

		if( sfx->servercount != cl.servercount )
			S_FreeSound( sfx ); // don't need this sound
	}

	s_preloadPending = false;
	s_preloadCursor = 0;
	s_preloadLongTracks = false;
	s_preloadDone = false;
	s_preloadBudgetHit = false;

	if( s_preload_mb.value <= 0.0f )
	{
		// no budget: keep the old behaviour and load everything in at once
		for( i = 0, sfx = s_knownSfx; i < s_numSfx; i++, sfx++ )
		{
			if( !sfx->name[0] )
				continue;
			S_LoadSound( sfx );
		}
		s_preloadDone = true;
	}
	else
	{
		// the rest is decoded by S_PreloadFrame() within a small per-frame
		// time budget, so connecting doesn't stall on hundreds of sounds;
		// whatever isn't ready in time is loaded on its first use
		s_preloadPending = true;
		Con_DPrintf( "Sound preload: %s already cached, deferring the rest (s_preload_mb %g)\n",
			Q_memprint( s_cachedBytes ), s_preload_mb.value );
	}

	s_registering = false;
}

/*
==================
S_RegisterSound

==================
*/
sound_t S_RegisterSound( const char *name )
{
	sfx_t	*sfx;

	if( COM_StringEmptyOrNULL( name ) || !snd.initialized )
		return -1;

	if( S_TestSoundChar( name, '!' ))
	{
		Q_strncpy( s_sentenceImmediateName, name, sizeof( s_sentenceImmediateName ));
		return SENTENCE_INDEX;
	}

	// some stupid mappers used leading '/' or '\' in path to models or sounds
	if( name[0] == '/' || name[0] == '\\' ) name++;
	if( name[0] == '/' || name[0] == '\\' ) name++;

	sfx = S_FindName( name, NULL );
	if( !sfx ) return -1;

	sfx->servercount = cl.servercount;
	if( !s_registering ) S_LoadSound( sfx );

	return sfx - s_knownSfx;
}

sound_t S_RegisterSvenSound( const char *name )
{
	sfx_t *sfx = S_FindName( name, NULL );
	if( !sfx || !S_LoadSoundInternal( sfx, false )) return -1;
	return sfx - s_knownSfx;
}

/*
==================
S_RegisterSvenSoundLazy

register a Sven sound without decoding it right away; the sample is loaded
from disk when it is actually played, or by the frame-budgeted preloader
==================
*/
sound_t S_RegisterSvenSoundLazy( const char *name )
{
	sfx_t *sfx = S_FindName( name, NULL );
	if( !sfx ) return -1;
	return sfx - s_knownSfx;
}

sfx_t *S_GetSfxByHandle( sound_t handle )
{
	if( !snd.initialized )
		return NULL;

	// create new sfx
	if( handle == SENTENCE_INDEX )
		return S_FindName( s_sentenceImmediateName, NULL );

	if( handle < 0 || handle >= s_numSfx )
		return NULL;

	return &s_knownSfx[handle];
}

/*
=================
S_InitSounds
=================
*/
void S_InitSounds( void )
{
	// create unused 0-entry
	Q_strncpy( s_knownSfx->name, "*default", sizeof( s_knownSfx->name ));
	s_knownSfx->hashValue = COM_HashKey( s_knownSfx->name, MAX_SFX_HASH );
	s_knownSfx->hashNext = s_sfxHashList[s_knownSfx->hashValue];
	s_sfxHashList[s_knownSfx->hashValue] = s_knownSfx;
	s_knownSfx->cache = S_CreateDefaultSound();
	S_AccountSound( s_knownSfx );
	s_numSfx = 1;
}

/*
=================
S_FreeSounds
=================
*/
void S_FreeSounds( void )
{
	sfx_t	*sfx;
	int	i;

	if( !snd.initialized )
		return;
	CL_SvenSoundCacheInvalidate();

	// stop all sounds
	S_StopAllSounds( true );

	// free all sounds
	for( i = 0, sfx = s_knownSfx; i < s_numSfx; i++, sfx++ )
		S_FreeSound( sfx );


	memset( s_knownSfx, 0, sizeof( s_knownSfx ));
	memset( s_sfxHashList, 0, sizeof( s_sfxHashList ));

	memset( s_sfxBytes, 0, sizeof( s_sfxBytes ));
	memset( s_sfxLastUse, 0, sizeof( s_sfxLastUse ));
	s_cachedBytes = 0;
	s_sfxUseClock = 0;
	s_preloadPending = false;
	s_preloadCursor = 0;
	s_preloadLongTracks = false;
	s_preloadDone = false;
	s_preloadBudgetHit = false;

	s_numSfx = 0;
}

/*
=================
S_PreloadBudgetExhausted

qtrue once the eager loading budget is spent. Sticky until the next map, so
releasing cache memory doesn't restart preloading and thrash the disk again
=================
*/
qboolean S_PreloadBudgetExhausted( void )
{
	size_t limit;

	if( !( s_preload_mb.value > 0.0f ))
		return false; // no budget, everything may be loaded eagerly

	if( !s_preloadBudgetHit )
	{
		double budget = s_preload_mb.value;
		if( s_cache_mb.value > 0.0f )
			budget = Q_min( budget, (double)s_cache_mb.value * 0.8 );
		double bytes = budget * 1024.0 * 1024.0;
		limit = bytes >= (double)SIZE_MAX ? SIZE_MAX : (size_t)bytes;
		if( s_cachedBytes >= limit )
			s_preloadBudgetHit = true;
	}

	return s_preloadBudgetHit;
}

/*
=================
S_NextPreloadSound

pick the next sound the preloader should decode: short samples first, long
tracks afterwards
=================
*/
static sfx_t *S_NextPreloadSound( double deadline )
{
	while( s_preloadCursor < s_numSfx )
	{
		if( Sys_DoubleTime() >= deadline )
			return NULL;

		sfx_t *sfx = &s_knownSfx[s_preloadCursor];
		s_preloadCursor++;

		if( !sfx->name[0] || sfx->cache )
			continue;

		if( !s_preloadLongTracks && FS_FileSize( S_SoundName( sfx ), false ) > S_PRELOAD_SMALL_FILE )
			continue; // long track: leave it for the second pass

		return sfx;
	}

	if( !s_preloadLongTracks )
	{
		s_preloadLongTracks = true;
		s_preloadCursor = 0;
		return S_NextPreloadSound( deadline );
	}

	s_preloadDone = true; // nothing left to preload
	return NULL;
}

/*
=================
S_PreloadFrame

called once per frame: decodes a few precached sounds within a small time
budget instead of stalling the whole connect sequence on all of them
=================
*/
void S_PreloadFrame( void )
{
	double budget, deadline;

	if( !s_preloadPending || s_preloadDone || !snd.initialized || s_registering )
		return;

	budget = Q_max( 0.1f, s_preload_ms.value ) * 0.001;
	deadline = Sys_DoubleTime() + budget;

	while( !s_preloadDone )
	{
		sfx_t *sfx;
		double start, cost;

		if( S_PreloadBudgetExhausted( ))
		{
			s_preloadDone = true;
			break;
		}

		if( Sys_DoubleTime() >= deadline )
			break; // out of this frame's budget, continue next frame

		sfx = S_NextPreloadSound( deadline );
		if( !sfx )
			break; // out of time or nothing left (s_preloadDone is set then)

		start = Sys_DoubleTime();
		S_LoadSoundInternal( sfx, false );
		cost = Sys_DoubleTime() - start;

		if( sfx->cache && cost > Q_max( 0.010, budget * 8.0 ))
		{
			// a single long track can burn more than a whole frame, don't
			// stack more of those on top of it
			Con_DPrintf( "Sound preload: %s took %.1f ms, loading the rest on demand\n",
				S_SoundName( sfx ), cost * 1000.0 );
			s_preloadDone = true;
		}
	}
}

/*
=================
S_SoundCacheMaintenance

keep the decoded sound cache inside s_cache_mb, releasing the least recently
used samples that are not currently playing. Sounds stay registered and are
loaded again on their next use
=================
*/
void S_SoundCacheMaintenance( void )
{
	int evicted = 0;
	size_t cap, target;

	if( !snd.initialized || s_registering || s_numSfx <= 1 )
		return;

	// while connecting the cache is being filled on purpose, don't fight it
	if( cls.state != ca_active )
		return;

	// the game DLL owns the cache through the sound interface, don't pull
	// wavdata_t out from under it
	if( clgame.soundFuncs.pfnS_FreeSound )
		return;

	double bytes = (double)Q_max( 0.0f, s_cache_mb.value ) * 1024.0 * 1024.0;
	cap = bytes >= (double)SIZE_MAX ? SIZE_MAX : (size_t)bytes;
	if( !cap || s_cachedBytes <= cap )
		return;

	s_preloadBudgetHit = true;

	target = cap - cap / 5; // release 20% at once, so we don't evict every frame

	// mark every sound that playback still points at
	memset( s_sfxInUse, 0, Q_min( (size_t)s_numSfx, sizeof( s_sfxInUse )));

	for( int i = 0; i < NUM_AMBIENTS; i++ )
	{
		if( snd.ambient_sfx[i] > 0 && snd.ambient_sfx[i] < s_numSfx )
			s_sfxInUse[snd.ambient_sfx[i]] = 1;
	}

	for( int i = 0; i < snd.total_channels; i++ )
	{
		channel_t *ch = &snd.channels[i];

		if( ch->sfx && ( ch->sfx - s_knownSfx ) >= 0 && ( ch->sfx - s_knownSfx ) < MAX_SFX )
			s_sfxInUse[ch->sfx - s_knownSfx] = 1;

		if( !ch->words )
			continue;

		for( int j = 0; j < CVOXWORDMAX && ch->words[j].sfx; j++ )
		{
			int index = ch->words[j].sfx - s_knownSfx;
			if( index >= 0 && index < MAX_SFX )
				s_sfxInUse[index] = 1;
		}
	}

	while( s_cachedBytes > target && evicted < 8 )
	{
		sfx_t *victim = NULL;

		for( int i = 1; i < s_numSfx; i++ )
		{
			sfx_t *sfx = &s_knownSfx[i];

			if( !sfx->cache || s_sfxInUse[i] )
				continue;

			if( sfx->servercount != cl.servercount )
				continue; // belongs to another server state, freed on disconnect

			if( !victim || s_sfxLastUse[i] < s_sfxLastUse[victim - s_knownSfx] )
				victim = sfx;
		}

		if( !victim )
			break; // everything left is in use, keep the extra memory for now

		S_UncacheSound( victim );
		evicted++;
	}

	if( evicted )
		Con_DPrintf( "Sound cache: released %d sound(s), now %s\n", evicted, Q_memprint( s_cachedBytes ));
}
