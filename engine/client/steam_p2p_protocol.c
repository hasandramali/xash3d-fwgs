#include "common.h"
#include "steam_p2p_protocol.h"
#include "tests.h"

static uint32_t SteamP2P_ReadBE32( const byte *data )
{
	return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
}

static void SteamP2P_WriteBE32( byte *output, uint32_t value )
{
	output[0] = (byte)(value >> 24);
	output[1] = (byte)(value >> 16);
	output[2] = (byte)(value >> 8);
	output[3] = (byte)value;
}

static qboolean SteamP2P_ReadCandidateString( const byte *data, size_t size, size_t *offset, char *output )
{
	const byte *terminator;
	size_t length;
	if( *offset >= size )
		return false;
	terminator = memchr( data + *offset, 0, size - *offset );
	if( !terminator )
		return false;
	length = (size_t)(terminator - (data + *offset));
	if( length >= STEAM_P2P_CANDIDATE_STRING )
		return false;
	memcpy( output, data + *offset, length + 1 );
	*offset += length + 1;
	return true;
}

size_t SteamP2P_EncodeCandidate( const steam_p2p_candidate_t *candidate, byte *output, size_t capacity )
{
	const char *fields[7];
	size_t lengths[7], needed = 11, offset = 0;
	uint32_t preference;
	if( !candidate || !output )
		return 0;
	fields[0] = candidate->name;
	fields[1] = candidate->protocol;
	fields[2] = candidate->address;
	fields[3] = candidate->username;
	fields[4] = candidate->password;
	fields[5] = candidate->type;
	fields[6] = candidate->network;
	for( uint index = 0; index < 7; index++ )
	{
		const char *terminator = memchr( fields[index], 0, STEAM_P2P_CANDIDATE_STRING );
		if( !terminator )
			return 0;
		lengths[index] = (size_t)(terminator - fields[index]) + 1;
		needed += lengths[index];
	}
	if( needed > capacity || candidate->extension > 9 )
		return 0;
	for( uint index = 0; index < 7; index++ )
	{
		memcpy( output + offset, fields[index], lengths[index] );
		offset += lengths[index];
		if( index == 2 )
		{
			output[offset++] = (byte)(candidate->port >> 8);
			output[offset++] = (byte)candidate->port;
			memcpy( &preference, &candidate->preference, sizeof( preference ));
			SteamP2P_WriteBE32( output + offset, preference );
			offset += 4;
		}
	}
	SteamP2P_WriteBE32( output + offset, candidate->generation );
	offset += 4;
	output[offset++] = candidate->extension;
	return offset;
}

qboolean SteamP2P_DecodeCandidate( const byte *data, size_t size, steam_p2p_candidate_t *candidate )
{
	steam_p2p_candidate_t parsed = { 0 };
	size_t offset = 0;
	uint32_t preference;
	if( !data || !candidate || size > STEAM_P2P_CANDIDATE_MAX )
		return false;
	if( !SteamP2P_ReadCandidateString( data, size, &offset, parsed.name ) ||
		!SteamP2P_ReadCandidateString( data, size, &offset, parsed.protocol ) ||
		!SteamP2P_ReadCandidateString( data, size, &offset, parsed.address ) || size - offset < 6 )
		return false;
	parsed.port = (uint16_t)(((uint16_t)data[offset] << 8) | data[offset + 1]);
	preference = SteamP2P_ReadBE32( data + offset + 2 );
	memcpy( &parsed.preference, &preference, sizeof( preference ));
	offset += 6;
	if( !SteamP2P_ReadCandidateString( data, size, &offset, parsed.username ) ||
		!SteamP2P_ReadCandidateString( data, size, &offset, parsed.password ) ||
		!SteamP2P_ReadCandidateString( data, size, &offset, parsed.type ) ||
		!SteamP2P_ReadCandidateString( data, size, &offset, parsed.network ) || size - offset < 4 )
		return false;
	parsed.generation = SteamP2P_ReadBE32( data + offset );
	offset += 4;
	if( size - offset > 1 )
		return false;
	if( offset < size )
		parsed.extension = Q_min( data[offset], 9 );
	*candidate = parsed;
	return true;
}

size_t SteamP2P_EncodeUnreliableDatagram( int channel, const byte *data, size_t size, byte *output, size_t capacity )
{
	size_t header_size = channel ? 6 : 2;
	if( channel < 0 || !data || !size || !output || capacity < header_size || size > capacity - header_size )
		return 0;
	memmove( output + header_size, data, size );
	output[0] = 0;
	output[1] = channel ? 1 : 0;
	if( channel )
	{
		output[2] = (byte)channel;
		output[3] = (byte)((uint32_t)channel >> 8);
		output[4] = (byte)((uint32_t)channel >> 16);
		output[5] = (byte)((uint32_t)channel >> 24);
	}
	return header_size + size;
}

qboolean SteamP2P_DecodeUnreliableDatagram( const byte *data, size_t size, int *channel, const byte **payload, size_t *payload_size )
{
	size_t header_size;
	uint32_t parsed_channel = 0;
	if( !data || size < 3 || !channel || !payload || !payload_size || data[0] != 0 )
		return false;
	if( data[1] == 0 )
		header_size = 2;
	else if( data[1] == 1 && size > 6 )
	{
		header_size = 6;
		parsed_channel = data[2] | ((uint32_t)data[3] << 8) | ((uint32_t)data[4] << 16) | ((uint32_t)data[5] << 24);
		if( parsed_channel > INT_MAX )
			return false;
	}
	else
		return false;
	*channel = (int)parsed_channel;
	*payload = data + header_size;
	*payload_size = size - header_size;
	return true;
}

