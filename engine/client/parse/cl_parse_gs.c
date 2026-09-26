/*
cl_parse.c - parse a message received from the server (GoldSrc 48 protocol)
Copyright (C) 2008 Uncle Mike

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
#include "net_encode.h"
#include "cl_tent.h"
#include "shake.h"
#include "input.h"
#include "server.h"

/*
======================
Baseline autocure (proedu)

Track which entity indices actually arrived in svc_spawnbaseline. When a
delta-packet newent decodes against &ent->baseline for an index that never
got its baseline (truncated/stalled signon stream), the resulting entity is
garbage/invisible. Detect it, drop the entity and invalidate the frame so the
engine's existing flush machinery nudges the server into a full (delta=0)
resend which re-baselines the affected indices.

Flag array is per eindex (MAX_GOLDSRC_ENTITY_BITS wide, same as the
on-wire Svengine entity index space). Cleared on every CL_ClearState.
======================
*/
#define GS_BASELINE_SLOTS	( 1 << MAX_GOLDSRC_ENTITY_BITS )

static qboolean gs_baseline_rx[GS_BASELINE_SLOTS];

qboolean CL_GSBaselineReceived( int entnum )
{
	if( entnum < 0 || entnum >= GS_BASELINE_SLOTS ) return false;
	return gs_baseline_rx[entnum];
}

void CL_GSBaselineSet( int entnum )
{
	if( entnum >= 0 && entnum < GS_BASELINE_SLOTS )
		gs_baseline_rx[entnum] = true;
}

void CL_GSBaselineResetAll( void )
{
	memset( gs_baseline_rx, 0, sizeof( gs_baseline_rx ));
}

static void CL_ParseExtraInfo( sizebuf_t *msg )
{
	string clientfallback;

	Q_strncpy( clientfallback, MSG_ReadString( msg ), sizeof( clientfallback ));
	if( !COM_StringEmpty( clientfallback ))
		Con_Reportf( S_ERROR "%s: TODO: add fallback directory %s!\n", __func__, clientfallback );

	cls.allow_cheats = MSG_ReadByte( msg ) ? true : false;
	CL_SetCheatState( cl.maxclients > 1, cls.allow_cheats );
}

static void CL_ParseNewMovevars( sizebuf_t *msg )
{
	Delta_InitClient(); // finalize client delta's

	clgame.movevars.gravity           = MSG_ReadFloat( msg );
	clgame.movevars.stopspeed         = MSG_ReadFloat( msg );
	clgame.movevars.maxspeed          = MSG_ReadFloat( msg );
	clgame.movevars.spectatormaxspeed = MSG_ReadFloat( msg );
	clgame.movevars.accelerate        = MSG_ReadFloat( msg );
	clgame.movevars.airaccelerate     = MSG_ReadFloat( msg );
	clgame.movevars.wateraccelerate   = MSG_ReadFloat( msg );
	clgame.movevars.friction          = MSG_ReadFloat( msg );
	clgame.movevars.edgefriction      = MSG_ReadFloat( msg );
	clgame.movevars.waterfriction     = MSG_ReadFloat( msg );
	clgame.movevars.entgravity        = MSG_ReadFloat( msg );
	clgame.movevars.bounce            = MSG_ReadFloat( msg );
	clgame.movevars.stepsize          = MSG_ReadFloat( msg );
	clgame.movevars.maxvelocity       = MSG_ReadFloat( msg );
	clgame.movevars.zmax              = MSG_ReadFloat( msg );
	clgame.movevars.waveHeight        = MSG_ReadFloat( msg );
	clgame.movevars.footsteps         = MSG_ReadByte( msg );
	clgame.movevars.rollangle         = MSG_ReadFloat( msg );
	clgame.movevars.rollspeed         = MSG_ReadFloat( msg );
	clgame.movevars.skycolor[0]       = MSG_ReadFloat( msg );
	clgame.movevars.skycolor[1]       = MSG_ReadFloat( msg );
	clgame.movevars.skycolor[2]       = MSG_ReadFloat( msg );
	clgame.movevars.skyvec[0]         = MSG_ReadFloat( msg );
	clgame.movevars.skyvec[1]         = MSG_ReadFloat( msg );
	clgame.movevars.skyvec[2]         = MSG_ReadFloat( msg );

	Q_strncpy( clgame.movevars.skyName, MSG_ReadString( msg ), sizeof( clgame.movevars.skyName ));

	// water alpha is not allowed
	if( !FBitSet( world.flags, FWORLD_WATERALPHA ))
		clgame.movevars.wateralpha = 1.0f;

	// update sky if changed
	if( Q_strcmp( clgame.oldmovevars.skyName, clgame.movevars.skyName ) && cl.video_prepped )
		R_SetupSky( clgame.movevars.skyName );

	clgame.oldmovevars = clgame.movevars;

	// FIXME: set world wave height when entities will be allocated
	if( clgame.entities )
		clgame.entities->curstate.scale = clgame.movevars.waveHeight;

	// keep features an actual!
	clgame.oldmovevars.features = clgame.movevars.features = host.features;
}

