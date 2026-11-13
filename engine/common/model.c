/*
model.c - modelloader
Copyright (C) 2007 Uncle Mike

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
#include "mod_local.h"
#include "sprite.h"
#include "xash3d_mathlib.h"
#include "alias.h"
#include "studio.h"
#include "wadfile.h"
#include "world.h"
#include "enginefeatures.h"
#include "client.h"
#include "server.h"
#include <float.h>

static model_info_t	mod_crcinfo[MAX_MODELS];
static model_t	mod_known[MAX_MODELS];
static int	mod_numknown = 0;
poolhandle_t      com_studiocache;		// cache for submodels
CVAR_DEFINE( mod_studiocache, "r_studiocache", "1", FCVAR_ARCHIVE, "enables studio cache for speedup tracing hitboxes" );
CVAR_DEFINE_AUTO( r_wadtextures, "0", FCVAR_LATCH, "completely ignore textures in the bsp-file if enabled" );
CVAR_DEFINE_AUTO( r_showhull, "0", 0, "draw collision hulls 1-3" );
CVAR_DEFINE_AUTO( r_allow_wad3_luma, "0", FCVAR_LATCH|FCVAR_ARCHIVE, "allow usage of luma textures in wad3 (tilde textures)" );

#if !XASH_DEDICATED
#if XASH_ANDROID
#define STUDIO_TEXTURE_CACHE_DEFAULT "512"
#else
#define STUDIO_TEXTURE_CACHE_DEFAULT "0"
#endif
CVAR_DEFINE_AUTO( r_studio_texture_cache, STUDIO_TEXTURE_CACHE_DEFAULT, FCVAR_ARCHIVE,
	"evict studio model GL textures above this budget in megabytes (0 = keep all textures)" );

#define STUDIO_RELOAD_FRAME_BUDGET (32u << 20)

typedef struct studiotexmeta_s
{
	qboolean	evicted;	// GL textures were freed, ptexture[i].index == 0
	qboolean	pending;	// needs background reload after a deferred visible touch
	qboolean	fromT;		// textures live in a companion T.mdl file
	uint32_t	*ofs;		// per-texture pixel data offset in the source file
	int		numtextures;
	size_t		bytes;		// estimated GL memory occupied by the textures
	float		touch;		// last time the model was seen, -1 = never seen
} studiotexmeta_t;

static studiotexmeta_t *mod_studiotex[MAX_MODELS];
static int mod_studiotex_frame = -1;
static size_t mod_studiotex_framebytes;
static double mod_studiotex_lastenforce;
#endif

/*
===============================================================================

			STUDIO TEXTURE CACHE

Evicts least-recently-seen studio model GL textures when they exceed the
r_studio_texture_cache budget. CPU model data is always kept intact, so
server-side tracing and animation are never affected. Evicted textures are
transparently re-uploaded from disk when the model becomes visible again.

===============================================================================
*/
#if !XASH_DEDICATED
static studiotexmeta_t *Mod_StudioTexMeta( const model_t *mod )
{
	int i = mod - mod_known;

	if( i < 0 || i >= MAX_MODELS )
		return NULL;

	return mod_studiotex[i];
}

static void Mod_StudioEvictTextures( model_t *mod )
{
	studiotexmeta_t *meta = Mod_StudioTexMeta( mod );
	studiohdr_t *phdr;
	mstudiotexture_t *pt;
	int defaulttex, i;

	if( !meta || meta->evicted )
		return;

	phdr = (studiohdr_t *)mod->cache.data;
	if( !phdr || phdr->numtextures <= 0 || phdr->textureindex <= 0 )
		return;

	pt = (mstudiotexture_t *)((byte *)phdr + phdr->textureindex);
	defaulttex = ref.dllFuncs.GL_FindTexture( REF_DEFAULT_TEXTURE );

	for( i = 0; i < phdr->numtextures; i++ )
	{
		if( pt[i].index && pt[i].index != defaulttex )
			ref.dllFuncs.GL_FreeTexture( pt[i].index );
		pt[i].index = 0;
	}

	meta->evicted = true;
	meta->pending = false;

	Con_Reportf( "%s: evicted %i textures from %s (%u bytes)\n", __func__, phdr->numtextures, mod->name, (unsigned)meta->bytes );
}

