/*
mod_alias.c - alias model loading
Copyright (C) 2010 Uncle Mike
Copyright (C) 2024 Alibek Omarov

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
#include "alias.h"
#if !XASH_DEDICATED
#include "ref_common.h"
#endif // XASH_DEDICATED
#include "mod_local.h"
#include "xash3d_mathlib.h"

static int g_posenum = 0;
static const trivertex_t *g_poseverts[MAXALIASFRAMES];

static const void *Mod_LoadAliasFrame( const daliasframe_t *pdaliasframe, maliasframedesc_t *frame, const aliashdr_t *aliashdr )
{
	daliasframe_t pinframe;
	const trivertex_t *pverts;
	int i;

	memcpy( &pinframe, pdaliasframe, sizeof( pinframe ));

	Q_strncpy( frame->name, pinframe.name, sizeof( frame->name ));
	frame->firstpose = g_posenum;
	frame->numposes = 1;

	for( i = 0; i < 3; i++ )
	{
		frame->bboxmin.v[i] = pinframe.bboxmin.v[i];
		frame->bboxmax.v[i] = pinframe.bboxmax.v[i];
	}

	pverts = (const trivertex_t *)(pdaliasframe + 1);

	g_poseverts[g_posenum] = pverts;
	g_posenum++;

	pverts += aliashdr->numverts;

	return (void *)pverts;
}

static const void *Mod_LoadAliasGroup( const daliasgroup_t *pingroup, maliasframedesc_t *frame, const aliashdr_t *aliashdr )
{
	daliasgroup_t group;
	const daliasinterval_t *pin_intervals;
	const void *ptemp;
	int i, numframes;

	memcpy( &group, pingroup, sizeof( group ));
	frame->firstpose = g_posenum;
	frame->numposes = numframes = group.numframes;

	for( i = 0; i < 3; i++ )
	{
		frame->bboxmin.v[i] = group.bboxmin.v[i];
		frame->bboxmax.v[i] = group.bboxmax.v[i];
	}

	pin_intervals = (const daliasinterval_t *)(pingroup + 1);

	// all the intervals are always equal 0.1 so we don't care about them
	daliasinterval_t interval;
	memcpy( &interval, pin_intervals, sizeof( interval ));
	frame->interval = interval.interval;
	pin_intervals += numframes;
	ptemp = (void *)pin_intervals;

	for( i = 0; i < numframes; i++ )
	{
		g_poseverts[g_posenum] = (const trivertex_t *)((const daliasframe_t *)ptemp + 1);
		ptemp = (const trivertex_t *)g_poseverts[g_posenum] + aliashdr->numverts;
		g_posenum++;
	}

	return ptemp;
}

static void Mod_CalcAliasBounds( model_t *mod, const aliashdr_t *aliashdr )
{
	int	i, j, k;
	float	radius;
	float	dist;
	vec3_t	v;

	ClearBounds( mod->mins, mod->maxs );
	radius = 0.0f;

	// process verts
	for( i = 0; i < aliashdr->numposes; i++ )
	{
		for( j = 0; j < aliashdr->numverts; j++ )
		{
			for( k = 0; k < 3; k++ )
				v[k] = g_poseverts[i][j].v[k] * aliashdr->scale[k] + aliashdr->scale_origin[k];

			AddPointToBounds( v, mod->mins, mod->maxs );
			dist = DotProduct( v, v );

			if( radius < dist )
				radius = dist;
		}
	}

	mod->radius = sqrt( radius );
}

static const void *Mod_LoadAllSkins( model_t *mod, int numskins, const daliasskintype_t *pskintype, const aliashdr_t *aliashdr )
{
	int	i, size;

	if(( numskins < 1 ) || ( numskins > MAX_SKINS ))
		Host_Error( "%s: Invalid # of skins: %d\n", __func__, numskins );

	size = aliashdr->skinwidth * aliashdr->skinheight;

	// just skipping textures, renderer will take care of them later
	for( i = 0; i < numskins; i++ )
	{
		daliasskintype_t skintype;
		memcpy( &skintype, pskintype, sizeof( skintype ));

		if( skintype.type == ALIAS_SKIN_SINGLE )
		{
			const byte *ptexture = (const byte *)pskintype + sizeof( daliasskintype_t );

			pskintype = (const daliasskintype_t *)( ptexture + size );
		}
		else
		{
			daliasskingroup_t skingroup;
			const daliasskingroup_t *pinskingroup = (const daliasskingroup_t *)((const byte *)pskintype + sizeof( daliasskintype_t ));
			memcpy( &skingroup, pinskingroup, sizeof( skingroup ));

			const daliasskininterval_t *pinskinintervals = (const daliasskininterval_t *)((const byte *)pinskingroup + sizeof( daliasskingroup_t ));
			const byte *ptexture = (const byte *)pinskinintervals + skingroup.numskins * sizeof( daliasskininterval_t );

			pskintype = (const daliasskintype_t *)( ptexture + size );
		}
	}

	return pskintype;
}


/*
====================
Mod_LoadAliasModel

load alias model
====================
*/
void Mod_LoadAliasModel( model_t *mod, const void *buffer, qboolean *loaded )
{
	const daliasframetype_t *pframetype;
	const daliasskintype_t *pskintype;
	const dtriangle_t *pintriangles;
	const daliashdr_t *pinmodel;
	const stvert_t *pinstverts;
	aliashdr_t *m_pAliasHeader;
	size_t size;
	char poolname[MAX_VA_STRING];
	int i;

	if( loaded ) *loaded = false;
	pinmodel = (const daliashdr_t *)buffer;
	i = pinmodel->version;

	if( i != ALIAS_VERSION )
	{
		Con_DPrintf( S_ERROR "%s has wrong version number (%i should be %i)\n", mod->name, i, ALIAS_VERSION );
		return;
	}

	if( pinmodel->numverts <= 0 || pinmodel->numtris <= 0 || pinmodel->numframes <= 0 )
		return; // how is it possible to make that?

	Q_snprintf( poolname, sizeof( poolname ), "^2%s^7", mod->name );
	mod->mempool = Mem_AllocPool( poolname );

	size = sizeof( aliashdr_t ) + (pinmodel->numframes - 1) * sizeof( maliasframedesc_t );
	mod->cache.data = m_pAliasHeader = Mem_Calloc( mod->mempool, size );

	// endian-adjust and copy the data, starting with the alias model header
	m_pAliasHeader->numverts = pinmodel->numverts;

	if( m_pAliasHeader->numverts > MAXALIASVERTS )
	{
		Con_DPrintf( S_ERROR "model %s has too many vertices\n", mod->name );
		return;
	}

	mod->flags = pinmodel->flags; // share effects flags
	m_pAliasHeader->boundingradius = pinmodel->boundingradius;
	m_pAliasHeader->numskins = pinmodel->numskins;
	m_pAliasHeader->skinwidth = pinmodel->skinwidth;
	m_pAliasHeader->skinheight = pinmodel->skinheight;
	m_pAliasHeader->numtris = pinmodel->numtris;
	mod->numframes = m_pAliasHeader->numframes = pinmodel->numframes;
	m_pAliasHeader->size = pinmodel->size;

	for( i = 0; i < 3; i++ )
	{
		m_pAliasHeader->scale[i] = pinmodel->scale[i];
		m_pAliasHeader->scale_origin[i] = pinmodel->scale_origin[i];
		m_pAliasHeader->eyeposition[i] = pinmodel->eyeposition[i];
	}

	// load the skins
	pskintype = (const daliasskintype_t *)&pinmodel[1];
	pskintype = Mod_LoadAllSkins( mod, m_pAliasHeader->numskins, pskintype, m_pAliasHeader );
	// will be done at renderer side...

	// load base s and t vertices
	pinstverts = (const stvert_t *)pskintype;
	// will be done at renderer side...

	// load triangle lists
	pintriangles = (const dtriangle_t *)&pinstverts[m_pAliasHeader->numverts];
	// will be done at renderer side

	// load the frames
	pframetype = (const daliasframetype_t *)&pintriangles[m_pAliasHeader->numtris];
	m_pAliasHeader->pposeverts = g_poseverts; // store the pointer to be accessed by renderer
	g_posenum = 0;

	for( i = 0; i < m_pAliasHeader->numframes; i++ )
	{
		daliasframetype_t dframetype;
		memcpy( &dframetype, pframetype, sizeof( dframetype ));

		if( dframetype.type == ALIAS_SINGLE )
			pframetype = (const daliasframetype_t *)Mod_LoadAliasFrame((const daliasframe_t *)((const byte *)pframetype + sizeof( daliasframetype_t )), &m_pAliasHeader->frames[i], m_pAliasHeader );
		else pframetype = (const daliasframetype_t *)Mod_LoadAliasGroup(( const daliasgroup_t *)((const byte *)pframetype + sizeof( daliasframetype_t )), &m_pAliasHeader->frames[i], m_pAliasHeader );
	}

	m_pAliasHeader->numposes = g_posenum;
	Mod_CalcAliasBounds( mod, m_pAliasHeader );
	mod->type = mod_alias;

	if( loaded ) *loaded = true;	// done
}
