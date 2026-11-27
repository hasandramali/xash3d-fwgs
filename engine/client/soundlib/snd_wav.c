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
#include <limits.h>
#include "soundlib.h"

static const byte *iff_data;
static const byte *iff_dataPtr;
static const byte *iff_end;
static const byte *iff_lastChunk;
static int iff_chunkLen;

typedef struct
{
	int blockAlign;
	int dataOffset;
	int dataSize;
	int decodedBlockBytes;
	int cachedBlock;
	int cachedBytes;
	short *decoded;
	byte *encoded;
} ima_stream_t;

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
IMA ADPCM tables (IMA/DVI recommended practices)
=================
*/
static const int imaIndexTable[16] =
{
	-1, -1, -1, -1, 2, 4, 6, 8,
	-1, -1, -1, -1, 2, 4, 6, 8
};

static const int imaStepTable[89] =
{
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
	34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
	157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
	724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
	3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

/*
=================
IMA_ADPCM_Nibble

decode one 4-bit IMA ADPCM nibble, updating the running predictor
and step index, and return the reconstructed 16-bit sample
=================
*/
static short IMA_ADPCM_Nibble( int *predictor, int *index, int nibble )
{
	int step = imaStepTable[*index];
	int diff = step >> 3;

	if( nibble & 4 ) diff += step;
	if( nibble & 2 ) diff += step >> 1;
	if( nibble & 1 ) diff += step >> 2;

	if( nibble & 8 )
		*predictor -= diff;
	else
		*predictor += diff;

	if( *predictor > 32767 ) *predictor = 32767;
	else if( *predictor < -32768 ) *predictor = -32768;

	*index += imaIndexTable[nibble];
	if( *index < 0 ) *index = 0;
	else if( *index > 88 ) *index = 88;

	return (short)*predictor;
}

/*
=================
IMA_ADPCM_DecodeWAV

decode Microsoft IMA ADPCM (WAVE_FORMAT_IMA_ADPCM, tag 0x11) block data
into native 16-bit PCM. Each block starts with a per-channel 4-byte header
(predictor int16, step index u8, reserved u8). The body is a stream of
4-byte words taken round-robin across the channels, each word carrying
eight samples unpacked low nibble first.
=================
*/
static void IMA_ADPCM_DecodeWAV( short *dst, const byte *src, int dataSize,
	int blockAlign, int channels )
{
	int blocks = dataSize / blockAlign + ( dataSize % blockAlign != 0 );

	for( int block = 0; block < blocks; block++ )
	{
		const byte *p = src + block * blockAlign;
		int blockRemaining = dataSize - block * blockAlign;
		int predictor[2], index[2];

		if( blockRemaining > blockAlign )
			blockRemaining = blockAlign;
		if( blockRemaining < 4 * channels )
			break;

		// per-channel block header, the predictor is the block's first sample
		for( int ch = 0; ch < channels; ch++ )
		{
			predictor[ch] = (short)( p[0] | ( p[1] << 8 ));
			index[ch] = p[2];
			if( index[ch] > 88 ) index[ch] = 88;
			p += 4;
		}

		for( int ch = 0; ch < channels; ch++ )
			*dst++ = (short)predictor[ch];

		if( channels == 1 )
		{
			int nibbles = ( blockRemaining - 4 ) * 2;

			for( int i = 0; i < nibbles; i++ )
			{
				int nibble = ( i & 1 ) ? ( p[i >> 1] >> 4 ) & 0x0F : p[i >> 1] & 0x0F;

				*dst++ = IMA_ADPCM_Nibble( &predictor[0], &index[0], nibble );
			}
		}
		else
		{
			int groups = ( blockRemaining - 4 * channels ) / ( 4 * channels );

			for( int g = 0; g < groups; g++ )
			{
				short decoded[2][8];

				for( int ch = 0; ch < channels; ch++ )
				{
					const byte *q = p + ( g * channels + ch ) * 4;

					for( int i = 0; i < 8; i++ )
					{
						int nibble = ( i & 1 ) ? ( q[i >> 1] >> 4 ) & 0x0F : q[i >> 1] & 0x0F;

						decoded[ch][i] = IMA_ADPCM_Nibble( &predictor[ch], &index[ch], nibble );
					}
				}

				for( int i = 0; i < 8; i++ )
				{
					for( int ch = 0; ch < channels; ch++ )
						*dst++ = decoded[ch][i];
				}
			}
		}
	}
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
		if( *last_chunk < 0 || (fs_offset_t)*last_chunk + 8 > FS_FileLength( file ) ||
			FS_Seek( file, *last_chunk, SEEK_SET ) == -1 )
			return false;

		if( FS_Eof( file ))
			return false;	// didn't find the chunk

		FS_Seek( file, 4, SEEK_CUR );
		if( FS_Read( file, &iff_chunk_len, sizeof( iff_chunk_len )) != sizeof( iff_chunk_len ))
			return false;

		iff_chunk_len = LittleLong( iff_chunk_len );

		if( iff_chunk_len < 0 )
			return false;	// didn't find the chunk

		FS_Seek( file, -8, SEEK_CUR );
		fs_offset_t nextChunk = FS_Tell( file ) + 8 + ( (uint64_t)iff_chunk_len + 1 ) / 2 * 2;
		if( nextChunk > INT_MAX )
			return false;
		*last_chunk = nextChunk;
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

	if( !iff_dataPtr || iff_end - iff_dataPtr < 12 || !IsFourCC( iff_dataPtr + 8, "WAVE" ))
	{
		Con_DPrintf( S_ERROR "%s: %s missing 'RIFF/WAVE' chunks\n", __func__, name );
		return false;
	}

	// get "fmt " chunk
	iff_data = iff_dataPtr + 12;
	FindChunk( name, "fmt " );

	if( !iff_dataPtr || iff_chunkLen < 16 )
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

	if( fmt != 1 && fmt != 3 && fmt != 85 && fmt != 0x11 )
	{
		Con_DPrintf( S_ERROR "%s: %s not a microsoft PCM format\n", __func__, name );
		return false;
	}

	// mp3 stream packed in a wav container (WAVE_FORMAT_MPEGLAYER3)
	mpeg_stream = ( fmt == 85 );

	sound.channels = GetLittleShort();
	if( sound.channels != 1 && sound.channels != 2 )
	{
		Con_DPrintf( S_ERROR "%s: only mono and stereo WAV files supported (%s)\n", __func__, name );
		return false;
	}

	sound.rate = GetLittleLong();
	iff_dataPtr += 4; // skip nAvgBytesPerSec
	int blockAlign = (unsigned short)GetLittleShort();

	bitsPerSample = GetLittleShort();

	if( mpeg_stream )
	{
		srcWidth = 2; // mp3 always 16bit
	}
	else if( fmt == 0x11 )
	{
		// IMA ADPCM is decoded into 16-bit PCM below; only 4-bit is supported
		if( bitsPerSample != 4 )
		{
			Con_DPrintf( S_ERROR "%s: unsupported IMA ADPCM bit depth %d (%s)\n", __func__, bitsPerSample, name );
			return false;
		}
		if( blockAlign <= 4 * sound.channels )
		{
			Con_DPrintf( S_ERROR "%s: %s has an invalid IMA ADPCM block alignment\n", __func__, name );
			return false;
		}
		srcWidth = 2;
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

	int samples;

	if( fmt == 0x11 )
	{
		// IMA ADPCM: one block yields 1 + 8*groups samples per channel,
		// where groups counts 4-byte words per channel in the block body
		int groups = ( blockAlign - 4 * sound.channels ) / ( 4 * sound.channels );
		int samplesPerBlock = sound.channels == 1 ? 1 + ( blockAlign - 4 ) * 2 : 1 + groups * 8;
		int blocks = dataSize / blockAlign;

		uint64_t decodedSamples = (uint64_t)blocks * samplesPerBlock * sound.channels;

		if( dataSize % blockAlign )
		{
			int tailBytes = dataSize % blockAlign - 4 * sound.channels;

			if( tailBytes >= 0 )
			{
				// mirror IMA_ADPCM_DecodeWAV: mono consumes every remaining
				// byte, stereo only whole 4-byte words per channel
				if( sound.channels == 1 )
					decodedSamples += ( 1 + tailBytes * 2 );
				else
					decodedSamples += ( 1 + ( tailBytes / 8 ) * 8 ) * sound.channels;
			}
		}
		if( decodedSamples > INT_MAX / sizeof( short ))
			return false;
		sound.samples = (int)decodedSamples;
	}
	else
	{
		samples = dataSize / srcWidth;

		if( sound.samples )
		{
			if( samples < sound.samples )
			{
				Con_DPrintf( S_ERROR "%s: %s has a bad loop length\n", __func__, name );
				return false;
			}
		}
		else sound.samples = samples;
	}

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

	if( fmt == 0x11 )
	{
		IMA_ADPCM_DecodeWAV( (short *)dst, src, dataSize, blockAlign, sound.channels );
	}
	else if( srcWidth == sound.width && ( srcWidth == 1 || srcWidth == 2 ))
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
	int last_chunk = 0;
	byte header[20];
	int chunkSize, dataSize;
	uint factSamples = 0;
	file_t *file;

	if( !filename || !*filename )
		return NULL;
	file = FS_Open( filename, "rb", false );
	if( !file )
		return NULL;
	if( !StreamFindNextChunk( file, "RIFF", &last_chunk ) ||
		FS_Read( file, header, 8 ) != 8 || !IsFourCC( header + 4, "WAVE" ))
		goto invalid;

	last_chunk = 12;
	if( !StreamFindNextChunk( file, "fmt ", &last_chunk ) || FS_Read( file, &chunkSize, 4 ) != 4 )
		goto invalid;
	chunkSize = LittleLong( chunkSize );
	if( chunkSize < 16 || FS_Read( file, header, 16 ) != 16 )
		goto invalid;
	int format = header[0] | ( header[1] << 8 );
	int channels = header[2] | ( header[3] << 8 );
	uint rate = (uint)header[4] | ( (uint)header[5] << 8 ) | ( (uint)header[6] << 16 ) | ( (uint)header[7] << 24 );
	int blockAlign = header[12] | ( header[13] << 8 );
	int bits = header[14] | ( header[15] << 8 );
	if( ( channels != 1 && channels != 2 ) || !rate || rate > INT_MAX )
		goto invalid;
	if( format != 1 && format != 0x11 )
		goto invalid;
	if( format == 1 && ( ( bits != 8 && bits != 16 ) || blockAlign != channels * ( bits / 8 ) ))
		goto invalid;
	int blockSamples = 0;
	if( format == 0x11 )
	{
		if( bits != 4 || blockAlign <= 4 * channels || chunkSize < 20 || FS_Read( file, header, 4 ) != 4 )
			goto invalid;
		blockSamples = channels == 1 ? 1 + ( blockAlign - 4 ) * 2 : 1 + ( ( blockAlign - 8 ) / 8 ) * 8;
		if( ( header[0] | ( header[1] << 8 )) < 2 || ( header[2] | ( header[3] << 8 )) != blockSamples )
			goto invalid;
		last_chunk = 12;
		if( StreamFindNextChunk( file, "fact", &last_chunk ))
		{
			if( FS_Read( file, &chunkSize, 4 ) != 4 || LittleLong( chunkSize ) < 4 || FS_Read( file, &factSamples, 4 ) != 4 )
				goto invalid;
			factSamples = LittleLong( factSamples );
		}
	}
	last_chunk = 12;
	if( !StreamFindNextChunk( file, "data", &last_chunk ) || FS_Read( file, &dataSize, 4 ) != 4 )
		goto invalid;
	dataSize = LittleLong( dataSize );
	fs_offset_t dataOffset = FS_Tell( file );
	fs_offset_t available = FS_FileLength( file ) - dataOffset;
	if( dataSize <= 0 || available <= 0 || dataOffset < 0 || dataOffset > INT_MAX )
		goto invalid;
	if( dataSize > available )
		dataSize = available;
	int width = format == 0x11 ? 2 : bits / 8;
	uint64_t frames = dataSize / blockAlign;
	if( format == 0x11 )
	{
		frames *= blockSamples;
		int tail = dataSize % blockAlign - 4 * channels;
		if( tail >= 0 )
			frames += channels == 1 ? 1 + tail * 2 : 1 + ( tail / 8 ) * 8;
		if( factSamples )
			frames = Q_min( frames, factSamples );
	}
	uint64_t decodedSize = frames * channels * width;
	if( !decodedSize || decodedSize > INT_MAX )
		goto invalid;
	stream_t *stream = Mem_Calloc( host.soundpool, sizeof( stream_t ));
	stream->file = file;
	stream->size = decodedSize;
	stream->buffsize = dataOffset;
	stream->channels = channels;
	stream->width = width;
	stream->rate = rate;
	stream->type = WF_PCMDATA;
	if( format == 0x11 )
	{
		int decodedBlockBytes = blockSamples * channels * width;
		ima_stream_t *decoder = Mem_Calloc( host.soundpool, sizeof( *decoder ) + decodedBlockBytes + blockAlign );
		decoder->blockAlign = blockAlign;
		decoder->dataOffset = dataOffset;
		decoder->dataSize = dataSize;
		decoder->decodedBlockBytes = decodedBlockBytes;
		decoder->cachedBlock = -1;
		decoder->decoded = (short *)( decoder + 1 );
		decoder->encoded = (byte *)decoder->decoded + decodedBlockBytes;
		stream->ptr = decoder;
	}
	return stream;

invalid:
	Con_DPrintf( S_ERROR "%s: unsupported or malformed WAV stream (%s)\n", __func__, filename );
	FS_Close( file );
	return NULL;
}

/*
=================
Stream_ReadWAV

assume stream is valid
=================
*/
int Stream_ReadWAV( stream_t *stream, int bytes, void *buffer )
{
	if( !stream->file || !buffer || bytes <= 0 ) return 0;

	int remaining = stream->size - stream->pos;
	if( remaining <= 0 ) return 0;
	if( bytes > remaining ) bytes = remaining;

	if( stream->ptr )
	{
		ima_stream_t *decoder = stream->ptr;
		int copied = 0;
		while( copied < bytes )
		{
			int block = stream->pos / decoder->decodedBlockBytes;
			int offset = stream->pos % decoder->decodedBlockBytes;
			if( decoder->cachedBlock != block )
			{
				int encodedOffset = block * decoder->blockAlign;
				int encodedBytes = Q_min( decoder->blockAlign, decoder->dataSize - encodedOffset );
				if( encodedBytes < 4 * stream->channels ||
					FS_Seek( stream->file, (fs_offset_t)decoder->dataOffset + encodedOffset, SEEK_SET ) == -1 ||
					FS_Read( stream->file, decoder->encoded, encodedBytes ) != encodedBytes )
					break;
				int body = encodedBytes - 4 * stream->channels;
				int frames = stream->channels == 1 ? 1 + body * 2 : 1 + ( body / 8 ) * 8;
				IMA_ADPCM_DecodeWAV( decoder->decoded, decoder->encoded, encodedBytes, decoder->blockAlign, stream->channels );
				decoder->cachedBytes = frames * stream->channels * 2;
				decoder->cachedBlock = block;
			}
			int count = Q_min( bytes - copied, decoder->cachedBytes - offset );
			if( count <= 0 )
				break;
			memcpy( (byte *)buffer + copied, (byte *)decoder->decoded + offset, count );
			stream->pos += count;
			copied += count;
		}
		return copied;
	}

	bytes = FS_Read( stream->file, buffer, bytes );
	if( bytes <= 0 )
		return 0;
	stream->pos += bytes;

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
	if( newpos < 0 || (size_t)newpos > stream->size )
		return false;
	if( stream->ptr )
	{
		stream->pos = newpos;
		return true;
	}
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
	if( stream->ptr )
		Mem_Free( stream->ptr );
	Mem_Free( stream );
}
