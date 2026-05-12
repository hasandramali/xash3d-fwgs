#ifndef CL_PARTICLEMAN_H
#define CL_PARTICLEMAN_H

#include "xash3d_types.h"

void CL_ParticleMan_Init( void );
void CL_ParticleMan_Reset( void );
void CL_ParticleMan_Frame( double time );
void CL_ParticleMan_Draw( qboolean fTrans );

#endif // CL_PARTICLEMAN_H