typedef struct delta_header_t
{
	qboolean remove;
	qboolean custom;
	qboolean instanced;
	uint16_t instanced_baseline_index;
	uint16_t offset;
} delta_header_t;

static int CL_ParseDeltaHeader( sizebuf_t *msg, qboolean delta, int oldnum, struct delta_header_t *hdr )
{
	int entnum = oldnum;
	memset( hdr, 0, sizeof( *hdr ));

	if( !delta )
	{
		// if we have one bit set, then it's a next entity in line
		// if we have next bit NON set, then it's a one of next 64 entities
		// if not, it's a new entity
		if( MSG_ReadOneBit( msg ))
			entnum++;
		else if( MSG_ReadOneBit( msg ) == 0 )
			entnum += MSG_ReadUBitLong( msg, 6 );
		else
			entnum = MSG_ReadUBitLong( msg, MAX_GOLDSRC_ENTITY_BITS );
	}
	else
	{
		// does this packet encode entity deletion?
		hdr->remove = MSG_ReadOneBit( msg );

		// same logic as above
		if( MSG_ReadOneBit( msg ) == 0 )
			entnum += MSG_ReadUBitLong( msg, 6 );
		else entnum = MSG_ReadUBitLong( msg, MAX_GOLDSRC_ENTITY_BITS );
	}

	// if we are not removing this entity
	if( !hdr->remove )
	{
		hdr->custom = MSG_ReadOneBit( msg );

		// do we got instanced baselines in svc_spawnbaselines?
		if( cl.instanced_baseline_count )
		{
			hdr->instanced = MSG_ReadOneBit( msg );
			if( hdr->instanced )
				hdr->instanced_baseline_index = MSG_ReadUBitLong( msg, 6 );
		}

		if( !delta && !hdr->instanced )
		{
			if( MSG_ReadOneBit( msg ))
				hdr->offset = MSG_ReadUBitLong( msg, 6 );
		}
	}

	return entnum;
}

static int CL_GetEntityDelta( const struct delta_header_t *hdr, int entnum )
{
	if( hdr->custom )
		return DT_CUSTOM_ENTITY_STATE_T;

	if( CL_IsPlayerIndex( entnum ))
		return DT_ENTITY_STATE_PLAYER_T;

	return DT_ENTITY_STATE_T;
}

static int CL_FlushEntityPacketGS( frame_t *frame, sizebuf_t *msg )
{
	int playerbytes = 0, numbase = 0;

	frame->valid = false;
	cl.validsequence = 0; // can't render a frame

	// read it all but ignore it
	while( 1 )
	{
		int newnum, bufstart;
		entity_state_t from = { 0 }, to;
		delta_header_t hdr;
		qboolean player;

		if( MSG_ReadWord( msg ) != 0 )
		{
			MSG_SeekToBit( msg, -16, SEEK_CUR );
			numbase = newnum = CL_ParseDeltaHeader( msg, true, numbase, &hdr );
		}
		else break;

		if( MSG_CheckOverflow( msg ))
		{
			// GoldSrc: same desync family as the delta decoder overflow (server
			// encoded against baseline sets we don't hold). Don't crash; leave
			// the overflow flag set so the main dispatch loop drains the
			// datagram and re-syncs on the next netchan frame.
			return playerbytes;
		}

		player = CL_IsPlayerIndex( newnum );
		bufstart = MSG_GetNumBytesRead( msg );

		if( hdr.remove )
			continue;

		Delta_ReadGSFields( msg, CL_GetEntityDelta( &hdr, newnum ), &from, &to, cl.mtime[0] );

		if( player )
			playerbytes += MSG_GetNumBytesRead( msg ) - bufstart;
	}

	if( MSG_CheckOverflow( msg ))
	{
		// GoldSrc: see the in-loop overflow comment above.
		return playerbytes;
	}

	return playerbytes;
}

