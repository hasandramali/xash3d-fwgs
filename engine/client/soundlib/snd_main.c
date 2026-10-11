/*
snd_main.c - load & save various sound formats
Copyright (C) 2010 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "soundlib.h"

static void Sound_Reset( void )
{
	// reset global variables
	sound.width = sound.rate = 0;
	sound.channels = sound.loopstart = 0;
	sound.samples = sound.flags = 0;
	sound.type = WF_UNKNOWN;

	sound.wav = NULL;
	sound.size = 0;
}

static const char *Sound_DetectFormat( const byte *buffer, size_t size )
{
	if( !buffer ) return NULL;
	if( size >= 12 && !memcmp( buffer, "RIFF", 4 ) && !memcmp( buffer + 8, "WAVE", 4 )) return "wav";
	if( size >= 4 && !memcmp( buffer, "OggS", 4 ))
	{
		if( size >= 27 )
		{
			size_t packet = 27 + buffer[26];
			if( buffer[26] && packet <= size && size - packet >= 8 && !memcmp( buffer + packet, "OpusHead", 8 )) return "opus";
		}
		return "ogg";
	}
	if(( size >= 3 && !memcmp( buffer, "ID3", 3 ))
		|| ( size >= 4 && buffer[0] == 0xff && ( buffer[1] & 0xe0 ) == 0xe0
			&& ( buffer[1] & 0x18 ) != 0x08 && ( buffer[1] & 6 ))) return "mp3";
	return NULL;
}

#if XASH_ENGINE_TESTS
#include "tests.h"

void Test_RunSoundFormats( void )
{
	byte opus[36] = { 'O', 'g', 'g', 'S' };
	byte mpeg[] = { 0xff, 0xfb, 0x90, 0 };
	byte reserved[] = { 0xff, 0xe8, 0x90, 0 };
	opus[26] = 1;
	opus[27] = 8;
	memcpy( opus + 28, "OpusHead", 8 );
	TASSERT_STR( Sound_DetectFormat((const byte *)"RIFF1234WAVE", 12 ), "wav" );
	TASSERT_STR( Sound_DetectFormat((const byte *)"OggS", 4 ), "ogg" );
	TASSERT_STR( Sound_DetectFormat( opus, sizeof( opus )), "opus" );
	TASSERT_STR( Sound_DetectFormat( opus, sizeof( opus ) - 1 ), "ogg" );
	TASSERT_STR( Sound_DetectFormat((const byte *)"ID3", 3 ), "mp3" );
	TASSERT_STR( Sound_DetectFormat( mpeg, sizeof( mpeg )), "mp3" );
	TASSERT( !Sound_DetectFormat( reserved, sizeof( reserved )));
	TASSERT( !Sound_DetectFormat((const byte *)"Ogg", 3 ));
	TASSERT( !Sound_DetectFormat((const byte *)"RIFF1234", 8 ));
	TASSERT( !Sound_DetectFormat( NULL, 12 ));
}
#endif

static qboolean Sound_LoadBuffer( const loadwavfmt_t *fallback, const char *name, const byte *buffer, size_t size )
{
	const char *detected = Sound_DetectFormat( buffer, size );
	const loadwavfmt_t *selected = fallback;
	if( detected )
	{
		for( selected = sound.loadformats; selected && selected->ext; selected++ )
			if( !Q_stricmp( selected->ext, detected )) break;
		if( !selected || !selected->ext ) return false;
	}
	Sound_Reset();
	if( selected->loadfunc( name, buffer, size )) return true;
	if( sound.wav ) Mem_Free( sound.wav );
	Sound_Reset();
	return false;
}

static MALLOC_LIKE( FS_FreeSound, 1 ) wavdata_t *SoundPack( void )
{
	wavdata_t *pack = Mem_Malloc( host.soundpool, sizeof( *pack ) + sound.size );

	pack->size = sound.size;
	pack->loop_start = sound.loopstart;
	pack->samples = sound.samples;
	pack->type = sound.type;
	pack->flags = sound.flags;
	pack->rate = sound.rate;
	pack->width = sound.width;
	pack->channels = sound.channels;
	memcpy( pack->buffer, sound.wav, sound.size );

	Mem_Free( sound.wav );
	sound.wav = NULL;

	return pack;
}

/*
================
FS_LoadSound

loading and unpack to wav any known sound
================
*/
wavdata_t *FS_LoadSound( const char *filename, const byte *buffer, size_t size )
{
	const char *ext = COM_FileExtension( filename );
	char loadname[MAX_OSPATH];
	qboolean anyformat = true;

	Sound_Reset(); // clear old sounddata
	Q_strncpy( loadname, filename, sizeof( loadname ));

	if( !COM_StringEmpty( ext ))
	{
		// we needs to compare file extension with list of supported formats
		// and be sure what is real extension, not a filename with dot
		for( const loadwavfmt_t *format = sound.loadformats; format && format->ext; format++ )
		{
			if( !Q_stricmp( format->ext, ext ))
			{
				COM_StripExtension( loadname );
				anyformat = false;
				break;
			}
		}
	}

	// special mode: skip any checks, load file from buffer
	if( filename[0] == '#' && buffer && size )
		goto load_internal;

	// now try all the formats in the selected list
	for( const loadwavfmt_t *format = sound.loadformats; format && format->ext; format++)
	{
		if( anyformat || !Q_stricmp( ext, format->ext ))
		{
			qboolean success = false;
			fs_offset_t filesize = 0;
			char path[MAX_OSPATH + 32];

			Q_snprintf( path, sizeof( path ), DEFAULT_SOUNDPATH "%s.%s", loadname, format->ext );

			byte *f = FS_LoadFile( path, &filesize, false );
			if( f && filesize > 0 )
			{
				success = Sound_LoadBuffer( format, path, f, filesize );
				Mem_Free( f ); // release buffer
			}

			if( success )
				return SoundPack(); // loaded

			Q_snprintf( path, sizeof( path ), "%s.%s", loadname, format->ext );
			f = FS_LoadFile( path, &filesize, false );
			if( f && filesize > 0 )
			{
				success = Sound_LoadBuffer( format, path, f, filesize );
				Mem_Free( f ); // release buffer
			}

			if( success )
				return SoundPack();
		}
	}

load_internal:
	for( const loadwavfmt_t *format = sound.loadformats; format && format->ext; format++ )
	{
		if( anyformat || !Q_stricmp( ext, format->ext ))
		{
			if( buffer && size > 0  )
			{
				if( Sound_LoadBuffer( format, loadname, buffer, size ))
					return SoundPack(); // loaded
			}
		}
	}

	if( filename[0] != '#' )
		Con_DPrintf( S_WARN "%s: couldn't load \"%s\"\n", __func__, loadname );

	return NULL;
}

