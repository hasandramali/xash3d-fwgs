/*
 * PrimeXT-inspired water shader for xash3d-fwgs (gl4es / GLSL ES 1.00)
 *
 * Implements a simplified version of PrimeXT's LIQUID_SURFACE rendering:
 *   - animated water normalmaps (gfx/water/water_normal_<N>)
 *   - Schlick Fresnel (WATER_F0_VALUE = 0.15, FRESNEL_FACTOR = 5.0)
 *   - depth-based water color (waterBorderFactor)
 *   - separate above-water / underwater fragment programs
 *
 * No FBOs are used; reflection is faked by mixing the water color towards
 * a sky-tinted color through the Fresnel term, plus a sun specular term.
 * This keeps the shader compatible with all gl4es/GLES 2.0 targets and
 * avoids the broken full-scene reprojection pass.
 */

#ifndef GL_WATERSHADER_H
#define GL_WATERSHADER_H

#include "gl_local.h"

// Max frames in the water_normal_N animation chain (PrimeXT loads until missing).
#define WATER_MAX_FRAMES        32

// Default cycle speed for the normal-map animation (PrimeXT uses 20.0).
#define WATER_ANIMTIME          20.0f

// Schlick / Fresnel constants taken from PrimeXT/game_dir/glsl/fresnel.h
#define WATER_F0_VALUE          0.15f
#define WATER_FRESNEL_FACTOR    5.0f

typedef struct
{
	GLuint  program;
	GLuint  vertShader;
	GLuint  fragShader;

	// engine-supplied uniforms (always present in both passes)
	GLint   u_modelView;
	GLint   u_projection;
	GLint   u_normalMap;
	GLint   u_cameraPos;
	GLint   u_time;
	GLint   u_fresnelFactor;
	GLint   u_fogColor;
	GLint   u_fogStart;
	GLint   u_fogEnd;
	GLint   u_fogEnabled;

	// above-water uniforms (set to -1 on the underwater program)
	GLint   u_waterColor;
	GLint   u_alpha;
	GLint   u_ambient;
	GLint   u_density;
	GLint   u_normalScale;
	GLint   u_choppy;
	GLint   u_specular;
	GLint   u_specularMin;
	GLint   u_specularColor;
	GLint   u_skyblend;
	GLint   u_skyColor;
	GLint   u_fogBlend;

	// vertex-shader uniforms (always present in both programs)
	GLint   u_waveheight;
	GLint   u_wavefreq;

	// OpenMW-style uniforms (always present in both programs)
	GLint   u_sunDir;
	GLint   u_scattering;
	GLint   u_rainIntensity;

	// underwater uniforms (set to -1 on the above-water program)
	GLint   u_underwaterColor;
	GLint   u_underwaterAlpha;
	GLint   u_underwaterDensity;

	// attributes (bound to fixed locations to avoid gl4es FPE wrapping)
	GLint   a_position;
	GLint   a_texCoord;
} gl_water_program_t;

typedef struct
{
	int                 initialized;
	int                 shaderSupport;

	gl_water_program_t  programAboveWater;
	gl_water_program_t  programUnderwater;

	// Animated normal-map frames (PrimeXT-style: gfx/water/water_normal_0..N).
	// Falls back to a single procedurally-generated texture when none exist.
	GLuint              normalFrames[WATER_MAX_FRAMES];
	int                 numNormalFrames;
	GLuint              normalProcedural;  // fallback proc texture
} gl_water_shader_state_t;

extern convar_t r_water_shader;
extern convar_t r_water_alpha;
extern convar_t r_water_ambient;
extern convar_t r_water_density;
extern convar_t r_water_normalscale;
extern convar_t r_water_choppy;
extern convar_t r_water_wave;
extern convar_t r_water_animspeed;
extern convar_t r_water_waveheight;
extern convar_t r_water_wavefreq;
extern convar_t r_water_specular_min;
extern convar_t r_water_specular;
extern convar_t r_water_specular_color_r;
extern convar_t r_water_specular_color_g;
extern convar_t r_water_specular_color_b;
extern convar_t r_water_skyblend;
extern convar_t r_water_skycolor_r;
extern convar_t r_water_skycolor_g;
extern convar_t r_water_skycolor_b;
extern convar_t r_water_underwater_alpha;
extern convar_t r_water_underwater_color_r;
extern convar_t r_water_underwater_color_g;
extern convar_t r_water_underwater_color_b;
extern convar_t r_water_underwater_density;
extern convar_t r_water_fresnel;
extern convar_t r_water_fogblend;
extern convar_t r_water_color_r;
extern convar_t r_water_color_g;
extern convar_t r_water_color_b;
extern convar_t r_water_sun_x;
extern convar_t r_water_sun_y;
extern convar_t r_water_sun_z;
extern convar_t r_water_sunlight_scattering;
extern convar_t r_water_rain_intensity;
extern convar_t r_water_debug;

extern gl_water_shader_state_t gWaterShader;

void    R_WaterShader_Init( void );
void    R_WaterShader_Shutdown( void );
void    R_WaterShader_VidInit( void );

// Returns true if the surface was drawn with the shader pipeline,
// false if the caller should fall back to the fixed-function water emit.
qboolean R_WaterShader_EmitPolys( msurface_t *warp );

#endif // GL_WATERSHADER_H
