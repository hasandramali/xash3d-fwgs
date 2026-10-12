#ifndef STEAM_P2P_PROTOCOL_H
#define STEAM_P2P_PROTOCOL_H

#define STEAM_P2P_CANDIDATE_STRING 1024
#define STEAM_P2P_CANDIDATE_MAX (7 * STEAM_P2P_CANDIDATE_STRING + 11)

typedef struct steam_p2p_candidate_s
{
	char name[STEAM_P2P_CANDIDATE_STRING];
	char protocol[STEAM_P2P_CANDIDATE_STRING];
	char address[STEAM_P2P_CANDIDATE_STRING];
	uint16_t port;
	float preference;
	char username[STEAM_P2P_CANDIDATE_STRING];
	char password[STEAM_P2P_CANDIDATE_STRING];
	char type[STEAM_P2P_CANDIDATE_STRING];
	char network[STEAM_P2P_CANDIDATE_STRING];
	uint32_t generation;
	byte extension;
} steam_p2p_candidate_t;

size_t SteamP2P_EncodeCandidate( const steam_p2p_candidate_t *candidate, byte *output, size_t capacity );
qboolean SteamP2P_DecodeCandidate( const byte *data, size_t size, steam_p2p_candidate_t *candidate );
size_t SteamP2P_EncodeUnreliableDatagram( int channel, const byte *data, size_t size, byte *output, size_t capacity );
qboolean SteamP2P_DecodeUnreliableDatagram( const byte *data, size_t size, int *channel, const byte **payload, size_t *payload_size );

#endif
