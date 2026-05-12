#include "common.h"
#include "client.h"
#include "cl_particleman.h"
#include "ref_common.h"
#include "triangleapi.h"
#include "r_efx.h"
#include "pm_defs.h"
#include "event_api.h"
#include "cl_tent.h"
#include "com_model.h"

typedef enum
{
	EMITTER_NONE = 0,
	EMITTER_RAIN,
	EMITTER_SNOW,
	EMITTER_SMOKER,
} pm_emitter_type_t;

typedef enum
{
	PM_PARTICLE_NONE = 0,
	PM_PARTICLE_RAIN,
	PM_PARTICLE_SNOW,
} pm_particle_type_t;

#define MAX_PM_EMITTERS 64
#define MAX_PM_PARTICLES 768

typedef struct
{
	pm_emitter_type_t type;
	vec3_t origin;
	vec3_t angles;
	vec3_t color;
	int spawnflags;
	int dripSpeed;
	int dripSize;
	int brightness;
	int burstSize;
	int health;
	float updateTime;
	float nextEmitTime;
	float spread;
	float scale;
	qboolean active;
} pm_emitter_t;

typedef struct
{
	pm_particle_type_t type;
	vec3_t origin;
	vec3_t velocity;
	vec3_t impact;
	vec3_t color;
	float dieTime;
	float alpha;
	float size;
	qboolean active;
} pm_particle_t;

typedef struct
{
	qboolean active;
	qboolean affectSky;
	vec3_t color;
	float startDist;
	float endDist;
	float density;
} pm_fog_t;

static qboolean pm_enabled = false;
static qboolean pm_initialized = false;
static pm_emitter_t pm_emitters[MAX_PM_EMITTERS];
static int pm_numEmitters = 0;
static pm_particle_t pm_particles[MAX_PM_PARTICLES];
static pm_fog_t pm_fog;
static int pm_iSmokeModelIndex = 0;
static double pm_lastFrameTime = 0.0;
static uint pm_worldmapCRC = 0;

static void CL_ParticleMan_ClearRuntime( void )
{
	int i;

	for( i = 0; i < MAX_PM_PARTICLES; ++i )
		pm_particles[i].active = false;

	for( i = 0; i < MAX_PM_EMITTERS; ++i )
		pm_emitters[i].active = false;

	pm_numEmitters = 0;
	memset( &pm_fog, 0, sizeof( pm_fog ));
}

static void CL_ParticleMan_ParseVector( const char *value, vec_t *out )
{
	float x = 0.0f, y = 0.0f, z = 0.0f;
	if( value )
		sscanf( value, "%f %f %f", &x, &y, &z );
	out[0] = x;
	out[1] = y;
	out[2] = z;
}

static void CL_ParticleMan_ParseColor( const char *value, vec_t *out )
{
	CL_ParticleMan_ParseVector( value, out );
	out[0] /= 255.0f;
	out[1] /= 255.0f;
	out[2] /= 255.0f;
}

static qboolean CL_ParticleMan_IsSkyTexture( const char *textureName )
{
	if( !textureName || !textureName[0] )
		return false;

	return !Q_strnicmp( textureName, "sky", 3 ) || !Q_strnicmp( textureName, "skycull", 7 );
}

static void CL_ParticleMan_SetDefaultEmitterValues( pm_emitter_t *emitter )
{
	emitter->type = EMITTER_NONE;
	VectorClear( emitter->origin );
	VectorClear( emitter->angles );
	VectorSet( emitter->color, 1.0f, 1.0f, 1.0f );
	emitter->spawnflags = 0;
	emitter->dripSpeed = 900;
	emitter->dripSize = 12;
	emitter->brightness = 180;
	emitter->burstSize = 8;
	emitter->health = 0;
	emitter->updateTime = 0.05f;
	emitter->nextEmitTime = 0.0f;
	emitter->spread = 96.0f;
	emitter->scale = 1.0f;
	emitter->active = true;
}

static void CL_ParticleMan_AddEmitter( const pm_emitter_t *emitter )
{
	if( pm_numEmitters >= MAX_PM_EMITTERS )
		return;

	pm_emitters[pm_numEmitters++] = *emitter;
}

