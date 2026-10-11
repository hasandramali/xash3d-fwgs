#include "common.h"
#include "client.h"
#include "sound.h"
#include "sven_sound.h"

typedef struct sven_sound_table_s
{
	char sounds[SVEN_SOUND_COUNT][SVEN_SOUND_PATH];
	char sentences[SVEN_SENTENCE_COUNT][384];
	int soundCount;
	int sentenceCount;
} sven_sound_table_t;

static sven_sound_table_t svenSoundTable;
static char svenSoundMap[MAX_QPATH];
static char svenRequestedMap[MAX_QPATH];
static qboolean svenSoundLoaded;
static double svenNextLoad;
static int svenRequestSignon;
static struct
{
	sven_sound_event_t event;
	double received;
} svenPending[128];
static int svenPendingCount;
static sound_t svenSoundHandles[SVEN_SOUND_COUNT];
static qboolean svenSoundsPrecached;

static void CL_SvenPlaySound( const sven_sound_event_t *event, double elapsed );

static char *CL_SvenNextLine( char **cursor, char *end )
{
	char *line = *cursor, *finish;
	if( line >= end ) return NULL;
	finish = line;
	while( finish < end && *finish != '\n' ) finish++;
	*cursor = finish < end ? finish + 1 : end;
	while( finish > line && ( finish[-1] == '\r' || finish[-1] == ' ' || finish[-1] == '\t' )) finish--;
	*finish = '\0';
	while( *line == ' ' || *line == '\t' ) line++;
	return line;
}

static qboolean CL_SvenParseTable( char *buffer, size_t size, const char *map, sven_sound_table_t *table )
{
	char *cursor = buffer, *end = buffer + size, *line;
	int section = 0;
	qboolean soundsClosed = false, sentencesClosed = false;
	memset( table, 0, sizeof( *table ));
	if( memchr( buffer, '\0', size )) return false;
	line = CL_SvenNextLine( &cursor, end );
	if( !line || Q_strcmp( line, map )) return false;
	while(( line = CL_SvenNextLine( &cursor, end )) != NULL )
	{
		if( !*line ) continue;
		if( !section )
		{
			if( !Q_strcmp( line, "SOUNDLIST {" ))
			{
				if( soundsClosed ) return false;
				section = 1;
			}
			else if( !Q_strcmp( line, "SENTENCELIST {" ))
			{
				if( sentencesClosed ) return false;
				section = 2;
			}
			continue;
		}
		if( !Q_strcmp( line, "}" ))
		{
			if( section == 1 ) soundsClosed = true;
			else sentencesClosed = true;
			section = 0;
			continue;
		}
		if( section == 1 )
		{
			if( table->soundCount >= SVEN_SOUND_COUNT || Q_strlen( line ) >= SVEN_SOUND_PATH ) return false;
			Q_strncpy( table->sounds[table->soundCount++], line, SVEN_SOUND_PATH );
		}
		else
		{
			char *body = line;
			while( *body && *body != ' ' && *body != '\t' ) body++;
			if( body == line || body - line >= 32 ) return false;
			while( *body == ' ' || *body == '\t' ) body++;
			if( !*body || table->sentenceCount >= SVEN_SENTENCE_COUNT || Q_strlen( body ) >= sizeof( table->sentences[0] )) return false;
			Q_strncpy( table->sentences[table->sentenceCount++], body, sizeof( table->sentences[0] ));
		}
	}
	return soundsClosed && sentencesClosed && !section;
}

static void CL_SvenSoundMapName( char *map, size_t size )
{
	COM_FileBase( clgame.mapname, map, size );
}

void CL_SvenSoundCacheReset( void )
{
	svenSoundLoaded = false;
	svenSoundMap[0] = svenRequestedMap[0] = '\0';
	svenNextLoad = 0;
	svenRequestSignon = -1;
	svenPendingCount = 0;
	svenSoundsPrecached = false;
	memset( &svenSoundTable, 0, sizeof( svenSoundTable ));
}

void CL_SvenSoundCacheInvalidate( void )
{
	svenSoundsPrecached = false;
}