/*
================
Sound_FreeSound

free WAV buffer
================
*/
void FS_FreeSound( wavdata_t *pack )
{
	if( !pack ) return;
	Mem_Free( pack );
}

/*
================
FS_OpenStream

open and reading basic info from sound stream
================
*/
stream_t *FS_OpenStream( const char *filename )
{
	const char	*ext = COM_FileExtension( filename );
	string		loadname;
	qboolean		anyformat = true;
	stream_t		*stream = NULL;

	Sound_Reset(); // clear old streaminfo
	Q_strncpy( loadname, filename, sizeof( loadname ));

	if( !COM_StringEmpty( ext ))
	{
		// we needs to compare file extension with list of supported formats
		// and be sure what is real extension, not a filename with dot
		for( const streamfmt_t *format = sound.streamformat; format && format->ext; format++ )
		{
			if( !Q_stricmp( format->ext, ext ))
			{
				COM_StripExtension( loadname );
				anyformat = false;
				break;
			}
		}
	}

	// now try all the formats in the selected list
	for( const streamfmt_t *format = sound.streamformat; format && format->ext; format++)
	{
		if( anyformat || !Q_stricmp( ext, format->ext ))
		{
			string path;

			Q_snprintf( path, sizeof( path ), "%s.%s", loadname, format->ext );

			if(( stream = format->openfunc( path )) != NULL )
			{
				stream->format = format;
				return stream; // done
			}
		}
	}

	// compatibility with original Xash3D, try media/ folder
	if( Q_strncmp( filename, "media/", sizeof( "media/" ) - 1 ))
	{
		Q_snprintf( loadname, sizeof( loadname ), "media/%s", filename );
		stream = FS_OpenStream( loadname );
	}
	else
	{
		Con_Reportf( "%s: couldn't open \"%s\" or \"%s\"\n", __func__, filename + 6, filename );
	}

	return stream;
}

/*
================
FS_ReadStream

extract stream as wav-data and put into buffer, move file pointer
================
*/
int FS_ReadStream( stream_t *stream, int bytes, void *buffer )
{
	if( !stream || !stream->format || !stream->format->readfunc )
		return 0;

	if( bytes <= 0 || buffer == NULL )
		return 0;

	return stream->format->readfunc( stream, bytes, buffer );
}

/*
================
FS_GetStreamPos

get stream position (in bytes)
================
*/
int FS_GetStreamPos( stream_t *stream )
{
	if( !stream || !stream->format || !stream->format->getposfunc )
		return -1;

	return stream->format->getposfunc( stream );
}

/*
================
FS_SetStreamPos

set stream position (in bytes)
================
*/
int FS_SetStreamPos( stream_t *stream, int newpos )
{
	if( !stream || !stream->format || !stream->format->setposfunc )
		return -1;

	return stream->format->setposfunc( stream, newpos );
}

/*
================
FS_FreeStream

close sound stream
================
*/
void FS_FreeStream( stream_t *stream )
{
	if( !stream || !stream->format || !stream->format->freefunc )
		return;

	stream->format->freefunc( stream );
}

#if XASH_LLVM_LIBFUZZER
#define IMPLEMENT_SOUNDLIB_FUZZ_TARGET( export, target ) \
int EXPORT export( const uint8_t *Data, size_t Size ); \
int EXPORT export( const uint8_t *Data, size_t Size ) \
{ \
	wavdata_t *wav; \
	host.type = HOST_NORMAL; \
	Memory_Init(); \
	Sound_Init(); \
	if( target( "#internal", Data, Size )) \
	{ \
		wav = SoundPack(); \
		FS_FreeSound( wav ); \
	} \
	Sound_Shutdown(); \
	return 0; \
} \

IMPLEMENT_SOUNDLIB_FUZZ_TARGET( Fuzz_Sound_LoadMPG, Sound_LoadMPG )
IMPLEMENT_SOUNDLIB_FUZZ_TARGET( Fuzz_Sound_LoadWAV, Sound_LoadWAV )
#endif // XASH_LLVM_LIBFUZZER