static void Mod_StudioTexBudgetEnforce( void )
{
	float budget_mb = r_studio_texture_cache.value;
	size_t budget, total;
	float hold;
	int i;

	if( budget_mb <= 0.0f || Host_IsDedicated( ))
		return;

	budget = (size_t)( budget_mb * 1024.0f * 1024.0f );

	total = 0;
	for( i = 0; i < mod_numknown; i++ )
	{
		studiotexmeta_t *meta = mod_studiotex[i];

		if( mod_known[i].type != mod_studio || !meta || meta->evicted )
			continue;

		total += meta->bytes;
	}

	if( total <= budget )
		return;

	// models currently being rendered keep their textures for a while
	hold = cls.state == ca_active ? 5.0f : 0.0f;

	while( total > budget )
	{
		model_t *best = NULL;
		studiotexmeta_t *bestmeta = NULL;
		float best_touch = FLT_MAX;

		for( i = 0; i < mod_numknown; i++ )
		{
			studiotexmeta_t *meta = mod_studiotex[i];
			float touch;

			if( mod_known[i].type != mod_studio || !meta || meta->evicted )
				continue;

			if( mod_known[i].cache.data == NULL || meta->bytes == 0 )
				continue;

			touch = meta->touch;
			if( touch >= 0.0f && host.realtime - touch < hold )
				continue;

			if( touch < best_touch )
			{
				best_touch = touch;
				best = &mod_known[i];
				bestmeta = meta;
			}
		}

		if( !best )
			break;

		Mod_StudioEvictTextures( best );
		total -= bestmeta->bytes;
	}
}

static qboolean Mod_StudioReloadTextures( model_t *mod )
{
	studiotexmeta_t *meta = Mod_StudioTexMeta( mod );
	studiohdr_t *phdr;
	mstudiotexture_t *pt;
	char texfile[MAX_QPATH];
	fs_offset_t filesize;
	byte *buf;
	int defaulttex, cmap, i;

	if( !meta )
		return false;

	phdr = (studiohdr_t *)mod->cache.data;

	if( !phdr || phdr->numtextures <= 0 || phdr->textureindex <= 0 )
	{
		// nothing was uploaded at load time, nothing to restore
		meta->evicted = false;
		return true;
	}

	Q_strncpy( texfile, meta->fromT ? Mod_StudioTexName( mod->name ) : mod->name, sizeof( texfile ));

	buf = FS_LoadFile( texfile, &filesize, false );
	if( !buf )
	{
		// file went away, stop retrying until the model is touched again
		meta->touch = host.realtime;
		meta->pending = false;
		Con_Printf( S_WARN "%s: can't reload textures for %s, file %s is missing\n", __func__, mod->name, texfile );
		return false;
	}

	pt = (mstudiotexture_t *)((byte *)phdr + phdr->textureindex);
	defaulttex = ref.dllFuncs.GL_FindTexture( REF_DEFAULT_TEXTURE );
	cmap = 0;

	for( i = 0; i < phdr->numtextures; i++ )
	{
		char texname[128], name[128], mdlname[MAX_QPATH];
		qboolean colormap;
		int flags = 0;
		int texnum;
		size_t size;

		if( FBitSet( pt[i].flags, STUDIO_NF_NORMALMAP ))
			SetBits( flags, TF_NORMALMAP );

		if( FBitSet( pt[i].flags, STUDIO_NF_NOMIPS ))
			SetBits( flags, TF_NOMIPMAP );

		if( FBitSet( host.features, ENGINE_IMPROVED_LINETRACE ) && FBitSet( pt[i].flags, STUDIO_NF_MASKED ))
			SetBits( flags, TF_KEEP_SOURCE );

		colormap = !Q_strnicmp( pt[i].name, "DM_Base", 7 ) || !Q_strnicmp( pt[i].name, "remap", 5 );
		if( colormap )
			SetBits( flags, TF_FORCE_COLOR );

		Q_strncpy( mdlname, mod->name, sizeof( mdlname ));
		COM_FileBase( pt[i].name, name, sizeof( name ));
		COM_StripExtension( mdlname );
		Q_snprintf( texname, sizeof( texname ), "#%s/%s.mdl", mdlname, name );

		if( meta->ofs[i] >= (uint32_t)filesize )
		{
			Con_Printf( S_WARN "%s: texture %s of %s has bad offset %u (file is %u bytes), using default\n",
				__func__, pt[i].name, mod->name, (unsigned)meta->ofs[i], (unsigned)filesize );
			pt[i].index = defaulttex;
			if( colormap && cmap < mod->numtextures && mod->textures )
				mod->textures[cmap++]->gl_texturenum = pt[i].index;
			continue;
		}

		// the loader reads width/height/flags from the texture struct
		// and pixel data from the pointer set below
		Image_SetMDLPointer( buf + meta->ofs[i] );
		size = sizeof( mstudiotexture_t ) + (size_t)pt[i].width * pt[i].height + 768;
		texnum = ref.dllFuncs.GL_LoadTexture( texname, (byte *)&pt[i], size, flags );

		if( !texnum )
		{
			Con_Printf( S_WARN "%s: couldn't upload texture %s of %s, using default\n", __func__, texname, mod->name );
			texnum = defaulttex;
		}

		pt[i].index = texnum;

		if( colormap && cmap < mod->numtextures && mod->textures )
			mod->textures[cmap++]->gl_texturenum = pt[i].index;
	}

	Mem_Free( buf );

	meta->evicted = false;
	meta->touch = host.realtime;

	Con_Reportf( "%s: re-uploaded %i textures for %s\n", __func__, phdr->numtextures, mod->name );

	return true;
}
#endif // !XASH_DEDICATED