static void CL_LoadSvenSoundCache( void )
{
	char map[MAX_QPATH], path[MAX_QPATH + 32];
	fs_offset_t size;
	byte *buffer;
	sven_sound_table_t *table;
	if( !cl_sven_proto || !Cvar_VariableInteger( "cl_sven_soundcache" ) || !clgame.mapname[0] ) return;
	CL_SvenSoundMapName( map, sizeof( map ));
	if( Q_strcmp( svenSoundMap, map ))
	{
		CL_SvenSoundCacheReset();
		Q_strncpy( svenSoundMap, map, sizeof( svenSoundMap ));
	}
	Q_snprintf( path, sizeof( path ), "maps/soundcache/%s.txt", map );
	if( cls.state >= ca_connected && !cls.demoplayback
		&& ( Q_strcmp( svenRequestedMap, map ) || ( svenRequestSignon < SIGNONS && cls.signon == SIGNONS )))
	{
		byte data[256];
		sizebuf_t message;
		MSG_Init( &message, "SvenSoundCache", data, sizeof( data ));
		MSG_BeginClientCmd( &message, clc_stringcmd );
		MSG_WriteStringf( &message, "dlfile \"%s\"", path );
		Netchan_CreateFragments( &cls.netchan, &message );
		Netchan_FragSend( &cls.netchan );
		Q_strncpy( svenRequestedMap, map, sizeof( svenRequestedMap ));
		svenRequestSignon = cls.signon;
	}
	if( svenSoundLoaded || host.realtime < svenNextLoad ) return;
	svenNextLoad = host.realtime + 1.0;
	buffer = FS_LoadFile( path, &size, false );
	if( !buffer ) return;
	if( size <= 0 || size > 4 * 1024 * 1024 )
	{
		Mem_Free( buffer );
		return;
	}
	table = Mem_Calloc( cls.mempool, sizeof( *table ));
	if( CL_SvenParseTable( (char *)buffer, size, map, table ))
	{
		svenSoundTable = *table;
		svenSoundLoaded = true;
		Con_DPrintf( "SVEN-SOUNDCACHE: %d sounds, %d sentences for %s\n", table->soundCount, table->sentenceCount, map );
	}
	else
	{
		Con_Printf( "SVEN-SOUNDCACHE: invalid table %s, waiting for server copy\n", path );
		svenNextLoad = host.realtime + 5.0;
	}
	Mem_Free( table );
	Mem_Free( buffer );
}

void CL_SvenSoundFrame( void )
{
	if( cls.state >= ca_connected ) CL_LoadSvenSoundCache();
	if( !cl.audio_prepped || !svenSoundLoaded || !Cvar_VariableInteger( "cl_sven_soundcache" )) return;
	if( !svenSoundsPrecached )
	{
		for( int index = 0; index < svenSoundTable.soundCount; index++ )
			svenSoundHandles[index] = S_RegisterSvenSound( svenSoundTable.sounds[index] );
		svenSoundsPrecached = true;
	}
	for( int index = 0; index < svenPendingCount; index++ )
		CL_SvenPlaySound( &svenPending[index].event, Q_max( 0.0, host.realtime - svenPending[index].received ));
	svenPendingCount = 0;
}

void CL_SvenSoundCacheOnDownload( const char *filename )
{
	char map[MAX_QPATH], path[MAX_QPATH + 32];
	if( cls.state < ca_connected || !clgame.mapname[0] || !filename ) return;
	CL_SvenSoundMapName( map, sizeof( map ));
	Q_snprintf( path, sizeof( path ), "maps/soundcache/%s.txt", map );
	if( Q_strcmp( path, filename ))
	{
		if( svenSoundLoaded && svenSoundsPrecached )
		{
			for( int index = 0; index < svenSoundTable.soundCount; index++ )
			{
				const char *sample = svenSoundTable.sounds[index];
				if( *sample == '*' ) sample++;
				Q_snprintf( path, sizeof( path ), DEFAULT_SOUNDPATH "%s", sample );
				if( !Q_strcmp( filename, path ) || !Q_strcmp( filename, sample ))
					svenSoundHandles[index] = S_RegisterSvenSound( sample );
			}
		}
		return;
	}
	svenSoundLoaded = false;
	svenSoundsPrecached = false;
	svenNextLoad = 0;
	CL_LoadSvenSoundCache();
}

static void CL_SvenSoundCacheFlush( void )
{
	S_StopAllSounds( true );
	CL_SvenSoundCacheReset();
	CL_LoadSvenSoundCache();
}

void CL_SvenSoundCache_Init( void )
{
	Cmd_AddCommand( "cl_flushsoundcache", CL_SvenSoundCacheFlush, "reload the server's Sven sound table" );
	Cmd_AddCommand( "cl_flushsoundcache_force", CL_SvenSoundCacheFlush, "force reload the server's Sven sound table" );
}

const char *CL_SvenSoundName( int index )
{
	if( !svenSoundLoaded || index < 0 || index >= svenSoundTable.soundCount ) return NULL;
	return svenSoundTable.sounds[index];
}

