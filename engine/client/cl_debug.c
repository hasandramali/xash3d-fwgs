/*
cl_debug.c - server message debugging
Copyright (C) 2018 Uncle Mike

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
#include "xash3d_mathlib.h"
#include "net_encode.h"
#include "cl_tent.h"
#include "shake.h"
#include "input.h"

#define MSG_COUNT		32		// last 32 messages parsed
#define MSG_MASK		(MSG_COUNT - 1)

typedef struct
{
	int	command;
	int	starting_offset;
	int	frame_number;
} oldcmd_t;

typedef struct
{
	oldcmd_t	oldcmd[MSG_COUNT];
	int	currentcmd;
	qboolean	parsing;
} msg_debug_t;

static msg_debug_t	cls_message_debug;

const char *CL_MsgInfo( int cmd )
{
	static string	sz;

	Q_strncpy( sz, "???", sizeof( sz ));

	if( cmd >= 0 && cmd <= svc_lastmsg )
	{
		// get engine message name
		const char *svc_string = NULL;

		switch( cls.net_protocol )
		{
		case PROTO_CURRENT:
			svc_string = svc_strings[cmd];
			break;
		case PROTO_QUAKE:
			svc_string = svc_quake_strings[cmd];
			break;
		case PROTO_GOLDSRC:
			svc_string = svc_goldsrc_strings[cmd];
			break;
		}

		// fall back to current protocol strings
		if( !svc_string )
			svc_string = svc_strings[cmd];

		Q_strncpy( sz, svc_string, sizeof( sz ));
	}
	else if( cmd > svc_lastmsg && cmd <= ( svc_lastmsg + MAX_USER_MESSAGES ))
	{
		for( int i = 0; i < MAX_USER_MESSAGES; i++ )
		{
			if( clgame.msg[i].number == cmd )
			{
				Q_strncpy( sz, clgame.msg[i].name, sizeof( sz ));
				break;
			}
		}
	}
	return sz;
}

/*
=====================
CL_Parse_Debug

enable message debugging
=====================
*/
void CL_Parse_Debug( qboolean enable )
{
	cls_message_debug.parsing = enable;
}

/*
=====================
CL_Parse_RecordCommand

record new message params into debug buffer
=====================
*/
void CL_Parse_RecordCommand( int cmd, int startoffset )
{
	if( cmd == svc_nop ) return;

	if( cl_trace_messages.value )
		Con_Reportf( "^3svc %04i %s\n", startoffset, CL_MsgInfo( cmd ));

	int	slot = ( cls_message_debug.currentcmd++ & MSG_MASK );
	cls_message_debug.oldcmd[slot].command = cmd;
	cls_message_debug.oldcmd[slot].starting_offset = startoffset;
	cls_message_debug.oldcmd[slot].frame_number = host.framecount;
}

/*
=====================
CL_ResetFrame
=====================
*/
void CL_ResetFrame( frame_t *frame )
{
	memset( &frame->graphdata, 0, sizeof( netbandwidthgraph_t ));
	frame->receivedtime = host.realtime;
	frame->valid = true;
	frame->choked = false;
	frame->latency = 0.0;
	frame->time = cl.mtime[0];
}

/*
=====================
CL_DumpAnnotatedMessageBytes

print bytes from the message, each byte interpreted as a potential
server command so an offset mismatch is easy to spot
=====================
*/
static void CL_DumpAnnotatedMessageBytes( sizebuf_t *msg, int start, int end, int highlight_offset )
{
	byte *p = (byte *)MSG_GetData( msg );
	int size = MSG_GetMaxBytes( msg );
	int i;

	start = bound( 0, start, size );
	end = bound( start, end, size );

	for( i = start; i < end; i++ )
	{
		if( i == highlight_offset )
			Con_Printf( S_RED "%04i: 0x%02x <%s> <-- parse failed here\n" S_DEFAULT, i, p[i], CL_MsgInfo( p[i] ));
		else Con_Printf( "%04i: 0x%02x <%s>\n", i, p[i], CL_MsgInfo( p[i] ));
	}
}

