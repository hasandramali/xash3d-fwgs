/*
 * HL2 water shader port for xash3d-fwgs (gl4es / GLES 2.0).
 *
 * Ported from fteqw's HL2 water shader:
 *   fteqw/plugins/hl2/glsl/vmt/water.glsl
 *   fteqw/engine/shaders/glsl/altwater.glsl
 *
 * Features:
 *   - real screen-space refraction (pglCopyTexSubImage2D framebuffer grab)
 *   - cubemap-based reflection (generated from sky color)
 *   - two-layer scrolling normalmap animation
 *   - Q1-style texture coordinate warping
 *   - Schlick Fresnel (view-dependent reflect/refract blend)
 *   - optional wave displacement (vertex shader)
 *   - diffuse warp-texture overlay
 *   - above-water and underwater programs
 */

#ifndef GL_WATERSHADER_H
#define GL_WATERSHADER_H

#include "gl_local.h"

#define WATER_CUBEMAP_SIZE  16

typedef struct
{
	GLuint  program;
	GLuint  vertShader;
	GLuint  fragShader;

	/* common uniforms */
	GLint   u_modelView;
	GLint   u_projection;
	GLint   u_normalMap;
	GLint   u_diffuseMap;
	GLint   u_refractMap;
	GLint   u_reflectCube;
	GLint   u_cameraPos;
	GLint   u_time;
	GLint   u_fresnelExp;
	GLint   u_strengthRefr;
	GLint   u_tintRefr;
	GLint   u_tintRefl;
	GLint   u_alpha;
	GLint   u_distScale;
	GLint   u_fogBlend;
	GLint   u_fogColor;
	GLint   u_fogStart;
	GLint   u_fogEnd;
	GLint   u_fogEnabled;
	GLint   u_waveheight;
	GLint   u_wavefreq;
	GLint   u_refractEnabled;

	/* underwater uniforms */
	GLint   u_uwColor;
	GLint   u_uwAlpha;
	GLint   u_uwDensity;
	GLint   u_uwScattering;

	/* attributes */
	GLint   a_position;
	GLint   a_texCoord;
} gl_water_program_t;

typedef struct
{
	int                 initialized;
	int                 shaderSupport;
	int                 framebufferWidth;
	int                 framebufferHeight;
	gl_water_program_t  programAbove;
	gl_water_program_t  programUnder;
	GLuint              normalTexture;
	GLuint              screenGrabTexture;
	GLuint              reflectCubemap;
	int                 lastFrameCaptured;
} gl_water_shader_state_t;

extern convar_t r_water_shader;
extern convar_t r_water_alpha;
extern convar_t r_water_fresnel;
extern convar_t r_water_strength;
extern convar_t r_water_skyblend;
extern convar_t r_water_skycolor_r;
extern convar_t r_water_skycolor_g;
extern convar_t r_water_skycolor_b;
extern convar_t r_water_tintrefr_r;
extern convar_t r_water_tintrefr_g;
extern convar_t r_water_tintrefr_b;
extern convar_t r_water_tintrefl_r;
extern convar_t r_water_tintrefl_g;
extern convar_t r_water_tintrefl_b;
extern convar_t r_water_color_r;
extern convar_t r_water_color_g;
extern convar_t r_water_color_b;
extern convar_t r_water_distscale;
extern convar_t r_water_fogblend;
extern convar_t r_water_debug;
extern convar_t r_water_waveheight;
extern convar_t r_water_wavefreq;
extern convar_t r_water_uw_alpha;
extern convar_t r_water_uw_color_r;
extern convar_t r_water_uw_color_g;
extern convar_t r_water_uw_color_b;
extern convar_t r_water_uw_density;
extern convar_t r_water_uw_scattering;

extern gl_water_shader_state_t gWaterShader;

void    R_WaterShader_Init( void );
void    R_WaterShader_Shutdown( void );
void    R_WaterShader_VidInit( void );
qboolean R_WaterShader_EmitPolys( msurface_t *warp );

#endif