/*
================
Mod_StudioSnapshotTextureOffsets

Called from the studio loader before the renderer uploads the textures,
while ptexture[i].index still holds pixel data offsets relative to the
start of the source file. Remembers them so evicted textures can be
re-uploaded from disk later.
================
*/
void Mod_StudioSnapshotTextureOffsets( model_t *mod, const studiohdr_t *hdr, qboolean fromT )
{
#if !XASH_DEDICATED
	studiotexmeta_t *meta;
	const mstudiotexture_t *pt;
	int slot = mod - mod_known;
	int i;

	if( slot < 0 || slot >= MAX_MODELS )
		return;

	meta = Mem_Calloc( mod->mempool, sizeof( *meta ));
	mod_studiotex[slot] = meta;
	meta->fromT = fromT;
	meta->touch = -1.0f;

	if( hdr->numtextures <= 0 || hdr->textureindex <= 0 )
		return;

	meta->numtextures = hdr->numtextures;
	meta->ofs = Mem_Calloc( mod->mempool, meta->numtextures * sizeof( uint32_t ));
	pt = (const mstudiotexture_t *)((const byte *)hdr + hdr->textureindex);

	for( i = 0; i < meta->numtextures; i++ )
	{
		size_t pixels = (size_t)pt[i].width * pt[i].height;

		meta->ofs[i] = pt[i].index;
		// RGBA upload plus mipmaps, slightly overestimated
		meta->bytes += pixels * 6 + 768;
	}
#endif
}

/*
================
Mod_StudioTexMetaFree

Called from Mod_FreeModel before the model slot is cleared. The metadata
itself lives in the model mempool and dies with it.
================
*/
void Mod_StudioTexMetaFree( model_t *mod )
{
#if !XASH_DEDICATED
	int i = mod - mod_known;

	if( i >= 0 && i < MAX_MODELS )
		mod_studiotex[i] = NULL;
#endif
}

/*
================
Mod_StudioTouch

Marks the studio model as seen this frame. Models whose textures were
evicted get them re-uploaded right away if the per-frame reload budget
allows, otherwise they are queued for background reload.
================
*/
void Mod_StudioTouch( model_t *mod, qboolean visible )
{
#if !XASH_DEDICATED
	studiotexmeta_t *meta;

	if( r_studio_texture_cache.value <= 0.0f || Host_IsDedicated( ))
		return;

	if( !mod || mod->type != mod_studio )
		return;

	meta = Mod_StudioTexMeta( mod );
	if( !meta )
		return;

	meta->touch = host.realtime;

	if( !meta->evicted )
		return;

	if( !visible )
	{
		meta->pending = true;
		return;
	}

	if( host.framecount != mod_studiotex_frame )
	{
		mod_studiotex_frame = host.framecount;
		mod_studiotex_framebytes = 0;
	}

	// always reload the first model of a frame synchronously, so huge
	// models can't starve behind small ones
	if( mod_studiotex_framebytes && mod_studiotex_framebytes + meta->bytes > STUDIO_RELOAD_FRAME_BUDGET )
	{
		meta->pending = true;
		return;
	}

	if( Mod_StudioReloadTextures( mod ))
		mod_studiotex_framebytes += meta->bytes;
#endif
}