// Message is hopelessly desynchronized: mark the frame unusable, drain the
// rest of the datagram and clear the sticky overflow flag so the main
// dispatch loop exits on the < 8 bits-left check instead of Host_Error (the
// next delta=0 full update from the server re-baselines).
static void CL_GSAbortDesync( frame_t *frame, sizebuf_t *msg )
{
	if( frame )
	{
		frame->valid = false;
		cl.validsequence = 0; // can't render a frame
	}
	MSG_EndBitWriting( msg );
	MSG_SeekToBit( msg, 0, SEEK_END );
	msg->bOverflow = false;
}

static qboolean CL_DeltaEntityGS( const delta_header_t *hdr, sizebuf_t *msg, frame_t *frame, int newnum, const entity_state_t *from, qboolean delta_update )
{
	cl_entity_t	*ent;
	entity_state_t	*to;
	qboolean newent = from == NULL;
	qboolean append = true;
	int pack = frame->num_entities;
	qboolean has_update = msg != NULL;
	static entity_state_t nullent;

	// alloc next slot to store update
	to = &cls.packet_entities[cls.next_client_entities % cls.num_client_entities];

	if(( newnum < 0 ) || ( newnum >= clgame.maxEntities ))
	{
		Con_DPrintf( S_ERROR "CL_DeltaEntity: invalid newnum: %d\n", newnum );
		// Desynced GoldSrc delta stream: the server encoded against entity/
		// baseline sets we don't hold (lost packet, Sven-SV engine delta
		// reset, sporelauncher volleys...). The real Sven client drops the
		// frame and re-syncs on the next full update, it does NOT crash.
		// Signal the caller to abort+flush this poison message.
		if( msg )
		{
			frame->valid = false;
			cl.validsequence = 0; // can't render a frame
		}
		return false;
	}

	ent = CL_EDICT_NUM( newnum );
	if( hdr->remove )
	{
		if( !newent )
			CL_KillDeadBeams( ent );
		else
			Con_Printf( S_WARN "%s: entity remove on non-delta update (%d)\n", __func__, newnum );
		return true;
	}

	ent->index = newnum; // enumerate entity index
	if( newent )
	{
		if( hdr->instanced )
			from = &cl.instanced_baseline[hdr->instanced_baseline_index];
		else if( hdr->offset != 0 )
		{
			// FIXME: the usage of `offset` is incorrect here as the entities might
			// not be ordered in cls.packet_entities the same way as on server
			// catch the error, print the scary message and fix it up to nullent
			if( cls.next_client_entities - hdr->offset < 0 )
			{
				Con_Printf( S_ERROR "%s: prevented delta-ing from invalid entity (%d - %d < 0)\n", __func__, cls.next_client_entities, hdr->offset );
				from = &nullent;
			}
			else from = &cls.packet_entities[(cls.next_client_entities - hdr->offset ) % cls.num_client_entities];
		}
		else
		{
			from = &ent->baseline;

			// Baseline autocure: a delta (delta=1) newent has no source other
			// than ent->baseline (delta headers never carry the offset field,
			// only full frames do). If that baseline never arrived, any entity
			// decoded from it is garbage. Drop it and invalidate the frame so
			// the flush path forces a full (delta=0) resync from the server.
			// CRITICAL: still consume the entity's wire fields below (decode
			// into the scratch slot without appending). Returning early here
			// leaves the field bits unread, which desyncs every subsequent
			// entity/command in the datagram (garbage eindexes, stray svc_bad
			// pads, malformed svc_disconnect tails).
			if( delta_update && !CL_GSBaselineReceived( newnum ))
			{
				Con_Printf( S_WARN "%s: eindex %d has no baseline (incomplete svc_spawnbaseline); dropped entity, requesting full resync\n", __func__, newnum );
				frame->valid = false;
				cl.validsequence = 0; // can't render a frame
				from = &nullent;
				append = false;
			}
		}
	}

	if( has_update )
		Delta_ReadGSFields( msg, CL_GetEntityDelta( hdr, newnum ), from, to, cl.mtime[0] );
	else memcpy( to, from, sizeof( entity_state_t ));

	to->entityType = hdr->custom ? ENTITY_BEAM : ENTITY_NORMAL;
	to->number = newnum;

	if( !append )
		return true; // fields consumed above; dropped entity stays out of the frame

	if( newent )
	{
		// interpolation must be reset
		SETVISBIT( frame->flags, pack );

		// release beams from previous entity

		// a1ba: check that this entity number was never used on client
		// as beams can be transferred before this entity was sent to client
		// (for example, beam was sent over during beam entity spawn
		// but referenced start point entity hasn't been sent over due to PVS)
		if( ent->curstate.messagenum != 0 )
			CL_KillDeadBeams( ent );
	}

	// add entity to packet
	cls.next_client_entities++;
	frame->num_entities++;

	return true;
}

