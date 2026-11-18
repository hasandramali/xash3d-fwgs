/*
snd_wav.c - wav format load & save
Copyright (C) 2010 Uncle Mike
Copyright (C) 2023 FTEQW developers

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include <stddef.h>
#include "soundlib.h"

static const byte *iff_data;
static const byte *iff_dataPtr;
static const byte *iff_end;
static const byte *iff_lastChunk;
static int iff_chunkLen;

static int IsFourCC( const void *ptr, const void *fourcc )
{
	return 0 == memcmp( ptr, fourcc, 4 );
}

/*
=================
GetLittleShort
=================
*/
static short GetLittleShort( void )
{
	short	val = 0;

	val += (*(iff_dataPtr+0) << 0);
	val += (*(iff_dataPtr+1) << 8);
	iff_dataPtr += 2;

	return val;
}

/*
=================
GetLittleLong
=================
*/
static int GetLittleLong( void )
{
	uint val = 0;

	val += ((uint)*(iff_dataPtr+0) << 0);
	val += ((uint)*(iff_dataPtr+1) << 8);
	val += ((uint)*(iff_dataPtr+2) <<16);
	val += ((uint)*(iff_dataPtr+3) <<24);
	iff_dataPtr += 4;

	return val;
}

/*
=================
FindNextChunk
=================
*/
static void FindNextChunk( const char *filename, const char *name )
{
	while( 1 )
	{
		ptrdiff_t remaining = iff_end - iff_lastChunk;

		if( remaining < 8 )
		{
			iff_dataPtr = NULL;
			return;
		}

		iff_dataPtr = iff_lastChunk + 4;
		remaining -= 8;

		iff_chunkLen = GetLittleLong();
		if( iff_chunkLen < 0 )
		{
			iff_dataPtr = NULL;
			return;
		}

		if( iff_chunkLen > remaining )
		{
			// only print this warning if selected chunk is truncated
			//
			// otherwise this warning becomes misleading because some
			// idiot programs like CoolEdit (i.e. Adobe Audition) don't always
			// respect pad byte. The file isn't actually truncated, it just
			// can't be reliably parsed as a whole

			if( IsFourCC( iff_lastChunk, "RIFF" )
				|| IsFourCC( iff_lastChunk, "fmt " )
				|| IsFourCC( iff_lastChunk, "cue " )
				|| IsFourCC( iff_lastChunk, "LIST" )
				|| IsFourCC( iff_lastChunk, "data" ))
			{
				Con_DPrintf( "%s: '%s' truncated by %td bytes\n", __func__, filename, iff_chunkLen - remaining );
			}
			iff_chunkLen = remaining;
		}

		remaining -= iff_chunkLen;
		iff_dataPtr -= 8;

		iff_lastChunk = iff_dataPtr + 8 + iff_chunkLen;
		if(( iff_chunkLen & 1 ) && remaining )
			iff_lastChunk++;
		if( IsFourCC( iff_dataPtr, name ))
			return;
	}
}

/*
=================
FindChunk
=================
*/
static void FindChunk( const char *filename, const char *name )
{
	iff_lastChunk = iff_data;
	FindNextChunk( filename, name );
}

/*
============
StreamFindNextChunk
============
*/
static qboolean StreamFindNextChunk( file_t *file, const char *name, int *last_chunk )
{
	char	chunkName[4];
	int	iff_chunk_len;

	while( 1 )
	{
		FS_Seek( file, *last_chunk, SEEK_SET );

		if( FS_Eof( file ))
			return false;	// didn't find the chunk

		FS_Seek( file, 4, SEEK_CUR );
		if( FS_Read( file, &iff_chunk_len, sizeof( iff_chunk_len )) != sizeof( iff_chunk_len ))
			return false;

		iff_chunk_len = LittleLong( iff_chunk_len );

		if( iff_chunk_len < 0 )
			return false;	// didn't find the chunk

		FS_Seek( file, -8, SEEK_CUR );
		*last_chunk = FS_Tell( file ) + 8 + (( iff_chunk_len + 1 ) & ~1 );
		if( FS_Read( file, chunkName, sizeof( chunkName )) != sizeof( chunkName ))
			return false;

		if( IsFourCC( chunkName, name ))
			return true;
	}

	return false;
}