/*
================
Mod_StudioTexFrame

Per-client-frame housekeeping: reloads textures of models that were seen
but couldn't be re-uploaded in the same frame, and periodically enforces
the texture budget.
================
*/
void Mod_StudioTexFrame( void )
{
#if !XASH_DEDICATED
	int i;

	if( r_studio_texture_cache.value <= 0.0f || Host_IsDedicated( ))
		return;

	if( host.framecount != mod_studiotex_frame )
	{
		mod_studiotex_frame = host.framecount;
		mod_studiotex_framebytes = 0;
	}

	if( host.realtime - mod_studiotex_lastenforce > 0.5 )
	{
		mod_studiotex_lastenforce = host.realtime;
		Mod_StudioTexBudgetEnforce();
	}

	if( cls.state != ca_active )
		return;

	for( i = 1; i < mod_numknown; i++ )
	{
		model_t *mod = &mod_known[i];
		studiotexmeta_t *meta = mod_studiotex[i];

		if( mod->type != mod_studio || !meta || !meta->evicted || !meta->pending )
			continue;

		if( mod_studiotex_framebytes && mod_studiotex_framebytes + meta->bytes > STUDIO_RELOAD_FRAME_BUDGET )
			return;

		if( Mod_StudioReloadTextures( mod ))
			mod_studiotex_framebytes += meta->bytes;
	}
}

/*
================
Mod_StudioTexList_f

Displays the state of every studio model in the texture cache.
================
*/
static void Mod_StudioTexList_f( void )
{
	size_t total = 0, evicted = 0;
	int nummodels = 0, numevicted = 0;
	int i;

	if( r_studio_texture_cache.value <= 0.0f )
	{
		Con_Printf( "studio texture cache is disabled\n" );
		return;
	}

	for( i = 1; i < mod_numknown; i++ )
	{
		model_t *mod = &mod_known[i];
		studiotexmeta_t *meta = mod_studiotex[i];

		if( mod->type != mod_studio || !meta || meta->bytes == 0 )
			continue;

		nummodels++;
		total += meta->bytes;

		if( meta->evicted )
		{
			evicted += meta->bytes;
			numevicted++;
		}

		Con_Printf( "%s: %s, %u bytes, %i textures, seen %.1fs ago\n",
			mod->name,
			meta->evicted ? ( meta->pending ? "evicted, reload queued" : "evicted" ) : "resident",
			(unsigned)meta->bytes, meta->numtextures,
			meta->touch >= 0.0f ? host.realtime - meta->touch : -1.0f );
	}

	Con_Printf( "budget %g Mb: %i studio models, %u bytes resident (%.1f Mb), %i evicted (%.1f Mb)\n",
		r_studio_texture_cache.value, nummodels,
		(unsigned)( total - evicted ), ( total - evicted ) / 1048576.0,
		numevicted, evicted / 1048576.0 );
}
#endif // !XASH_DEDICATED
}

/*
===============================================================================

			MOD COMMON UTILS

===============================================================================
*/

/*
===============================================================================

			MOD COMMON UTILS

===============================================================================
*/
/*
================
Mod_Modellist_f
================
*/
static void Mod_Modellist_f( void )
{
	int	i, nummodels;
	model_t	*mod;

	Con_Printf( "\n" );
	Con_Printf( "-----------------------------------\n" );

	for( i = nummodels = 0, mod = mod_known; i < mod_numknown; i++, mod++ )
	{
		const char *color_str;

		if( mod->needload == NL_UNREFERENCED )
			continue; // free slot

		switch( mod->type )
		{
		case mod_alias:
			color_str = S_YELLOW;
			break;
		case mod_studio:
			color_str = S_GREEN;
			break;
		case mod_sprite:
			color_str = S_MAGENTA;
			break;
		case mod_brush:
			color_str = mod->name[0] == '*' ? S_CYAN : S_BLUE;
			break;
		default:
			color_str = S_RED;
			break;
		}

		Con_Printf( "%3d:\t%s%s\n" S_DEFAULT, i, color_str, mod->name );
		nummodels++;
	}

	Con_Printf( "-----------------------------------\n" );
	Con_Printf( "%i total models, %i total allocated slots\n", nummodels, mod_numknown );
	Con_Printf( "\n" );
}