static qboolean CL_CopyPacketEntity( frame_t *frame, int num, const entity_state_t *from )
{
	delta_header_t fakehdr =
	{
		.custom = FBitSet( from->entityType, ENTITY_BEAM ) == ENTITY_BEAM,
	};
	return CL_DeltaEntityGS( &fakehdr, NULL, frame, num, from, false );
}

static int CL_ParsePacketEntitiesGS( sizebuf_t *msg, qboolean delta )
{
	frame_t *oldframe;
	int numbase = 0;
	int playerbytes = 0;
	int dbg = Cvar_VariableInteger( "cl_goldsrc_debug" );

	// save first uncompressed packet as timestamp
	if( cls.changelevel && !delta && cls.demorecording )
		CL_WriteDemoJumpTime();

	int count = MSG_ReadWord( msg );

	frame_t *frame = &cl.frames[cl.parsecountmod];
	memset( frame->flags, 0, sizeof( frame->flags ));
	frame->first_entity = cls.next_client_entities;
	frame->num_entities = 0;
	frame->valid = true;

	if( delta )
	{
		// Svengine writes the delta sequence number in 16 bits; the clientdata
		// path (CL_ParseClientData) already reads a WORD for PROTO_GOLDSRC.
		uint oldpacket = MSG_ReadWord( msg );
		oldframe = &cl.frames[oldpacket & CL_UPDATE_MASK];

		if( !CL_ValidateDeltaPacket( oldpacket, oldframe ))
		{
			MSG_StartBitWriting( msg );
			CL_FlushEntityPacketGS( frame, msg );
			MSG_EndBitWriting( msg );
			return playerbytes;
		}
	}
	else
	{
		oldframe = NULL;
		cls.demowaiting = false;
	}

	cl.validsequence = cls.netchan.incoming_sequence;

	MSG_StartBitWriting( msg );

	entity_state_t *oldent = NULL;
	int oldindex = 0;
	int oldnum = CL_UpdateOldEntNum( oldindex, oldframe, &oldent );

	// read it all but ignore it
	while( 1 )
	{
		int bufstart, newnum;
		qboolean player;
		const char *srcMark = "?";
		delta_header_t hdr;
		int entStartBit = MSG_GetNumBitsRead( msg );
		int val = MSG_ReadWord( msg );

		if( val )
		{
			MSG_SeekToBit( msg, -16, SEEK_CUR );
			numbase = newnum = CL_ParseDeltaHeader( msg, delta, numbase, &hdr );
		}
		else break;

		if( MSG_CheckOverflow( msg ))
		{
			// Overflow while decoding a delta packet entity is a desync (the
			// server encoded against entity/baseline sets we don't hold, e.g.
			// Sven sporelauncher alt-fire volleys), NOT necessarily corruption.
			// The real Sven client drops the frame and lets the next full
			// update re-baseline instead of crashing out. Mirror that.
			Con_Printf( S_WARN "%s: overflow, dropping frame and requesting full resync\n", __func__ );
			frame->valid = false;
			cl.validsequence = 0; // can't render a frame
			MSG_EndBitWriting( msg );
			// MSG_CheckOverflow's bOverflow is sticky (MSG_EndBitWriting does
			// NOT clear it); the main dispatch loop tests it at the top of
			// every iteration and would otherwise still Host_Error. Drain the
			// rest of the datagram and clear the flag so the loop exits on
			// the < 8 bits-left check instead of crashing.
			MSG_SeekToBit( msg, 0, SEEK_END );
			msg->bOverflow = false;
			return playerbytes;
		}

		player = CL_IsPlayerIndex( newnum );

		while( oldnum < newnum )
		{
			if( !delta )
				Con_Printf( S_WARN "%s: old frame copy on non-delta update (%d < %d)\n", __func__, oldnum, newnum );

			// one or more entities from the old packet are unchanged
			if( !CL_CopyPacketEntity( frame, oldnum, oldent ))
			{
				CL_GSAbortDesync( frame, msg );
				return playerbytes;
			}
			oldnum = CL_UpdateOldEntNum( ++oldindex, oldframe, &oldent );
		}

		bufstart = MSG_GetNumBytesRead( msg );

		if( oldnum == newnum )
		{
			if( !delta )
				Con_Printf( S_WARN "%s: delta entity on non-delta update (%d)\n", __func__, oldnum );

			// from delta
			srcMark = "old-delta";
			if( !CL_DeltaEntityGS( &hdr, msg, frame, newnum, oldent, delta ))
			{
				CL_GSAbortDesync( frame, msg );
				return playerbytes;
			}
			oldnum = CL_UpdateOldEntNum( ++oldindex, oldframe, &oldent );
		}
		else if( oldnum > newnum )
		{
			// from baseline
			srcMark = "baseline";
			if( !CL_DeltaEntityGS( &hdr, msg, frame, newnum, NULL, delta ))
			{
				CL_GSAbortDesync( frame, msg );
				return playerbytes;
			}
		}

		// entity-level wire ledger: decode the delta packet header so the
		// packed "who/what" info (remove/custom/instanced flags, table, and
		// the exact bit span each entity consumed) is readable without any
		// offline re-assembly of the hex stream.
		if( dbg >= 4 && !hdr.remove )
		{
			const char *tbl = hdr.custom ? "custom" : ( player ? "player" : "normal" );
			Con_DPrintf( "GS-ENT: delta=%d eindex=%d custom=%d inst=%d off=%d table=%s src=%s bits=%d..%d (%d bits)\n",
				delta, newnum, hdr.custom, hdr.instanced, hdr.offset, tbl, srcMark,
				entStartBit, MSG_GetNumBitsRead( msg ), MSG_GetNumBitsRead( msg ) - entStartBit );
		}
		else if( dbg >= 4 )
		{
			Con_DPrintf( "GS-ENT: delta=%d eindex=%d REMOVE bits=%d..%d\n",
				delta, newnum, entStartBit, MSG_GetNumBitsRead( msg ));
		}

		if( player ) playerbytes += MSG_GetNumBitsRead( msg ) - bufstart;
	}

	if( MSG_CheckOverflow( msg ))
	{
		Con_Printf( S_WARN "%s: overflow after decoder, dropping frame and requesting full resync\n", __func__ );
		frame->valid = false;
		cl.validsequence = 0; // can't render a frame
		MSG_EndBitWriting( msg );
		MSG_SeekToBit( msg, 0, SEEK_END );
		msg->bOverflow = false;
		return playerbytes;
	}

	// any remaining entities in the old frame are copied over
	while( oldnum != MAX_ENTNUMBER )
	{
		// one or more entities from the old packet are unchanged
		CL_CopyPacketEntity( frame, oldnum, oldent );
		oldnum = CL_UpdateOldEntNum( ++oldindex, oldframe, &oldent );
	}

	MSG_EndBitWriting( msg );

	if( frame->num_entities != count )
		Con_Reportf( S_WARN "CL_Parse%sPacketEntitiesGS: (%i should be %i)\n", delta ? "Delta" : "", frame->num_entities, count );

	if( !frame->valid )
		return playerbytes;

	CL_ProcessPacket( frame );
	CL_SetSolidEntities();

	// first update, received world, remove loading plaque
	if( cls.signon == ( SIGNONS - 1 ))
	{
		cls.signon = SIGNONS;
		CL_SignonReply( PROTO_GOLDSRC );
	}

	return playerbytes;
}

