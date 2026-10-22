/*
voice.c - voice chat implementation
Copyright (C) 2022 Velaron
Copyright (C) 2022 SNMetamorph

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#define CUSTOM_MODES 1 // required to correctly link with Opus Custom
#include <opus_custom.h>
#include <opus.h>
#include "common.h"
#include "client.h"
#include "voice.h"
#include "crclib.h"
#include "tests.h"

voice_state_t voice = { 0 };

static CVAR_DEFINE_AUTO( voice_enable, "1", FCVAR_PRIVILEGED, "enable voice chat reception" );
#if XASH_ANDROID || XASH_IOS
static CVAR_DEFINE_AUTO( voice_receiveonly, "1", FCVAR_PRIVILEGED, "receive voice without opening a microphone" );
#else
static CVAR_DEFINE_AUTO( voice_receiveonly, "0", FCVAR_PRIVILEGED, "receive voice without opening a microphone" );
#endif
CVAR_DEFINE_AUTO( voice_loopback, "0", FCVAR_PRIVILEGED, "loopback voice back to the speaker" );
static CVAR_DEFINE_AUTO( voice_scale, "1.0", FCVAR_PRIVILEGED | FCVAR_ARCHIVE, "incoming voice volume scale" );
static CVAR_DEFINE_AUTO( voice_showincoming, "0", FCVAR_PRIVILEGED, "print incoming voice packet sizes and decoded samples" );
static CVAR_DEFINE_AUTO( voice_transmit_scale, "1.0", FCVAR_PRIVILEGED | FCVAR_ARCHIVE, "outcoming voice volume scale" );
static CVAR_DEFINE_AUTO( voice_avggain, "0.5", FCVAR_PRIVILEGED | FCVAR_ARCHIVE, "automatic voice gain control (average)" );
static CVAR_DEFINE_AUTO( voice_maxgain, "2.0", FCVAR_PRIVILEGED | FCVAR_ARCHIVE, "automatic voice gain control (maximum)" );
static CVAR_DEFINE_AUTO( voice_inputprocessing, "1", FCVAR_PRIVILEGED | FCVAR_ARCHIVE, "microphone noise suppression and speech filtering" );
static CVAR_DEFINE_AUTO( voice_inputfromfile, "0", FCVAR_PRIVILEGED, "input voice from voice_input.wav" );

static void Voice_StartChannel( uint samples, byte *data, int entnum );

static qboolean Voice_ReceiveOnly( void )
{
#if XASH_ANDROID || XASH_IOS
	const char *microphone = getenv( "XASH3D_MICROPHONE" );
	if( !microphone || Q_strcmp( microphone, "1" ))
		return true;
#endif
	return voice_receiveonly.value != 0;
}

/*
===============================================================================

	UTILITY FUNCTIONS

===============================================================================
*/

/*
=========================
Voice_IsGoldSrcMode

Check if codec is GoldSrc mode
=========================
*/
static qboolean Voice_IsGoldSrcMode( const char *codec )
{
	// check for an empty codec string
	// if the server does not use ReVoice or VTC
	// it will send an empty codec
	// however, decoding is still possible
	// if the voice data contains Opus
	if( COM_StringEmptyOrNULL( codec ))
		return true;

	if( Q_strstr( codec, "voice_speex" ))
		return true;

	return false;
}

/*
=========================
Voice_IsOpusCustomMode

Check if codec is Opus Custom mode
=========================
*/
static qboolean Voice_IsOpusCustomMode( const char *codec )
{
	return Q_strcmp( codec, VOICE_OPUS_CUSTOM_CODEC ) == 0;
}

/*
=========================
Voice_GetBitrateForQuality

Get bitrate for given quality level
=========================
*/
static int Voice_GetBitrateForQuality( int quality, qboolean goldsrc )
{
	switch( quality )
	{
	case 1: return 6000;   // 6 kbps
	case 2: return 12000;  // 12 kbps
	case 3: return 24000;  // 24 kbps
	case 4: return 36000;  // 36 kbps
	case 5: return 48000;  // 48 kbps
	default: return 36000; // default
	}
}

/*
===============================================================================

	OPUS INTEGRATION

===============================================================================
*/

/*
=========================
Voice_InitCustomMode

Initialize Opus Custom mode
=========================
*/
static qboolean Voice_InitCustomMode( void )
{
	int err = 0;

	voice.width = sizeof( int16_t );
	voice.samplerate = VOICE_OPUS_CUSTOM_SAMPLERATE;
	voice.frame_size = VOICE_OPUS_CUSTOM_FRAME_SIZE;

	voice.custom_mode = opus_custom_mode_create( SOUND_44k, voice.frame_size, &err );

	if( !voice.custom_mode )
	{
		Con_Printf( S_ERROR "Can't create Opus Custom mode: %s\n", opus_strerror( err ));
		return false;
	}

	return true;
}

/*
=========================
Voice_InitOpusDecoder

Initialize Opus decoders for clients
=========================
*/
static qboolean Voice_InitOpusDecoder( void )
{
	int err = 0;

	for( int i = 0; i < cl.maxclients; i++ )
	{
		if( voice.goldsrc )
		{
			voice.gs_decoders[i] = opus_decoder_create( GS_DEFAULT_SAMPLE_RATE, VOICE_PCM_CHANNELS, &err );
			if( !voice.gs_decoders[i] )
			{
				Con_Printf( S_ERROR "Can't create GoldSrc Opus decoder for %i: %s\n", i, opus_strerror( err ));
				return false;
			}
		}
		else
		{
			voice.decoders[i] = opus_custom_decoder_create( voice.custom_mode, VOICE_PCM_CHANNELS, &err );
			if( !voice.decoders[i] )
			{
				Con_Printf( S_ERROR "Can't create Custom Opus decoder for %i: %s\n", i, opus_strerror( err ));
				return false;
			}
		}
	}

	return true;
}

