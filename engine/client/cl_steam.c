/*
cl_steam.c - steam(tm) broker implementation
Copyright (C) 2026 Xash3D FWGS contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include <inttypes.h>
#include "common.h"
#include "client.h"
#include "net_ws.h"
#include "net_ws_private.h"
#include "tests.h"

// What is a broker?
// From Wikipedia, the free encyclopedia:
// "The broker pattern is an architecture pattern that involves the use of an
// intermediary software entity, called a "broker", to facilitate communication
// between two or more software components. The broker acts as a "middleman"
// between the components, allowing them to communicate without being aware of
// each other's existence.
//
// Due to proprietary nature of Steamworks SDK, it cannot be run on same amount
// of platforms supported by Xash3D FWGS, neither we can link directly due to
// GNU GPLv3 license. However, here comes the broker, by running it (in trusted
// network, preferrably) on a machine that has Steam client installed, the
// engine can communicate with it, acquiring needed information to log-in into
// Steam protected multiplayer servers.

// Protocol constants
#define SBRK_FRAME_HEADER			"SBRK"
#define SBRK_FRAME_HEADER_SIZE		(sizeof(SBRK_FRAME_HEADER) - 1)
#define SBRK_FRAME_LENGTH_SIZE		2
#define SBRK_RESPONSE_HEADER		"sb_connect\n"
#define SBRK_RESPONSE_HEADER_SIZE	(sizeof(SBRK_RESPONSE_HEADER) - 1)
#define SBRK_RESPONSE_V2_HEADER		"sb_connect2\n"
#define SBRK_RESPONSE_V2_HEADER_SIZE	(sizeof(SBRK_RESPONSE_V2_HEADER) - 1)
#define SBRK_FRIEND_RESPONSE_HEADER	"sb_friend_result\n"
#define SBRK_FRIEND_RESPONSE_HEADER_SIZE	(sizeof(SBRK_FRIEND_RESPONSE_HEADER) - 1)
#define SBRK_MASTERLIST_ENTRY_HEADER	"sb_masterlist_entry "
#define SBRK_MASTERLIST_ENTRY_HEADER_SIZE	(sizeof(SBRK_MASTERLIST_ENTRY_HEADER) - 1)
#define SBRK_MASTERLIST_END_HEADER	"sb_masterlist_end "
#define SBRK_MASTERLIST_END_HEADER_SIZE	(sizeof(SBRK_MASTERLIST_END_HEADER) - 1)
#define SBRK_MAX_FRAME_SIZE			4096
#define SBRK_CONNECT_TIMEOUT		10.0
#define SBRK_CONNECT_RETRY_DELAY	5.0
#define SBRK_TICKET_RESPONSE_TIMEOUT	15.0
#define SBRK_MASTERLIST_TIMEOUT	15.0
#define SBRK_TICKET_SIZE_MAX 		2048
#define SBRK_P2P_MAX_RETRIES		2
#define SBRK_P2P_RETRY_DELAY		2.0
#define SBRK_P2P_SILENCE_TIMEOUT	30.0

static CVAR_DEFINE_AUTO( cl_steam_broker_addr, "127.0.0.1:27420", FCVAR_PRIVILEGED|FCVAR_ARCHIVE, "address of steam broker instance" );
static CVAR_DEFINE_AUTO( cl_steam_appid, "0", FCVAR_PRIVILEGED|FCVAR_ARCHIVE, "Steam appid used for GoldSrc auth tickets (0 = auto-detect from gamedir; 10 half-life/cs1.6, 50 opposing force, 130 blue shift, 225840 svencoop)" );

typedef enum
{
	SBRK_STATE_IDLE,
	SBRK_STATE_CONNECTING,
	SBRK_STATE_CONNECTED,
	SBRK_STATE_GAMESHUTDOWN
} sbrk_state_t;

typedef struct
{
	netadr_t adr;
	int socket;
	sbrk_state_t state;
	int challenge;
	uint32_t ticket_request_id;
	netadr_t serveradr;
	double connection_timeout;
	double idle_cycle_timeout;
	double ticket_timeout;
	qboolean masterlist_pending;
	double masterlist_deadline;
	uint8_t rx_buffer[SBRK_MAX_FRAME_SIZE + 64];
	uint8_t tx_buffer[SBRK_MAX_FRAME_SIZE + 64];
	uint32_t rx_buffer_pos;
	uint32_t tx_buffer_pos;
} steam_broker_t;

static steam_broker_t broker;
static uint32_t ticket_request_serial;

static qboolean SteamBroker_IsTicketCurrent( int challenge, uint32_t request_id )
{
	return cls.state == ca_connecting && cls.broker_wait && broker.ticket_request_id &&
		request_id == broker.ticket_request_id && challenge == broker.challenge &&
		NET_CompareAdr( cls.serveradr, broker.serveradr );
}

static void SteamBroker_CancelTicket( void )
{
	cls.broker_wait = false;
	broker.ticket_request_id = 0;
	broker.ticket_timeout = 0;
	memset( &broker.serveradr, 0, sizeof( broker.serveradr ));
}

// Tracker paging chain: offset pages are fetched sequentially until a
// short page arrives. Generation guards against overlapping scans.
static int s_tracker_page = 0;
static int s_tracker_total = 0;
static unsigned int s_tracker_gen = 0;

static int SteamBroker_GetGoldSrcAppId( void );
static qboolean SteamBroker_ParseSteamID( const char *text, uint64_t *out );

static netadr_t p2p_peer;
static uint32_t p2p_serial, p2p_request;
static qboolean p2p_ready;
static qboolean p2p_failed;
static double p2p_deadline;
static unsigned int p2p_retries;
static double p2p_healthy_since;
static unsigned int p2p_read, p2p_write;
typedef struct
{
	size_t length;
	byte data[4000];
} p2p_packet_t;
static p2p_packet_t p2p_packets[64];

static void SteamBroker_ClearP2P( void )
{
	p2p_request = 0;
	p2p_ready = false;
	p2p_failed = false;
	p2p_deadline = 0;
	p2p_healthy_since = 0;
	p2p_read = p2p_write = 0;
	memset( &p2p_peer, 0, sizeof( p2p_peer ));
}

static void SteamBroker_ReceiveP2P( const byte *payload, size_t size )
{
	uint32_t request = 0;
	uint64_t identity = 0, expected;
	if( size <= 24 || size - 24 > sizeof( p2p_packets[0].data ) || memcmp( payload, "sb_p2p_data\n", 12 ))
		return;
	for( int index = 0; index < 4; index++ ) request |= (uint32_t)payload[12 + index] << ( index * 8 );
	for( int index = 0; index < 8; index++ ) identity |= (uint64_t)payload[16 + index] << ( index * 8 );
	memcpy( &expected, p2p_peer.ip6_1, sizeof( expected ));
	if( !p2p_request || p2p_failed || request != p2p_request || identity != expected || p2p_write - p2p_read >= ARRAYSIZE( p2p_packets ))
		return;
	unsigned int slot = p2p_write++ % ARRAYSIZE( p2p_packets );
	p2p_packets[slot].length = size - 24;
	memcpy( p2p_packets[slot].data, payload + 24, size - 24 );
}

static void SteamBroker_P2PState( uint32_t request, const char *state )
{
	if( !p2p_request || p2p_failed || request != p2p_request )
		return;
	Con_Printf( "Steam P2P: %s\n", state );
	if( !Q_strcmp( state, "ready" ))
	{
		p2p_ready = true;
		p2p_deadline = 0;
		if( cls.state == ca_connecting )
		{
			cls.connect_retry = 0;
			cls.connect_time = MAX_HEARTBEAT;
		}
	}
	else if( !Q_strcmp( state, "rechecking" ))
	{
		p2p_ready = false;
		p2p_deadline = Platform_DoubleTime() + 60.0;
	}
}
static void SteamBroker_P2PError( uint32_t request, const char *error )
{
	if( !p2p_request || p2p_failed || request != p2p_request )
		return;
	Con_Printf( S_ERROR "%s\n", error );
	p2p_failed = true;
	p2p_ready = false;
	if( Q_stristr( error, "timed out" ) || Q_stristr( error, "timeout" ))
		p2p_deadline = Platform_DoubleTime() + SBRK_P2P_RETRY_DELAY;
	else
	{
		p2p_deadline = 0;
		Cbuf_AddText( "disconnect\n" );
	}
}

void Steam_MasterlistScanBegin( void );
static qboolean Steam_MasterlistSeenBefore( const netadr_t *adr );
static qboolean SteamTracker_RequestPage( int page );

static void SteamBroker_DumpHex( const char *name, const void *data, size_t size )
{
	const uint8_t *bytes = (const uint8_t *)data;
	char line[80];
	int pos = 0;
	size_t i;

	for( i = 0; i < size; i++ )
	{
		pos += Q_snprintf( line + pos, sizeof( line ) - pos, "%02x ", bytes[i] );
		if( ( i % 16 ) == 15 || i == size - 1 )
		{
			Con_DPrintf( "%s [%zu/%zu]: %s\n", name, i + 1, size, line );
			pos = 0;
		}
	}
}

/*
 * Decode a legacy GoldSrc (InitiateGameConnection) Steam auth ticket and
 * print its fields. This is the single most useful diagnostic when a server
 * rejects a ticket: compare these fields against a known-good reference.
 *
 * Layout: uint32 tokenLen | token[tokenLen] | uint32 sessionSize | session[sessionSize]
 *   session: uint8 version | uint8 count | uint16 reserved | uint64 steamid |
 *            uint32 appid | uint32 timestamp | uint32 clientIP | uint32 serverIP |
 *            uint8 signature[sessionSize - 32]
 */