static float MSG_ReadGSBitCoord( sizebuf_t *sb )
{
	float value = 0;

	int ival = MSG_ReadOneBit( sb );
	int fval = MSG_ReadOneBit( sb );

	if( ival || fval )
	{
		int sign = MSG_ReadOneBit( sb );

		if( ival )
			ival = MSG_ReadUBitLong( sb, 24 );
		if( fval )
			fval = MSG_ReadUBitLong( sb, 3 );

		value = (float)( fval / 8.0 + ival );
		if( sign )
			value = -value;
	}

	return value;
}

static void MSG_ReadGSBitVec3Coord( sizebuf_t *sb, vec3_t fa )
{
	VectorClear( fa );

	qboolean x = MSG_ReadOneBit( sb );
	qboolean y = MSG_ReadOneBit( sb );
	qboolean z = MSG_ReadOneBit( sb );
	if( x )
		fa[0] = MSG_ReadGSBitCoord( sb );
	if( y )
		fa[1] = MSG_ReadGSBitCoord( sb );
	if( z )
		fa[2] = MSG_ReadGSBitCoord( sb );
}

static void CL_ParseSoundPacketGS( sizebuf_t *msg )
{
	sound_t	handle = 0;

	MSG_StartBitWriting( msg );

	int flags = MSG_ReadUBitLong( msg, 9 );

	float volume;
	if( FBitSet( flags, SND_VOLUME ))
		volume = (float)MSG_ReadByte( msg ) / 255.0f;
	else volume = VOL_NORM;

	float attn;
	if( FBitSet( flags, SND_ATTENUATION ))
		attn = (float)MSG_ReadByte( msg ) / 64.0f;
	else attn = 1.0f;

	int chan = MSG_ReadUBitLong( msg, 3 );
	int entnum = MSG_ReadUBitLong( msg, MAX_GOLDSRC_SOUND_BITS );
	int sound;
	if( FBitSet( flags, SND_GOLDSRC_LARGE_INDEX ))
		sound = MSG_ReadWord( msg );
	else sound = MSG_ReadByte( msg );
	vec3_t pos;
	MSG_ReadGSBitVec3Coord( msg, pos );

	int pitch;
	if( FBitSet( flags, SND_PITCH ))
		pitch = MSG_ReadByte( msg );
	else pitch = PITCH_NORM;

	MSG_EndBitWriting( msg );

	ClearBits( flags, SND_GOLDSRC_LARGE_INDEX );

	if( FBitSet( flags, SND_SENTENCE ))
	{
		char	sentenceName[32];
		Q_snprintf( sentenceName, sizeof( sentenceName ), "!%i", sound );
		handle = S_RegisterSound( sentenceName );
	}
	else handle = cl.sound_index[sound];	// see precached sound

	if( !cl.audio_prepped )
		return; // too early

	// g-cont. sound and ambient sound have only difference with channel
	if( chan == CHAN_STATIC )
	{
		S_AmbientSound( pos, entnum, handle, volume, attn, pitch, flags );
	}
	else
	{
		S_StartSound( pos, entnum, chan, handle, volume, attn, pitch, flags );
	}
}