/*
=========================
Voice_InitOpusEncoder

Initialize Opus encoder with quality settings
=========================
*/
static qboolean Voice_InitOpusEncoder( int quality )
{
	int err = 0;
	int bitrate = Voice_GetBitrateForQuality( quality, voice.goldsrc );

	if( voice.goldsrc )
	{
		voice.gs_encoder = opus_encoder_create( GS_DEFAULT_SAMPLE_RATE, VOICE_PCM_CHANNELS, OPUS_APPLICATION_VOIP, &err );
		if( !voice.gs_encoder )
		{
			Con_Printf( S_ERROR "Can't create GoldSrc Opus encoder: %s\n", opus_strerror( err ));
			return false;
		}
		opus_encoder_ctl( voice.gs_encoder, OPUS_SET_DTX( 1 ));
		opus_encoder_ctl( voice.gs_encoder, OPUS_SET_BITRATE( bitrate ));
		opus_encoder_ctl( voice.gs_encoder, OPUS_SET_SIGNAL( OPUS_SIGNAL_VOICE ));
	}
	else
	{
		voice.encoder = opus_custom_encoder_create( voice.custom_mode, VOICE_PCM_CHANNELS, &err );
		if( !voice.encoder )
		{
			Con_Printf( S_ERROR "Can't create Opus encoder: %s\n", opus_strerror( err ));
			return false;
		}
		opus_custom_encoder_ctl( voice.encoder, OPUS_SET_BITRATE( bitrate ));
	}

	return true;
}

/*
=========================
Voice_ShutdownOpusDecoder

Cleanup Opus decoders
=========================
*/
static void Voice_ShutdownOpusDecoder( void )
{
	for( int i = 0; i < MAX_CLIENTS; i++ )
	{
		if( voice.goldsrc )
		{
			if( voice.gs_decoders[i] )
			{
				opus_decoder_destroy( voice.gs_decoders[i] );
				voice.gs_decoders[i] = NULL;
			}
		}
		else
		{
			if( voice.decoders[i] )
			{
				opus_custom_decoder_destroy( voice.decoders[i] );
				voice.decoders[i] = NULL;
			}
		}
	}
}

/*
=========================
Voice_ShutdownOpusEncoder

Cleanup Opus encoder
=========================
*/
static void Voice_ShutdownOpusEncoder( void )
{
	if( voice.goldsrc )
	{
		if( voice.gs_encoder )
		{
			opus_encoder_destroy( voice.gs_encoder );
			voice.gs_encoder = NULL;
		}
	}
	else
	{
		if( voice.encoder )
		{
			opus_custom_encoder_destroy( voice.encoder );
			voice.encoder = NULL;
		}
	}
}

/*
=========================
Voice_ShutdownCustomMode

Cleanup Opus Custom mode
=========================
*/
static void Voice_ShutdownCustomMode( void )
{
	if( voice.custom_mode )
	{
		opus_custom_mode_destroy( voice.custom_mode );
		voice.custom_mode = NULL;
	}
}

/*
=========================
Voice_InitGoldSrcMode

Initialize GoldSrc voice mode
=========================
*/
static qboolean Voice_InitGoldSrcMode( int quality )
{
	voice.goldsrc = true;
	voice.width = sizeof( int16_t );
	voice.samplerate = GS_DEFAULT_SAMPLE_RATE;
	voice.frame_size = GS_DEFAULT_FRAME_SIZE;
	voice.gs_sequence = 0;

	if( !Voice_InitOpusDecoder())
	{
		Con_Printf( S_ERROR "Can't create GoldSrc decoders, voice chat is disabled.\n" );
		return false;
	}

	if( !Voice_ReceiveOnly() && !Voice_InitOpusEncoder( quality ))
	{
		Con_Printf( S_WARN "Other players will not be able to hear you.\n" );
		return false;
	}

	return true;
}

/*
=========================
Voice_InitOpusCustomMode

Initialize Opus Custom voice mode
=========================
*/
static qboolean Voice_InitOpusCustomMode( int quality )
{
	voice.goldsrc = false;
	voice.samplerate = VOICE_OPUS_CUSTOM_SAMPLERATE;
	voice.frame_size = VOICE_OPUS_CUSTOM_FRAME_SIZE;
	voice.width = sizeof( int16_t );

	if( !Voice_InitCustomMode() || !Voice_InitOpusDecoder())
	{
		Con_Printf( S_ERROR "Can't create Opus Custom decoders, voice chat is disabled.\n" );
		return false;
	}

	if( !Voice_ReceiveOnly() && !Voice_InitOpusEncoder( quality ))
	{
		Con_Printf( S_WARN "Other players will not be able to hear you.\n" );
		return false;
	}

	return true;
}

/*
=========================
Voice_ShutdownGoldSrcMode

Cleanup GoldSrc mode
=========================
*/
static void Voice_ShutdownGoldSrcMode( void )
{
	Voice_ShutdownOpusDecoder();
	Voice_ShutdownOpusEncoder();
}

/*
=========================
Voice_ShutdownOpusCustomMode

Cleanup Opus Custom mode
=========================
*/
static void Voice_ShutdownOpusCustomMode( void )
{
	Voice_ShutdownOpusDecoder();
	Voice_ShutdownOpusEncoder();
	Voice_ShutdownCustomMode();
}

/*
===============================================================================

	VOICE PROCESSING

===============================================================================
*/