static void CL_ParticleMan_ParseEntityBlock( const char *classname, const vec3_t origin, const vec3_t angles, const vec3_t color,
	int spawnflags, int dripSpeed, int dripSize, int brightness, int burstSize, float updateTime, float scale,
	float spread, int health, float fogStart, float fogEnd, float fogDensity )
{
	pm_emitter_t emitter;

	if( !Q_stricmp( classname, "env_fog" ))
	{
		pm_fog.active = true;
		VectorCopy( color, pm_fog.color );
		pm_fog.startDist = fogStart;
		pm_fog.endDist = fogEnd;
		pm_fog.density = fogDensity > 0.0f ? fogDensity / 1000.0f : 0.0f;
		pm_fog.affectSky = ( spawnflags & 1 ) != 0;
		return;
	}

	CL_ParticleMan_SetDefaultEmitterValues( &emitter );
	VectorCopy( origin, emitter.origin );
	VectorCopy( angles, emitter.angles );
	VectorCopy( color, emitter.color );
	emitter.spawnflags = spawnflags;
	emitter.dripSpeed = dripSpeed > 0 ? dripSpeed : emitter.dripSpeed;
	emitter.dripSize = dripSize > 0 ? dripSize : emitter.dripSize;
	emitter.brightness = brightness > 0 ? brightness : emitter.brightness;
	emitter.burstSize = burstSize >= 0 ? burstSize : emitter.burstSize;
	emitter.updateTime = updateTime > 0.0f ? updateTime : emitter.updateTime;
	emitter.scale = scale > 0.0f ? scale : emitter.scale;
	emitter.spread = spread > 0.0f ? spread : emitter.spread;
	emitter.health = health;

	if( !Q_stricmp( classname, "env_rain" ) || !Q_stricmp( classname, "func_rain" ))
	{
		emitter.type = EMITTER_RAIN;
		if( VectorLength( emitter.color ) <= 0.0f )
			VectorSet( emitter.color, 0.72f, 0.78f, 0.82f );
		CL_ParticleMan_AddEmitter( &emitter );
	}
	else if( !Q_stricmp( classname, "env_snow" ) || !Q_stricmp( classname, "func_snow" ))
	{
		emitter.type = EMITTER_SNOW;
		emitter.dripSpeed = dripSpeed > 0 ? dripSpeed : 220;
		emitter.dripSize = dripSize > 0 ? dripSize : 5;
		VectorSet( emitter.color, 0.96f, 0.96f, 0.96f );
		CL_ParticleMan_AddEmitter( &emitter );
	}
	else if( !Q_stricmp( classname, "env_smoker" ))
	{
		emitter.type = EMITTER_SMOKER;
		emitter.scale = scale > 0.0f ? scale * 0.1f : 1.0f;
		emitter.spread = spread >= 0.0f ? spread : 0.0f;
		CL_ParticleMan_AddEmitter( &emitter );
	}
}

static void CL_ParticleMan_LoadMapEntities( void )
{
	char *entityCursor;

	if( !cl.worldmodel || !cl.worldmodel->entities )
		return;

	if( pm_worldmapCRC == cl.worldmapCRC )
		return;

	CL_ParticleMan_ClearRuntime();
	pm_worldmapCRC = cl.worldmapCRC;

	entityCursor = cl.worldmodel->entities;

	while(( entityCursor = COM_Parse( entityCursor )) != NULL )
	{
		char classname[64] = "";
		vec3_t origin = { 0, 0, 0 };
		vec3_t angles = { 0, 0, 0 };
		vec3_t color = { 0, 0, 0 };
		int spawnflags = 0;
		int dripSpeed = 0;
		int dripSize = 0;
		int brightness = 0;
		int burstSize = -1;
		int health = 0;
		float updateTime = 0.0f;
		float scale = 0.0f;
		float spread = -1.0f;
		float fogStart = 0.0f;
		float fogEnd = 0.0f;
		float fogDensity = 0.0f;

		if( com_token[0] != '{' )
			continue;

		while(( entityCursor = COM_Parse( entityCursor )) != NULL )
		{
			char key[256];
			char value[1024];

			if( com_token[0] == '}' )
				break;

			Q_strcpy( key, com_token );
			entityCursor = COM_Parse( entityCursor );
			if( !entityCursor || com_token[0] == '}' )
				break;

			Q_strcpy( value, com_token );

			if( !Q_stricmp( key, "classname" ))
			{
				Q_strncpy( classname, value, sizeof( classname ) - 1 );
				classname[sizeof( classname ) - 1] = '\0';
			}
			else if( !Q_stricmp( key, "origin" ))
				CL_ParticleMan_ParseVector( value, origin );
			else if( !Q_stricmp( key, "angles" ))
				CL_ParticleMan_ParseVector( value, angles );
			else if( !Q_stricmp( key, "rendercolor" ))
				CL_ParticleMan_ParseColor( value, color );
			else if( !Q_stricmp( key, "spawnflags" ))
				spawnflags = Q_atoi( value );
			else if( !Q_stricmp( key, "m_dripSpeed" ))
				dripSpeed = Q_atoi( value );
			else if( !Q_stricmp( key, "m_dripSize" ))
				dripSize = Q_atoi( value );
			else if( !Q_stricmp( key, "m_brightness" ))
				brightness = Q_atoi( value );
			else if( !Q_stricmp( key, "m_burstSize" ))
				burstSize = Q_atoi( value );
			else if( !Q_stricmp( key, "m_flUpdateTime" ))
				updateTime = (float)Q_atof( value );
			else if( !Q_stricmp( key, "scale" ))
				scale = (float)Q_atof( value );
			else if( !Q_stricmp( key, "dmg" ))
				spread = (float)Q_atof( value );
			else if( !Q_stricmp( key, "health" ))
				health = Q_atoi( value );
			else if( !Q_stricmp( key, "startdist" ) || !Q_stricmp( key, "fogStartDistance" ))
				fogStart = (float)Q_atof( value );
			else if( !Q_stricmp( key, "enddist" ) || !Q_stricmp( key, "fogStopDistance" ))
				fogEnd = (float)Q_atof( value );
			else if( !Q_stricmp( key, "density" ))
				fogDensity = (float)Q_atof( value );
		}

		if( classname[0] )
		{
			CL_ParticleMan_ParseEntityBlock( classname, origin, angles, color, spawnflags, dripSpeed, dripSize,
				brightness, burstSize, updateTime, scale, spread, health, fogStart, fogEnd, fogDensity );
		}
	}
}