static void Mod_UnloadRenderData( model_t *mod )
{
#if !XASH_DEDICATED
	// loader failed before renderer got a chance to process this model
	if( mod->type == mod_bad )
		return;

	switch( mod->type )
	{
	case mod_sprite:
		Mod_SpriteUnloadTextures( mod->cache.data );
		break;
	default:
		break;
	}

	ref.dllFuncs.Mod_ProcessRenderData( mod, false, NULL, 0 );
#endif
}

/*
================
Mod_FreeUserData
================
*/
static void Mod_FreeUserData( model_t *mod )
{
	// ignore submodels and freed models
	if( mod->needload == NL_UNREFERENCED || mod->name[0] == '*' )
		return;

	if( Host_IsDedicated() )
	{
		if( svgame.physFuncs.Mod_ProcessUserData != NULL )
		{
			// let the server.dll free custom data
			svgame.physFuncs.Mod_ProcessUserData( mod, false, NULL );
		}
	}
	else
	{
		Mod_UnloadRenderData( mod );
	}
}

/*
================
Mod_FreeModel
================
*/
void Mod_FreeModel( model_t *mod )
{
	if( mod )
		Mod_StudioTexMetaFree( mod );

	// already freed?
	if( !mod || mod->needload == NL_UNREFERENCED )
		return;

	if( mod->type != mod_brush || mod->name[0] != '*' )
	{
		Mod_FreeUserData( mod );
		Mem_FreePool( &mod->mempool );
	}

	if( mod->type == mod_brush && FBitSet( mod->flags, MODEL_WORLD ) )
	{
		world.version = 0;
		world.shadowdata = NULL;
		world.deluxedata = NULL;

		// data already freed by Mem_FreePool above
		world.hull_models = NULL;
		world.compressed_phs = NULL;
		world.phsofs = NULL;
	}

	memset( mod, 0, sizeof( *mod ));
}

/*
===============================================================================

			MODEL INITIALIZE\SHUTDOWN

===============================================================================
*/
/*
================
Mod_Init
================
*/
void Mod_Init( void )
{
	com_studiocache = Mem_AllocPool( "Studio Cache" );
	Cvar_RegisterVariable( &mod_studiocache );
	Cvar_RegisterVariable( &r_wadtextures );
	Cvar_RegisterVariable( &r_showhull );
	Cvar_RegisterVariable( &r_allow_wad3_luma );
#if !XASH_DEDICATED
	Cvar_RegisterVariable( &r_studio_texture_cache );
	Cmd_AddCommand( "studiotexlist", Mod_StudioTexList_f, "display studio models texture cache state" );
#endif

	Cmd_AddCommand( "mapstats", Mod_PrintWorldStats_f, "show stats for currently loaded map" );
	Cmd_AddCommand( "modellist", Mod_Modellist_f, "display loaded models list" );

	Mod_ResetStudioAPI ();
	Mod_InitStudioHull ();
}

/*
================
Mod_FreeAll
================
*/
void Mod_FreeAll( void )
{
#if !XASH_DEDICATED
	Mod_ReleaseHullPolygons();
#endif
	for( int i = 0; i < mod_numknown; i++ )
		Mod_FreeModel( &mod_known[i] );
	mod_numknown = 0;
}

/*
================
Mod_ClearUserData
================
*/
void Mod_ClearUserData( void )
{
	for( int i = 0; i < mod_numknown; i++ )
		Mod_FreeUserData( &mod_known[i] );
}

/*
================
Mod_Shutdown
================
*/
void Mod_Shutdown( void )
{
	Mod_FreeAll();
	Mem_FreePool( &com_studiocache );
}

/*
===============================================================================

			MODELS MANAGEMENT

===============================================================================
*/
/*
==================
Mod_FindName

never return NULL
==================
*/
model_t *Mod_FindName( const char *filename, qboolean trackCRC )
{
	char	modname[MAX_QPATH];
	model_t	*mod;
	int	i;

	Q_strncpy( modname, filename, sizeof( modname ));

	// search the currently loaded models
	for( i = 0, mod = mod_known; i < mod_numknown; i++, mod++ )
	{
		if( !Q_stricmp( mod->name, modname ))
		{
			if( mod->mempool || mod->name[0] == '*' )
				mod->needload = NL_PRESENT;
			else
				mod->needload = NL_NEEDS_LOADED;

			return mod;
		}
	}

	// find a free model slot spot
	for( i = 0, mod = mod_known; i < mod_numknown; i++, mod++ )
	{
		if( mod->needload == NL_UNREFERENCED )
			break; // this is a valid spot
	}

	if( i == mod_numknown )
	{
		if( mod_numknown == MAX_MODELS )
			Host_Error( "MAX_MODELS limit exceeded (%d)\n", MAX_MODELS );
		mod_numknown++;
	}

	// copy name, so model loader can find model file
	Q_strncpy( mod->name, modname, sizeof( mod->name ));
	if( trackCRC ) mod_crcinfo[i].flags = FCRC_SHOULD_CHECKSUM;
	else mod_crcinfo[i].flags = 0;
	mod->needload = NL_NEEDS_LOADED;
	mod_crcinfo[i].initialCRC = 0;

	return mod;
}