/*
=========================
Voice_ApplyGainAdjust

Apply automatic gain control to voice samples
=========================
*/
static void Voice_ApplyGainAdjust( int16_t *samples, int count, float scale )
{
	float filtered[VOICE_OPUS_CUSTOM_FRAME_SIZE];
	float energy = 0.0f, peak = 0.0f;
	voice_autogain_t *state = &voice.autogain;
	qboolean processing = voice_inputprocessing.value != 0;

	if( count <= 0 || count > ARRAYSIZE( filtered ) || !voice.samplerate ) return;
	float coefficient = expf( -2.0f * M_PI * 80.0f / voice.samplerate );
	if( !isfinite( scale )) scale = 1.0f;
	scale = bound( 0.0f, scale, 4.0f );
	if( !state->initialized )
	{
		state->current_gain = 1.0f;
		state->gate_gain = 1.0f;
		state->noise_rms = 64.0f;
		state->previous_input = samples[0];
		state->initialized = true;
	}
	for( int index = 0; index < count; index++ )
	{
		float input = samples[index];
		float output = coefficient * ( state->highpass_output + input - state->previous_input );
		state->previous_input = input;
		state->highpass_output = output;
		filtered[index] = processing ? output : input;
		energy += filtered[index] * filtered[index];
		peak = Q_max( peak, fabsf( filtered[index] ));
	}
	float rms = sqrtf( energy / count );
	if( rms < state->noise_rms * 1.5f )
		state->noise_rms += 0.05f * ( rms - state->noise_rms );
	state->noise_rms = bound( 16.0f, state->noise_rms, 128.0f );
	float threshold = Q_max( 96.0f, state->noise_rms * 2.5f );
	float gate = processing ? bound( 0.1f, rms * rms / ( threshold * threshold ), 1.0f ) : 1.0f;
	float target = 4096.0f * ( 0.5f + bound( 0.0f, voice_avggain.value, 1.0f ));
	float desired = rms > threshold ? Q_min( target / rms, bound( 1.0f, voice_maxgain.value, 10.0f )) : 1.0f;
	float duration = (float)count / voice.samplerate;
	float smoothing = 1.0f - expf( -duration / ( desired < state->current_gain ? 0.02f : 0.5f ));
	float gain = state->current_gain + smoothing * ( desired - state->current_gain );
	float ceiling = peak > 0.0f ? 28000.0f / peak : 10.0f;
	float start = Q_min( state->current_gain * scale, ceiling );
	float finish = Q_min( gain * scale, ceiling );
	float gate_end = state->gate_gain + ( 1.0f - expf( -duration / ( gate > state->gate_gain ? 0.01f : 0.15f ))) * ( gate - state->gate_gain );
	for( int index = 0; index < count; index++ )
	{
		float fraction = (float)( index + 1 ) / count;
		float amplitude = start + ( finish - start ) * fraction;
		float attenuation = state->gate_gain + ( gate_end - state->gate_gain ) * fraction;
		samples[index] = (int16_t)bound( -28000.0f, filtered[index] * amplitude * attenuation, 28000.0f );
	}
	state->current_gain = gain;
	state->gate_gain = gate_end;
}

/*
=========================
Voice_GetOpusCompressedData

Get compressed voice data for Opus Custom mode
=========================
*/
static uint Voice_GetOpusCompressedData( byte *out, uint maxsize, uint *frames )
{
	uint ofs = 0, size = 0;
	uint frame_size_bytes = voice.frame_size * voice.width;

	if( voice.input_file )
	{
		double curtime = Platform_DoubleTime();
		double updateInterval = curtime - voice.start_time;
		voice.start_time = curtime;

		uint numbytes = updateInterval * voice.samplerate * voice.width * VOICE_PCM_CHANNELS;
		numbytes = Q_min( numbytes, voice.input_file->size - voice.input_file_pos );
		numbytes = Q_min( numbytes, sizeof( voice.input_buffer ) - voice.input_buffer_pos );

		memcpy( voice.input_buffer + voice.input_buffer_pos, voice.input_file->buffer + voice.input_file_pos, numbytes );
		voice.input_buffer_pos += numbytes;
		voice.input_file_pos += numbytes;
	}

	if( !voice.input_file )
		VoiceCapture_Lock( true );

	for( ofs = 0; voice.input_buffer_pos - ofs >= frame_size_bytes && ofs <= voice.input_buffer_pos; ofs += frame_size_bytes )
	{
		int bytes;

		if( !voice.input_file )
			Voice_ApplyGainAdjust((int16_t *)( voice.input_buffer + ofs ), voice.frame_size, voice_transmit_scale.value );

		bytes = opus_custom_encode( voice.encoder, (const int16_t *)( voice.input_buffer + ofs ),
					    voice.frame_size, out + size + sizeof( uint16_t ), maxsize );

		if( bytes > 0 )
		{
			// write compressed frame size
			*((uint16_t *)&out[size] ) = LittleShort( bytes );

			size += bytes + sizeof( uint16_t );
			maxsize -= bytes + sizeof( uint16_t );

			( *frames )++;
		}
		else
		{
			Con_Printf( S_ERROR "%s: failed to encode frame: %s\n", __func__, opus_strerror( bytes ));
		}
	}

	// did we compress anything? update counters
	if( ofs )
	{
		fs_offset_t remaining = voice.input_buffer_pos - ofs;
		// move remaining samples to the beginning of buffer
		memmove( voice.input_buffer, voice.input_buffer + ofs, remaining );
		voice.input_buffer_pos = remaining;
	}

	if( !voice.input_file )
		VoiceCapture_Lock( false );

	return size;
}