/*
=============
Sound_LoadWAV
=============
*/
qboolean Sound_LoadWAV( const char *name, const byte *buffer, fs_offset_t filesize )
{
	qboolean	mpeg_stream = false;

	if( !buffer || filesize <= 0 )
		return false;

	iff_data = buffer;
	iff_end = buffer + filesize;

	// find "RIFF" chunk
	FindChunk( name, "RIFF" );

	if( !iff_dataPtr || !IsFourCC( iff_dataPtr + 8, "WAVE" ))
	{
		Con_DPrintf( S_ERROR "%s: %s missing 'RIFF/WAVE' chunks\n", __func__, name );
		return false;
	}

	// get "fmt " chunk
	iff_data = iff_dataPtr + 12;
	FindChunk( name, "fmt " );

	if( !iff_dataPtr )
	{
		Con_DPrintf( S_ERROR "%s: %s missing 'fmt ' chunk\n", __func__, name );
		return false;
	}

	int fmtChunkLen = iff_chunkLen;

	iff_dataPtr += 8;
	int fmt = (unsigned short)GetLittleShort();
	int srcWidth = 0;
	int bitsPerSample = 0;

	if( fmt == 0xFFFE )
	{
		// WAVE_FORMAT_EXTENSIBLE: the real format tag is carried in the
		// SubFormat GUID (first two bytes) at offset 24 of the fmt data
		const byte *fmtdata = iff_dataPtr - 2;

		if( fmtChunkLen >= 40 )
			fmt = (unsigned short)( fmtdata[24] | ( fmtdata[25] << 8 ));
		else
			fmt = 0;
	}

	if( fmt != 1 && fmt != 3 )
	{
		if( fmt != 85 )
		{
			Con_DPrintf( S_ERROR "%s: %s not a microsoft PCM format\n", __func__, name );
			return false;
		}
		else
		{
			// mpeg stream in wav container
			mpeg_stream = true;
		}
	}

	sound.channels = GetLittleShort();
	if( sound.channels != 1 && sound.channels != 2 )
	{
		Con_DPrintf( S_ERROR "%s: only mono and stereo WAV files supported (%s)\n", __func__, name );
		return false;
	}

	sound.rate = GetLittleLong();
	iff_dataPtr += 6; // skip nAvgBytesPerSec and nBlockAlign

	bitsPerSample = GetLittleShort();

	if( mpeg_stream )
	{
		srcWidth = 2; // mp3 always 16bit
	}
	else
	{
		srcWidth = bitsPerSample / 8;

		if( fmt == 1 )
		{
			if( bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32 )
			{
				Con_DPrintf( S_ERROR "%s: unsupported PCM bit depth %d (%s)\n", __func__, bitsPerSample, name );
				return false;
			}
		}
		else // IEEE float
		{
			if( bitsPerSample != 32 && bitsPerSample != 64 )
			{
				Con_DPrintf( S_ERROR "%s: unsupported float bit depth %d (%s)\n", __func__, bitsPerSample, name );
				return false;
			}
		}
	}

	if( srcWidth <= 0 )
	{
		Con_DPrintf( S_ERROR "%s: %s has invalid sample width\n", __func__, name );
		return false;
	}

	// the engine mixes signed 8-bit or 16-bit PCM, so everything else
	// (24/32-bit PCM, IEEE float) is converted down to 16-bit below
	sound.width = ( !mpeg_stream && fmt == 1 && bitsPerSample == 8 ) ? 1 : 2;

	// get cue chunk
	FindChunk( name, "cue " );

	if( iff_dataPtr && iff_end - iff_dataPtr >= 36 )
	{
		iff_dataPtr += 32;
		sound.loopstart = GetLittleLong();
		SetBits( sound.flags, SOUND_LOOPED );
		FindNextChunk( name, "LIST" ); // if the next chunk is a LIST chunk, look for a cue length marker

		if( iff_dataPtr && iff_end - iff_dataPtr >= 32 )
		{
			if( IsFourCC( iff_dataPtr + 28, "mark" ))
			{
				// this is not a proper parse, but it works with CoolEdit...
				iff_dataPtr += 24;
				sound.samples = sound.loopstart + GetLittleLong(); // samples in loop
			}
		}
	}
	else
	{
		sound.loopstart = 0;
		sound.samples = 0;
	}

	// find data chunk
	FindChunk( name, "data" );

	if( !iff_dataPtr )
	{
		Con_DPrintf( S_ERROR "%s: %s missing 'data' chunk\n", __func__, name );
		return false;
	}

	iff_dataPtr += 4;
	int dataSize = GetLittleLong();
	ptrdiff_t available = iff_end - iff_dataPtr;

	// never read past the buffer even if the declared data size lies
	if( dataSize > available )
		dataSize = available;

	int samples = dataSize / srcWidth;

	if( sound.samples )
	{
		if( samples < sound.samples )
		{
			Con_DPrintf( S_ERROR "%s: %s has a bad loop length\n", __func__, name );
			return false;
		}
	}
	else sound.samples = samples;

	if( sound.samples <= 0 )
	{
		Con_Reportf( S_ERROR "%s: file with %i samples (%s)\n", __func__, sound.samples, name );
		return false;
	}

	sound.type = WF_PCMDATA;
	sound.samples /= sound.channels;

	// g-cont. get support for mp3 streams packed in wav container
	// e.g. CAd menu sounds
	if( mpeg_stream )
	{
		int	hdr_size = (iff_dataPtr - buffer);

		if(( filesize - hdr_size ) < FRAME_SIZE )
		{
			sound.tempbuffer = (byte *)Mem_Realloc( host.soundpool, sound.tempbuffer, FRAME_SIZE );
			memcpy( sound.tempbuffer, buffer + (iff_dataPtr - buffer), filesize - hdr_size );
			return Sound_LoadMPG( name, sound.tempbuffer, FRAME_SIZE );
		}

		return Sound_LoadMPG( name, buffer + hdr_size, filesize - hdr_size );
	}

	if( FBitSet( sound.flags, SOUND_LOOPED ) && sound.loopstart >= sound.samples )
	{
		Con_DPrintf( S_WARN "%s: %s has bad loop start %u (%u samples total), disabling looping\n", __func__, name, sound.loopstart, sound.samples );
		ClearBits( sound.flags, SOUND_LOOPED );
		sound.loopstart = 0;
	}

	// Load the data
	sound.size = sound.samples * sound.width * sound.channels;
	sound.wav = Mem_Malloc( host.soundpool, sound.size );

	const byte *src = buffer + (iff_dataPtr - buffer);
	byte *dst = sound.wav;
	int totalSamples = sound.samples * sound.channels;

	if( srcWidth == sound.width && ( srcWidth == 1 || srcWidth == 2 ))
	{
		// fast path: data is already in the engine's native format
		memcpy( dst, src, sound.size );

		// swap 16-bit samples from little endian to native
		if( sound.width == 2 )
		{
			short *p = (short *)dst;
			int count = sound.size / 2;
			for( int i = 0; i < count; i++ )
				p[i] = LittleShort( p[i] );
		}

		// now convert 8-bit sounds to signed
		if( sound.width == 1 )
		{
			signed char	*pData = (signed char *)dst;

			for( int i = 0; i < sound.samples; i++ )
			{
				for( int j = 0; j < sound.channels; j++ )
				{
					*pData = (byte)((int)((byte)*pData) - 128 );
					pData++;
				}
			}
		}
	}
	else
	{
		// convert 24/32-bit PCM or IEEE float to signed 16-bit native
		short *out = (short *)dst;

		for( int i = 0; i < totalSamples; i++ )
		{
			const byte *s = src + (size_t)i * srcWidth;
			int v;

			if( fmt == 3 )
			{
				if( srcWidth == 4 )
				{
					float f;

					memcpy( &f, s, sizeof( f ));
					v = ( f != f ) ? 0 : (int)( f * 32767.0f );
				}
				else
				{
					double d;

					memcpy( &d, s, sizeof( d ));
					v = ( d != d ) ? 0 : (int)( d * 32767.0 );
				}
			}
			else if( srcWidth == 3 )
			{
				int32_t t = (int32_t)( s[0] | ( s[1] << 8 ) | ( s[2] << 16 ));

				if( t & 0x800000 ) t |= ~0xFFFFFF; // sign extend from 24 bits
				v = t >> 8;
			}
			else // 32-bit PCM
			{
				int32_t t;

				memcpy( &t, s, sizeof( t ));
				v = LittleLong( t ) >> 16;
			}

			if( v > 32767 ) v = 32767;
			else if( v < -32768 ) v = -32768;

			out[i] = (short)v;
		}
	}

	// silence known-broken WAVs that contain stray non-zero samples masquerading as silence
	if( Q_stristr( name, "null.wav" ) || Q_stristr( name, "_period.wav" ) || Q_stristr( name, "_comma.wav" ))
	{
		static const uint32_t broken_crcs[] =
		{
			0x14a36f29, // common/null.wav (HL1/Q1)
			0x005a43ab, // vox/_period.wav (HL1)
			0x7749ed15, // vox/_comma.wav (HL1)
		};
		uint32_t crc;

		CRC32_Init( &crc );
		CRC32_ProcessBuffer( &crc, buffer, filesize );
		crc = CRC32_Final( crc );

		for( size_t i = 0; i < ARRAYSIZE( broken_crcs ); i++ )
		{
			if( crc == broken_crcs[i] )
			{
				memset( sound.wav, 0, sound.size );
				break;
			}
		}
	}

	return true;
}