static void SteamBroker_DumpLegacyTicket( const char *name, const uint8_t *ticket, size_t size )
{
	uint32_t tokenLen, sessionSize;
	const uint8_t *token, *session;
	uint64_t steamid;
	uint32_t appid, timestamp, clientIP, serverIP;
	char ipbuf[32];
	int i;

	if( size < 8 )
	{
		Con_Printf( S_ERROR "%s: ticket too small (%zu bytes) to decode\n", __func__, size );
		return;
	}

	tokenLen = ticket[0] | ( ticket[1] << 8 ) | ( ticket[2] << 16 ) | ( (uint32_t)ticket[3] << 24 );
	if( tokenLen > 64 || ( size_t)tokenLen + 8 > size )
	{
		Con_Printf( S_ERROR "%s: bad tokenLen %u (ticket size %zu)\n", __func__, tokenLen, size );
		return;
	}

	token = ticket + 4;
	session = ticket + 4 + tokenLen;
	sessionSize = session[0] | ( session[1] << 8 ) | ( session[2] << 16 ) | ( (uint32_t)session[3] << 24 );

	if( (size_t)sessionSize + 4 + tokenLen + 4 > size )
	{
		Con_Printf( S_ERROR "%s: bad sessionSize %u (ticket size %zu)\n", __func__, sessionSize, size );
		return;
	}

	// session data starts AFTER the 4-byte sessionSize field
	{
		const uint8_t *s = session + 4;
		steamid = (uint64_t)s[8] | ( (uint64_t)s[9] << 8 ) | ( (uint64_t)s[10] << 16 ) | ( (uint64_t)s[11] << 24 ) |
		          ( (uint64_t)s[12] << 32 ) | ( (uint64_t)s[13] << 40 ) | ( (uint64_t)s[14] << 48 ) | ( (uint64_t)s[15] << 56 );
		appid = s[16] | ( s[17] << 8 ) | ( s[18] << 16 ) | ( (uint32_t)s[19] << 24 );
		timestamp = s[20] | ( s[21] << 8 ) | ( s[22] << 16 ) | ( (uint32_t)s[23] << 24 );
		clientIP = s[24] | ( s[25] << 8 ) | ( s[26] << 16 ) | ( (uint32_t)s[27] << 24 );
		serverIP = s[28] | ( s[29] << 8 ) | ( s[30] << 16 ) | ( (uint32_t)s[31] << 24 );

	for( i = 0; i < 4; i++ )
	{
		uint8_t b = ( i == 0 ) ? ( clientIP & 0xff ) : ( i == 1 ) ? ( ( clientIP >> 8 ) & 0xff ) : ( i == 2 ) ? ( ( clientIP >> 16 ) & 0xff ) : ( ( clientIP >> 24 ) & 0xff );
		Q_snprintf( ipbuf + ( i ? Q_strlen( ipbuf ) + 1 : 0 ), sizeof( ipbuf ) - Q_strlen( ipbuf ), "%u%s", b, i < 3 ? "." : "" );
	}

	Con_Printf( S_NOTE "%s: legacy GoldSrc ticket decode:\n", name );
	Con_Printf( "    tokenLen=%u sessionSize=%u total=%zu\n", tokenLen, sessionSize, size );
	Con_Printf( "    version=0x%02x count=%u\n", session[4], session[8] );
	}
	Con_Printf( "    steamid=%"PRIu64" appid=%u timestamp=%u\n", steamid, appid, timestamp );
	Con_Printf( "    clientIP=%s serverIP=%u.%u.%u.%u sigLen=%u\n",
		ipbuf, serverIP & 0xff, ( serverIP >> 8 ) & 0xff, ( serverIP >> 16 ) & 0xff, ( serverIP >> 24 ) & 0xff,
		(uint32_t)( sessionSize > 32 ? sessionSize - 32 : 0 ));
	Con_Printf( "    token=%02x%02x%02x%02x...%02x%02x%02x%02x\n",
		token[0], token[1], token[2], token[3], token[tokenLen-4], token[tokenLen-3], token[tokenLen-2], token[tokenLen-1] );
}

static void SteamBroker_SetState( sbrk_state_t new_state )
{
	if( broker.state != new_state )
	{
		// we also may logging transitions if needed
		broker.state = new_state;
	}
}

static qboolean SteamBroker_UpdateBrokerAddress( void )
{
	if( NET_NetadrType( &broker.adr ) == NA_UNDEFINED )
	{
		if( !NET_StringToAdr( cl_steam_broker_addr.string, &broker.adr ))
			return false;
	}
	return true;
}

static void SteamBroker_CloseSocket( void )
{
	if( NET_IsSocketValid( broker.socket ))
	{
		closesocket( broker.socket );
		broker.socket = INVALID_SOCKET;
	}
	broker.rx_buffer_pos = 0;
	broker.tx_buffer_pos = 0;
}

static void SteamBroker_Disconnect( void )
{
	SteamBroker_ClearP2P();
	SteamBroker_CloseSocket();
	SteamBroker_SetState( SBRK_STATE_IDLE );
	cls.broker_wait = false; // stop waiting for a ticket; the engine will re-request once we reconnect
	broker.masterlist_pending = false;
}

static qboolean SteamBroker_ConnectImpl( void )
{
	int addr_family;
	struct sockaddr_storage addr = { 0 };

	if( NET_NetadrType( &broker.adr ) == NA_IP )
	{
		addr_family = AF_INET;
	}
	else if( NET_NetadrType( &broker.adr ) == NA_IP6 )
	{
		addr_family = AF_INET6;
	}
	else
	{
		Con_Printf( S_ERROR "%s: unsupported broker address type for %s\n", __func__, cl_steam_broker_addr.string );
		return false;
	}

	broker.socket = socket( addr_family, SOCK_STREAM, IPPROTO_TCP );
	if( !NET_IsSocketValid( broker.socket ))
	{
		Con_Printf( S_ERROR "%s: failed to create socket\n", __func__ );
		return false;
	}

	if( !NET_MakeSocketNonBlocking( broker.socket ))
	{
		Con_Printf( S_ERROR "%s: failed to set non-blocking mode, error %s\n", __func__, NET_ErrorString( ));
		SteamBroker_CloseSocket();
		return false;
	}

	NET_NetadrToSockadr( &broker.adr, &addr );

	int result = connect( broker.socket, (struct sockaddr *)&addr, NET_SockAddrLen( &addr ));
	if( NET_IsSocketError( result ))
	{
		int err = WSAGetLastError();
		if( err != WSAEWOULDBLOCK && err != WSAEALREADY && err != WSAEINPROGRESS )
		{
			Con_Printf( S_ERROR "%s: failed to connect to broker at %s with error %s\n", __func__, cl_steam_broker_addr.string, NET_ErrorString( ));
			SteamBroker_CloseSocket();
			return false;
		}
	}

	broker.connection_timeout = Platform_DoubleTime() + SBRK_CONNECT_TIMEOUT;
	SteamBroker_SetState( SBRK_STATE_CONNECTING );
	return true;
}

static qboolean SteamBroker_SendFrame( const char *payload, size_t payload_size )
{
	if( payload_size > SBRK_MAX_FRAME_SIZE )
	{
		Con_Printf( S_WARN "%s: payload too large (%zu > %u)\n", __func__, payload_size, SBRK_MAX_FRAME_SIZE );
		return false;
	}

	uint8_t frame[SBRK_MAX_FRAME_SIZE + 6];
	sizebuf_t sb;

	MSG_Init( &sb, "SteamBroker_SendFrame", frame, sizeof( frame ));

	MSG_WriteBytes( &sb, SBRK_FRAME_HEADER, SBRK_FRAME_HEADER_SIZE );
	MSG_WriteShort( &sb, payload_size );
	MSG_WriteBytes( &sb, payload, payload_size );

	size_t frame_size = MSG_GetRealBytesWritten( &sb );
	int sent = send( broker.socket, (const char *)frame, frame_size, 0 );
	if( NET_IsSocketError( sent ))
	{
		int err = WSAGetLastError();
		if( err != WSAEWOULDBLOCK && err != WSAEALREADY )
		{
			Con_Printf( S_ERROR "%s: send error %s\n", __func__, NET_ErrorString( ));
			SteamBroker_Disconnect( );
			return false;
		}
		sent = 0;
	}

	// bufferize unsent data for deferred sending
	size_t unsent = frame_size - sent;
	if( unsent > 0 )
	{
		size_t available = sizeof( broker.tx_buffer ) - broker.tx_buffer_pos;
		if( available < unsent )
		{
			Con_Printf( S_ERROR "%s: transmit buffer overflow (%zu > %zu)\n", __func__, unsent, available );
			SteamBroker_Disconnect( );
			return false;
		}

		memcpy( broker.tx_buffer + broker.tx_buffer_pos, frame + sent, unsent );
		broker.tx_buffer_pos += unsent;
	}

	return true;
}