/*
=========================
Voice_GetGSCompressedData

Get compressed voice data for GoldSrc mode
=========================
*/
static uint Voice_GetGSCompressedData( byte *out, uint maxsize, uint *frames )
{
	uint ofs = 0, size = 0;
	const uint      frame_size_samples = GS_DEFAULT_FRAME_SIZE;
	const uint      frame_size_bytes = GS_DEFAULT_FRAME_SIZE * voice.width;

	if( voice.input_file )
	{
		double curtime = Platform_DoubleTime();
		double updateInterval = curtime - voice.start_time;
		voice.start_time = curtime;

		uint numbytes = updateInterval * voice.samplerate * voice.width * VOICE_PCM_CHANNELS;
		numbytes = Q_min( numbytes, voice.input_file->size - voice.input_file_pos );
		numbytes = Q_min( numbytes, sizeof( voice.input_buffer ) - voice.input_buffer_pos );

		memcpy( voice.input_buffer + voice.input_buffer_pos, voice.input_file->buffer + voice.input_file_pos, numbytes );
		voice.input_buffer_pos += numbytes;
		voice.input_file_pos += numbytes;
	}

	if( !voice.input_file )
		VoiceCapture_Lock( true );

	*frames = 0;

	while( voice.input_buffer_pos - ofs >= frame_size_bytes && maxsize > sizeof( uint32_t ))
	{
		int     bytes;
		int     is_silence = 1;
		int16_t samples[GS_DEFAULT_FRAME_SIZE];
		voice_autogain_t saved_gain = voice.autogain;
		memcpy( samples, voice.input_buffer + ofs, frame_size_bytes );

		for( uint i = 0; i < frame_size_samples; ++i )
		{
			if( samples[i] != 0 )
			{
				is_silence = 0;
				break;
			}
		}
		if( is_silence )
		{
			ofs += frame_size_bytes;
			continue;
		}

		if( !voice.input_file )
			Voice_ApplyGainAdjust( samples, frame_size_samples, voice_transmit_scale.value );

		bytes = opus_encode( voice.gs_encoder, samples,
				     frame_size_samples, out + size + 4, maxsize - 4 );

		if( bytes > 0 )
		{
			uint16_t frameBytes = LittleShort( bytes );
			uint16_t sequence = LittleShort( voice.gs_sequence++ );
			memcpy( out + size, &frameBytes, sizeof( frameBytes ));
			memcpy( out + size + 2, &sequence, sizeof( sequence ));

			size += bytes + sizeof( uint32_t );
			maxsize -= bytes + sizeof( uint32_t );

			( *frames )++;
		}
		else if( bytes == OPUS_BUFFER_TOO_SMALL )
		{
			voice.autogain = saved_gain;
			break;
		}
		else
		{
			Con_Printf( S_ERROR "%s: failed to encode frame: %s\n", __func__, opus_strerror( bytes ));
		}

		ofs += frame_size_bytes;
	}

	if( ofs )
	{
		fs_offset_t remaining = voice.input_buffer_pos - ofs;
		memmove( voice.input_buffer, voice.input_buffer + ofs, remaining );
		voice.input_buffer_pos = remaining;
	}

	if( !voice.input_file )
		VoiceCapture_Lock( false );

	return size;
}

/*
=========================
Voice_ProcessGSData

Process GoldSrc voice data and return number of samples
=========================
*/
static int Voice_ProcessGSData( int ent, const uint8_t *data, uint32_t size )
{
	uint samples = 0;
	uint offset = sizeof( uint64_t );
	uint source_rate = GS_DEFAULT_SAMPLE_RATE;
	uint payload_end;
	uint32_t crc_in_packet;
	uint32_t crc = CRC32_INIT_VALUE;
	OpusDecoder *decoder;

	if( !data || size < sizeof( uint64_t ) + sizeof( uint32_t ) || ent <= 0 || ent > cl.maxclients )
		return 0;

	payload_end = size - sizeof( uint32_t );
	memcpy( &crc_in_packet, data + payload_end, sizeof( crc_in_packet ));
	crc_in_packet = LittleLong( crc_in_packet );
	CRC32_ProcessBuffer( &crc, data, payload_end );
	crc = CRC32_Final( crc );

	if( crc != crc_in_packet )
	{
		Con_Printf( S_WARN "Voice packet CRC32 mismatch\n" );
		return 0;
	}

	decoder = voice.gs_decoders[ent - 1];
	if( !decoder )
		return 0;

	while( offset < payload_end )
	{
		uint vpc_type = data[offset++];
		uint16_t data_len;
		uint chunk_end;

		if( payload_end - offset < sizeof( data_len ))
			return 0;
		memcpy( &data_len, data + offset, sizeof( data_len ));
		data_len = (uint16_t)LittleShort( data_len );
		offset += sizeof( data_len );

		if( vpc_type == GS_VPC_SETSAMPLERATE )
		{
			if( !data_len )
				return 0;
			source_rate = data_len;
			continue;
		}
		if( vpc_type == GS_VPC_VDATA_SILENCE )
		{
			uint silence_samples = (uint)data_len * GS_DEFAULT_SAMPLE_RATE / source_rate;
			if( silence_samples > GS_MAX_DECOMPRESSED_SAMPLES - samples )
				return 0;
			memset( (int16_t *)voice.decompress_buffer + samples, 0, silence_samples * sizeof( int16_t ));
			samples += silence_samples;
			continue;
		}

		if( data_len > payload_end - offset )
			return 0;
		chunk_end = offset + data_len;
		if( vpc_type != GS_VPC_VDATA_OPUS_PLC )
		{
			Con_Printf( S_WARN "Unsupported Steam voice data type: %u\n", vpc_type );
			return 0;
		}

		while( offset < chunk_end )
		{
			uint16_t frame_size;
			int decoded;
			if( chunk_end - offset < sizeof( uint32_t ))
				return 0;
			memcpy( &frame_size, data + offset, sizeof( frame_size ));
			frame_size = (uint16_t)LittleShort( frame_size );
			offset += sizeof( uint32_t );
			if( frame_size == 0xFFFF )
			{
				opus_decoder_ctl( decoder, OPUS_RESET_STATE );
				continue;
			}
			if( frame_size > chunk_end - offset || samples == GS_MAX_DECOMPRESSED_SAMPLES )
				return 0;
			if( !frame_size )
			{
				if( GS_DEFAULT_FRAME_SIZE > GS_MAX_DECOMPRESSED_SAMPLES - samples )
					return 0;
				memset( (int16_t *)voice.decompress_buffer + samples, 0, GS_DEFAULT_FRAME_SIZE * sizeof( int16_t ));
				samples += GS_DEFAULT_FRAME_SIZE;
				continue;
			}
			decoded = opus_decode( decoder, data + offset, frame_size,
				(int16_t *)voice.decompress_buffer + samples, GS_MAX_DECOMPRESSED_SAMPLES - samples, 0 );
			if( decoded < 0 )
			{
				Con_Printf( S_WARN "Opus decode error: %s\n", opus_strerror( decoded ));
				return 0;
			}
			samples += decoded;
			offset += frame_size;
		}
	}

	return samples;
}