/*
=================
Stream_OpenWAV
=================
*/
stream_t *Stream_OpenWAV( const char *filename )
{
	stream_t	*stream;
	int 	last_chunk = 0;
	char	chunkName[4];
	file_t	*file;
	short	t;

	if( !filename || !*filename )
		return NULL;

	// open
	file = FS_Open( filename, "rb", false );
	if( !file ) return NULL;

	// find "RIFF" chunk
	if( !StreamFindNextChunk( file, "RIFF", &last_chunk ))
	{
		Con_DPrintf( S_ERROR "%s: %s missing RIFF chunk\n", __func__, filename );
		FS_Close( file );
		return NULL;
	}

	FS_Seek( file, 4, SEEK_CUR );

	if( FS_Read( file, chunkName, 4 ) != 4 )
	{
		Con_DPrintf( S_ERROR "%s: %s missing WAVE chunk, truncated\n", __func__, filename );
		FS_Close( file );
		return false;
	}

	if( !IsFourCC( chunkName, "WAVE" ))
	{
		Con_DPrintf( S_ERROR "%s: %s missing WAVE chunk\n", __func__, filename );
		FS_Close( file );
		return NULL;
	}

	// get "fmt " chunk
	int iff_data = FS_Tell( file );
	last_chunk = iff_data;
	if( !StreamFindNextChunk( file, "fmt ", &last_chunk ))
	{
		Con_DPrintf( S_ERROR "%s: %s missing 'fmt ' chunk\n", __func__, filename );
		FS_Close( file );
		return NULL;
	}

	FS_Read( file, chunkName, 4 );

	FS_Read( file, &t, sizeof( t ));
	t = LittleShort( t );
	if( t != 1 )
	{
		Con_DPrintf( S_ERROR "%s: %s not a microsoft PCM format\n", __func__, filename );
		FS_Close( file );
		return NULL;
	}

	FS_Read( file, &t, sizeof( t ));
	sound.channels = LittleShort( t );

	FS_Read( file, &sound.rate, sizeof( int ));
	sound.rate = LittleLong( sound.rate );

	FS_Seek( file, 6, SEEK_CUR );

	FS_Read( file, &t, sizeof( t ));
	sound.width = LittleShort( t ) / 8;

	sound.loopstart = 0;

	// find data chunk
	last_chunk = iff_data;
	if( !StreamFindNextChunk( file, "data", &last_chunk ))
	{
		Con_DPrintf( S_ERROR "%s: %s missing 'data' chunk\n", __func__, filename );
		FS_Close( file );
		return NULL;
	}

	FS_Read( file, &sound.samples, sizeof( int ));
	sound.samples = ( LittleLong( sound.samples ) / sound.width ) / sound.channels;

	// at this point we have valid stream
	stream = Mem_Calloc( host.soundpool, sizeof( stream_t ));
	stream->file = file;
	stream->size = sound.samples * sound.width * sound.channels;
	stream->buffsize = FS_Tell( file ); // header length
	stream->channels = sound.channels;
	stream->width = sound.width;
	stream->rate = sound.rate;
	stream->type = WF_PCMDATA;

	return stream;
}