static pm_particle_t *CL_ParticleMan_AllocParticle( void )
{
	int i;

	for( i = 0; i < MAX_PM_PARTICLES; ++i )
	{
		if( !pm_particles[i].active )
		{
			pm_particles[i].active = true;
			return &pm_particles[i];
		}
	}

	return NULL;
}

static void CL_ParticleMan_EmitSmoke( const pm_emitter_t *emitter )
{
	vec3_t pos;
	vec3_t dir;
	float scale;
	int renderMode;
	int flags;

	if( pm_iSmokeModelIndex <= 0 )
		return;

	VectorCopy( emitter->origin, pos );
	pos[0] += COM_RandomFloat( -emitter->spread, emitter->spread );
	pos[1] += COM_RandomFloat( -emitter->spread, emitter->spread );
	VectorSet( dir, COM_RandomFloat( -10.0f, 10.0f ), COM_RandomFloat( -10.0f, 10.0f ), COM_RandomFloat( 18.0f, 32.0f ));
	scale = emitter->scale > 0.0f ? emitter->scale : 1.0f;
	renderMode = kRenderTransAlpha;
	flags = FTENT_FADEOUT | FTENT_SLOWGRAVITY | FTENT_SPRANIMATE;

	R_TempSprite( pos, dir, scale, pm_iSmokeModelIndex, renderMode, kRenderFxNone, 0.75f, 1.5f, flags );
}

static void CL_ParticleMan_ComputeFallDirection( const pm_emitter_t *emitter, vec3_t dir )
{
	vec3_t forward, right, up;

	AngleVectors( emitter->angles, forward, right, up );

	VectorCopy( forward, dir );
	if( VectorLength( dir ) < 0.001f )
		VectorSet( dir, 0.0f, 0.0f, -1.0f );

	if( dir[2] > -0.2f )
	{
		vec3_t planar;
		VectorSet( planar, dir[0], dir[1], 0.0f );
		if( VectorLength( planar ) > 0.001f )
			VectorNormalize( planar );
		VectorScale( planar, 0.35f, dir );
		dir[2] -= 0.94f;
	}

	VectorNormalize( dir );
}

static qboolean CL_ParticleMan_FindOutdoorSample( vec3_t skyPos, vec3_t impactPos )
{
	pmtrace_t trSky;
	pmtrace_t trGround;
	vec3_t sampleBase;
	vec3_t traceStart;
	vec3_t traceEnd;
	const char *textureName;
	int attempts;

	for( attempts = 0; attempts < 6; ++attempts )
	{
		VectorCopy( cl.simorg, sampleBase );
		sampleBase[0] += COM_RandomFloat( -384.0f, 384.0f );
		sampleBase[1] += COM_RandomFloat( -384.0f, 384.0f );

		VectorCopy( sampleBase, traceStart );
		traceStart[2] += 2048.0f;
		VectorCopy( sampleBase, traceEnd );
		traceEnd[2] -= 64.0f;

		trSky = CL_TraceLine( traceStart, traceEnd, PM_WORLD_ONLY );
		textureName = PM_CL_TraceTexture( trSky.ent, traceStart, traceEnd );

		if( !CL_ParticleMan_IsSkyTexture( textureName ))
			continue;

		VectorCopy( trSky.endpos, skyPos );

		VectorCopy( skyPos, traceStart );
		traceStart[2] -= 4.0f;
		VectorCopy( traceStart, traceEnd );
		traceEnd[2] -= 4096.0f;
		trGround = CL_TraceLine( traceStart, traceEnd, PM_WORLD_ONLY );

		if( trGround.fraction >= 1.0f )
			continue;

		VectorCopy( trGround.endpos, impactPos );
		return true;
	}

	return false;
}