/*
=========================
Voice_CreateGSVoicePacket

Create GoldSrc voice packet
=========================
*/
static uint Voice_CreateGSVoicePacket( byte *out, const byte *voice_data, uint voice_size )
{
	uint     offset = 0;
	uint32_t crc;

	if( !out || !voice_data || voice_size == 0 || voice_size > VOICE_MAX_GS_DATA_SIZE - 18 )
	{
		Con_Printf( S_WARN "Invalid voice data for packet creation\n" );
		return 0;
	}

	memcpy( out + offset, cls.steamid, sizeof( cls.steamid ));

	offset += sizeof( uint64_t );

	out[offset] = GS_VPC_SETSAMPLERATE;
	offset++;

	uint16_t sample_rate = LittleShort( GS_DEFAULT_SAMPLE_RATE );
	memcpy( out + offset, &sample_rate, sizeof( sample_rate ));
	offset += sizeof( uint16_t );

	out[offset] = GS_VPC_VDATA_OPUS_PLC;
	offset++;

	uint16_t payload_size = LittleShort( voice_size );
	memcpy( out + offset, &payload_size, sizeof( payload_size ));
	offset += sizeof( uint16_t );

	memcpy( out + offset, voice_data, voice_size );
	offset += voice_size;

	crc = CRC32_INIT_VALUE;
	CRC32_ProcessBuffer( &crc, out, offset );
	crc = CRC32_Final( crc );
	crc = LittleLong( crc );
	memcpy( out + offset, &crc, sizeof( crc ));
	offset += sizeof( uint32_t );

	return offset;
}

/*
===============================================================================

	VOICE CHAT INTEGRATION

===============================================================================
*/

/*
=========================
Voice_Status

Notify user dll about voice transmission
=========================
*/
static void Voice_Status( int entindex, qboolean bTalking )
{
	if( cls.state == ca_active && clgame.dllFuncs.pfnVoiceStatus )
		clgame.dllFuncs.pfnVoiceStatus( entindex, bTalking );
}

/*
=========================
Voice_StatusTimeout

Waits few milliseconds and if there was no
voice transmission, sends notification
=========================
*/
static void Voice_StatusTimeout( voice_status_t *status, int entindex, double frametime )
{
	if( status->talking_ack )
	{
		status->talking_timeout += frametime;
		if( status->talking_timeout > 0.2 )
		{
			status->talking_ack = false;
			Voice_Status( entindex, false );
		}
	}
}

/*
=========================
Voice_StatusAck

Sends notification to user dll and
zeroes timeouts for this client
=========================
*/
static void Voice_StatusAck( voice_status_t *status, int playerIndex )
{
	if( !status->talking_ack )
		Voice_Status( playerIndex, true );

	status->talking_ack = true;
	status->talking_timeout = 0.0;
}

/*
=========================
Voice_IsRecording

Check if voice is currently recording
=========================
*/
qboolean Voice_IsRecording( void )
{
	return voice.is_recording;
}

/*
=========================
Voice_RecordStop

Stop voice recording
=========================
*/
void Voice_RecordStop( void )
{
	if( voice.input_file )
	{
		FS_FreeSound( voice.input_file );
		voice.input_file = NULL;
	}

	VoiceCapture_Activate( false );
	if( voice.device_opened )
	{
		VoiceCapture_Shutdown();
		voice.device_opened = false;
	}
	voice.is_recording = false;

	Voice_Status( VOICE_LOCALCLIENT_INDEX, false );

	voice.input_buffer_pos = 0;
	memset( voice.input_buffer, 0, sizeof( voice.input_buffer ));
	memset( &voice.autogain, 0, sizeof( voice.autogain ));
}

/*
=========================
Voice_RecordStart

Start voice recording
=========================
*/
void Voice_RecordStart( void )
{
	Voice_RecordStop();

	if( !voice_enable.value || Voice_ReceiveOnly() )
		return;
	if( !voice.initialized && cl_sven_proto && cls.net_protocol == PROTO_GOLDSRC && cls.state == ca_active )
		Voice_Init( "", 5, false );
	if( !voice.initialized || ( voice.goldsrc ? !voice.gs_encoder : !voice.encoder ))
		return;

	if( voice_inputfromfile.value )
	{
		voice.input_file = FS_LoadSound( "voice_input.wav", NULL, 0 );

		if( voice.input_file )
		{
			Sound_Process( &voice.input_file, voice.samplerate, voice.width, VOICE_PCM_CHANNELS, SOUND_RESAMPLE );
			voice.input_file_pos = 0;

			voice.start_time = Platform_DoubleTime();
			voice.is_recording = true;
		}
		else
		{
			FS_FreeSound( voice.input_file );
			voice.input_file = NULL;
		}
	}

	if( !Voice_IsRecording() )
	{
		voice.device_opened = VoiceCapture_Init();
		if( voice.device_opened )
			voice.is_recording = VoiceCapture_Activate( true );
		else
			Con_Printf( S_WARN "No microphone is available. Voice reception remains enabled.\n" );
	}

	if( Voice_IsRecording())
		Voice_Status( VOICE_LOCALCLIENT_INDEX, true );
}