/*
=================
Stream_ReadWAV

assume stream is valid
=================
*/
int Stream_ReadWAV( stream_t *stream, int bytes, void *buffer )
{
	if( !stream->file ) return 0;	// invalid file

	int remaining = stream->size - stream->pos;
	if( remaining <= 0 ) return 0;
	if( bytes > remaining ) bytes = remaining;

	stream->pos += bytes;
	FS_Read( stream->file, buffer, bytes );

	if( stream->width == 2 )
	{
		short *p = (short *)buffer;
		int count = bytes / 2;
		for( int i = 0; i < count; i++ )
			p[i] = LittleShort( p[i] );
	}

	return bytes;
}

/*
=================
Stream_SetPosWAV

assume stream is valid
=================
*/
int Stream_SetPosWAV( stream_t *stream, int newpos )
{
	// NOTE: stream->pos it's real file position without header size
	if( FS_Seek( stream->file, stream->buffsize + newpos, SEEK_SET ) != -1 )
	{
		stream->pos = newpos;
		return true;
	}

	return false;
}

/*
=================
Stream_GetPosWAV

assume stream is valid
=================
*/
int Stream_GetPosWAV( stream_t *stream )
{
	return stream->pos;
}

/*
=================
Stream_FreeWAV

assume stream is valid
=================
*/
void Stream_FreeWAV( stream_t *stream )
{
	if( stream->file )
		FS_Close( stream->file );
	Mem_Free( stream );
}