#if XASH_ENGINE_TESTS
void Test_RunSteamP2PProtocol( void )
{
	static const byte native_candidate[] = {
		0x64,0x61,0x74,0x61,0x00,0x75,0x64,0x70,0x00,0x31,0x39,0x32,0x2e,0x30,0x2e,0x32,
		0x2e,0x31,0x30,0x00,0x69,0x87,0x3f,0x40,0x00,0x00,0x74,0x65,0x73,0x74,0x2d,0x75,
		0x73,0x65,0x72,0x00,0x74,0x65,0x73,0x74,0x2d,0x70,0x61,0x73,0x73,0x77,0x6f,0x72,
		0x64,0x00,0x6c,0x6f,0x63,0x61,0x6c,0x00,0x65,0x74,0x68,0x65,0x72,0x6e,0x65,0x74,
		0x00,0x00,0x00,0x00,0x07,0x05 };
	static const byte server_packet[] = { 0, 1, 1, 0, 0, 0, 1, 0, 0, 0 };
	steam_p2p_candidate_t candidate = { 0 }, sentinel;
	byte encoded[STEAM_P2P_CANDIDATE_MAX];
	const byte *payload;
	size_t payload_size;
	int channel;

	TASSERT( SteamP2P_DecodeCandidate( native_candidate, sizeof( native_candidate ), &candidate ));
	TASSERT_STR( candidate.name, "data" );
	TASSERT_STR( candidate.protocol, "udp" );
	TASSERT_STR( candidate.address, "192.0.2.10" );
	TASSERT_EQi( candidate.port, 27015 );
	TASSERT( candidate.preference == 0.75f );
	TASSERT_STR( candidate.type, "local" );
	TASSERT_STR( candidate.network, "ethernet" );
	TASSERT_STR( candidate.username, "test-user" );
	TASSERT_STR( candidate.password, "test-password" );
	TASSERT_EQi( candidate.generation, 7 );
	TASSERT_EQi( candidate.extension, 5 );
	TASSERT_EQi( SteamP2P_EncodeCandidate( &candidate, encoded, sizeof( encoded )), sizeof( native_candidate ));
	TASSERT( !memcmp( encoded, native_candidate, sizeof( native_candidate )));
	steam_p2p_candidate_t empty_credentials = candidate, decoded_empty;
	Q_strncpy( empty_credentials.username, "abcdefghijklmnop", sizeof( empty_credentials.username ));
	empty_credentials.password[0] = 0;
	empty_credentials.network[0] = 0;
	size_t empty_size = SteamP2P_EncodeCandidate( &empty_credentials, encoded, sizeof( encoded ));
	TASSERT( empty_size > 0 );
	TASSERT( !memcmp( encoded + 26, "abcdefghijklmnop", 17 ));
	TASSERT( SteamP2P_DecodeCandidate( encoded, empty_size, &decoded_empty ));
	TASSERT_STR( decoded_empty.username, "abcdefghijklmnop" );
	TASSERT_STR( decoded_empty.type, "local" );
	TASSERT( !decoded_empty.password[0] && !decoded_empty.network[0] );
	sentinel = candidate;
	for( size_t truncated = 0; truncated < sizeof( native_candidate ) - 1; truncated++ )
	{
		TASSERT( !SteamP2P_DecodeCandidate( native_candidate, truncated, &candidate ));
		TASSERT( !memcmp( &candidate, &sentinel, sizeof( candidate )));
	}
	TASSERT( SteamP2P_DecodeCandidate( native_candidate, sizeof( native_candidate ) - 1, &candidate ));
	TASSERT_EQi( candidate.extension, 0 );
	memset( encoded, 0xa5, sizeof( encoded ));
	TASSERT_EQi( SteamP2P_EncodeCandidate( &candidate, encoded, 1 ), 0 );
	TASSERT_EQi( encoded[0], 0xa5 );
	memset( candidate.name, 'x', sizeof( candidate.name ));
	TASSERT_EQi( SteamP2P_EncodeCandidate( &candidate, encoded, sizeof( encoded )), 0 );
	TASSERT( SteamP2P_DecodeUnreliableDatagram( server_packet, sizeof( server_packet ), &channel, &payload, &payload_size ));
	TASSERT_EQi( channel, 1 );
	TASSERT_EQi( payload_size, 4 );
	TASSERT_EQi( SteamP2P_EncodeUnreliableDatagram( channel, payload, payload_size, encoded, sizeof( encoded )), sizeof( server_packet ));
	TASSERT( !memcmp( encoded, server_packet, sizeof( server_packet )));
	TASSERT_EQi( SteamP2P_EncodeUnreliableDatagram( 0, payload, payload_size, encoded, sizeof( encoded )), 6 );
	TASSERT( SteamP2P_DecodeUnreliableDatagram( encoded, 6, &channel, &payload, &payload_size ));
	TASSERT_EQi( channel, 0 );
	TASSERT_EQi( payload_size, 4 );
	encoded[0] = 1;
	TASSERT( !SteamP2P_DecodeUnreliableDatagram( encoded, 6, &channel, &payload, &payload_size ));
	for( size_t truncated = 0; truncated <= 6; truncated++ )
		TASSERT( !SteamP2P_DecodeUnreliableDatagram( server_packet, truncated, &channel, &payload, &payload_size ));
	TASSERT_EQi( SteamP2P_EncodeUnreliableDatagram( -1, server_packet, sizeof( server_packet ), encoded, sizeof( encoded )), 0 );
}
#endif