qboolean CL_SvenSoundActive( void )
{
	return cl_sven_proto && svenSoundLoaded && Cvar_VariableInteger( "cl_sven_soundcache" );
}

const char *CL_SvenSentenceText( int index )
{
	if( !svenSoundLoaded || index < 0 || index >= svenSoundTable.sentenceCount ) return NULL;
	return svenSoundTable.sentences[index];
}

static qboolean CL_SvenReadSound( int size, void *buffer, sven_sound_event_t *event )
{
	byte data[32] = { 0 };
	sizebuf_t message;
	if( !buffer || size < 5 || size > 27 ) return false;
	memcpy( data, buffer, size );
	memset( event, 0, sizeof( *event ));
	event->index = -1;
	event->entity = -1;
	event->volume = VOL_NORM;
	event->pitch = PITCH_NORM;
	event->attenuation = ATTN_NORM;
	MSG_Init( &message, "SvenStartSound", data, size );
	event->flags = (unsigned short)MSG_ReadShort( &message );
	if( event->flags & SVEN_SND_ENTITY ) event->entity = MSG_ReadShort( &message );
	if( event->flags & SVEN_SND_VOLUME ) event->volume = MSG_ReadByte( &message ) / 255.0f;
	if( event->flags & SVEN_SND_PITCH ) event->pitch = MSG_ReadByte( &message );
	if( event->flags & SVEN_SND_ATTN )
	{
		event->attenuation = MSG_ReadByte( &message );
		if( !( event->flags & SVEN_SND_LINEAR )) event->attenuation /= 64.0f;
	}
	if( event->flags & SVEN_SND_ORIGIN )
	{
		for( int axis = 0; axis < 3; axis++ ) event->origin[axis] = MSG_ReadLong( &message ) * ( 1.0f / 8.0f );
	}
	if( event->flags & SVEN_SND_OFFSET ) event->offset = MSG_ReadFloat( &message );
	event->channel = MSG_ReadByte( &message );
	event->index = MSG_ReadShort( &message );
	return !MSG_CheckOverflow( &message ) && MSG_GetNumBytesRead( &message ) == size
		&& event->entity >= -1 && isfinite( event->offset ) && event->offset >= 0;
}

static void CL_SvenPlaySound( const sven_sound_event_t *event, double elapsed )
{
	sound_t handle;
	char sentence[32];
	const char *sample;
	sfx_t *sfx;
	if( event->flags & SVEN_SND_SENTENCE )
	{
		if( !CL_SvenSentenceText( event->index ) || elapsed > 1.0 ) return;
		Q_snprintf( sentence, sizeof( sentence ), "!@%d", event->index );
		sample = sentence;
	}
	else sample = CL_SvenSoundName( event->index );
	if( !sample ) return;
	if( event->flags & SVEN_SND_SENTENCE ) handle = S_RegisterSound( sample );
	else if( svenSoundsPrecached ) handle = svenSoundHandles[event->index];
	else handle = S_RegisterSvenSound( sample );
	sfx = S_GetSfxByHandle( handle );
	if( !sfx ) return;
	if( elapsed > 1.0 && !( event->flags & SVEN_SND_LOOP )
		&& ( !sfx->cache || !FBitSet( sfx->cache->flags, SOUND_LOOPED ) || ( event->flags & SVEN_SND_SINGLE ))) return;
	S_StartSvenSound( event, handle );
}

static void CL_SvenQueueSound( const sven_sound_event_t *event )
{
	if( svenPendingCount == ARRAYSIZE( svenPending ))
		memmove( svenPending, svenPending + 1, ( --svenPendingCount ) * sizeof( svenPending[0] ));
	svenPending[svenPendingCount].event = *event;
	svenPending[svenPendingCount++].received = host.realtime;
}

static qboolean CL_SvenControlPending( const sven_sound_event_t *event )
{
	if( event->entity >= 0 && ( event->channel || ( event->flags & SVEN_SND_STOP )))
	{
		for( int index = 0; index < svenPendingCount; )
		{
			if( svenPending[index].event.entity == event->entity && svenPending[index].event.channel == event->channel )
			{
				if( !( event->flags & SVEN_SND_STOP ) && ( event->flags & ( SVEN_SND_CHANGE_VOL | SVEN_SND_CHANGE_PITCH ))
					&& ( event->index < 0 || event->index == svenPending[index].event.index ))
				{
					if( event->flags & SVEN_SND_CHANGE_VOL ) svenPending[index].event.volume = event->volume;
					if( event->flags & SVEN_SND_CHANGE_PITCH ) svenPending[index].event.pitch = event->pitch;
					return true;
				}
				memmove( &svenPending[index], &svenPending[index + 1], ( --svenPendingCount - index ) * sizeof( svenPending[0] ));
			}
			else index++;
		}
	}
	return false;
}