/*
=========================
Voice_Disconnect

We're disconnected from server
stop recording and notify user dlls
=========================
*/
void Voice_Disconnect( void )
{
	Voice_RecordStop();

	if( voice.local.talking_ack )
	{
		Voice_Status( VOICE_LOOPBACK_INDEX, false );
		voice.local.talking_ack = false;
	}

	for( int i = 1; i <= MAX_CLIENTS; i++ )
		Voice_Status( i, false );

	VoiceCapture_Shutdown();
	voice.device_opened = false;

	if( voice.goldsrc )
		Voice_ShutdownGoldSrcMode();
	else
		Voice_ShutdownOpusCustomMode();

	voice.initialized = false;
}

/*
=========================
Voice_StartChannel

Feed the decoded data to engine sound subsystem
=========================
*/
static void Voice_StartChannel( uint samples, byte *data, int entnum )
{
	if( clgame.dllFuncs.pfnVoice_StartChannel != NULL )
	{
		if( clgame.dllFuncs.pfnVoice_StartChannel( samples, data, entnum ))
			return;
	}

	SND_ForceInitMouth( entnum );
	S_RawEntSamples( entnum, samples, voice.samplerate, voice.width, VOICE_PCM_CHANNELS, data, bound( 0, 255 * voice_scale.value, 255 ), ATTN_NONE );
	Voice_Status( entnum, true );
}

/*
=========================
Voice_StopChannel

Called by mixer when channel idles
=========================
*/
void Voice_StopChannel( int entnum )
{
	Voice_Status( entnum, false );
}


/*
=========================
Voice_AddIncomingData

Received encoded voice data, decode it
=========================
*/
void Voice_LoopbackAck( void )
{
	Voice_StatusAck( &voice.local, VOICE_LOOPBACK_INDEX );
}

/*
=========================
Voice_AddIncomingData

Received encoded voice data, decode it
=========================
*/
void Voice_AddIncomingData( int ent, const byte *data, uint size, uint frames )
{
	const int playernum = ent - 1;
	int samples = 0;
	int ofs = 0;

	if( !voice.initialized && cl_sven_proto && cls.net_protocol == PROTO_GOLDSRC && voice_enable.value
		&& ent > 0 && ent <= cl.maxclients && data && size >= 12 )
		Voice_Init( "", 5, false );
	if( !voice.initialized || !voice_enable.value )
		return;

	if( voice.goldsrc )
	{
		samples = Voice_ProcessGSData( ent, (const uint8_t *)data, size );
	}
	else
	{
		// Validate player index and decoder
		if( playernum < 0 || playernum >= cl.maxclients || !voice.decoders[playernum] )
			return;

		// decode frame by frame
		for( ;; )
		{
			int      frame_samples;
			uint16_t compressed_size;

			// no compressed size mark
			if( ofs + sizeof( uint16_t ) > size )
				break;

			memcpy( &compressed_size, data + ofs, sizeof( compressed_size ));
			compressed_size = (uint16_t)LittleShort( compressed_size );
			ofs += sizeof( uint16_t );

			// no frame data
			if( ofs + compressed_size > size )
				break;
			if( voice.frame_size > GS_MAX_DECOMPRESSED_SAMPLES - samples )
				break;

			frame_samples = opus_custom_decode( voice.decoders[playernum], data + ofs, compressed_size,
							    (int16_t *)voice.decompress_buffer + samples, voice.frame_size );
			if( frame_samples <= 0 )
				break;

			ofs += compressed_size;
			samples += frame_samples;
		}
	}

	if( samples > 0 )
		Voice_StartChannel( samples, voice.decompress_buffer, ent );
	if( voice_showincoming.value )
		Con_Printf( "Voice: player %d, %u bytes, %d samples\n", ent, size, samples );
}

/*
=========================
CL_AddVoiceToDatagram

Encode our voice data and send it to server
=========================
*/
void CL_AddVoiceToDatagram( void )
{
	byte buffer[VOICE_MAX_DATA_SIZE];
	uint size, frames = 0;

	if( cls.state != ca_active || !voice_enable.value || Voice_ReceiveOnly() || !Voice_IsRecording())
		return;

	if( voice.goldsrc )
	{
		if( !voice.gs_encoder )
			return;

		uint available = MSG_GetNumBytesLeft( &cls.datagram );
		if( available <= 21 ) return;
		uint packet_limit = Q_min( available - 3, 1200 );
		size = Voice_GetGSCompressedData( buffer, packet_limit - 18, &frames );
		if( size > 0 && MSG_GetNumBytesLeft( &cls.datagram ) >= size + 21 )
		{
			uint packet_size = Voice_CreateGSVoicePacket( voice.compress_buffer, buffer, size );
			MSG_BeginClientCmd( &cls.datagram, clc_voicedata );
			MSG_WriteShort( &cls.datagram, packet_size );
			MSG_WriteBytes( &cls.datagram, voice.compress_buffer, packet_size );
		}
		return;
	}

	if( !voice.encoder )
		return;

	size = Voice_GetOpusCompressedData( voice.compress_buffer, sizeof( voice.compress_buffer ), &frames );
	if( size > 0 && MSG_GetNumBytesLeft( &cls.datagram ) >= size + 32 )
	{
		MSG_BeginClientCmd( &cls.datagram, clc_voicedata );
		MSG_WriteByte( &cls.datagram, voice_loopback.value != 0 );
		MSG_WriteByte( &cls.datagram, frames );
		MSG_WriteShort( &cls.datagram, size );
		MSG_WriteBytes( &cls.datagram, voice.compress_buffer, size );
	}
}