/*
==================
Mod_LoadModel

Loads a model into the cache
==================
*/
static model_t *Mod_LoadModel( model_t *mod, qboolean crash )
{
	char		tempname[MAX_QPATH];
	fs_offset_t		length = 0;
	qboolean		loaded, loaded2 = false;

	if( !mod )
	{
		Host_Error( "%s: mod == NULL\n", __func__ );
		return NULL;
	}

	// check if already loaded (or inline bmodel)
	if( mod->mempool || mod->name[0] == '*' )
	{
		mod->needload = NL_PRESENT;
		return mod;
	}

	if( mod->needload != NL_NEEDS_LOADED )
	{
		Host_Error( "%s: trying to load model not marked for loading (%d)\n", __func__, mod->needload );
		return NULL;
	}

	// store modelname to show error
	Q_strncpy( tempname, mod->name, sizeof( tempname ));
	COM_FixSlashes( tempname );

	byte *buf = FS_LoadFile( tempname, &length, false );

	if( !buf || length < sizeof( uint ))
	{
		memset( mod, 0, sizeof( model_t ));

		if( crash ) Host_Error( "Could not load model %s from disk\n", tempname );
		else Con_Printf( S_ERROR "Could not load model %s from disk\n", tempname );

		return NULL;
	}

	Con_Reportf( "loading %s\n", mod->name );
	mod->needload = NL_PRESENT;
	mod->type = mod_bad;

	// call the apropriate loader
	switch( *(uint *)buf )
	{
	case LittleLong( IDSTUDIOHEADER ):
		Mod_LoadStudioModel( mod, buf, length, &loaded );
		break;
	case LittleLong( IDSPRITEHEADER ):
		Mod_LoadSpriteModel( mod, buf, length, &loaded );
		break;
	case LittleLong( IDALIASHEADER ):
		Mod_LoadAliasModel( mod, buf, &loaded );
		break;
	case LittleLong( Q1BSP_VERSION ):
	case LittleLong( HLBSP_VERSION ):
	case LittleLong( QBSP2_VERSION ):
		Mod_LoadBrushModel( mod, buf, length, &loaded );
		break;
	default:
		Mem_Free( buf );
		if( crash ) Host_Error( "%s has unknown format\n", tempname );
		else Con_Printf( S_ERROR "%s has unknown format\n", tempname );
		return NULL;
	}

	if( loaded )
	{
		if( world.loading )
			SetBits( mod->flags, MODEL_WORLD ); // mark worldmodel

		if( Host_IsDedicated() )
		{
			if( svgame.physFuncs.Mod_ProcessUserData != NULL )
			{
				// let the server.dll load custom data
				svgame.physFuncs.Mod_ProcessUserData( mod, true, buf );
			}
			loaded2 = true;
		}
#if !XASH_DEDICATED
		else
		{
			loaded2 = ref.dllFuncs.Mod_ProcessRenderData( mod, true, buf, length );
		}
#endif
	}

	if( mod->type == mod_alias )
	{
		aliashdr_t *hdr = mod->cache.data;
		if( hdr ) // clean up temporary pointer after passing the alias model to the renderer
			hdr->pposeverts = NULL;
	}

	if( !loaded || !loaded2 )
	{
		Mod_FreeModel( mod );
		Mem_Free( buf );

		if( crash ) Host_Error( "Could not load model %s\n", tempname );
		else Con_Printf( S_ERROR "Could not load model %s\n", tempname );

		return NULL;
	}

	model_info_t *p = &mod_crcinfo[mod - mod_known];
	mod->needload = NL_PRESENT;

#if !XASH_DEDICATED
	// keep the studio texture budget satisfied after every load, so
	// precaching never spikes above the limit
	Mod_StudioTexBudgetEnforce();
#endif

	if( FBitSet( p->flags, FCRC_SHOULD_CHECKSUM ))
	{
		uint32_t currentCRC;

		CRC32_Init( &currentCRC );
		CRC32_ProcessBuffer( &currentCRC, buf, length );
		currentCRC = CRC32_Final( currentCRC );

		if( FBitSet( p->flags, FCRC_CHECKSUM_DONE ))
		{
			if( currentCRC != p->initialCRC )
				Host_Error( "%s has a bad checksum\n", tempname );
		}
		else
		{
			SetBits( p->flags, FCRC_CHECKSUM_DONE );
			p->initialCRC = currentCRC;
		}
	}
	Mem_Free( buf );

	return mod;
}