void CL_ParseSvenStartSound( const char *name, int size, void *buffer )
{
	sven_sound_event_t event;
	if( !cl_sven_proto || !Cvar_VariableInteger( "cl_sven_soundcache" )) return;
	if( !CL_SvenReadSound( size, buffer, &event ))
	{
		Con_DPrintf( "SVEN-SOUNDCACHE: malformed %s message (%d bytes)\n", name, size );
		return;
	}
	CL_LoadSvenSoundCache();
	if( CL_SvenControlPending( &event )) return;
	if(( event.flags & SVEN_SND_STOP ) || ( event.index < 0 && ( event.flags & ( SVEN_SND_CHANGE_VOL | SVEN_SND_CHANGE_PITCH ))))
	{
		S_StartSvenSound( &event, -1 );
		return;
	}
	if( !cl.audio_prepped || !svenSoundLoaded )
	{
		CL_SvenQueueSound( &event );
		return;
	}
	CL_SvenPlaySound( &event, 0 );
}

#if XASH_ENGINE_TESTS
#include "tests.h"

static void Test_SvenSoundTable( void )
{
	static sven_sound_table_t table;
	char valid[] = "test\r\n SOUNDLIST {\r\n a.wav \r\n\r\n a.wav\n../media/music.mp3\n}\nSENTENCELIST {\nSAME vox/hello(p80) world\nSAME custom/other\n}\nCUSTOMMATERIALS {\n}";
	char unfinished[] = "test\nSOUNDLIST {\na.wav\n}\nSENTENCELIST {\nSAME vox/hello";
	char wrongMap[] = "other\nSOUNDLIST {\n}\nSENTENCELIST {\n}";
	char longPath[512], oversized[512];
	TASSERT( CL_SvenParseTable( valid, sizeof( valid ) - 1, "test", &table ));
	TASSERT_EQi( table.soundCount, 3 );
	TASSERT_STR( table.sounds[0], "a.wav" );
	TASSERT_STR( table.sounds[1], "a.wav" );
	TASSERT_STR( table.sounds[2], "../media/music.mp3" );
	TASSERT_EQi( table.sentenceCount, 2 );
	TASSERT_STR( table.sentences[0], "vox/hello(p80) world" );
	TASSERT_STR( table.sentences[1], "custom/other" );
	TASSERT( !CL_SvenParseTable( unfinished, sizeof( unfinished ) - 1, "test", &table ));
	TASSERT( !CL_SvenParseTable( wrongMap, sizeof( wrongMap ) - 1, "test", &table ));
	memset( longPath, 'a', 259 );
	longPath[259] = '\0';
	Q_snprintf( oversized, sizeof( oversized ), "test\nSOUNDLIST {\n%s\n}\nSENTENCELIST {\n}", longPath );
	TASSERT( CL_SvenParseTable( oversized, Q_strlen( oversized ), "test", &table ));
	TASSERT_EQi( Q_strlen( table.sounds[0] ), 259 );
	longPath[259] = 'a';
	longPath[260] = '\0';
	Q_snprintf( oversized, sizeof( oversized ), "test\nSOUNDLIST {\n%s\n}\nSENTENCELIST {\n}", longPath );
	TASSERT( !CL_SvenParseTable( oversized, Q_strlen( oversized ), "test", &table ));
}

