/*
 * HL2-inspired water shader for xash3d-fwgs (gl4es / GLES 2.0).
 *
 * Adapted from fteqw's HL2 water shader (fteqw/plugins/hl2/glsl/vmt/water.glsl
 * and fteqw/engine/shaders/glsl/altwater.glsl).
 *
 * Simplified for FBO-less rendering: reflection is faked via sky color,
 * refraction is faked via water color with depth-based tinting.
 * Features two-layer scrolling normalmap animation, Schlick Fresnel,
 * specular highlights, diffuse warp texture overlay, and fog.
 */

#ifndef GL_WATERSHADER_H
#define GL_WATERSHADER_H

#include "gl_local.h"

typedef struct
{
	GLuint  program;
	GLuint  vertShader;
	GLuint  fragShader;

	/* engine uniforms */
	GLint   u_modelView;
	GLint   u_projection;
	GLint   u_normalMap;
	GLint   u_waterTex;
	GLint   u_cameraPos;
	GLint   u_time;
	GLint   u_fresnelFactor;
	GLint   u_fogColor;
	GLint   u_fogStart;
	GLint   u_fogEnd;
	GLint   u_fogEnabled;

	/* above-water uniforms */
	GLint   u_waterColor;
	GLint   u_alpha;
	GLint   u_density;
	GLint   u_specular;
	GLint   u_specularColor;
	GLint   u_skyColor;
	GLint   u_skyblend;
	GLint   u_fogBlend;
	GLint   u_sunDir;
	GLint   u_sunlightScattering;

	/* underwater uniforms */
	GLint   u_underwaterColor;
	GLint   u_underwaterAlpha;
	GLint   u_underwaterDensity;

	/* vertex uniforms */
	GLint   u_waveheight;
	GLint   u_wavefreq;

	/* attributes */
	GLint   a_position;
	GLint   a_texCoord;
} gl_water_program_t;

typedef struct
{
	int                 initialized;
	int                 shaderSupport;
	gl_water_program_t  programAboveWater;
	gl_water_program_t  programUnderwater;
	GLuint              normalTexture;
} gl_water_shader_state_t;

extern convar_t r_water_shader;
extern convar_t r_water_alpha;
extern convar_t r_water_density;
extern convar_t r_water_specular;
extern convar_t r_water_specular_color_r;
extern convar_t r_water_specular_color_g;
extern convar_t r_water_specular_color_b;
extern convar_t r_water_skyblend;
extern convar_t r_water_skycolor_r;
extern convar_t r_water_skycolor_g;
extern convar_t r_water_skycolor_b;
extern convar_t r_water_fresnel;
extern convar_t r_water_fogblend;
extern convar_t r_water_color_r;
extern convar_t r_water_color_g;
extern convar_t r_water_color_b;
extern convar_t r_water_underwater_alpha;
extern convar_t r_water_underwater_color_r;
extern convar_t r_water_underwater_color_g;
extern convar_t r_water_underwater_color_b;
extern convar_t r_water_underwater_density;
extern convar_t r_water_sun_x;
extern convar_t r_water_sun_y;
extern convar_t r_water_sun_z;
extern convar_t r_water_sunlight_scattering;
extern convar_t r_water_debug;
extern convar_t r_water_waveheight;
extern convar_t r_water_wavefreq;

extern gl_water_shader_state_t gWaterShader;

void    R_WaterShader_Init( void );
void    R_WaterShader_Shutdown( void );
void    R_WaterShader_VidInit( void );

qboolean R_WaterShader_EmitPolys( msurface_t *warp );

#endif