static qboolean SteamBroker_ProcessFrame( void )
{
	if( broker.rx_buffer_pos < SBRK_FRAME_HEADER_SIZE + SBRK_FRAME_LENGTH_SIZE )
		return false;

	sizebuf_t sb;
	MSG_Init( &sb, "SteamBroker_ProcessFrame", broker.rx_buffer, broker.rx_buffer_pos );

	// verify frame header
	char header[SBRK_FRAME_HEADER_SIZE];
	if( !MSG_ReadBytes( &sb, header, sizeof( header ), SBRK_FRAME_HEADER_SIZE ))
		return false;

	if( memcmp( header, SBRK_FRAME_HEADER, SBRK_FRAME_HEADER_SIZE ) != 0 )
	{
		Con_Printf( S_ERROR "%s: invalid frame header\n", __func__ );
		SteamBroker_Disconnect( );
		return false;
	}

	uint16_t payload_size = MSG_ReadShort( &sb );
	uint32_t frame_size = SBRK_FRAME_HEADER_SIZE + SBRK_FRAME_LENGTH_SIZE + payload_size;
	if( payload_size > SBRK_MAX_FRAME_SIZE )
	{
		SteamBroker_Disconnect();
		return false;
	}

	if( MSG_GetNumBytesLeft( &sb ) < payload_size )
		return false; // need more data

	const byte *payload = broker.rx_buffer + SBRK_FRAME_HEADER_SIZE + SBRK_FRAME_LENGTH_SIZE;
	if( payload_size >= 24 && !memcmp( payload, "sb_p2p_data\n", 12 ))
	{
		SteamBroker_ReceiveP2P( payload, payload_size );
	}
	else if( payload_size > 13 && !memcmp( payload, "sb_p2p_state ", 13 ))
	{
		char status[256];
		size_t size = Q_min( payload_size, sizeof( status ) - 1 );
		memcpy( status, payload, size );
		status[size] = 0;
		unsigned int request;
		int offset = 0;
		if( sscanf( status + 13, "%u %n", &request, &offset ) == 1 && offset > 0 )
			SteamBroker_P2PState( request, status + 13 + offset );
	}
	else if( payload_size > 13 && !memcmp( payload, "sb_p2p_error ", 13 ))
	{
		char error[512];
		size_t size = Q_min( payload_size, sizeof( error ) - 1 );
		memcpy( error, payload, size );
		error[size] = 0;
		unsigned int request;
		if( sscanf( error + 13, "%u", &request ) == 1 )
			SteamBroker_P2PError( request, error );
	}
	else if( payload_size >= SBRK_FRIEND_RESPONSE_HEADER_SIZE &&
		!memcmp( broker.rx_buffer + SBRK_FRAME_HEADER_SIZE + SBRK_FRAME_LENGTH_SIZE,
			SBRK_FRIEND_RESPONSE_HEADER, SBRK_FRIEND_RESPONSE_HEADER_SIZE ))
	{
		char address[64];
		netadr_t adr;
		size_t address_size = payload_size - SBRK_FRIEND_RESPONSE_HEADER_SIZE;
		if( address_size > 0 && address_size < sizeof( address ))
		{
			memcpy( address, broker.rx_buffer + SBRK_FRAME_HEADER_SIZE + SBRK_FRAME_LENGTH_SIZE + SBRK_FRIEND_RESPONSE_HEADER_SIZE, address_size );
			address[address_size] = '\0';
			if( !Q_strncmp( address, "ERR:", 4 ))
			{
				// Broker-side diagnosed failure (offline? wrong game?
				// no public address?). Surface the reason verbatim.
				Con_Printf( S_ERROR "Steam friend server lookup: %s\n", address + 4 );
			}
			else if( NET_StringToAdr( address, &adr ) && NET_NetadrType( &adr ) == NA_IP )
			{
				Con_Printf( "Steam friend server found: %s; connecting over UDP\n", address );
				Cbuf_AddTextf( "connect %s\n", address );
			}
			else
				Con_Printf( S_ERROR "Steam friend server lookup returned no address\n" );
		}
		else
			Con_Printf( S_ERROR "Steam friend server lookup returned no address\n" );
		memmove( broker.rx_buffer, broker.rx_buffer + frame_size, broker.rx_buffer_pos - frame_size );
		broker.rx_buffer_pos -= frame_size;
		return true;
	}

	// Steam masterlist stream: one address per frame plus a terminator.
	// Every returned address is re-queried through the normal per-server
	// path, so names/pings/players reach the server-list UI unchanged.
	if( payload_size >= SBRK_MASTERLIST_ENTRY_HEADER_SIZE &&
		!memcmp( broker.rx_buffer + SBRK_FRAME_HEADER_SIZE + SBRK_FRAME_LENGTH_SIZE,
			SBRK_MASTERLIST_ENTRY_HEADER, SBRK_MASTERLIST_ENTRY_HEADER_SIZE ))
	{
		char address[64];
		netadr_t adr;
		size_t address_size = payload_size - SBRK_MASTERLIST_ENTRY_HEADER_SIZE;
		if( address_size > 0 && address_size < sizeof( address ))
		{
			memcpy( address, broker.rx_buffer + SBRK_FRAME_HEADER_SIZE + SBRK_FRAME_LENGTH_SIZE + SBRK_MASTERLIST_ENTRY_HEADER_SIZE, address_size );
			address[address_size] = '\0';
			if( NET_StringToAdr( address, &adr ) && NET_NetadrType( &adr ) == NA_IP )
			{
				if( Steam_MasterlistSeenBefore( &adr ))
				{
					Con_DPrintf( "%s: duplicate %s (already queried this scan)\n", __func__, address );
				}
				else
				{
					Con_DPrintf( "%s: Steam server: %s\n", __func__, address );
					NET_QueryServerByAddress( adr, PROTO_GOLDSRC );
				}
			}
			else
				Con_Printf( S_ERROR "%s: broker returned unusable server address \"%s\"\n", __func__, address );
		}
		else
			Con_Printf( S_ERROR "%s: broker returned malformed masterlist entry\n", __func__ );
		memmove( broker.rx_buffer, broker.rx_buffer + frame_size, broker.rx_buffer_pos - frame_size );
		broker.rx_buffer_pos -= frame_size;
		return true;
	}

	if( payload_size >= SBRK_MASTERLIST_END_HEADER_SIZE &&
		!memcmp( broker.rx_buffer + SBRK_FRAME_HEADER_SIZE + SBRK_FRAME_LENGTH_SIZE,
			SBRK_MASTERLIST_END_HEADER, SBRK_MASTERLIST_END_HEADER_SIZE ))
	{
		char tail[64];
		size_t tail_size = payload_size - SBRK_MASTERLIST_END_HEADER_SIZE;
		if( tail_size >= sizeof( tail ))
			tail_size = sizeof( tail ) - 1;
		memcpy( tail, broker.rx_buffer + SBRK_FRAME_HEADER_SIZE + SBRK_FRAME_LENGTH_SIZE + SBRK_MASTERLIST_END_HEADER_SIZE, tail_size );
		tail[tail_size] = '\0';
		broker.masterlist_pending = false; // terminator arrived: no fallback
		Con_Printf( "Steam server list complete: %s\n", tail );
		memmove( broker.rx_buffer, broker.rx_buffer + frame_size, broker.rx_buffer_pos - frame_size );
		broker.rx_buffer_pos -= frame_size;
		return true;
	}

	qboolean response_v2 = payload_size >= SBRK_RESPONSE_V2_HEADER_SIZE &&
		!memcmp( broker.rx_buffer + SBRK_FRAME_HEADER_SIZE + SBRK_FRAME_LENGTH_SIZE,
			SBRK_RESPONSE_V2_HEADER, SBRK_RESPONSE_V2_HEADER_SIZE );
	uint response_header_size = response_v2 ? SBRK_RESPONSE_V2_HEADER_SIZE : SBRK_RESPONSE_HEADER_SIZE;
	char response_header[SBRK_RESPONSE_V2_HEADER_SIZE];
	if( MSG_ReadBytes( &sb, response_header, sizeof( response_header ), response_header_size ))
	{
		if( payload_size >= response_header_size + (response_v2 ? 20 : 16) &&
			(response_v2 || memcmp( response_header, SBRK_RESPONSE_HEADER, SBRK_RESPONSE_HEADER_SIZE ) == 0) )
		{
			int32_t challenge = MSG_ReadLong( &sb );
			uint32_t request_id = response_v2 ? MSG_ReadDword( &sb ) : broker.ticket_request_id;
			if( MSG_CheckOverflow( &sb ) || !SteamBroker_IsTicketCurrent( challenge, request_id ))
			{
				Con_DPrintf( "%s: ignoring cancelled or superseded ticket response\n", __func__ );
			}
			else
			{
				uint64_t steam_id;
				MSG_ReadBytes( &sb, &steam_id, sizeof( steam_id ), sizeof( steam_id ));
				uint32_t ticket_size = MSG_ReadDword( &sb );
				uint8_t ticket_data[SBRK_TICKET_SIZE_MAX];

				if( MSG_CheckOverflow( &sb ) || !ticket_size || ticket_size > SBRK_TICKET_SIZE_MAX ||
					ticket_size > payload_size - response_header_size - (response_v2 ? 20 : 16) )
				{
					Con_Printf( S_ERROR "%s: ticket size exceeds limit (%u)\n", __func__, ticket_size );
				}
				else if( MSG_ReadBytes( &sb, ticket_data, sizeof( ticket_data ), ticket_size ))
				{
					broker.ticket_timeout = 0;
					Con_Printf( "%s: SteamID: %"PRIu64", ticket size: %u\n", __func__, steam_id, ticket_size );
					Con_DPrintf( "%s: server %s, challenge=%d, steam_id=%"PRIu64", ticket_size=%u\n", __func__, NET_AdrToString( broker.serveradr ), challenge, steam_id, ticket_size );
					SteamBroker_DumpHex( "ticket", ticket_data, ticket_size );
					SteamBroker_DumpLegacyTicket( "SteamBroker", ticket_data, ticket_size );

					memcpy( cls.steamid, &steam_id, sizeof( cls.steamid ));
					CL_SendGoldSrcConnectPacket( broker.serveradr, broker.challenge, ticket_data, ticket_size );
					cls.broker_wait = false;
				}
				else
				{
					Con_Printf( S_ERROR "%s: failed to read ticket data\n", __func__ );
				}
			}
		}
	}
	
	// remove processed frame from buffer
	memmove( broker.rx_buffer, broker.rx_buffer + frame_size, broker.rx_buffer_pos - frame_size );
	broker.rx_buffer_pos -= frame_size;

	return true;
}