static void Test_SvenSoundPacket( void )
{
	byte crowbar[] = { 0x1a, 0, 1, 0, 96, 0x80, 0, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0xff, 0xff, 1, 129, 0 };
	byte unbound[] = { 0, 0, 6, 0x8a, 1 };
	byte data[32];
	sizebuf_t message;
	sven_sound_event_t event;
	uint savedFeatures = host.features;
	TASSERT( CL_SvenReadSound( sizeof( crowbar ), crowbar, &event ));
	TASSERT_EQi( event.entity, 1 );
	TASSERT_EQi( event.index, 129 );
	TASSERT_EQi( event.pitch, 96 );
	TASSERT( event.origin[0] == 16 && event.origin[1] == 0 && event.origin[2] == -16 );
	TASSERT( CL_SvenReadSound( sizeof( unbound ), unbound, &event ));
	TASSERT_EQi( event.entity, -1 );
	TASSERT_EQi( event.index, 394 );
	MSG_Init( &message, "test", data, sizeof( data ));
	MSG_WriteShort( &message, SVEN_SND_STOP | SVEN_SND_ENTITY );
	MSG_WriteShort( &message, 7 );
	MSG_WriteByte( &message, CHAN_WEAPON );
	MSG_WriteShort( &message, -1 );
	TASSERT( CL_SvenReadSound( 7, data, &event ));
	TASSERT_EQi( event.index, -1 );
	TASSERT_EQi( event.entity, 7 );
	TASSERT_EQi( event.channel, CHAN_WEAPON );
	TASSERT( !CL_SvenReadSound( 4, data, &event ));
	MSG_Clear( &message );
	MSG_WriteWord( &message, SVEN_SND_ENTITY | SVEN_SND_VOLUME | SVEN_SND_PITCH | SVEN_SND_ATTN | SVEN_SND_ORIGIN | SVEN_SND_LINEAR | SVEN_SND_OFFSET );
	MSG_WriteShort( &message, 1024 );
	MSG_WriteByte( &message, 128 );
	MSG_WriteByte( &message, 85 );
	MSG_WriteByte( &message, 0xf2 );
	host.features &= ~ENGINE_WRITE_LARGE_COORD;
	for( int axis = 0; axis < 3; axis++ ) MSG_WriteCoord( &message, 16.0f * axis - 16 );
	MSG_WriteFloat( &message, 2.25f );
	MSG_WriteByte( &message, CHAN_STATIC );
	MSG_WriteShort( &message, 0 );
	TASSERT( CL_SvenReadSound( MSG_GetNumBytesWritten( &message ), data, &event ));
	TASSERT_EQi( event.index, 0 );
	TASSERT_EQi( event.entity, 1024 );
	TASSERT_EQi( event.pitch, 85 );
	TASSERT( fabsf( event.volume - 128.0f / 255 ) < 0.0001f );
	TASSERT_EQi( event.attenuation, 242 );
	TASSERT( event.offset == 2.25f );
	TASSERT( event.origin[0] == -16 && event.origin[1] == 0 && event.origin[2] == 16 );
	host.features |= ENGINE_WRITE_LARGE_COORD;
	TASSERT( CL_SvenReadSound( MSG_GetNumBytesWritten( &message ), data, &event ));
	TASSERT( event.origin[0] == -16 && event.origin[1] == 0 && event.origin[2] == 16 );
	TASSERT( !CL_SvenReadSound( MSG_GetNumBytesWritten( &message ) - 1, data, &event ));
	TASSERT( !CL_SvenReadSound( MSG_GetNumBytesWritten( &message ) + 1, data, &event ));
	host.features = savedFeatures;
}

static void Test_SvenSoundPending( void )
{
	sven_sound_event_t event = { 0 };
	event.flags = SVEN_SND_LOOP;
	event.channel = CHAN_WEAPON;
	event.entity = 5;
	event.index = 17;
	CL_SvenSoundCacheReset();
	CL_SvenQueueSound( &event );
	event.flags = SVEN_SND_CHANGE_PITCH;
	event.pitch = 85;
	TASSERT( CL_SvenControlPending( &event ));
	TASSERT_EQi( svenPendingCount, 1 );
	TASSERT_EQi( svenPending[0].event.flags, SVEN_SND_LOOP );
	TASSERT_EQi( svenPending[0].event.pitch, 85 );
	event.flags = SVEN_SND_STOP;
	event.index = -1;
	TASSERT( !CL_SvenControlPending( &event ));
	TASSERT_EQi( svenPendingCount, 0 );
	event.entity = -1;
	event.flags = SVEN_SND_LOOP;
	CL_SvenQueueSound( &event );
	event.flags = SVEN_SND_STOP;
	TASSERT( !CL_SvenControlPending( &event ));
	TASSERT_EQi( svenPendingCount, 1 );
	CL_SvenSoundCacheReset();
	for( int index = 0; index < 132; index++ )
	{
		event.index = index;
		CL_SvenQueueSound( &event );
	}
	TASSERT_EQi( svenPendingCount, 128 );
	TASSERT_EQi( svenPending[0].event.index, 4 );
	Q_strncpy( svenRequestedMap, "test", sizeof( svenRequestedMap ));
	CL_SvenSoundCacheReset();
	TASSERT_EQi( svenPendingCount, 0 );
	TASSERT_EQi( svenRequestedMap[0], 0 );
}

void Test_RunSvenSound( void )
{
	TRUN( Test_SvenSoundTable() );
	TRUN( Test_SvenSoundPacket() );
	TRUN( Test_SvenSoundPending() );
}
#endif