static void CL_ParticleMan_SpawnWeatherParticle( const pm_emitter_t *emitter, double time )
{
	pm_particle_t *particle;
	vec3_t skyPos, impactPos;
	vec3_t dir;
	float speed;
	float distance;

	if( !CL_ParticleMan_FindOutdoorSample( skyPos, impactPos ))
		return;

	particle = CL_ParticleMan_AllocParticle();
	if( !particle )
		return;

	CL_ParticleMan_ComputeFallDirection( emitter, dir );
	speed = (float)emitter->dripSpeed;
	if( speed < 128.0f )
		speed = emitter->type == EMITTER_RAIN ? 900.0f : 220.0f;

	particle->type = emitter->type == EMITTER_RAIN ? PM_PARTICLE_RAIN : PM_PARTICLE_SNOW;
	VectorCopy( skyPos, particle->origin );
	particle->origin[2] -= 2.0f;
	VectorCopy( impactPos, particle->impact );
	VectorScale( dir, speed, particle->velocity );
	VectorCopy( emitter->color, particle->color );
	particle->alpha = emitter->brightness > 0 ? (float)emitter->brightness / 255.0f : 0.8f;
	particle->size = emitter->dripSize > 0 ? (float)emitter->dripSize : ( emitter->type == EMITTER_RAIN ? 12.0f : 4.0f );

	distance = VectorDistance( particle->origin, particle->impact );
	if( particle->velocity[2] > -1.0f )
		particle->velocity[2] = -Q_max( speed, 128.0f );

	particle->dieTime = (float)time + Q_max( 0.05f, distance / fabsf( particle->velocity[2] ));
}

static void CL_ParticleMan_SpawnImpact( const pm_particle_t *particle )
{
	vec3_t impactPos;

	VectorCopy( particle->impact, impactPos );
	impactPos[2] += 1.0f;

	if( particle->type == PM_PARTICLE_RAIN )
	{
		R_ParticleBurst( impactPos, 3, 8, 0.18f );
	}
	else if( particle->type == PM_PARTICLE_SNOW )
	{
		// CS:CZ style snow mark - white mark on ground
		// We use a small white burst for now
		R_ParticleBurst( impactPos, 2, 0, 0.20f ); // color 0 is white in default palette
	}
}

static void CL_ParticleMan_UpdateParticles( double time, float frametime )
{
	int i;

	for( i = 0; i < MAX_PM_PARTICLES; ++i )
	{
		pm_particle_t *particle = &pm_particles[i];

		if( !particle->active )
			continue;

		if( time >= particle->dieTime )
		{
			CL_ParticleMan_SpawnImpact( particle );
			particle->active = false;
			continue;
		}

		VectorMA( particle->origin, frametime, particle->velocity, particle->origin );

		if( particle->type == PM_PARTICLE_SNOW )
		{
			particle->origin[0] += COM_RandomFloat( -12.0f, 12.0f ) * frametime;
			particle->origin[1] += COM_RandomFloat( -12.0f, 12.0f ) * frametime;
		}
	}
}

static void CL_ParticleMan_UpdateEmitters( double time )
{
	int i;
	int emitCount;

	for( i = 0; i < pm_numEmitters; ++i )
	{
		pm_emitter_t *emitter = &pm_emitters[i];

		if( !emitter->active || ( emitter->spawnflags & 1 ))
			continue;

		if( time < emitter->nextEmitTime )
			continue;

		if( emitter->type == EMITTER_SMOKER )
		{
			CL_ParticleMan_EmitSmoke( emitter );
			emitter->nextEmitTime = (float)time + COM_RandomFloat( 0.10f, 0.20f );
			continue;
		}

		emitCount = emitter->burstSize > 0 ? emitter->burstSize : 8;
		if( emitCount > 32 )
			emitCount = 32;

		while( emitCount-- > 0 )
			CL_ParticleMan_SpawnWeatherParticle( emitter, time );

		emitter->nextEmitTime = (float)time + Q_max( emitter->updateTime, 0.03f );
	}
}

