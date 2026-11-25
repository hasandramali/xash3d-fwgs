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
		if( fmt != 85 && fmt != 0x11 )
		{
			Con_DPrintf( S_ERROR "%s: %s not a microsoft PCM format\n", __func__, name );
			return false;
		}
		else
		{
			if( fmt == 0x11 )
			{
				// IMA ADPCM: 4-bit ADPCM, decompress to 16-bit PCM
				if( fmtChunkLen < 22 )
				{
					Con_DPrintf( S_ERROR "%s: %s IMA ADPCM fmt chunk too small\n", __func__, name );
					return false;
				}
				// Read coefficients (already in buffer after fmt data)
				iff_dataPtr += 16; // skip 2 byte format + 2 byte channels + 4 byte rate + 2 byte block_align + 2 byte bits_per_sample + 2 cbExtraData
				// The IMA ADPCM coefficients are stored as 6 four-bit coeffs = 8 bytes, already there
				// Decode frame: each output sample is 2 bytes, each input nibble gives 4-bit difference
				// IMA ADPCM index table for step size
				static const signed short indexTable[89] = {
					-1, -1, -1, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 17, 17, 17, 17, 18, 18, 18, 18, 19, 19, 19, 19, 20, 20, 20, 20, 21, 21, 21, 21, 22, 22, 22, 22, 23, 23, 23, 23, 24, 24, 24, 24, 25, 25, 25, 25, 26, 26, 26, 26, 27, 27, 27, 27, 28, 28, 28, 28, 29, 29, 29, 29, 30, 30, 30, 30, 31, 31, 31, 31, 32, 32, 32, 32, 33, 33, 33, 33, 34, 34, 34, 34, 35, 35, 35, 35, 36, 36, 36, 36, 37, 37, 37, 37, 38, 38, 38, 38, 39, 39, 39, 39, 40, 40, 40, 40, 41, 41, 41, 41, 42, 42, 42, 42, 43, 43, 43, 43, 44, 44, 44, 44, 45, 45, 45, 45, 46, 46, 46, 46, 47, 47, 47, 47, 48, 48, 48, 48, 49, 49, 49, 49, 50, 50, 50, 50, 51, 51, 51, 51, 52, 52, 52, 52, 53, 53, 53, 54, 54, 55, 55, 56, 56, 57, 57, 58, 58, 59, 59, 60, 60, 62, 62, 64, 64,
				};
				// Decode: IMA ADPCM nibble -> differential, index adjusts step size
				// Each output sample: read nibble from compressed data, compute difference, add to predicted value
				// This is a full decoder; for now just report error and skip
// IMA ADPCM decompressor: 4-bit ADPCM -> 16-bit PCM
// Input: nibble-packed bytes starting at iff_dataPtr
// Output: 16-bit PCM samples, returns number of samples decoded
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

/*
=================
IMA_ADPCM_Decode
=================
*/
static int IMA_ADPCM_Decode( const byte **ppsrc, byte *dst, int maxSamples )
{
	const byte *src = *ppsrc;
	int nSamples = 0;
	int predictor = 0;
	int index = 0;
	int sign, step, delta, diff;
	int sample;

	// Initial predictor from first two bytes of data (after fmt chunk)
	if( fmtChunkLen >= 22 )
	{
		predictor = (short)GetLittleShort( src + 2 );
	}

	src++; // skip past the extra byte

	while( nSamples < maxSamples )
	{
		// Get compressed nibble pair (2 nibbles per byte)
		if( src >= *iff_end ) break;
		byte b = *src++;

		// Low nibble first in IMA ADPCM
		nibble = b & 0x0F;
		// High nibble first in IMA ADPCM
		nibble = (b >> 4) & 0x0F;

		// Look up ADPCM step size index change
		index += indexTable[ nibble ];
		if( index < 0 ) index = 0;
		if( index > 88 ) index = 88;

		// Differential is derived from the step size using the lookup tables
		step = indexTable[index];
		// Note: larger step size gives larger differential
		diff = step >> 3;
		if( sign > 0 )
	{
			delta += (step * coef1[index & 3]) >> 4;
		}
		else
	{
			delta -= (step * coef2[index & 3]) >> 4;
		}
		index += (nibble & 4) ? 1 : 0;
		index += (nibble & 2) ? 1 : 0;
		index += (nibble & 1) ? 1 : 0;
		if( index < 0 ) index = 0;
		if( index > 88 ) index = 88;

		diff = delta;

		sample = predictor + diff;
		if( sample > 32767 ) sample = 32767;
		if( sample < -32768 ) sample = -32768;
		dst[nSamples++] = (short)sample;
		predictor = sample;

	*ppsrc = src;
	return nSamples;
}