/*
=========================
Voice_RegisterCvars

Register voice related cvars and commands
=========================
*/
void Voice_RegisterCvars( void )
{
	Cvar_RegisterVariable( &voice_enable );
	Cvar_RegisterVariable( &voice_receiveonly );
#if XASH_ANDROID || XASH_IOS
	const char *microphone = getenv( "XASH3D_MICROPHONE" );
	Cvar_SetValue( "voice_receiveonly", !microphone || Q_strcmp( microphone, "1" ));
#endif
	Cvar_RegisterVariable( &voice_showincoming );
	Cvar_RegisterVariable( &voice_loopback );
	Cvar_RegisterVariable( &voice_scale );
	Cvar_RegisterVariable( &voice_transmit_scale );
	Cvar_RegisterVariable( &voice_avggain );
	Cvar_RegisterVariable( &voice_maxgain );
	Cvar_RegisterVariable( &voice_inputprocessing );
	Cvar_RegisterVariable( &voice_inputfromfile );
}

/*
=========================
Voice_Shutdown

Completely shutdown the voice subsystem
=========================
*/
static void Voice_Shutdown( void )
{
	Voice_Disconnect();

	voice.initialized = false;
	voice.is_recording = false;
	voice.goldsrc = false;
	voice.start_time = 0.0;
	voice.samplerate = 0;
	voice.frame_size = 0;
	voice.width = 0;

	voice.input_buffer_pos = 0;
	voice.input_file_pos = 0;
	memset( voice.input_buffer, 0, sizeof( voice.input_buffer ));
	memset( voice.compress_buffer, 0, sizeof( voice.compress_buffer ));
	memset( voice.decompress_buffer, 0, sizeof( voice.decompress_buffer ));
	memset( &voice.local, 0, sizeof( voice.local ));
	memset( &voice.autogain, 0, sizeof( voice.autogain ));
}

/*
=========================
Voice_Idle

Run timeout for clients
=========================
*/
void Voice_Idle( double frametime )
{
	if( Voice_IsRecording() && ( host.status != HOST_FRAME || !voice_enable.value || Voice_ReceiveOnly() ))
		Voice_RecordStop();
	if( FBitSet( voice_enable.flags | voice_receiveonly.flags, FCVAR_CHANGED ))
	{
		ClearBits( voice_enable.flags, FCVAR_CHANGED );
		ClearBits( voice_receiveonly.flags, FCVAR_CHANGED );

		if( voice_enable.value )
		{
			if( cls.state == ca_active )
				Voice_Init( voice.codec, voice.quality, false );
		}
		else
			Voice_Shutdown();
	}

	// update local player status first
	Voice_StatusTimeout( &voice.local, VOICE_LOOPBACK_INDEX, frametime );
}

/*
=========================
Voice_Init

Initialize the voice subsystem
=========================
*/
qboolean Voice_Init( const char *pszCodecName, int quality, qboolean preinit )
{
	string codec;
	Q_strncpy( codec, pszCodecName, sizeof( codec ));
	pszCodecName = codec;
	Q_strncpy( voice.codec, pszCodecName, sizeof( voice.codec ));
	voice.quality = quality;

	if( !voice_enable.value )
		return false;

	if( preinit )
		return true;

	Voice_Disconnect();

	if( Voice_IsGoldSrcMode( pszCodecName ))
	{
		if( !Voice_InitGoldSrcMode( quality ))
		{
			Voice_Shutdown();
			return false;
		}
	}
	else if( Voice_IsOpusCustomMode( pszCodecName ))
	{
		if( !Voice_InitOpusCustomMode( quality ))
		{
			Voice_Shutdown();
			return false;
		}
	}
	else
	{
		// unsupported codec
		Con_Printf( S_WARN "Server requested unsupported voice codec: %s\n", pszCodecName );
		Voice_Shutdown();
		return false;
	}

	voice.initialized = true;
	Con_Printf( "Voice initialized: %s, %u Hz%s\n", voice.goldsrc ? "Steam/Opus" : voice.codec,
		voice.samplerate, Voice_ReceiveOnly() ? ", receive only" : ", push-to-talk" );
	return true;
}

voice_audio_info_t Voice_GetAudioInfo( void )
{
	voice_audio_info_t info =
	{
		.width = voice.width,
		.samplerate = voice.samplerate,
		.frame_size = voice.frame_size,
	};

	return info;
}

#if XASH_ENGINE_TESTS
static void Test_VoiceInputProcessing( void )
{
	voice_autogain_t saved_gain = voice.autogain;
	uint saved_rate = voice.samplerate;
	float saved_processing = voice_inputprocessing.value;
	float saved_maxgain = voice_maxgain.value;
	float saved_avggain = voice_avggain.value;
	int16_t samples[GS_DEFAULT_FRAME_SIZE];
	int peak = 0;
	memset( &voice.autogain, 0, sizeof( voice.autogain ));
	voice.samplerate = GS_DEFAULT_SAMPLE_RATE;
	voice_inputprocessing.value = 1;
	voice_maxgain.value = 5;
	voice_avggain.value = 0.5f;
	for( int frame = 0; frame < 100; frame++ )
	{
		for( int index = 0; index < ARRAYSIZE( samples ); index++ ) samples[index] = index & 1 ? 32 : -32;
		Voice_ApplyGainAdjust( samples, ARRAYSIZE( samples ), 1.0f );
	}
	for( int index = 0; index < ARRAYSIZE( samples ); index++ ) peak = Q_max( peak, abs( samples[index] ));
	TASSERT( peak < 16 );
	for( int index = 0; index < ARRAYSIZE( samples ); index++ ) samples[index] = index & 1 ? 1000 : -1000;
	samples[100] = 30000;
	samples[300] = -30000;
	Voice_ApplyGainAdjust( samples, ARRAYSIZE( samples ), 4.0f );
	peak = 0;
	for( int index = 0; index < ARRAYSIZE( samples ); index++ ) peak = Q_max( peak, abs( samples[index] ));
	TASSERT( peak <= 28000 );
	memset( &voice.autogain, 0, sizeof( voice.autogain ));
	for( int index = 0; index < ARRAYSIZE( samples ); index++ ) samples[index] = 2000;
	Voice_ApplyGainAdjust( samples, ARRAYSIZE( samples ), 1.0f );
	TASSERT_EQi( samples[100], 0 );
	voice.autogain = saved_gain;
	voice.samplerate = saved_rate;
	voice_inputprocessing.value = saved_processing;
	voice_maxgain.value = saved_maxgain;
	voice_avggain.value = saved_avggain;
}