/*
=====================
CL_DumpBadMessage

maximum-verbosity dump of the message that failed to parse; called right
before Host_Error to give as much context as possible about the bad command
=====================
*/
void CL_DumpBadMessage( sizebuf_t *msg, int svc_num, int startoffset )
{
	int i;
	int size = MSG_GetMaxBytes( msg );

	Con_Printf( "\n" S_RED "== Parse error: %s (0x%02x) ==\n" S_DEFAULT, CL_MsgInfo( svc_num ), svc_num );
	Con_Printf( "protocol: %s, state: %d, signon: %d, incoming seq: %u, ack: %u, reliable ack: %u\n",
		cls.net_protocol == PROTO_GOLDSRC ? "GOLDSRC" : ( cls.net_protocol == PROTO_QUAKE ? "QUAKE" : "CURRENT" ),
		cls.state, cls.signon, cls.netchan.incoming_sequence, cls.netchan.incoming_acknowledged, cls.netchan.incoming_reliable_acknowledged );
	Con_Printf( "message \"%s\": size %d bytes, read pos %d bits (%d bytes), bad command byte at offset %d\n",
		MSG_GetName( msg ), size, MSG_GetNumBitsRead( msg ), MSG_GetNumBytesRead( msg ), startoffset );

	if( size <= 0 )
		return;

	// for huge reassembled fragments limit the annotated dump to a sane window
	if( size <= 1024 )
	{
		Con_Printf( "annotated byte dump (each byte shown as if it were a server command):\n" );
		CL_DumpAnnotatedMessageBytes( msg, 0, size, startoffset );
	}
	else
	{
		Con_Printf( "annotated byte dump around the failing command (message is %d bytes):\n", size );
		CL_DumpAnnotatedMessageBytes( msg, startoffset - 16, startoffset + 256, startoffset );
	}

	Con_Printf( "\nlast %i parsed commands:\n", MSG_COUNT );
	for( i = 0; i < MSG_COUNT; i++ )
	{
		oldcmd_t *old = &cls_message_debug.oldcmd[i];
		Con_Printf( "%08i %04i %s\n", old->frame_number, old->starting_offset, CL_MsgInfo( old->command ));
	}
}

/*
=====================
CL_WriteErrorMessage

write net_message into buffer.dat for debugging
=====================
*/
static void CL_WriteErrorMessage( int current_count, sizebuf_t *msg )
{
	const char	*buffer_file = "buffer.dat";
	file_t		*fp = FS_Open( buffer_file, "wb", false );
	if( !fp )
	{
		Con_Printf( S_ERROR "%s: can't open %s for write\n", __func__, buffer_file );
		return;
	}

	FS_Write( fp, &cls.starting_count, sizeof( int ));
	FS_Write( fp, &current_count, sizeof( int ));
	FS_Write( fp, &cls.net_protocol, sizeof( cls.net_protocol ));
	FS_Write( fp, MSG_GetData( msg ), MSG_GetMaxBytes( msg ));
	FS_Close( fp );

	Con_Printf( "Wrote erroneous message to %s\n", buffer_file );
}

/*
=====================
CL_WriteMessageHistory

list last 32 messages for debugging net troubleshooting
=====================
*/
void CL_WriteMessageHistory( void )
{
	oldcmd_t	*old;
	sizebuf_t	*msg = &net_message;
	int	thecmd;

	if( !cls.initialized || cls.state == ca_disconnected )
		return;

	if( !cls_message_debug.parsing )
		return;

	Con_Printf( "Last %i messages parsed.\n", MSG_COUNT );

	// finish here
	thecmd = cls_message_debug.currentcmd - 1;
	thecmd -= ( MSG_COUNT - 1 );	// back up to here

	for( int i = 0; i < MSG_COUNT - 1; i++ )
	{
		thecmd &= MSG_MASK;
		old = &cls_message_debug.oldcmd[thecmd];
		Con_Printf( "%i %04i %s\n", old->frame_number, old->starting_offset, CL_MsgInfo( old->command ));
		thecmd++;
	}

	old = &cls_message_debug.oldcmd[thecmd];
	Con_Printf( S_RED "BAD: " S_DEFAULT "%i %04i %s\n", old->frame_number, old->starting_offset, CL_MsgInfo( old->command ));

	Con_Printf( "\nannotated bytes around the failing command (each byte shown as if it were a server command):\n" );
	CL_DumpAnnotatedMessageBytes( msg, old->starting_offset - 8, old->starting_offset + 16, old->starting_offset );

	CL_WriteErrorMessage( old->starting_offset, msg );
	cls_message_debug.parsing = false;
}

void CL_ReplayBufferDat_f( void )
{
	file_t *f = FS_Open( Cmd_Argv( 1 ), "rb", true );
	sizebuf_t msg;
	char buffer[NET_MAX_MESSAGE];
	int starting_count, current_count, protocol;
	fs_offset_t len;

	if( !f )
		return;

	FS_Read( f, &starting_count, sizeof( starting_count ));
	FS_Read( f, &current_count, sizeof( current_count ));
	FS_Read( f, &protocol, sizeof( protocol ));

	cls.net_protocol = protocol;

	len = FS_Read( f, buffer, sizeof( buffer ));
	FS_Close( f );

	MSG_Init( &msg, __func__, buffer, len );

	Delta_Shutdown();
	Delta_Init();

	clgame.maxEntities = MAX_EDICTS;
	clgame.entities = Mem_Calloc( clgame.mempool, sizeof( *clgame.entities ) * clgame.maxEntities );

	// ad-hoc implement
#if 0
	{
		const int message_pos = 12; // put real number here
		MSG_SeekToBit( &msg, ( message_pos - 12 + 1 ) << 3, SEEK_SET );

		CL_ParseYourMom( &msg, protocol );
	}
#endif

	Sys_Quit( __func__ );
}