static void SteamBroker_ConnectFriend_f( void )
{
	const char *steam_id;
	char command[64];
	uint64_t parsed;

	if( Cmd_Argc() != 2 )
	{
		Con_Printf( S_USAGE "@connect_steamid <friend-steamid64 | STEAM_X:Y:Z>\n" );
		return;
	}
	steam_id = Cmd_Argv( 1 );
	if( !SteamBroker_ParseSteamID( steam_id, &parsed ))
	{
		Con_Printf( S_ERROR "Invalid SteamID (want 64-bit digits or STEAM_X:Y:Z)\n" );
		return;
	}
	if( broker.state != SBRK_STATE_CONNECTED )
	{
		Con_Printf( S_ERROR "Steam login/broker is not connected\n" );
		return;
	}
	Q_snprintf( command, sizeof( command ), "sb_friend %llu", (unsigned long long)parsed );
	if( SteamBroker_SendFrame( command, Q_strlen( command )))
		Con_Printf( "Looking up Steam friend's advertised game server...\n" );
}

qboolean SteamBroker_RequestMasterList( void )
{
	if( broker.state != SBRK_STATE_CONNECTED )
		return false;

	// sb_masterlist <gamedir> <appid>
	char buf[128];
	int appid = SteamBroker_GetGoldSrcAppId();
	int len = Q_snprintf( buf, sizeof( buf ), "sb_masterlist %s %d", GI->gamefolder, appid );

	if( len <= 0 )
		return false;

	if( !SteamBroker_SendFrame( buf, len ))
		return false;

	// Some brokers (e.g. older ones) silently ignore unknown commands.
	// If no terminator arrives in time, fall through to the next source
	// instead of waiting forever.
	broker.masterlist_pending = true;
	broker.masterlist_deadline = Platform_DoubleTime() + SBRK_MASTERLIST_TIMEOUT;

	Con_Printf( "Requesting Steam server list for %s (appid %d) via broker...\n", GI->gamefolder, appid );
	return true;
}

static void SteamBroker_MasterList_f( void )
{
	Steam_MasterlistScanBegin();
	// Happy eyeballs: fire every available source at once. The per-scan
	// dedupe below makes overlaps free, so a silent broker (older builds
	// that swallow sb_masterlist) can no longer starve the scan.
	SteamBroker_RequestMasterList();
	SteamWebAPI_RequestMasterList();
	SteamTracker_RequestMasterList();
}

// Per-scan dedupe: parallel sources (broker + webapi + tracker) usually
// return the same servers; feed each address into the per-server query
// path at most once per scan so the menu never gets double rows.
#define MASTERLIST_DEDUPE_SIZE 512
static struct { uint8_t ip[4]; uint16_t port; unsigned int gen; } s_masterlist_seen[MASTERLIST_DEDUPE_SIZE];
static unsigned int s_masterlist_gen = 0;
static int s_masterlist_seen_pos = 0;

void Steam_MasterlistScanBegin( void )
{
	// New scan: fresh dedupe window and a fresh tracker page chain.
	// In-flight pages from an older scan are ignored by generation.
	s_tracker_page = 0;
	s_tracker_total = 0;
	s_tracker_gen++;
	s_masterlist_gen++;
	if( s_masterlist_gen == 0 ) // wrap: invalidate everything
	{
		memset( s_masterlist_seen, 0, sizeof( s_masterlist_seen ));
		s_masterlist_gen = 1;
	}
}

static qboolean Steam_MasterlistSeenBefore( const netadr_t *adr )
{
	int i;

	for( i = 0; i < MASTERLIST_DEDUPE_SIZE; i++ )
	{
		if( s_masterlist_seen[i].gen == s_masterlist_gen &&
			s_masterlist_seen[i].port == adr->port &&
			!memcmp( s_masterlist_seen[i].ip, adr->ip, 4 ))
			return true;
	}
	i = s_masterlist_seen_pos;
	s_masterlist_seen_pos = ( s_masterlist_seen_pos + 1 ) % MASTERLIST_DEDUPE_SIZE;
	memcpy( s_masterlist_seen[i].ip, adr->ip, 4 );
	s_masterlist_seen[i].port = adr->port;
	s_masterlist_seen[i].gen = s_masterlist_gen;
	return false;
}

// Parse a SteamID in either raw 64-bit or classic STEAM_X:Y:Z (also VALVE_)
// form into a 64-bit ID. Classic form: id64 = (universe<<56)|(1<<52)|(1<<32)
// | (Z*2+Y); legacy universe 0 is mapped to public (1), like stock clients.
static qboolean SteamBroker_ParseSteamID( const char *text, uint64_t *out )
{
	const char *p = text;
	uint64_t universe = 0, auth = 0, account = 0;

	if( !Q_strnicmp( p, "STEAM_", 6 ))
		p += 6;
	else if( !Q_strnicmp( p, "VALVE_", 6 ))
		p += 6;
	else
	{
		// raw 64-bit digits?
		uint64_t id = 0;
		if( !*p )
			return false;
		for( ; *p; p++ )
		{
			if( *p < '0' || *p > '9' )
				return false;
			if( id > ( UINT64_MAX - ( *p - '0' )) / 10 )
				return false;
			id = id * 10 + (uint64_t)( *p - '0' );
		}
		if( id == 0 )
			return false;
		*out = id;
		return true;
	}

	// X:Y:Z triple
	while( *p == ' ' ) p++;
	if( *p < '0' || *p > '9' ) return false;
	while( *p >= '0' && *p <= '9' )
	{
		universe = universe * 10 + (uint64_t)( *p++ - '0' );
		if( universe > 255 ) return false;
	}
	if( *p != ':' )
		return false;
	p++;
	if( *p < '0' || *p > '9' ) return false;
	while( *p >= '0' && *p <= '9' )
	{
		auth = auth * 10 + (uint64_t)( *p++ - '0' );
		if( auth > 1 ) return false;
	}
	if( *p != ':' || auth > 1 )
		return false;
	p++;
	if( *p < '0' || *p > '9' ) return false;
	while( *p >= '0' && *p <= '9' )
	{
		account = account * 10 + (uint64_t)( *p++ - '0' );
		if( account > UINT32_MAX / 2 ) return false;
	}
	if( *p != '\0' || auth > 1 )
		return false;
	if( universe == 0 )
		universe = 1; // legacy STEAM_0 means public in practice
	if( account == 0 && auth == 0 )
		return false;

	*out = ( universe << 56 ) | ( (uint64_t)1 << 52 ) | ( (uint64_t)1 << 32 ) | ( account * 2 + auth );
	return true;
}

