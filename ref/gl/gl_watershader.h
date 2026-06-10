/*
 * HL2-style water shader for xash3d-fwgs (gl4es / GLES 2.0).
 *
 * Adapted from fteqw's water.glsl / altwater.glsl with Source-engine-like
 * screen-space refraction: the framebuffer is captured once per frame
 * via pglCopyTexSubImage2D and used as a refraction texture, giving a
 * subtle wobbly/blurry distortion through the water surface.
 *
 * No cubemap or FBO reflection — the Fresnel term blends between the
 * refracted scene and the water body colour for a simple "fake reflection".
 * Underwater surfaces use a dedicated shader with caustics and colour tint.
 */

#ifndef GL_WATERSHADER_H
#define GL_WATERSHADER_H

#include "gl_local.h"

typedef struct
{
	GLuint  program;
	GLuint  vertShader;
	GLuint  fragShader;

	GLint   u_modelView;
	GLint   u_projection;
	GLint   u_normalMap;
	GLint   u_diffuseMap;
	GLint   u_refractMap;
	GLint   u_cameraPos;
	GLint   u_time;
	GLint   u_fresnelExp;
	GLint   u_fresnelMin;
	GLint   u_fresnelRange;
	GLint   u_strengthRefr;
	GLint   u_waterColor;
	GLint   u_waterGamma;
	GLint   u_alpha;
	GLint   u_distScale;
	GLint   u_fogBlend;
	GLint   u_fogColor;
	GLint   u_fogStart;
	GLint   u_fogEnd;
	GLint   u_fogEnabled;
	GLint   u_waveheight;
	GLint   u_wavefreq;
	GLint   u_waveSpeed;
	GLint   u_refractionSpeed;
	GLint   u_refractEnabled;

	GLint   a_position;
	GLint   a_texCoord;
} gl_water_program_t;

typedef struct
{
	int                 initialized;
	int                 shaderSupport;
	int                 framebufferWidth;
	int                 framebufferHeight;
	gl_water_program_t  program;
	gl_water_program_t  programUnderwater;
	GLuint              normalTexture;
	GLuint              screenGrabTexture;
	int                 lastFrameCaptured;
} gl_water_shader_state_t;

extern convar_t r_water_shader;
extern convar_t r_water_alpha;
extern convar_t r_water_fresnel;
extern convar_t r_water_fresnel_min;
extern convar_t r_water_fresnel_range;
extern convar_t r_water_strength;
extern convar_t r_water_watercolor_r;
extern convar_t r_water_watercolor_g;
extern convar_t r_water_watercolor_b;
extern convar_t r_water_distscale;
extern convar_t r_water_fogblend;
extern convar_t r_water_debug;
extern convar_t r_water_waveheight;
extern convar_t r_water_wavefreq;
extern convar_t r_water_wavespeed;
extern convar_t r_water_refraction_speed;
extern convar_t r_water_gamma;

extern gl_water_shader_state_t gWaterShader;

void    R_WaterShader_Init( void );
void    R_WaterShader_Shutdown( void );
void    R_WaterShader_VidInit( void );
qboolean R_WaterShader_EmitPolys( msurface_t *warp );

#endif