static void CL_ParseSpawnStaticSound( sizebuf_t *msg )
{
	vec3_t pos;
	sound_t handle = 0;

	MSG_ReadVec3Coord( msg, pos );
	int sound    = MSG_ReadShort( msg );
	float volume = MSG_ReadByte( msg ) * ( 1.0f / 255.0f );
	float attn   = MSG_ReadByte( msg ) * ( 1.0f / 64.0f );
	int entnum   = MSG_ReadShort( msg );
	int pitch    = MSG_ReadByte( msg );
	int flags    = MSG_ReadByte( msg );

	if( FBitSet( flags, SND_SENTENCE ))
	{
		char	sentenceName[32];
		Q_snprintf( sentenceName, sizeof( sentenceName ), "!%i", sound );
		handle = S_RegisterSound( sentenceName );
	}
	else handle = cl.sound_index[sound];	// see precached sound

	S_AmbientSound( pos, entnum, handle, volume, attn, pitch, flags );
}

/*
=====================================================================

ACTION MESSAGES

=====================================================================
*/
/*
=====================
CL_ParseGoldSrcServerMessage

dispatch messages
=====================
*/
void CL_ParseGoldSrcServerMessage( sizebuf_t *msg )
{
	size_t		bufStart, playerbytes;
	int		cmd, param1;
	const char	*s;

	// parse the message
	while( 1 )
	{
		// A disconnect/reset handler (svc_signonnum received with a value <=
		// current, svc_disconnect, ...) leaves the client state torn down
		// (clgame.entities == NULL). hw.dll exits its parse loop on signon
		// regression instead of draining the rest of the datagram (reverse
		// verified: svc_signonnum <= current is an error/reset path, not a
		// normal round restart). Parsing the remaining svc commands of this
		// same sizebuf (e.g. svc_spawnbaseline) would dereference freed
		// entity state and crash the client.
		if( cls.state == ca_disconnected )
		{
			MSG_Clear( msg );
			return;
		}

		if( MSG_CheckOverflow( msg ))
		{
			if( cls.net_protocol == PROTO_GOLDSRC )
			{
				// Any GoldSrc sub-parser (clientdata, delta entities, sound...)
				// can over-read when the server encodes against baseline sets we
				// don't hold or when the datagram is truncated. The Sven client
				// tolerates this by consuming the datagram and re-syncing on the
				// next netchan frame instead of dying; overflow is only detected
				// here at the top of each message, so drain + re-sync is safe.
				Con_Printf( S_WARN "%s: GoldSrc datagram overflow at byte %d, draining and re-syncing\n",
					__func__, (int)MSG_GetNumBytesRead( msg ));
				MSG_Clear( msg );
				return;
			}
			Host_Error( "%s: overflow!\n", __func__ );
			return;
		}

		// mark start position
		bufStart = MSG_GetNumBytesRead( msg );

		// end of message (align bits)
		if( MSG_GetNumBitsLeft( msg ) < 8 )
			break;

		cmd = MSG_ReadServerCmd( msg );

		// Sven Co-op dedicated servers pad the reliable stream with 0x00 bytes
		// directly after fixed-size user messages (buffer.dat shows the exact
		// pattern twice: cmd 148 VoiceMask, 8-byte mask, then 00 00 before the
		// next payload). Under the GoldSrc wire format 0x00 is svc_bad, which
		// Host_Error()s the client mid-signon. The Sven client engine tolerates
		// this padding, so skip the stray byte and keep parsing instead of
		// aborting. Harmless on vanilla GoldSrc too: mid-message 0x00 is never a
		// valid command, so consuming it is strictly more robust (the tail is
		// still guarded by the overflow check and the sizeof-left sanity skip).
		if( cmd == svc_bad && cls.net_protocol == PROTO_GOLDSRC )
		{
			if( Cvar_VariableInteger( "cl_goldsrc_debug" ) >= 1 )
				Con_Printf( "SVC-PAD: skipping svc_bad(0x00) pad byte at offset %d (Sven stream padding)\n", bufStart );
			continue;
		}

		// proedu: any GoldSrc command outside the keepalive/padding set is real
		// connection progress for the signon stall watchdog. svc_nop and
		// svc_roomtype are the pure filler a parked server feeds forever, so they
		// deliberately do NOT refresh the timer.
		if( cmd != svc_nop && cmd != svc_roomtype && cls.net_protocol == PROTO_GOLDSRC )
			CL_NoteConnectProgress();

		// STEAM/SIGNON DEBUG: trace every GoldSrc server command during connect
		if( Cvar_VariableInteger( "cl_goldsrc_debug" ) >= 1 )
			Con_DPrintf( "%s: svc cmd=%d (%s) signon=%d state=%d msgbits=%d byte=%d bit=%d\n", __func__, cmd, CL_MsgInfo( cmd ), cls.signon, cls.state, MSG_GetNumBitsLeft( msg ), (int)bufStart, MSG_GetNumBitsRead( msg ) );

		if( Cvar_VariableInteger( "cl_goldsrc_debug" ) >= 3 && cls.net_protocol == PROTO_GOLDSRC && cls.signon < SIGNONS )
		{
			int startByte = MSG_GetNumBitsWritten( msg ) >> 3;
			int maxBytes = MSG_GetMaxBytes( msg );
			int rem = maxBytes > startByte ? maxBytes - startByte : 0;
			if( rem > 0 )
			{
				Con_DPrintf( "%s: DUMP svc payload (svc cmd %d, %d bytes from offset %d)\n", __func__, cmd, rem, startByte );
				CL_DumpHex( "svc_pay", MSG_GetData( msg ) + startByte, rem );
			}
		}

		// record command for debugging spew on parse problem
		CL_Parse_RecordCommand( cmd, bufStart, MSG_GetNumBitsWritten( msg ) );

		if( CL_ParseCommonMessage( msg, PROTO_GOLDSRC, cmd, bufStart ))
			continue;

		if( CL_ParseCommonHLMessage( msg, PROTO_GOLDSRC, cmd, bufStart ))
			continue;

		// other commands
		switch( cmd )
		{
		case svc_disconnect:
			s = MSG_ReadString( msg );
			if( !COM_StringEmpty( s ))
				Con_Printf( "Server issued disconnect. Reason: %s\n", s );
			CL_Drop ();
			Host_AbortCurrentFrame ();
			break;
		case svc_event:
			MSG_StartBitWriting( msg );
			CL_ParseEvent( msg, PROTO_GOLDSRC );
			MSG_EndBitWriting( msg );
			cl.frames[cl.parsecountmod].graphdata.event += MSG_GetNumBytesRead( msg ) - bufStart;
			break;
		case svc_goldsrc_version:
			param1 = MSG_ReadLong( msg );
			if( param1 != PROTOCOL_GOLDSRC_VERSION )
			{
				// The genuine svc_goldsrc_version ("04 30 00 00 00" == 48) is only
				// sent once at signon, where the real protocol decision is made in
				// CL_ParseServerData. A stray 0x04 byte later in a datagram with a
				// random value is a misaligned/garbage tail, not a protocol change
				// (byte-verified against buffer.dat: the whole stream is aligned
				// exactly down to svc_nop, then 0xE0 at the boundary cannot be a
				// registered Sven message since they only occupy ~64..149). The
				// Sven reference client survives this by consuming the datagram and
				// re-syncing on the next netchan frame, so drain instead of crash.
				Con_Printf( S_WARN "%s: svc_goldsrc_version=%d (expected %d) at byte %d -- stray/garbage tail, draining datagram\n",
					__func__, param1, PROTOCOL_GOLDSRC_VERSION, (int)bufStart );
				MSG_SeekToBit( msg, 0, SEEK_END );
			}
			break;
		case svc_sound:
			CL_ParseSoundPacketGS( msg );
			cl.frames[cl.parsecountmod].graphdata.sound += MSG_GetNumBytesRead( msg ) - bufStart;
			break;
		case svc_deltatable:
			Delta_ParseTableField_GS( msg );
			break;
		case svc_clientdata:
			MSG_StartBitWriting( msg );
			CL_ParseClientData( msg, PROTO_GOLDSRC );
			MSG_EndBitWriting( msg );
			cl.frames[cl.parsecountmod].graphdata.client += MSG_GetNumBytesRead( msg ) - bufStart;
			break;
		case svc_goldsrc_stopsound:
			param1 = MSG_ReadWord( msg );
			S_StopSound( param1 >> 3, param1 & 7, NULL );
			cl.frames[cl.parsecountmod].graphdata.sound += MSG_GetNumBytesRead( msg ) - bufStart;
			break;
		case svc_pings:
			MSG_StartBitWriting( msg );
			CL_UpdateUserPings( msg );
			MSG_EndBitWriting( msg );
			break;
		case svc_goldsrc_damage:
		case svc_spawnstatic:
			// this does nothing
			break;
		case svc_event_reliable:
			MSG_StartBitWriting( msg );
			CL_ParseReliableEvent( msg, PROTO_GOLDSRC );
			MSG_EndBitWriting( msg );
			cl.frames[cl.parsecountmod].graphdata.event += MSG_GetNumBytesRead( msg ) - bufStart;
			break;
		case svc_spawnbaseline:
			MSG_StartBitWriting( msg );
			CL_ParseBaseline( msg, PROTO_GOLDSRC );
			MSG_EndBitWriting( msg );
			break;
		case svc_setpause:
			cl.paused = ( MSG_ReadByte( msg ) != 0 );
			break;
		case svc_goldsrc_killedmonster:
			// does nothing in Sven Co-op
			break;
		case svc_goldsrc_foundsecret:
		{
			// Sven Co-op sends a one-byte payload length, then that many bytes.
			// Without consuming it the parser treats the next byte as a command (svc_bad).
			int len = MSG_ReadByte( msg );
			while( len-- > 0 )
				MSG_ReadByte( msg );
			break;
		}
		case svc_goldsrc_spawnstaticsound:
			CL_ParseSpawnStaticSound( msg );
			break;
		case svc_goldsrc_decalname:
			param1 = MSG_ReadByte( msg );
			s = MSG_ReadString( msg );
			Q_strncpy( host.draw_decals[param1], s, sizeof( host.draw_decals[param1] ));
			break;
		case svc_packetentities:
			playerbytes = CL_ParsePacketEntitiesGS( msg, false );
			cl.frames[cl.parsecountmod].graphdata.players += playerbytes;
			cl.frames[cl.parsecountmod].graphdata.entities += MSG_GetNumBytesRead( msg ) - bufStart - playerbytes;
			break;
		case svc_deltapacketentities:
			playerbytes = CL_ParsePacketEntitiesGS( msg, true );
			cl.frames[cl.parsecountmod].graphdata.players += playerbytes;
			cl.frames[cl.parsecountmod].graphdata.entities += MSG_GetNumBytesRead( msg ) - bufStart - playerbytes;
			break;
		case svc_resourcelist:
			MSG_StartBitWriting( msg );
			CL_ParseResourceList( msg, PROTO_GOLDSRC );
			MSG_EndBitWriting( msg );
			break;
		case svc_deltamovevars:
			CL_ParseNewMovevars( msg );
			break;
		case svc_goldsrc_sendextrainfo:
			CL_ParseExtraInfo( msg );
			break;
		case svc_goldsrc_timescale:
			// we can set sys_timescale to anything we want but in GoldSrc it's locked for
			// HLTV and demoplayback. Do we really want to have it then if both are out of scope?
			Con_Reportf( S_ERROR "%s: svc_goldsrc_timescale: implement me!\n", __func__ );
			MSG_ReadFloat( msg );
			break;
		default:
			CL_ParseUserMessage( msg, cmd, PROTO_GOLDSRC );
			cl.frames[cl.parsecountmod].graphdata.usr += MSG_GetNumBytesRead( msg ) - bufStart;
			break;
		}
	}
}