void SteamBroker_ConnectBySteamID( const char *text )
{
	uint64_t steamid;

	if( !SteamBroker_ParseSteamID( text, &steamid ))
	{
		Con_Printf( S_USAGE "connect STEAM_X:Y:Z | connect <steamid64>\n" );
		return;
	}

	if( broker.state != SBRK_STATE_CONNECTED )
	{
		Con_Printf( S_ERROR "Steam broker is not connected (login first); cannot resolve %s\n", text );
		return;
	}

	Cbuf_AddTextf( "connect steam:%"PRIu64"\n", steamid );
}

qboolean SteamBroker_StartP2P( netadr_t peer )
{
	char command[128];
	uint64_t identity;
	if( broker.state != SBRK_STATE_CONNECTED || NET_NetadrType( &peer ) != NA_STEAM_P2P )
		return false;
	if( p2p_request && NET_CompareAdr( peer, p2p_peer ))
		return !p2p_failed || p2p_deadline > 0;
	SteamBroker_ClearP2P();
	p2p_peer = peer;
	p2p_request = ++p2p_serial;
	if( !p2p_request ) p2p_request = ++p2p_serial;
	p2p_deadline = Platform_DoubleTime() + 60.0;
	memcpy( &identity, peer.ip6_1, sizeof( identity ));
	int length = Q_snprintf( command, sizeof( command ), "sb_p2p_start %u %"PRIu64" %d", p2p_request, identity, SteamBroker_GetGoldSrcAppId() );
	if( SteamBroker_SendFrame( command, length ))
	{
		Con_Printf( "Steam P2P: negotiating %"PRIu64"\n", identity );
		return true;
	}
	SteamBroker_ClearP2P();
	return false;
}

qboolean SteamBroker_P2PConnectTimedOut( netadr_t peer )
{
	if( !p2p_request || !NET_CompareAdr( peer, p2p_peer ))
		return false;
	SteamBroker_P2PError( p2p_request, "Steam P2P: server handshake timed out" );
	return p2p_deadline > 0;
}

qboolean SteamBroker_P2PReady( netadr_t peer )
{
	return p2p_request && !p2p_failed && p2p_ready && NET_CompareAdr( peer, p2p_peer );
}

void SteamBroker_SendP2P( netadr_t peer, const void *data, size_t length )
{
	byte payload[SBRK_MAX_FRAME_SIZE];
	uint64_t identity;
	if( !p2p_request || !NET_CompareAdr( peer, p2p_peer ) || !length )
		return;
	if( length > NET_STEAM_P2P_MTU )
	{
		Con_DPrintf( S_WARN "Steam P2P: packet exceeds unreliable MTU (%zu)\n", length );
		return;
	}
	memcpy( payload, "sb_p2p_send\n", 12 );
	for( int index = 0; index < 4; index++ ) payload[12 + index] = p2p_request >> ( index * 8 );
	memcpy( &identity, peer.ip6_1, sizeof( identity ));
	for( int index = 0; index < 8; index++ ) payload[16 + index] = identity >> ( index * 8 );
	memcpy( payload + 24, data, length );
	SteamBroker_SendFrame( (const char *)payload, length + 24 );
}

qboolean SteamBroker_GetP2P( netadr_t *peer, byte *data, size_t *length )
{
	if( !p2p_request || p2p_failed || p2p_read == p2p_write )
		return false;
	unsigned int slot = p2p_read++ % ARRAYSIZE( p2p_packets );
	*peer = p2p_peer;
	*length = p2p_packets[slot].length;
	memcpy( data, p2p_packets[slot].data, *length );
	return true;
}

#define TRACKER_MASTERLIST_URL	"https://api.gamemonitoring.net/servers?game=%d&status=online&limit=100&offset=%d"
#define WEBAPI_MASTERLIST_URL	"https://api.steampowered.com/IGameServersService/GetServerList/v1/?key=%s&filter=%%5Cappid%%5C%d&limit=500"
#define TRACKER_MASTERLIST_MAX	300
#define TRACKER_MASTERLIST_PAGES	10 // 10 x 100 = up to 1000 servers per scan

static CVAR_DEFINE_AUTO( cl_masterlist_webapi_key, "", FCVAR_ARCHIVE, "Steam Web API key for the official IGameServersService masterlist (free at steamcommunity.com/dev/apikey; empty = use community tracker)" );

static qboolean SteamTracker_FindObject( const char *p, const char *end, const char **obj, const char **objend )
{
	int depth = 0;
	qboolean in_string = false;
	const char *start = NULL;

	for( ; p < end; p++ )
	{
		if( in_string )
		{
			if( *p == '\\' && p + 1 < end )
				p++;
			else if( *p == '"' )
				in_string = false;
		}
		else if( *p == '"' )
			in_string = true;
		else if( *p == '{' )
		{
			if( depth == 0 )
				start = p;
			depth++;
		}
		else if( *p == '}' )
		{
			if( --depth == 0 && start )
			{
				*obj = start;
				*objend = p + 1;
				return true;
			}
			if( depth < 0 )
				return false;
		}
		else if( *p == ']' && depth == 0 )
			return false; // end of items[] without an object
	}
	return false;
}

// Skip JSON whitespace (the live API is compact, but caches/proxies
// may pretty-print; values must never depend on spacing).
static const char *SteamTracker_SkipWS( const char *p, const char *end )
{
	while( p < end && ( *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ))
		p++;
	return p;
}

// Bounded literal search: key must match fully inside [span, end).
static const char *SteamTracker_FindKey( const char *span, const char *end, const char *key )
{
	size_t keylen = Q_strlen( key );
	const char *p = span;

	while( p + keylen <= end )
	{
		if( !Q_strncmp( p, key, keylen ))
			return p + keylen;
		p++;
	}
	return NULL;
}

typedef enum
{
	MASTERLIST_TRACKER, // gamemonitoring JSON: response.items[], "request", freshness filters
	MASTERLIST_WEBAPI // Valve IGameServersService: response.servers[], "addr", live by construction
} masterlist_src_t;

// Returns servers fed this page; *raw_items counts objects seen (used
// for paging: a full page means more may follow).
static int SteamTracker_ParseAndFeed( const byte *data, size_t size, masterlist_src_t src, int *raw_items )
{
	const char *p = (const char *)data;
	const char *end = p + size;
	const char *items;
	const char *arraykey = ( src == MASTERLIST_WEBAPI ) ? "\"servers\":" : "\"items\":";
	const char *addrkey = ( src == MASTERLIST_WEBAPI ) ? "\"addr\":" : "\"request\":";
	int count = 0;

	*raw_items = 0;

	// locate the array start
	items = SteamTracker_FindKey( p, end, arraykey );
	if( !items )
	{
		Con_Printf( S_ERROR "%s: no server array in masterlist response\n", __func__ );
		return 0;
	}
	while( items < end && *items != '[' )
		items++;
	if( items >= end )
		return 0;
	p = items + 1;

	while( count < TRACKER_MASTERLIST_MAX )
	{
		const char *obj, *objend, *v;
		char address[64];
		size_t alen;
		netadr_t adr;

		if( !SteamTracker_FindObject( p, end, &obj, &objend ))
			break;
		p = objend;
		( *raw_items )++;

		if( src == MASTERLIST_TRACKER )
		{
			// status:true required (absent status counts as not-online)
			v = SteamTracker_FindKey( obj, objend, "\"status\":" );
			if( !v )
				continue;
			v = SteamTracker_SkipWS( v, objend );
			if( v + 4 > objend || Q_strncmp( v, "true", 4 ))
				continue;
			// private absent, false or null required (the API omits the
			// key at times and emits explicit nulls elsewhere)
			v = SteamTracker_FindKey( obj, objend, "\"private\":" );
			if( v )
			{
				v = SteamTracker_SkipWS( v, objend );
				if( v + 5 > objend || ( Q_strncmp( v, "false", 5 ) && Q_strncmp( v, "null", 4 )))
					continue;
			}
			// hide_address absent, false or null required (same rule)
			v = SteamTracker_FindKey( obj, objend, "\"hide_address\":" );
			if( v )
			{
				v = SteamTracker_SkipWS( v, objend );
				if( v + 5 > objend || ( Q_strncmp( v, "false", 5 ) && Q_strncmp( v, "null", 4 )))
					continue;
			}
		}
		// "request"/"addr" : "ip:port" (spacing tolerant, quote verified)
		v = SteamTracker_FindKey( obj, objend, addrkey );
		if( !v )
			continue;
		v = SteamTracker_SkipWS( v, objend );
		if( v >= objend || *v != '"' )
			continue;
		v++;
		alen = 0;
		while( v + alen < (const char *)objend && v[alen] != '"' && alen < sizeof( address ) - 1 )
			alen++;
		if( alen == 0 || alen >= sizeof( address ) - 1 )
			continue;
		memcpy( address, v, alen );
		address[alen] = '\0';

		if( NET_StringToAdr( address, &adr ) && NET_NetadrType( &adr ) == NA_IP )
		{
			if( Steam_MasterlistSeenBefore( &adr ))
			{
				Con_DPrintf( "%s: duplicate %s (already queried this scan)\n", __func__, address );
				continue;
			}
			Con_DPrintf( "%s: masterlist server: %s\n", __func__, address );
			NET_QueryServerByAddress( adr, PROTO_GOLDSRC );
			count++;
		}
	}

	return count;
}