static uint Voice_TestPacketCRC( byte *packet, uint payload_size )
{
	uint32_t checksum = CRC32_INIT_VALUE;
	CRC32_ProcessBuffer( &checksum, packet, payload_size );
	checksum = LittleLong( CRC32_Final( checksum ));
	memcpy( packet + payload_size, &checksum, sizeof( checksum ));
	return payload_size + sizeof( checksum );
}

void Test_RunVoice( void )
{
	voice_state_t saved_voice = voice;
	int saved_maxclients = cl.maxclients;
	int error = 0;
	int16_t pcm[960];
	byte packet[VOICE_MAX_GS_DATA_SIZE] = { 0 };
	OpusEncoder *encoder = opus_encoder_create( GS_DEFAULT_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &error );
	int encoded;
	uint size;
	uint chunk_size;

	TRUN( Test_VoiceInputProcessing() );
	memset( &voice, 0, sizeof( voice ));
	cl.maxclients = 1;
	voice.gs_decoders[0] = opus_decoder_create( GS_DEFAULT_SAMPLE_RATE, 1, &error );
	TASSERT( encoder && voice.gs_decoders[0] );
	if( !encoder || !voice.gs_decoders[0] )
		goto cleanup;

	for( uint index = 0; index < sizeof( pcm ) / sizeof( pcm[0] ); index++ )
		pcm[index] = ((int)(index % 80) - 40) * 200;
	encoded = opus_encode( encoder, pcm, 960, packet + 18, sizeof( packet ) - 18 );
	TASSERT( encoded > 0 );
	if( encoded <= 0 )
		goto cleanup;
	packet[8] = GS_VPC_SETSAMPLERATE;
	packet[9] = GS_DEFAULT_SAMPLE_RATE & 255;
	packet[10] = GS_DEFAULT_SAMPLE_RATE >> 8;
	packet[11] = GS_VPC_VDATA_OPUS_PLC;
	packet[12] = (encoded + 4) & 255;
	packet[13] = (encoded + 4) >> 8;
	packet[14] = encoded & 255;
	packet[15] = encoded >> 8;
	size = Voice_TestPacketCRC( packet, 18 + encoded );
	TASSERT_EQi( Voice_ProcessGSData( 1, packet, size ), 960 );
	TASSERT_EQi( Voice_ProcessGSData( 0, packet, size ), 0 );
	TASSERT_EQi( Voice_ProcessGSData( 2, packet, size ), 0 );
	TASSERT_EQi( Voice_ProcessGSData( 1, NULL, size ), 0 );
	for( uint truncated = 0; truncated < 12; truncated++ )
		TASSERT_EQi( Voice_ProcessGSData( 1, packet, truncated ), 0 );
	packet[size - 1] ^= 1;
	TASSERT_EQi( Voice_ProcessGSData( 1, packet, size ), 0 );
	packet[size - 1] ^= 1;
	chunk_size = 7 + encoded;
	memcpy( packet + 18 + encoded, packet + 11, chunk_size );
	size = Voice_TestPacketCRC( packet, 11 + chunk_size * 2 );
	TASSERT_EQi( Voice_ProcessGSData( 1, packet, size ), 1920 );
	packet[12] = 255;
	packet[13] = 255;
	size = Voice_TestPacketCRC( packet, 11 + chunk_size * 2 );
	TASSERT_EQi( Voice_ProcessGSData( 1, packet, size ), 0 );
	packet[11] = GS_VPC_VDATA_SILENCE;
	packet[12] = 240;
	packet[13] = 0;
	size = Voice_TestPacketCRC( packet, 14 );
	TASSERT_EQi( Voice_ProcessGSData( 1, packet, size ), 240 );
	packet[12] = 255;
	packet[13] = 255;
	size = Voice_TestPacketCRC( packet, 14 );
	TASSERT_EQi( Voice_ProcessGSData( 1, packet, size ), 0 );
	packet[11] = GS_VPC_VDATA_OPUS_PLC;
	packet[12] = 4;
	packet[13] = 0;
	packet[14] = 255;
	packet[15] = 255;
	size = Voice_TestPacketCRC( packet, 18 );
	TASSERT_EQi( Voice_ProcessGSData( 1, packet, size ), 0 );
	packet[14] = 0;
	packet[15] = 0;
	size = Voice_TestPacketCRC( packet, 18 );
	TASSERT_EQi( Voice_ProcessGSData( 1, packet, size ), GS_DEFAULT_FRAME_SIZE );

cleanup:
	if( encoder )
		opus_encoder_destroy( encoder );
	if( voice.gs_decoders[0] )
		opus_decoder_destroy( voice.gs_decoders[0] );
	voice = saved_voice;
	cl.maxclients = saved_maxclients;
}
#endif