/*
==================
Mod_ForName

Loads in a model for the given name
==================
*/
model_t *Mod_ForName( const char *name, qboolean crash, qboolean trackCRC )
{
	if( COM_StringEmptyOrNULL( name ))
		return NULL;

	model_t *mod = Mod_FindName( name, trackCRC );
	return Mod_LoadModel( mod, crash );
}

/*
==================
Mod_PurgeStudioCache

free studio cache on change level
==================
*/
static void Mod_PurgeStudioCache( void )
{
	// refresh hull data
	SetBits( r_showhull.flags, FCVAR_CHANGED );
#if !XASH_DEDICATED
	Mod_ReleaseHullPolygons();
#endif
	// release previois map
	Mod_FreeModel( mod_known );	// world is stuck on slot #0 always

	// we should release all the world submodels
	// and clear studio sequences
	for( int i = 1; i < mod_numknown; i++ )
	{
		// Clear studio sequence caches for *every* studio model, even the ones
		// currently unreferenced: the studio cache pool is emptied below and a
		// stale submodels array would dangle and crash Mod_CacheCheck later.
		if( mod_known[i].type == mod_studio )
			mod_known[i].submodels = NULL;

		if( mod_known[i].needload == NL_UNREFERENCED )
			continue;

		if( mod_known[i].name[0] == '*' )
			Mod_FreeModel( &mod_known[i] );
		else
			mod_known[i].needload = NL_FREE_UNUSED;
	}

	Mem_EmptyPool( com_studiocache );
	Mod_ClearStudioCache();
}

/*
==================
Mod_LoadWorld

Loads in the map and all submodels
==================
*/
model_t *Mod_LoadWorld( const char *name, qboolean preload )
{
	// already loaded?
	if( !Q_stricmp( mod_known->name, name ))
		return mod_known;

	// free sequence files on studiomodels
	Mod_PurgeStudioCache();

	// load the newmap
	world.loading = true;
	model_t *pworld = Mod_FindName( name, false );
	if( preload ) Mod_LoadModel( pworld, true );
	world.loading = false;

	ASSERT( pworld == mod_known );

	return pworld;
}

/*
==================
Mod_FreeUnused

Purge all unused models
==================
*/
void Mod_FreeUnused( void )
{
	model_t	*mod;
	int	i;

	// never tries to release worldmodel
	for( i = 1, mod = &mod_known[1]; i < mod_numknown; i++, mod++ )
	{
		if( mod->needload == NL_FREE_UNUSED )
			Mod_FreeModel( mod );
	}
}

/*
===============================================================================

			MODEL ROUTINES

===============================================================================
*/
/*
===============
Mod_Calloc

===============
*/
void *GAME_EXPORT Mod_Calloc( int number, size_t size )
{
	if( number <= 0 || size <= 0 )
		return NULL;

	cache_user_t *cu = (cache_user_t *)Mem_Calloc( com_studiocache, sizeof( cache_user_t ) + number * size );
	cu->data = (void *)cu; // make sure that cu->data is not NULL

	return cu;
}

/*
===============
Mod_CacheCheck

===============
*/
void *GAME_EXPORT Mod_CacheCheck( cache_user_t *c )
{
	// Never dereference a cache user that isn't within the studio cache pool:
	// the game DLL keeps these pointers across model reloads and they can go
	// stale once the pool is emptied (Mod_EmptyPool in Mod_PurgeStudioCache).
	if( !Mem_IsAllocatedRangeExt( com_studiocache, c, sizeof( *c ) ))
		return NULL;

	if( !c->data )
		return NULL;

	if( !Mem_IsAllocatedExt( com_studiocache, c->data ))
		return NULL;

	return c->data;
}