static void SteamTracker_MasterListResponse( const char *url, qboolean success, const byte *data, size_t size, void *userdata )
{
	uintptr_t tag = (uintptr_t)userdata;
	masterlist_src_t src = (masterlist_src_t)( tag & 3 );
	unsigned int gen = (unsigned int)( tag >> 2 );
	(void)url;

	if( gen != s_tracker_gen )
		return; // stale chain from an older scan

	if( !success || !data || !size )
	{
		Con_Printf( S_ERROR "%s: masterlist download failed\n", __func__ );
		return;
	}

	{
		int raw = 0;
		int fed = SteamTracker_ParseAndFeed( data, size, src, &raw );

		if( src == MASTERLIST_WEBAPI )
		{
			Con_Printf( "Steam WebAPI server list complete: %d servers\n", fed );
			return;
		}

		s_tracker_total += fed;
		s_tracker_page++;

		if( raw >= 100 && s_tracker_page < TRACKER_MASTERLIST_PAGES )
		{
			// Full page: more may follow on the next offset.
			SteamTracker_RequestPage( s_tracker_page );
		}
		else
		{
			Con_Printf( "Community server list complete: %d servers (%d pages)\n",
				s_tracker_total, s_tracker_page );
		}
	}
}

// Fetch one tracker offset page (page 0 = offset 0) inside the current chain.
static qboolean SteamTracker_RequestPage( int page )
{
	char url[256];
	int appid = SteamBroker_GetGoldSrcAppId();
	uintptr_t tag = ( (uintptr_t)s_tracker_gen << 2 ) | (uintptr_t)MASTERLIST_TRACKER;

	if( Q_snprintf( url, sizeof( url ), TRACKER_MASTERLIST_URL, appid, page * 100 ) <= 0 )
		return false;

	if( page == 0 )
		Con_Printf( "Requesting community server list for appid %d...\n", appid );
	else
		Con_DPrintf( "%s: requesting page %d (offset %d)\n", __func__, page, page * 100 );

	if( !HTTP_GetToMemory( url, SteamTracker_MasterListResponse, (void *)tag ))
		return false;
	return true;
}

qboolean SteamTracker_RequestMasterList( void )
{
	// Chain state is (re)initialized by Steam_MasterlistScanBegin; start
	// the first page of the current chain here.
	return SteamTracker_RequestPage( s_tracker_page );
}

// Official Valve list via IGameServersService. Needs a free Web API key
// (steamcommunity.com/dev/apikey); without one the community tracker above
// is used instead. Same feed path, "addr" fields, no freshness filters
// (entries are live by construction).
qboolean SteamWebAPI_RequestMasterList( void )
{
	char url[512];
	int appid = SteamBroker_GetGoldSrcAppId();

	if( !cl_masterlist_webapi_key.string[0] )
		return false;

	if( Q_snprintf( url, sizeof( url ), WEBAPI_MASTERLIST_URL, cl_masterlist_webapi_key.string, appid ) <= 0 )
		return false;

	uintptr_t tag = ((uintptr_t)s_tracker_gen << 2 ) | (uintptr_t)MASTERLIST_WEBAPI;
	if( !HTTP_GetToMemory( url, SteamTracker_MasterListResponse, (void *)tag ))
		return false;

	Con_Printf( "Requesting Steam WebAPI server list for appid %d...\n", appid );
	return true;
}

static void SteamBroker_HandleDataTx( void )
{
	if( broker.tx_buffer_pos == 0 )
		return;

	int sent = send( broker.socket, (const char *)broker.tx_buffer, broker.tx_buffer_pos, 0 );
	if( NET_IsSocketError( sent ))
	{
		int err = WSAGetLastError();
		if( err != WSAEWOULDBLOCK && err != WSAEALREADY )
		{
			Con_Printf( S_ERROR "%s: send error %s\n", __func__, NET_ErrorString( ));
			SteamBroker_Disconnect( );
		}
		return;
	}

	if( sent > 0 )
	{
		// remove sent data from buffer
		memmove( broker.tx_buffer, broker.tx_buffer + sent, broker.tx_buffer_pos - sent );
		broker.tx_buffer_pos -= sent;
	}
}

static void SteamBroker_HandleDataRx( void )
{
	int available = sizeof( broker.rx_buffer ) - broker.rx_buffer_pos;
	if( available <= 0 )
	{
		Con_Printf( S_ERROR "%s: receive buffer overflow\n", __func__ );
		SteamBroker_Disconnect( );
		return;
	}

	int received = recv( broker.socket, (char *)broker.rx_buffer + broker.rx_buffer_pos, available, 0 );
	if( NET_IsSocketError( received ))
	{
		int err = WSAGetLastError();
		if( err != WSAEWOULDBLOCK && err != WSAEALREADY )
		{
			Con_Printf( S_ERROR "%s: recv error %s\n", __func__, NET_ErrorString( ));
			SteamBroker_Disconnect( );
		}
		return;
	}

	if( received == 0 )
	{
		Con_Printf( S_NOTE "%s: connection closed by broker\n", __func__ );
		SteamBroker_Disconnect( );
		return;
	}

	broker.rx_buffer_pos += received;

	while( SteamBroker_ProcessFrame( ));
}

static void SteamBroker_UpdateIdle( void )
{
	if( broker.idle_cycle_timeout < Platform_DoubleTime( ))
	{
		if( SteamBroker_UpdateBrokerAddress( ))
		{
			SteamBroker_ConnectImpl( );
		}
		else
		{
			Con_Printf( S_ERROR "%s: failed to resolve broker address \"%s\"\n", __func__, cl_steam_broker_addr.string );
		}
		broker.idle_cycle_timeout = Platform_DoubleTime() + SBRK_CONNECT_RETRY_DELAY;
	}
}

static void SteamBroker_AnnounceGameStart( const char *gamedir )
{
	if( Q_stricmp( cl_ticket_generator.string, "steam" ) != 0 )
		return;

	if( broker.state != SBRK_STATE_CONNECTED )
		return;

	// sb_gamedir <gamedir>
	char buf[512];
	int len = Q_snprintf( buf, sizeof( buf ), "sb_gamedir %s", gamedir );

	if( len > 0 )
		SteamBroker_SendFrame( buf, len );
}

static void SteamBroker_AnnounceGameShutdown( void )
{
	if( Q_stricmp( cl_ticket_generator.string, "steam" ) != 0 )
		return;

	if( broker.state != SBRK_STATE_CONNECTED )
		return;

	SteamBroker_SendFrame( "sb_terminate", sizeof( "sb_terminate" ) - 1 );
}

static void SteamBroker_UpdateConnecting( void )
{
	if( Platform_DoubleTime() > broker.connection_timeout )
	{
		Con_Printf( S_WARN "%s: connection to %s timed out\n", __func__, cl_steam_broker_addr.string );
		SteamBroker_Disconnect();
		return;
	}

	fd_set writefds;
	FD_ZERO( &writefds );
	FD_SET( broker.socket, &writefds );

	struct timeval tv = { 0 };

#if XASH_WIN32
	int select_result = select( 0, NULL, &writefds, NULL, &tv );
#else
	int select_result = select( broker.socket + 1, NULL, &writefds, NULL, &tv );
#endif
	if( select_result == SOCKET_ERROR )
	{
		Con_Printf( S_ERROR "%s: select() failed\n", __func__ );
		SteamBroker_Disconnect();
		return;
	}

	if( FD_ISSET( broker.socket, &writefds ))
	{
		// socket is writable - connection established or failed
		int err = 0;
		socklen_t err_len = sizeof( err );
		if( NET_IsSocketError( getsockopt( broker.socket, SOL_SOCKET, SO_ERROR, (char *)&err, &err_len )))
		{
			Con_Printf( S_ERROR "%s: getsockopt() failed\n", __func__ );
			SteamBroker_Disconnect();
			return;
		}
		else if( err != 0 )
		{
			Con_Printf( S_ERROR "%s: connection failed with error %d\n", __func__, err );
			SteamBroker_Disconnect();
			return;
		}
		else
		{
			broker.connection_timeout = 0;
			Con_Printf( S_NOTE "%s: connected to broker at %s\n", __func__, cl_steam_broker_addr.string );
			SteamBroker_SetState( SBRK_STATE_CONNECTED );
			SteamBroker_AnnounceGameStart( GI->gamefolder );

			// a ticket request may be pending (broker_wait): re-issue it now that we are connected again
			if( cls.broker_wait )
				SteamBroker_InitiateGameConnection( broker.serveradr, broker.challenge );
		}
	}
}

