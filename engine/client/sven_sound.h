#ifndef SVEN_SOUND_H
#define SVEN_SOUND_H

#define SVEN_SOUND_PATH 260
#define SVEN_SOUND_COUNT 4096
#define SVEN_SENTENCE_COUNT 2048

enum
{
	SVEN_SND_VOLUME = 0x0001,
	SVEN_SND_PITCH = 0x0002,
	SVEN_SND_ATTN = 0x0004,
	SVEN_SND_ORIGIN = 0x0008,
	SVEN_SND_INDEX = 0x0010,
	SVEN_SND_STOP = 0x0020,
	SVEN_SND_CHANGE_VOL = 0x0040,
	SVEN_SND_CHANGE_PITCH = 0x0080,
	SVEN_SND_SENTENCE = 0x0100,
	SVEN_SND_LOOP_ONLY = 0x0200,
	SVEN_SND_SINGLE = 0x0400,
	SVEN_SND_LOOP = 0x0800,
	SVEN_SND_LINEAR = 0x1000,
	SVEN_SND_USE_ENTITY = 0x2000,
	SVEN_SND_NO_REVERB = 0x4000,
	SVEN_SND_OFFSET = 0x8000
};

typedef struct sven_sound_event_s
{
	int flags;
	int index;
	int channel;
	int entity;
	int pitch;
	float volume;
	float attenuation;
	float offset;
	vec3_t origin;
} sven_sound_event_t;

void CL_SvenSoundCache_Init( void );
void CL_SvenSoundCacheReset( void );
void CL_SvenSoundCacheInvalidate( void );
void CL_SvenSoundFrame( void );
void CL_SvenSoundCacheOnDownload( const char *filename );
void CL_ParseSvenStartSound( const char *name, int size, void *buffer );
const char *CL_SvenSoundName( int index );
qboolean CL_SvenSoundActive( void );
const char *CL_SvenSentenceText( int index );
void S_StartSvenSound( const sven_sound_event_t *event, sound_t handle );

#endif