static void CL_ParticleMan_DrawRainParticle( const pm_particle_t *particle )
{
	vec3_t tail;
	vec3_t dir;
	float length;

	VectorCopy( particle->velocity, dir );
	VectorNormalize( dir );
	length = particle->size > 0.0f ? particle->size : 12.0f;
	VectorMA( particle->origin, -length, dir, tail );

	gTriApi.Color4f( particle->color[0], particle->color[1], particle->color[2], particle->alpha );
	gTriApi.Vertex3f( particle->origin[0], particle->origin[1], particle->origin[2] );
	gTriApi.Vertex3f( tail[0], tail[1], tail[2] );
}

static void CL_ParticleMan_DrawSnowParticle( const pm_particle_t *particle )
{
	vec3_t forward, right, up;
	vec3_t p1, p2, p3, p4;
	float size;

	size = particle->size > 0.0f ? particle->size * 0.35f : 1.5f;
	AngleVectors( cl.viewangles, forward, right, up );
	VectorScale( right, size, right );
	VectorScale( up, size, up );

	VectorSubtract( particle->origin, right, p1 );
	VectorAdd( particle->origin, right, p2 );
	VectorSubtract( particle->origin, up, p3 );
	VectorAdd( particle->origin, up, p4 );

	gTriApi.Color4f( particle->color[0], particle->color[1], particle->color[2], particle->alpha );
	gTriApi.Vertex3f( p1[0], p1[1], p1[2] );
	gTriApi.Vertex3f( p2[0], p2[1], p2[2] );
	gTriApi.Vertex3f( p3[0], p3[1], p3[2] );
	gTriApi.Vertex3f( p4[0], p4[1], p4[2] );
}

void CL_ParticleMan_Init( void )
{
	pm_enabled = Sys_CheckParm( "-particleman" ) != 0;
	pm_initialized = true;
	pm_lastFrameTime = 0.0;
	pm_worldmapCRC = 0;

	CL_ParticleMan_ClearRuntime();

	if( !pm_enabled )
		return;

	CL_LoadModel( "sprites/steam1.spr", &pm_iSmokeModelIndex );
}

void CL_ParticleMan_Reset( void )
{
	pm_lastFrameTime = 0.0;
	pm_worldmapCRC = 0;
	CL_ParticleMan_ClearRuntime();
}

void CL_ParticleMan_Frame( double time )
{
	float frametime;

	if( !pm_initialized || !pm_enabled )
		return;

	CL_ParticleMan_LoadMapEntities();

	if( pm_lastFrameTime == 0.0 )
	{
		pm_lastFrameTime = time;
		return;
	}

	frametime = (float)( time - pm_lastFrameTime );
	if( frametime < 0.0f )
		frametime = 0.0f;
	if( frametime > 0.1f )
		frametime = 0.1f;

	CL_PushPMStates();
	CL_SetSolidPlayers( -1 );

	CL_ParticleMan_UpdateEmitters( time );
	CL_ParticleMan_UpdateParticles( time, frametime );

	CL_PopPMStates();
	pm_lastFrameTime = time;
}

void CL_ParticleMan_Draw( qboolean fTrans )
{
	int i;

	if( !pm_initialized || !pm_enabled )
		return;

	if( !fTrans )
	{
		if( pm_fog.active )
		{
			gTriApi.Fog( pm_fog.color, pm_fog.startDist, pm_fog.endDist, true );
			gTriApi.FogParams( pm_fog.density, pm_fog.affectSky ? 1 : 0 );
		}
		else
		{
			vec3_t noFog = { 0.0f, 0.0f, 0.0f };
			gTriApi.Fog( noFog, 0.0f, 0.0f, false );
			gTriApi.FogParams( 0.0f, 0 );
		}
		return;
	}

	gTriApi.RenderMode( kRenderTransAdd );
	gTriApi.CullFace( TRI_NONE );

	gTriApi.Begin( TRI_LINES );
	for( i = 0; i < MAX_PM_PARTICLES; ++i )
	{
		if( !pm_particles[i].active || pm_particles[i].type != PM_PARTICLE_RAIN )
			continue;

		CL_ParticleMan_DrawRainParticle( &pm_particles[i] );
	}
	gTriApi.End();

	gTriApi.Begin( TRI_LINES );
	for( i = 0; i < MAX_PM_PARTICLES; ++i )
	{
		if( !pm_particles[i].active || pm_particles[i].type != PM_PARTICLE_SNOW )
			continue;

		CL_ParticleMan_DrawSnowParticle( &pm_particles[i] );
	}
	gTriApi.End();
}