static void SteamBroker_UpdateConnected( void )
{
	// If we are waiting for a ticket and the broker hasn't answered in time
	// (e.g. the Android broker is stuck reconnecting to the Steam CM), drop the
	// connection so the IDLE state reconnects and the request is retried instead
	// of hanging forever in broker_wait.
	if( cls.broker_wait && broker.ticket_timeout != 0 && Platform_DoubleTime() > broker.ticket_timeout )
	{
		Con_Printf( S_WARN "%s: no ticket response within %.0f seconds, reconnecting to broker\n", __func__, SBRK_TICKET_RESPONSE_TIMEOUT );
		SteamBroker_Disconnect( );
		return;
	}

	// Same idea for the server list: a broker that silently ignores
	// sb_masterlist (older builds) must not wedge the scan forever.
	// Fall through to the next source instead.
	if( broker.masterlist_pending && Platform_DoubleTime() > broker.masterlist_deadline )
	{
		broker.masterlist_pending = false;
		Con_Printf( "Broker server list timed out with no reply, trying next source...\n" );
		if( !SteamWebAPI_RequestMasterList())
			SteamTracker_RequestMasterList();
	}

	SteamBroker_HandleDataTx( );
	SteamBroker_HandleDataRx( );
}

static int SteamBroker_GetGoldSrcAppId( void )
{
	if( cl_steam_appid.value > 0 )
		return (int)cl_steam_appid.value;

	const char *gamefolder = GI->gamefolder;

	if( !Q_stricmp( gamefolder, "svencoop" ))
		return 225840;
	if( !Q_stricmp( gamefolder, "gearbox" ))
		return 50;
	if( !Q_stricmp( gamefolder, "bshift" ))
		return 130;
	if( !Q_stricmp( gamefolder, "tfc" ))
		return 20;
	if( !Q_stricmp( gamefolder, "dod" ))
		return 30;
	if( !Q_stricmp( gamefolder, "dmc" ))
		return 40;
	if( !Q_stricmp( gamefolder, "ricochet" ))
		return 60;

	return 10; // valve, cstrike (Counter-Strike 1.6), hldm and other GoldSrc mods
}

qboolean SteamBroker_InitiateGameConnection( netadr_t serveradr, int challenge )
{
	// only ipv4 supported
	if( NET_NetadrType( &serveradr ) != NA_IP && NET_NetadrType( &serveradr ) != NA_STEAM_P2P )
		return false;

	if( broker.state != SBRK_STATE_CONNECTED )
	{
		Con_Printf( S_WARN "%s: broker not connected\n", __func__ );
		return false;
	}

	broker.challenge = challenge;
	broker.serveradr = serveradr;
	broker.ticket_request_id = ++ticket_request_serial;
	if( !broker.ticket_request_id )
		broker.ticket_request_id = ++ticket_request_serial;

	// sb_connect <ip:port> <server_steamid> <secure> <challenge> <appid>
	char buf[512];
	int appid = SteamBroker_GetGoldSrcAppId();
	int len = Q_snprintf( buf, sizeof( buf ), "sb_connect %s %"PRIu64" %d %d %d %u", NET_AdrToString( serveradr ), cls.server_steamid, cls.vac2_secure ? 1 : 0, challenge, appid, broker.ticket_request_id );

	Con_DPrintf( "%s: requesting ticket (server %s, steamid %"PRIu64", secure %d, challenge %d, appid %d)\n", __func__, NET_AdrToString( serveradr ), cls.server_steamid, cls.vac2_secure ? 1 : 0, challenge, appid );
	Con_DPrintf( "%s: sb_connect request: %s\n", __func__, buf );
	SteamBroker_DumpHex( "sb_connect", (const uint8_t *)buf, (size_t)len );

	if( !SteamBroker_SendFrame( buf, len ))
		return false;

	// arm the ticket-response watchdog
	broker.ticket_timeout = Platform_DoubleTime() + SBRK_TICKET_RESPONSE_TIMEOUT;

	return true;
}

void SteamBroker_TerminateGameConnection( void )
{
	p2p_retries = 0;
	SteamBroker_CancelTicket();
	if( p2p_request )
	{
		char command[64];
		int length = Q_snprintf( command, sizeof( command ), "sb_p2p_stop %u", p2p_request );
		SteamBroker_SendFrame( command, length );
		SteamBroker_ClearP2P();
	}
	if( broker.state != SBRK_STATE_CONNECTED )
		return;

	if( Q_stricmp( cl_ticket_generator.string, "steam" ) != 0 )
		return;

	// sb_disconnect <ip:port> <challenge>
	char buf[512];
	int len = Q_snprintf( buf, sizeof( buf ), "sb_disconnect %s %d", NET_AdrToString( cls.serveradr ), broker.challenge );

	SteamBroker_SendFrame( buf, len );
}

#if XASH_ENGINE_TESTS
void Test_RunSteamConnection( void )
{
	p2p_packet_t saved_packets[64];
	netadr_t saved_peer = p2p_peer;
	uint32_t saved_request = p2p_request;
	qboolean saved_ready = p2p_ready;
	qboolean saved_failed = p2p_failed;
	double saved_deadline = p2p_deadline;
	connstate_t saved_p2p_state = cls.state;
	int saved_retry = cls.connect_retry;
	double saved_connect_time = cls.connect_time;
	unsigned int saved_read = p2p_read, saved_write = p2p_write;
	memcpy( saved_packets, p2p_packets, sizeof( saved_packets ));
	SteamBroker_ClearP2P();
	uint64_t test_peer = UINT64_C( 76561198000000002 );
	NET_NetadrSetType( &p2p_peer, NA_STEAM_P2P );
	memcpy( p2p_peer.ip6_1, &test_peer, sizeof( test_peer ));
	p2p_request = 7;
	cls.state = ca_connecting;
	cls.connect_retry = 5;
	TASSERT( !SteamBroker_P2PReady( p2p_peer ));
	SteamBroker_P2PState( 6, "ready" );
	TASSERT( !SteamBroker_P2PReady( p2p_peer ));
	TASSERT_EQi( cls.connect_retry, 5 );
	SteamBroker_P2PState( 7, "gathered local_candidates=1" );
	TASSERT( !SteamBroker_P2PReady( p2p_peer ));
	SteamBroker_P2PState( 7, "ready" );
	TASSERT( SteamBroker_P2PReady( p2p_peer ));
	TASSERT_EQi( cls.connect_retry, 0 );
	TASSERT( cls.connect_time == MAX_HEARTBEAT );
	TASSERT( p2p_deadline == 0 );
	p2p_failed = true;
	TASSERT( !SteamBroker_P2PReady( p2p_peer ));
	SteamBroker_P2PState( 7, "ready" );
	TASSERT( !SteamBroker_P2PReady( p2p_peer ));
	p2p_failed = false;
	SteamBroker_P2PState( 7, "rechecking" );
	TASSERT( !SteamBroker_P2PReady( p2p_peer ));
	TASSERT( p2p_deadline > 0 );
	byte payload[25] = { 0 }, received[4000];
	netadr_t received_peer;
	size_t received_size = 0;
	memcpy( payload, "sb_p2p_data\n", 12 );
	payload[12] = 7;
	for( int index = 0; index < 8; index++ ) payload[16 + index] = test_peer >> ( index * 8 );
	payload[24] = 123;
	SteamBroker_ReceiveP2P( payload, 24 );
	TASSERT( !SteamBroker_GetP2P( &received_peer, received, &received_size ));
	payload[12] = 6;
	SteamBroker_ReceiveP2P( payload, sizeof( payload ));
	TASSERT( !SteamBroker_GetP2P( &received_peer, received, &received_size ));
	payload[12] = 7;
	payload[16] ^= 1;
	SteamBroker_ReceiveP2P( payload, sizeof( payload ));
	TASSERT( !SteamBroker_GetP2P( &received_peer, received, &received_size ));
	payload[16] ^= 1;
	p2p_failed = true;
	SteamBroker_ReceiveP2P( payload, sizeof( payload ));
	TASSERT_EQi( p2p_write - p2p_read, 0 );
	TASSERT( !SteamBroker_GetP2P( &received_peer, received, &received_size ));
	p2p_failed = false;
	SteamBroker_ReceiveP2P( payload, sizeof( payload ));
	TASSERT( SteamBroker_GetP2P( &received_peer, received, &received_size ));
	TASSERT( NET_CompareAdr( received_peer, p2p_peer ));
	TASSERT( !NET_IsLocalAddress( received_peer ));
	TASSERT( received_size == 1 && received[0] == 123 );
	TASSERT( !SteamBroker_GetP2P( &received_peer, received, &received_size ));
	for( int index = 0; index < 65; index++ ) SteamBroker_ReceiveP2P( payload, sizeof( payload ));
	TASSERT_EQi( p2p_write - p2p_read, 64 );
	SteamBroker_ClearP2P();
	TASSERT( !p2p_ready && !p2p_failed && p2p_deadline == 0 );
	TASSERT( !SteamBroker_GetP2P( &received_peer, received, &received_size ));
	p2p_peer = saved_peer;
	p2p_request = saved_request;
	p2p_ready = saved_ready;
	p2p_failed = saved_failed;
	p2p_deadline = saved_deadline;
	cls.state = saved_p2p_state;
	cls.connect_retry = saved_retry;
	cls.connect_time = saved_connect_time;
	p2p_read = saved_read;
	p2p_write = saved_write;
	memcpy( p2p_packets, saved_packets, sizeof( saved_packets ));
	uint64_t identity;
	TASSERT( SteamBroker_ParseSteamID( "STEAM_0:0:564129898", &identity ));
	TASSERT( identity == UINT64_C( 76561199088525524 ));
	TASSERT( SteamBroker_ParseSteamID( "76561198000000001", &identity ));
	TASSERT( identity == UINT64_C( 76561198000000001 ));
	TASSERT( SteamBroker_ParseSteamID( "STEAM_0:1:123", &identity ));
	TASSERT( identity == UINT64_C( 76561197960265975 ));
	TASSERT( !SteamBroker_ParseSteamID( "18446744073709551616", &identity ));
	TASSERT( !SteamBroker_ParseSteamID( "STEAM_256:1:123", &identity ));
	TASSERT( !SteamBroker_ParseSteamID( "STEAM_0:99999999999999999999:123", &identity ));
	TASSERT( !SteamBroker_ParseSteamID( "STEAM_0:1:2147483648", &identity ));
	TASSERT( !SteamBroker_ParseSteamID( "STEAM_0::1", &identity ));
	TASSERT( !SteamBroker_ParseSteamID( "STEAM_0:1:", &identity ));
	TASSERT( !SteamBroker_ParseSteamID( "0", &identity ));
	TASSERT( !SteamBroker_ParseSteamID( "", &identity ));
	steam_broker_t saved_broker = broker;
	connstate_t saved_state = cls.state;
	qboolean saved_wait = cls.broker_wait;
	netadr_t saved_address = cls.serveradr;

	memset( &broker, 0, sizeof( broker ));
	memset( &cls.serveradr, 0, sizeof( cls.serveradr ));
	NET_NetadrSetType( &cls.serveradr, NA_IP );
	cls.serveradr.port = 27015;
	cls.state = ca_connecting;
	cls.broker_wait = true;
	broker.serveradr = cls.serveradr;
	broker.challenge = 42;
	broker.ticket_request_id = 7;
	broker.ticket_timeout = 123;
	TASSERT( SteamBroker_IsTicketCurrent( 42, 7 ));
	TASSERT( !SteamBroker_IsTicketCurrent( 42, 6 ));
	TASSERT( !SteamBroker_IsTicketCurrent( 41, 7 ));
	cls.serveradr.port++;
	TASSERT( !SteamBroker_IsTicketCurrent( 42, 7 ));
	cls.serveradr = broker.serveradr;
	cls.state = ca_disconnected;
	TASSERT( !SteamBroker_IsTicketCurrent( 42, 7 ));
	cls.state = ca_connected;
	TASSERT( !SteamBroker_IsTicketCurrent( 42, 7 ));
	cls.state = ca_connecting;
	cls.broker_wait = false;
	TASSERT( !SteamBroker_IsTicketCurrent( 42, 7 ));
	cls.broker_wait = true;
	SteamBroker_CancelTicket();
	TASSERT( !cls.broker_wait );
	TASSERT_EQi( broker.ticket_request_id, 0 );
	TASSERT( broker.ticket_timeout == 0 );
	TASSERT_EQi( NET_NetadrType( &broker.serveradr ), NA_UNDEFINED );
	cls.broker_wait = true;
	TASSERT( !SteamBroker_IsTicketCurrent( 42, 7 ));
	broker.serveradr = cls.serveradr;
	broker.ticket_request_id = 8;
	TASSERT( !SteamBroker_IsTicketCurrent( 42, 7 ));
	TASSERT( SteamBroker_IsTicketCurrent( 42, 8 ));

	broker = saved_broker;
	cls.state = saved_state;
	cls.broker_wait = saved_wait;
	cls.serveradr = saved_address;
}
#endif