/*
===============
Mod_LoadCacheFile

===============
*/
void GAME_EXPORT Mod_LoadCacheFile( const char *filename, cache_user_t *cu )
{
	char	modname[MAX_QPATH];
	fs_offset_t	size;

	Assert( cu != NULL );

	if( COM_StringEmptyOrNULL( filename ))
		return;

	// Refuse to write into a stale/corrupt cache user (mirror of Mod_CacheCheck)
	if( !Mem_IsAllocatedRangeExt( com_studiocache, cu, sizeof( *cu ) ))
		return;

	Q_strncpy( modname, filename, sizeof( modname ));
	COM_FixSlashes( modname );

	byte *buf = FS_LoadFile( modname, &size, false );
	if( !buf || !size ) Host_Error( "LoadCacheFile: ^1can't load %s^7\n", filename );
	cu->data = Mem_Malloc( com_studiocache, size );
	memcpy( cu->data, buf, size );
	Mem_Free( buf );

	// this handles when studio model renderer tries to load sequence files on it's own
	// which is what they always do in HLSDK
#if XASH_BIG_ENDIAN
	if( size >= sizeof( int ) && LittleLong( IDSEQGRPHEADER ) == *(uint *)cu->data )
	{
		studiohdr_t *phdr = (studiohdr_t *)REF_GET_PARM( PARM_GET_STUDIO_HDR, 0 );
		if( !phdr )
			return;

		mstudioseqdesc_t *pseq = (mstudioseqdesc_t *)((byte *)phdr + phdr->seqindex );

		for( int i = 0; i < phdr->numseq; i++ )
		{
			if( pseq[i].seqgroup == 0 )
				continue;

			mstudioseqgroup_t *pgrp = (mstudioseqgroup_t *)((byte *)phdr + phdr->seqgroupindex ) + pseq[i].seqgroup;

			// assuming filename passes seqgroup's name
			if( !Q_stricmp( pgrp->name, filename ))
				Mod_SwapStudioSeqGroupAnims( phdr, &pseq[i], (byte *)cu->data );
		}
	}
#endif
}

/*
==================
Mod_ValidateCRC

==================
*/
qboolean Mod_ValidateCRC( const char *name, uint32_t crc )
{
	model_t *mod = Mod_FindName( name, true );
	model_info_t *p = &mod_crcinfo[mod - mod_known];

	if( !FBitSet( p->flags, FCRC_CHECKSUM_DONE ))
		return true;
	if( p->initialCRC == crc )
		return true;
	return false;
}

/*
==================
Mod_NeedCRC

==================
*/
void Mod_NeedCRC( const char *name, qboolean needCRC )
{
	model_t *mod = Mod_FindName( name, true );
	model_info_t *p = &mod_crcinfo[mod - mod_known];

	if( needCRC ) SetBits( p->flags, FCRC_SHOULD_CHECKSUM );
	else ClearBits( p->flags, FCRC_SHOULD_CHECKSUM );
}

#if XASH_LLVM_LIBFUZZER

static const uint8_t *fuzz_data;
static size_t fuzz_size;

static byte *Fuzz_LoadFile( const char *path, fs_offset_t *filesizeptr, qboolean gamedironly )
{
	byte *buf = Mem_Malloc( host.mempool, fuzz_size );
	memcpy( buf, fuzz_data, fuzz_size );
	*filesizeptr = fuzz_size;
	return buf;
}

int EXPORT Fuzz_Mod_LoadModel( const uint8_t *Data, size_t Size );
int EXPORT Fuzz_Mod_LoadModel( const uint8_t *Data, size_t Size )
{
	model_t mod = { .name = "test", .needload = NL_NEEDS_LOADED };

	Memory_Init();

	host.type = HOST_DEDICATED;
	host.mempool = Mem_AllocPool( "fuzzing pool" );
	fuzz_data = Data;
	fuzz_size = Size;
	refState.draw_surfaces = NULL;

	g_fsapi.LoadFile = Fuzz_LoadFile;

	if( Mod_LoadModel( &mod, false ) && mod.mempool )
		Mem_FreePool( &mod.mempool );

	Mem_FreePool( &host.mempool );

	return 0;
}

#endif // XASH_LLVM_LIBFUZZER