static void SteamBroker_RetryP2P( void )
{
	netadr_t peer = p2p_peer;
	unsigned int retries = p2p_retries;

	if( cls.demoplayback || cls.state < ca_connecting || cls.state > ca_active )
	{
		SteamBroker_ClearP2P();
		return;
	}

	if( retries >= SBRK_P2P_MAX_RETRIES )
	{
		Con_Printf( S_ERROR "Steam P2P: connection did not recover after %u retries; disconnecting\n", retries );
		CL_Disconnect();
		return;
	}

	Con_Printf( "Steam P2P: reconnecting after timeout (attempt %u/%u)\n", retries + 1, SBRK_P2P_MAX_RETRIES );
	CL_Disconnect();
	p2p_retries = retries + 1;
	cls.state = ca_connecting;
	cls.net_protocol = PROTO_GOLDSRC;
	cls.serveradr = peer;
	cls.connect_time = host.realtime;
	cls.max_fragment_size = FRAGMENT_MAX_SIZE;
	UI_SetActiveMenu( false );
}

static void SteamBroker_UpdateP2P( void )
{
	if( p2p_request && !p2p_failed && !p2p_ready && p2p_deadline && Platform_DoubleTime() >= p2p_deadline )
	{
		Con_Printf( S_ERROR "Steam P2P: broker negotiation timed out\n" );
		p2p_failed = true;
		p2p_deadline = Platform_DoubleTime() + SBRK_P2P_RETRY_DELAY;
	}
	if( p2p_request && p2p_ready && !p2p_failed && cls.state == ca_active && !cls.demoplayback )
	{
		double silence = host.realtime - cls.netchan.last_received;
		double now = Platform_DoubleTime();
		if( silence > SBRK_P2P_SILENCE_TIMEOUT && host.status == HOST_FRAME && host.pureframetime < 5.0 )
		{
			Con_Printf( S_WARN "Steam P2P: no server packets for %.0fs\n", silence );
			p2p_ready = false;
			p2p_failed = true;
			p2p_deadline = now + SBRK_P2P_RETRY_DELAY;
		}
		if( silence < 5.0 )
		{
			if( !p2p_healthy_since )
				p2p_healthy_since = now;
			if( now - p2p_healthy_since >= SBRK_P2P_SILENCE_TIMEOUT )
				p2p_retries = 0;
		}
		else p2p_healthy_since = 0;
	}
	else p2p_healthy_since = 0;
	if( p2p_request && p2p_failed && p2p_deadline && Platform_DoubleTime() >= p2p_deadline )
		SteamBroker_RetryP2P();
}

void SteamBroker_Frame( void )
{
	if( FBitSet( cl_steam_broker_addr.flags | cl_ticket_generator.flags, FCVAR_CHANGED ))
	{
		ClearBits( cl_ticket_generator.flags, FCVAR_CHANGED );
		ClearBits( cl_steam_broker_addr.flags, FCVAR_CHANGED );

		if( broker.state != SBRK_STATE_IDLE )
		{
			SteamBroker_Disconnect();
		}

		// reinitialize address
		NET_NetadrSetType( &broker.adr, NA_UNDEFINED );
	}

	if( Q_stricmp( cl_ticket_generator.string, "steam" ) != 0 )
		return;

	// update state machine
	switch( broker.state )
	{
	case SBRK_STATE_IDLE:
		SteamBroker_UpdateIdle( );
		break;
	case SBRK_STATE_CONNECTING:
		SteamBroker_UpdateConnecting( );
		break;
	case SBRK_STATE_CONNECTED:
		SteamBroker_UpdateConnected( );
		break;
	case SBRK_STATE_GAMESHUTDOWN:
		// do nothing, just wait for game shutdown
		break;
	}
	SteamBroker_UpdateP2P();
}

void SteamBroker_Init( void )
{
	broker.state = SBRK_STATE_IDLE;
	broker.socket = INVALID_SOCKET;
	broker.rx_buffer_pos = 0;
	broker.tx_buffer_pos = 0;
	broker.ticket_timeout = 0;
	Cvar_RegisterVariable( &cl_steam_broker_addr );
	Cmd_AddCommand( "@connect_steamid", SteamBroker_ConnectFriend_f, "connect to a Steam friend's advertised game server over UDP" );
	Cmd_AddCommand( "steam_masterlist", SteamBroker_MasterList_f, "request Steam internet server list" );
	Cvar_RegisterVariable( &cl_steam_appid );
	Cvar_RegisterVariable( &cl_masterlist_webapi_key );
	NET_NetadrSetType( &broker.adr, NA_UNDEFINED );
}

void SteamBroker_Shutdown( void )
{
	if( Q_stricmp( cl_ticket_generator.string, "steam" ) != 0 )
		return;

	SteamBroker_AnnounceGameShutdown( );
	SteamBroker_SetState( SBRK_STATE_GAMESHUTDOWN );
}
