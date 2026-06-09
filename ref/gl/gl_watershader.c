/*
 * HL2-inspired water shader for xash3d-fwgs (gl4es / GLES 2.0).
 *
 * Adapted from fteqw's HL2 water shader
 * (fteqw/plugins/hl2/glsl/vmt/water.glsl and
 *  fteqw/engine/shaders/glsl/altwater.glsl).
 *
 * Replaces the earlier broken PrimeXT-style r_water_shader implementation.
 *
 * The HL2 approach uses:
 *   - two-layer scrolling normalmap animation (q1-style warp coords)
 *   - Schlick Fresnel for view-dependent reflection/refraction blend
 *   - depth-based water tint (fake "refraction")
 *   - sky-color-based fake reflection
 *   - specular highlights on wave normals
 *   - optional diffuse warp-texture overlay
 *   - separate above-water / underwater fragment programs
 *
 * No FBOs are used. The "reflection" is a Fresnel-tinted sky colour with
 * a sun-like specular term; the "refraction" is the water body colour
 * modulated by distance-from-viewer (depth proxy). This keeps the shader
 * compatible with gl4es and GLES 2.0 targets.
 */

#include "gl_local.h"
#include "gl_watershader.h"

CVAR_DEFINE_AUTO( r_water_shader,               "0",     FCVAR_GLCONFIG, "enable HL2-style water shader" );
CVAR_DEFINE_AUTO( r_water_alpha,                "0.80",  FCVAR_GLCONFIG, "above-water opacity (0=transparent, 1=opaque)" );
CVAR_DEFINE_AUTO( r_water_density,              "0.035", FCVAR_GLCONFIG, "depth-based tint strength (0=off, 0.1=heavy)" );
CVAR_DEFINE_AUTO( r_water_specular,             "0.80",  FCVAR_GLCONFIG, "specular highlight intensity (0..2)" );
CVAR_DEFINE_AUTO( r_water_specular_color_r,     "255",   FCVAR_GLCONFIG, "specular red (0-255)" );
CVAR_DEFINE_AUTO( r_water_specular_color_g,     "246",   FCVAR_GLCONFIG, "specular green (0-255)" );
CVAR_DEFINE_AUTO( r_water_specular_color_b,     "217",   FCVAR_GLCONFIG, "specular blue (0-255)" );
CVAR_DEFINE_AUTO( r_water_skyblend,             "0.85",  FCVAR_GLCONFIG, "fake reflection blend (0=no reflection, 1=full)" );
CVAR_DEFINE_AUTO( r_water_skycolor_r,           "135",   FCVAR_GLCONFIG, "fake reflection red (0-255)" );
CVAR_DEFINE_AUTO( r_water_skycolor_g,           "180",   FCVAR_GLCONFIG, "fake reflection green (0-255)" );
CVAR_DEFINE_AUTO( r_water_skycolor_b,           "210",   FCVAR_GLCONFIG, "fake reflection blue (0-255)" );
CVAR_DEFINE_AUTO( r_water_fresnel,              "5.0",   FCVAR_GLCONFIG, "Fresnel exponent (3-7, HL2 default ~5)" );
CVAR_DEFINE_AUTO( r_water_fogblend,             "1.0",   FCVAR_GLCONFIG, "fog influence on water (0..1)" );
CVAR_DEFINE_AUTO( r_water_color_r,              "32",    FCVAR_GLCONFIG, "water body color red (0-255)" );
CVAR_DEFINE_AUTO( r_water_color_g,              "64",    FCVAR_GLCONFIG, "water body color green (0-255)" );
CVAR_DEFINE_AUTO( r_water_color_b,              "80",    FCVAR_GLCONFIG, "water body color blue (0-255)" );
CVAR_DEFINE_AUTO( r_water_underwater_alpha,     "0.50",  FCVAR_GLCONFIG, "underwater pass opacity (0..1)" );
CVAR_DEFINE_AUTO( r_water_underwater_color_r,   "24",    FCVAR_GLCONFIG, "underwater tint red (0-255)" );
CVAR_DEFINE_AUTO( r_water_underwater_color_g,   "48",    FCVAR_GLCONFIG, "underwater tint green (0-255)" );
CVAR_DEFINE_AUTO( r_water_underwater_color_b,   "64",    FCVAR_GLCONFIG, "underwater tint blue (0-255)" );
CVAR_DEFINE_AUTO( r_water_underwater_density,   "0.020", FCVAR_GLCONFIG, "underwater depth tint strength" );
CVAR_DEFINE_AUTO( r_water_debug,                "0",     0,              "debug (1=log, 2=tint red)" );
CVAR_DEFINE_AUTO( r_water_sun_x,                "0.4",   FCVAR_GLCONFIG, "sun direction X" );
CVAR_DEFINE_AUTO( r_water_sun_y,                "0.4",   FCVAR_GLCONFIG, "sun direction Y" );
CVAR_DEFINE_AUTO( r_water_sun_z,                "0.82",  FCVAR_GLCONFIG, "sun direction Z" );
CVAR_DEFINE_AUTO( r_water_sunlight_scattering,  "0.20",  FCVAR_GLCONFIG, "subsurface scattering strength (0..1)" );
CVAR_DEFINE_AUTO( r_water_waveheight,           "0.0",   FCVAR_GLCONFIG, "vertex wave displacement amplitude" );
CVAR_DEFINE_AUTO( r_water_wavefreq,             "0.04",  FCVAR_GLCONFIG, "vertex wave frequency" );

gl_water_shader_state_t gWaterShader;

/* ---------- nanogl / wes / regal stubs -------------------------------- */
#if XASH_NANOGL || XASH_WES || XASH_REGAL

static void R_WaterShader_RegisterCvars( void )
{
	static qboolean registered = false;
	if( registered ) return;
	registered = true;

	gEngfuncs.Cvar_RegisterVariable( &r_water_shader );
	gEngfuncs.Cvar_RegisterVariable( &r_water_alpha );
	gEngfuncs.Cvar_RegisterVariable( &r_water_density );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skyblend );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fresnel );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fogblend );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_alpha );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_density );
	gEngfuncs.Cvar_RegisterVariable( &r_water_debug );
	gEngfuncs.Cvar_RegisterVariable( &r_water_sun_x );
	gEngfuncs.Cvar_RegisterVariable( &r_water_sun_y );
	gEngfuncs.Cvar_RegisterVariable( &r_water_sun_z );
	gEngfuncs.Cvar_RegisterVariable( &r_water_sunlight_scattering );
	gEngfuncs.Cvar_RegisterVariable( &r_water_waveheight );
	gEngfuncs.Cvar_RegisterVariable( &r_water_wavefreq );
}

void R_WaterShader_Init( void )
{
	R_WaterShader_RegisterCvars();
	memset( &gWaterShader, 0, sizeof( gWaterShader ));
}

void R_WaterShader_Shutdown( void ) {}
void R_WaterShader_VidInit( void )  {}

qboolean R_WaterShader_EmitPolys( msurface_t *warp )
{
	(void)warp;
	return false;
}

#else  /* !XASH_NANOGL && !XASH_WES && !XASH_REGAL */

#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER   0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif

#define WATER_MAX_POLY_VERTS  64
#define WATER_ATTRIB_POSITION  0
#define WATER_ATTRIB_TEXCOORD  8

/* -----------------------------------------------------------------------
 * GLSL ES 1.00 shader sources
 *
 * Adapted from fteqw's HL2 water shader:
 *   - vertex shader with optional wave displacement
 *   - fragment shader with two-layer normalmap scrolling, Fresnel,
 *     fake reflection (sky colour), fake refraction (water colour),
 *     specular highlights, diffuse warp texture overlay, and fog
 * --------------------------------------------------------------------- */

static const char *water_vertex_source =
	"#ifdef GL_ES\n"
	"precision highp float;\n"
	"#endif\n"
	"attribute vec4 a_position;\n"
	"attribute vec2 a_texCoord;\n"
	"uniform mat4 u_modelView;\n"
	"uniform mat4 u_projection;\n"
	"uniform highp float u_time;\n"
	"uniform highp float u_waveheight;\n"
	"uniform highp float u_wavefreq;\n"
	"varying vec3 v_worldPos;\n"
	"varying vec3 v_viewPos;\n"
	"varying vec2 v_texCoord;\n"
	"varying vec3 v_geoNormal;\n"
	"\n"
	"float waveHeight( vec2 p, float t, float freq, float amp )\n"
	"{\n"
	"    float f1 = freq;\n"
	"    float f2 = freq * 0.83;\n"
	"    float f3 = freq * 1.31;\n"
	"    float f4 = freq * 0.57;\n"
	"    float h1 = sin( p.x * f1 + t * 1.1 ) * 0.50;\n"
	"    float h2 = cos( p.y * f2 + t * 0.7 ) * 0.30;\n"
	"    float h3 = sin((p.x + p.y) * f3 + t * 1.5 ) * 0.15;\n"
	"    float h4 = cos( p.x * 0.7 - p.y * 0.7 + t * 0.9 ) * 0.15;\n"
	"    return (h1 + h2 + h3 + h4) * amp;\n"
	"}\n"
	"\n"
	"void main()\n"
	"{\n"
	"    vec3  pos     = a_position.xyz;\n"
	"    float t       = u_time;\n"
	"    float freq    = max( u_wavefreq, 0.001 );\n"
	"    float amp     = u_waveheight;\n"
	"    if( amp > 0.001 )\n"
	"        pos.z += waveHeight( pos.xy, t, freq, amp );\n"
	"\n"
	"    float f1 = freq;\n"
	"    float f2 = freq * 0.83;\n"
	"    float f3 = freq * 1.31;\n"
	"    float f4 = freq * 0.57;\n"
	"    float dHdx =  cos( pos.x * f1 + t * 1.1 ) * 0.50 * f1\n"
	"               +   cos((pos.x + pos.y) * f3 + t * 1.5 ) * 0.15 * f3\n"
	"               -   sin( pos.x * 0.7 - pos.y * 0.7 + t * 0.9 ) * 0.15 * f4 * 0.7;\n"
	"    float dHdy = -sin( pos.y * f2 + t * 0.7 ) * 0.30 * f2\n"
	"               +   cos((pos.x + pos.y) * f3 + t * 1.5 ) * 0.15 * f3\n"
	"               +   sin( pos.x * 0.7 - pos.y * 0.7 + t * 0.9 ) * 0.15 * f4 * (-0.7);\n"
	"    if( amp > 0.001 ) { dHdx *= amp; dHdy *= amp; }\n"
	"    else { dHdx = 0.0; dHdy = 0.0; }\n"
	"    v_geoNormal = normalize( vec3( -dHdx, -dHdy, 1.0 ));\n"
	"\n"
	"    v_worldPos = pos;\n"
	"    v_texCoord = a_texCoord;\n"
	"    vec4 viewPos = u_modelView * vec4( pos, 1.0 );\n"
	"    v_viewPos = viewPos.xyz;\n"
	"    gl_Position = u_projection * viewPos;\n"
	"}\n";

/* HL2-inspired above-water fragment shader.
 *
 * Based on fteqw's altwater.glsl / water.glsl approach:
 *   - q1-style texture coordinate warping
 *   - two-layer scrolling normalmap sampling
 *   - Schlick Fresnel
 *   - fake refraction = water color + depth tint
 *   - fake reflection = sky color (Fresnel-blended)
 *   - specular highlight
 *   - optional diffuse warp texture overlay
 *   - fog
 */
static const char *water_frag_above_source =
	"#ifdef GL_ES\n"
	"precision mediump float;\n"
	"#endif\n"
	"uniform sampler2D u_normalMap;\n"
	"uniform sampler2D u_waterTex;\n"
	"uniform vec3      u_cameraPos;\n"
	"uniform vec3      u_waterColor;\n"
	"uniform vec3      u_skyColor;\n"
	"uniform vec3      u_specularColor;\n"
	"uniform vec3      u_sunDir;\n"
	"uniform highp float u_time;\n"
	"uniform float     u_fresnelFactor;\n"
	"uniform float     u_alpha;\n"
	"uniform float     u_density;\n"
	"uniform float     u_specular;\n"
	"uniform float     u_skyblend;\n"
	"uniform float     u_sunlightScattering;\n"
	"uniform float     u_fogBlend;\n"
	"uniform vec3      u_fogColor;\n"
	"uniform float     u_fogStart;\n"
	"uniform float     u_fogEnd;\n"
	"uniform float     u_fogEnabled;\n"
	"varying vec3 v_worldPos;\n"
	"varying vec3 v_viewPos;\n"
	"varying vec2 v_texCoord;\n"
	"varying vec3 v_geoNormal;\n"
	"\n"
	"const float WATER_F0 = 0.15;\n"
	"const float WAVE_SCALE = 0.004;\n"
	"\n"
	"void main()\n"
	"{\n"
	"    vec2 uv = v_worldPos.xy * WAVE_SCALE;\n"
	"    float t = u_time;\n"
	"\n"
	"    /* q1-style warp for normalmap coords (HL2 water style) */\n"
	"    vec2 ntc;\n"
	"    ntc.s = uv.s + sin( uv.t * 3.0 + t ) * 0.1;\n"
	"    ntc.t = uv.t + cos( uv.s * 3.0 + t * 0.8 ) * 0.1;\n"
	"\n"
	"    /* two-layer scrolling normalmap (HL2 altwater approach) */\n"
	"    vec3 n1 = texture2D( u_normalMap, ntc * 1.2 + vec2( t * 0.08, 0.0 )).xyz;\n"
	"    vec3 n2 = texture2D( u_normalMap, ntc * 0.6 - vec2( 0.0, t * 0.06 )).xyz;\n"
	"    vec3 N = normalize((n1 + n2) * 2.0 - 1.0);\n"
	"\n"
	"    vec3 V = normalize( u_cameraPos - v_worldPos );\n"
	"    float NdotV = max( dot( N, V ), 0.0 );\n"
	"\n"
	"    /* Schlick Fresnel */\n"
	"    float fresnel = WATER_F0 + (1.0 - WATER_F0) * pow( 1.0 - NdotV, u_fresnelFactor );\n"
	"    fresnel = clamp( fresnel, 0.0, 0.95 );\n"
	"\n"
	"    /* fake refraction = water color with depth-based darkening */\n"
	"    float dist = length( v_viewPos );\n"
	"    float depthFactor = clamp( dist * u_density, 0.0, 1.0 );\n"
	"    vec3 refr = u_waterColor * (1.0 - depthFactor * 0.6);\n"
	"\n"
	"    /* subsurface sunlight scattering */\n"
	"    float scatter = u_sunlightScattering * pow( max( dot( N, u_sunDir ), 0.0 ), 8.0 );\n"
	"    refr += vec3( 0.2, 0.4, 0.6 ) * scatter;\n"
	"\n"
	"    /* fake reflection = sky color */\n"
	"    vec3 refl = u_skyColor;\n"
	"\n"
	"    /* Fresnel blend */\n"
	"    vec3 color = mix( refr, refl, fresnel * u_skyblend );\n"
	"\n"
	"    /* specular */\n"
	"    vec3 R = reflect( -V, N );\n"
	"    float spec = pow( max( dot( R, u_sunDir ), 0.0 ), 64.0 ) * u_specular;\n"
	"    color += u_specularColor * spec;\n"
	"\n"
	"    /* diffuse warp texture overlay (like HL2 water texture on top) */\n"
	"    vec4 waterTexel = texture2D( u_waterTex, v_texCoord );\n"
	"    color = mix( color, waterTexel.rgb, 0.15 );\n"
	"\n"
	"    if( u_fogEnabled > 0.5 )\n"
	"    {\n"
	"        float fogF = clamp((dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);\n"
	"        color = mix( color, u_fogColor, fogF * u_fogBlend );\n"
	"    }\n"
	"\n"
	"    gl_FragColor = vec4( color, clamp( u_alpha, 0.0, 1.0 ));\n"
	"}\n";

/* Underwater fragment shader.
 * Simple deep tint with caustic shimmer and light shafts. */
static const char *water_frag_underwater_source =
	"#ifdef GL_ES\n"
	"precision mediump float;\n"
	"#endif\n"
	"uniform vec3      u_sunDir;\n"
	"uniform vec3      u_underwaterColor;\n"
	"uniform highp float u_time;\n"
	"uniform float     u_underwaterAlpha;\n"
	"uniform float     u_underwaterDensity;\n"
	"uniform float     u_sunlightScattering;\n"
	"uniform float     u_fogBlend;\n"
	"uniform vec3      u_fogColor;\n"
	"uniform float     u_fogStart;\n"
	"uniform float     u_fogEnd;\n"
	"uniform float     u_fogEnabled;\n"
	"varying vec3 v_worldPos;\n"
	"varying vec3 v_viewPos;\n"
	"varying vec3 v_geoNormal;\n"
	"\n"
	"float caustic( vec2 pos, float t )\n"
	"{\n"
	"    vec2 uv = pos * 0.002;\n"
	"    float c1 = sin( uv.x * 3.0 + t * 0.8 ) * cos( uv.y * 2.5 - t * 0.6 );\n"
	"    float c2 = sin((uv.x + uv.y) * 4.0 + t * 1.2 ) * 0.5;\n"
	"    float c3 = cos((uv.x - uv.y) * 5.0 - t * 0.9 ) * 0.3;\n"
	"    return clamp( c1 * 0.5 + c2 + c3, 0.0, 1.0 );\n"
	"}\n"
	"\n"
	"void main()\n"
	"{\n"
	"    float dist  = length( v_viewPos );\n"
	"    float depthFactor = clamp( dist * u_underwaterDensity, 0.0, 1.0 );\n"
	"\n"
	"    float c = caustic( v_worldPos.xy, u_time );\n"
	"    float sunFactor = clamp( 1.0 - depthFactor, 0.0, 1.0 );\n"
	"    float sunAngle  = max( dot( v_geoNormal, u_sunDir ), 0.0 );\n"
	"\n"
	"    vec3 color = u_underwaterColor * (1.0 - depthFactor * 0.7);\n"
	"    color += vec3( 0.15, 0.25, 0.10 ) * c * (1.0 - depthFactor * 0.5);\n"
	"    color += vec3( 0.3, 0.4, 0.5 ) * sunFactor * sunAngle * u_sunlightScattering;\n"
	"\n"
	"    if( u_fogEnabled > 0.5 )\n"
	"    {\n"
	"        float fogF = clamp((dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);\n"
	"        color = mix( color, u_fogColor, fogF * u_fogBlend );\n"
	"    }\n"
	"\n"
	"    gl_FragColor = vec4( color, clamp( u_underwaterAlpha, 0.0, 1.0 ));\n"
	"}\n";

/* ---------------------------------------------------------------------- */
/* Shader compilation                                                     */
/* ---------------------------------------------------------------------- */

static GLuint R_WaterShader_CompileShader( GLenum type, const char *src )
{
	GLuint sh = pglCreateShaderObjectARB( type );
	if( !sh ) return 0;

	pglShaderSourceARB( sh, 1, &src, NULL );
	pglCompileShaderARB( sh );

	GLint ok = 0;
	pglGetObjectParameterivARB( sh, GL_OBJECT_COMPILE_STATUS_ARB, &ok );
	if( !ok )
	{
		GLint len = 0;
		pglGetObjectParameterivARB( sh, GL_OBJECT_INFO_LOG_LENGTH_ARB, &len );
		if( len > 1 )
		{
			char *log = malloc( len );
			if( log )
			{
				pglGetInfoLogARB( sh, len, NULL, log );
				gEngfuncs.Con_Printf( S_ERROR "water shader compile:\n%s\n", log );
				free( log );
			}
		}
		pglDeleteObjectARB( sh );
		return 0;
	}
	return sh;
}

static void R_WaterShader_DeleteProgram( gl_water_program_t *p )
{
	if( !p ) return;
	if( p->vertShader ) pglDeleteObjectARB( p->vertShader );
	if( p->fragShader ) pglDeleteObjectARB( p->fragShader );
	if( p->program )    pglDeleteObjectARB( p->program );
	memset( p, 0, sizeof( *p ));
}

static qboolean R_WaterShader_CompileProgram( gl_water_program_t *p,
                                              const char *vs, const char *fs )
{
	memset( p, 0, sizeof( *p ));

	p->vertShader = R_WaterShader_CompileShader( GL_VERTEX_SHADER, vs );
	if( !p->vertShader ) return false;

	p->fragShader = R_WaterShader_CompileShader( GL_FRAGMENT_SHADER, fs );
	if( !p->fragShader ) { R_WaterShader_DeleteProgram( p ); return false; }

	p->program = pglCreateProgramObjectARB();
	if( !p->program ) { R_WaterShader_DeleteProgram( p ); return false; }

	pglAttachObjectARB( p->program, p->vertShader );
	pglAttachObjectARB( p->program, p->fragShader );

	pglBindAttribLocationARB( p->program, WATER_ATTRIB_POSITION, "a_position" );
	pglBindAttribLocationARB( p->program, WATER_ATTRIB_TEXCOORD, "a_texCoord" );

	pglLinkProgramARB( p->program );

	GLint linked = 0;
	pglGetObjectParameterivARB( p->program, GL_OBJECT_LINK_STATUS_ARB, &linked );
	if( !linked )
	{
		GLint len = 0;
		pglGetObjectParameterivARB( p->program, GL_OBJECT_INFO_LOG_LENGTH_ARB, &len );
		if( len > 1 )
		{
			char *log = malloc( len );
			if( log )
			{
				pglGetInfoLogARB( p->program, len, NULL, log );
				gEngfuncs.Con_Printf( S_ERROR "water shader link:\n%s\n", log );
				free( log );
			}
		}
		R_WaterShader_DeleteProgram( p );
		return false;
	}

	p->u_modelView     = pglGetUniformLocationARB( p->program, "u_modelView" );
	p->u_projection    = pglGetUniformLocationARB( p->program, "u_projection" );
	p->u_normalMap     = pglGetUniformLocationARB( p->program, "u_normalMap" );
	p->u_waterTex      = pglGetUniformLocationARB( p->program, "u_waterTex" );
	p->u_cameraPos     = pglGetUniformLocationARB( p->program, "u_cameraPos" );
	p->u_time          = pglGetUniformLocationARB( p->program, "u_time" );
	p->u_fresnelFactor = pglGetUniformLocationARB( p->program, "u_fresnelFactor" );
	p->u_fogColor      = pglGetUniformLocationARB( p->program, "u_fogColor" );
	p->u_fogStart      = pglGetUniformLocationARB( p->program, "u_fogStart" );
	p->u_fogEnd        = pglGetUniformLocationARB( p->program, "u_fogEnd" );
	p->u_fogEnabled    = pglGetUniformLocationARB( p->program, "u_fogEnabled" );

	/* above-water */
	p->u_waterColor        = pglGetUniformLocationARB( p->program, "u_waterColor" );
	p->u_alpha             = pglGetUniformLocationARB( p->program, "u_alpha" );
	p->u_density           = pglGetUniformLocationARB( p->program, "u_density" );
	p->u_specular          = pglGetUniformLocationARB( p->program, "u_specular" );
	p->u_specularColor     = pglGetUniformLocationARB( p->program, "u_specularColor" );
	p->u_skyColor          = pglGetUniformLocationARB( p->program, "u_skyColor" );
	p->u_skyblend          = pglGetUniformLocationARB( p->program, "u_skyblend" );
	p->u_fogBlend          = pglGetUniformLocationARB( p->program, "u_fogBlend" );
	p->u_sunDir            = pglGetUniformLocationARB( p->program, "u_sunDir" );
	p->u_sunlightScattering = pglGetUniformLocationARB( p->program, "u_sunlightScattering" );

	/* underwater */
	p->u_underwaterColor   = pglGetUniformLocationARB( p->program, "u_underwaterColor" );
	p->u_underwaterAlpha   = pglGetUniformLocationARB( p->program, "u_underwaterAlpha" );
	p->u_underwaterDensity = pglGetUniformLocationARB( p->program, "u_underwaterDensity" );

	/* vertex */
	p->u_waveheight        = pglGetUniformLocationARB( p->program, "u_waveheight" );
	p->u_wavefreq          = pglGetUniformLocationARB( p->program, "u_wavefreq" );

	p->a_position = WATER_ATTRIB_POSITION;
	p->a_texCoord = WATER_ATTRIB_TEXCOORD;

	return true;
}

/* ---------------------------------------------------------------------- */
/* Normal-map loading                                                     */
/* ---------------------------------------------------------------------- */

#define WATER_PROC_NORMAL_SIZE 256

static void R_WaterShader_GenerateProceduralNormal( byte *out, int size )
{
	for( int y = 0; y < size; y++ )
	{
		for( int x = 0; x < size; x++ )
		{
			float u = (float)x / (float)size;
			float v = (float)y / (float)size;
			float nx = sinf( u * 12.0f + 0.7f ) * 0.40f
			         + sinf( u * 24.0f + v * 8.0f  + 2.1f ) * 0.25f
			         + sinf( u * 48.0f - v * 16.0f + 5.3f ) * 0.20f
			         + sinf( u * 80.0f + v * 60.0f + 4.2f ) * 0.15f;
			float ny = cosf( v * 10.0f + 1.3f ) * 0.40f
			         + cosf( u *  6.0f + v * 20.0f + 3.7f ) * 0.25f
			         + cosf( u * 20.0f + v * 40.0f + 0.5f ) * 0.20f
			         + cosf( u * 70.0f - v * 50.0f + 6.8f ) * 0.15f;

			float len = sqrtf( nx*nx + ny*ny + 1.0f );
			out[(y * size + x) * 3 + 0] = (byte)((nx / len * 0.5f + 0.5f) * 255.0f);
			out[(y * size + x) * 3 + 1] = (byte)((ny / len * 0.5f + 0.5f) * 255.0f);
			out[(y * size + x) * 3 + 2] = (byte)((1.0f / len * 0.5f + 0.5f) * 255.0f);
		}
	}
}

static GLuint R_WaterShader_UploadProceduralNormal( void )
{
	const int size = WATER_PROC_NORMAL_SIZE;
	byte *buf = (byte *)malloc( size * size * 3 );
	if( !buf ) return 0;

	R_WaterShader_GenerateProceduralNormal( buf, size );

	GLuint tex = 0;
	pglGenTextures( 1, &tex );
	pglBindTexture( GL_TEXTURE_2D, tex );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT );
	pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGB, size, size, 0, GL_RGB, GL_UNSIGNED_BYTE, buf );

	free( buf );
	return tex;
}

static void R_WaterShader_LoadNormalTexture( void )
{
	/* Try to load a single normalmap texture first */
	int idx = GL_LoadTexture( "gfx/water/water_normal_0.tga", NULL, 0, TF_NORMALMAP );

	if( !idx )
		idx = GL_LoadTexture( "gfx/textures/water_normal_0.tga", NULL, 0, TF_NORMALMAP );

	if( idx )
	{
		const gl_texture_t *t = R_GetTexture( idx );
		if( t && t->texnum )
		{
			gWaterShader.normalTexture = t->texnum;
			gEngfuncs.Con_Reportf( "R_WaterShader: loaded normalmap\n" );
			return;
		}
	}

	/* Fall back to procedural */
	gWaterShader.normalTexture = R_WaterShader_UploadProceduralNormal();
	if( gWaterShader.normalTexture )
		gEngfuncs.Con_Reportf( "R_WaterShader: using procedural normalmap\n" );
}

/* ---------------------------------------------------------------------- */
/* Init / shutdown                                                        */
/* ---------------------------------------------------------------------- */

static void R_WaterShader_RegisterCvars( void )
{
	static qboolean registered = false;
	if( registered ) return;
	registered = true;

	gEngfuncs.Cvar_RegisterVariable( &r_water_shader );
	gEngfuncs.Cvar_RegisterVariable( &r_water_alpha );
	gEngfuncs.Cvar_RegisterVariable( &r_water_density );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skyblend );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fresnel );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fogblend );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_alpha );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_density );
	gEngfuncs.Cvar_RegisterVariable( &r_water_debug );
	gEngfuncs.Cvar_RegisterVariable( &r_water_sun_x );
	gEngfuncs.Cvar_RegisterVariable( &r_water_sun_y );
	gEngfuncs.Cvar_RegisterVariable( &r_water_sun_z );
	gEngfuncs.Cvar_RegisterVariable( &r_water_sunlight_scattering );
	gEngfuncs.Cvar_RegisterVariable( &r_water_waveheight );
	gEngfuncs.Cvar_RegisterVariable( &r_water_wavefreq );
}

void R_WaterShader_Init( void )
{
	R_WaterShader_RegisterCvars();

	if( gWaterShader.initialized )
		return;

	memset( &gWaterShader, 0, sizeof( gWaterShader ));

	if( !GL_Support( GL_SHADER_GLSL100_EXT ))
	{
		gEngfuncs.Con_Printf( "R_WaterShader: GLSL not supported, disabled\n" );
		return;
	}

	if( !R_WaterShader_CompileProgram( &gWaterShader.programAboveWater,
	                                   water_vertex_source, water_frag_above_source ))
	{
		gEngfuncs.Con_Printf( S_ERROR "R_WaterShader: failed to build above-water program\n" );
		return;
	}

	if( !R_WaterShader_CompileProgram( &gWaterShader.programUnderwater,
	                                   water_vertex_source, water_frag_underwater_source ))
	{
		gEngfuncs.Con_Printf( S_ERROR "R_WaterShader: failed to build underwater program\n" );
		R_WaterShader_DeleteProgram( &gWaterShader.programAboveWater );
		return;
	}

	gWaterShader.shaderSupport = 1;
	gWaterShader.initialized   = 1;

	gEngfuncs.Con_Reportf( "R_WaterShader: ready (HL2-style, FBO-less)\n" );
}

void R_WaterShader_Shutdown( void )
{
	if( !gWaterShader.initialized )
		return;

	if( glw_state.initialized )
	{
		R_WaterShader_DeleteProgram( &gWaterShader.programAboveWater );
		R_WaterShader_DeleteProgram( &gWaterShader.programUnderwater );

		if( gWaterShader.normalTexture )
			pglDeleteTextures( 1, &gWaterShader.normalTexture );
	}

	memset( &gWaterShader, 0, sizeof( gWaterShader ));
}

void R_WaterShader_VidInit( void )
{
	if( !gWaterShader.shaderSupport )
		return;

	/* Reload normalmap on vid restart */
	if( gWaterShader.normalTexture )
	{
		pglDeleteTextures( 1, &gWaterShader.normalTexture );
		gWaterShader.normalTexture = 0;
	}

	R_WaterShader_LoadNormalTexture();
}

/* ---------------------------------------------------------------------- */
/* Per-surface drawing                                                    */
/* ---------------------------------------------------------------------- */

qboolean R_WaterShader_EmitPolys( msurface_t *warp )
{
	if( !gWaterShader.shaderSupport ) return false;
	if( !r_water_shader.value )       return false;
	if( !warp || !warp->polys )       return false;
	if( !gWaterShader.normalTexture ) return false;

	const qboolean underwater =
	    ( warp->polys->verts[0][2] >= RI.rvp.vieworigin[2] );

	gl_water_program_t *prog = underwater
	    ? &gWaterShader.programUnderwater
	    : &gWaterShader.programAboveWater;

	pglUseProgramObjectARB( prog->program );

	/* matrices */
	if( prog->u_modelView >= 0 )
	{
		float m[16];
		pglGetFloatv( GL_MODELVIEW_MATRIX, m );
		pglUniformMatrix4fvARB( prog->u_modelView, 1, GL_FALSE, m );
	}
	if( prog->u_projection >= 0 )
	{
		float m[16];
		pglGetFloatv( GL_PROJECTION_MATRIX, m );
		pglUniformMatrix4fvARB( prog->u_projection, 1, GL_FALSE, m );
	}

	/* camera position */
	if( prog->u_cameraPos >= 0 )
	{
		pglUniform3fARB( prog->u_cameraPos,
		                 RI.rvp.vieworigin[0],
		                 RI.rvp.vieworigin[1],
		                 RI.rvp.vieworigin[2] );
	}

	/* water color + alpha */
	{
		float r = r_water_color_r.value / 255.0f;
		float g = r_water_color_g.value / 255.0f;
		float b = r_water_color_b.value / 255.0f;
		float a = r_water_alpha.value;

		cl_entity_t *e = RI.currententity;
		if( e )
		{
			qboolean colModified = ( r_water_color_r.value != 32.0f
				|| r_water_color_g.value != 64.0f
				|| r_water_color_b.value != 80.0f );
			qboolean alphaModified = ( r_water_alpha.value != 0.80f );

			if( !colModified &&
			    ( e->curstate.rendercolor.r || e->curstate.rendercolor.g || e->curstate.rendercolor.b ))
			{
				r = e->curstate.rendercolor.r / 255.0f;
				g = e->curstate.rendercolor.g / 255.0f;
				b = e->curstate.rendercolor.b / 255.0f;
			}

			if( !alphaModified )
			{
				switch( e->curstate.rendermode )
				{
				case kRenderTransTexture:
				case kRenderTransColor:
				case kRenderTransAlpha:
				case kRenderTransAdd:
				case kRenderGlow:
					a = e->curstate.renderamt / 255.0f;
					break;
				default:
					break;
				}
			}
		}

		if( r_water_debug.value >= 2.0f )
			r = 1.0f, g = 0.0f, b = 0.0f;

		if( prog->u_waterColor >= 0 )
			pglUniform3fARB( prog->u_waterColor, r, g, b );
		if( prog->u_alpha >= 0 )
			pglUniform1fARB( prog->u_alpha, Q_min( 1.0f, Q_max( 0.0f, a )));
	}

	/* time */
	if( prog->u_time >= 0 )
		pglUniform1fARB( prog->u_time, (float)gp_cl->time );

	/* fresnel */
	if( prog->u_fresnelFactor >= 0 )
		pglUniform1fARB( prog->u_fresnelFactor,
		                 r_water_fresnel.value > 0.1f ? r_water_fresnel.value : 5.0f );

	/* fog */
	if( prog->u_fogColor >= 0 )
		pglUniform3fARB( prog->u_fogColor, RI.fogColor[0], RI.fogColor[1], RI.fogColor[2] );
	if( prog->u_fogStart >= 0 )
		pglUniform1fARB( prog->u_fogStart, RI.fogStart );
	if( prog->u_fogEnd >= 0 )
		pglUniform1fARB( prog->u_fogEnd, RI.fogEnd );
	if( prog->u_fogEnabled >= 0 )
		pglUniform1fARB( prog->u_fogEnabled, RI.fogEnabled ? 1.0f : 0.0f );
	if( prog->u_fogBlend >= 0 )
		pglUniform1fARB( prog->u_fogBlend, r_water_fogblend.value );

	/* sun direction */
	if( prog->u_sunDir >= 0 )
	{
		vec3_t dir;
		dir[0] = r_water_sun_x.value;
		dir[1] = r_water_sun_y.value;
		dir[2] = r_water_sun_z.value;
		VectorNormalize( dir );
		pglUniform3fARB( prog->u_sunDir, dir[0], dir[1], dir[2] );
	}

	/* above-water uniforms */
	if( prog->u_density >= 0 )
		pglUniform1fARB( prog->u_density, r_water_density.value );
	if( prog->u_specular >= 0 )
		pglUniform1fARB( prog->u_specular, r_water_specular.value );
	if( prog->u_specularColor >= 0 )
		pglUniform3fARB( prog->u_specularColor,
		                 r_water_specular_color_r.value / 255.0f,
		                 r_water_specular_color_g.value / 255.0f,
		                 r_water_specular_color_b.value / 255.0f );
	if( prog->u_skyblend >= 0 )
		pglUniform1fARB( prog->u_skyblend, r_water_skyblend.value );
	if( prog->u_skyColor >= 0 )
		pglUniform3fARB( prog->u_skyColor,
		                 r_water_skycolor_r.value / 255.0f,
		                 r_water_skycolor_g.value / 255.0f,
		                 r_water_skycolor_b.value / 255.0f );
	if( prog->u_sunlightScattering >= 0 )
		pglUniform1fARB( prog->u_sunlightScattering, r_water_sunlight_scattering.value );

	/* underwater uniforms */
	if( prog->u_underwaterAlpha >= 0 )
		pglUniform1fARB( prog->u_underwaterAlpha, r_water_underwater_alpha.value );
	if( prog->u_underwaterColor >= 0 )
		pglUniform3fARB( prog->u_underwaterColor,
		                 r_water_underwater_color_r.value / 255.0f,
		                 r_water_underwater_color_g.value / 255.0f,
		                 r_water_underwater_color_b.value / 255.0f );
	if( prog->u_underwaterDensity >= 0 )
		pglUniform1fARB( prog->u_underwaterDensity, r_water_underwater_density.value );

	/* vertex wave uniforms */
	if( prog->u_waveheight >= 0 )
	{
		float waveH = r_water_waveheight.value;
		cl_entity_t *ec = RI.currententity;
		if( ec && ec->curstate.scale > 0.001f && fabsf( ec->curstate.scale - 1.0f ) > 0.001f )
			waveH *= ec->curstate.scale;
		pglUniform1fARB( prog->u_waveheight, waveH );
	}
	if( prog->u_wavefreq >= 0 )
		pglUniform1fARB( prog->u_wavefreq, r_water_wavefreq.value );

	/* bind normalmap on unit 0 */
	pglActiveTextureARB( GL_TEXTURE0_ARB );
	pglBindTexture( GL_TEXTURE_2D, gWaterShader.normalTexture );
	if( prog->u_normalMap >= 0 )
		pglUniform1iARB( prog->u_normalMap, 0 );

	/* bind warp texture on unit 1 */
	if( prog->u_waterTex >= 0 )
	{
		pglActiveTextureARB( GL_TEXTURE1_ARB );
		texture_t *wt = warp->texinfo ? warp->texinfo->texture : NULL;
		GLuint wtNum = ( wt && wt->fb_texturenum ) ? wt->fb_texturenum : gWaterShader.normalTexture;
		pglBindTexture( GL_TEXTURE_2D, wtNum );
		pglUniform1iARB( prog->u_waterTex, 1 );
	}

	pglActiveTextureARB( GL_TEXTURE0_ARB );

	/* render state */
	pglDepthMask( GL_FALSE );
	pglEnable( GL_DEPTH_TEST );
	pglDepthFunc( GL_LEQUAL );
	pglEnable( GL_BLEND );
	pglBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	pglDisable( GL_CLIP_PLANE0 );
	pglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );

	pglEnableVertexAttribArrayARB( prog->a_position );
	pglEnableVertexAttribArrayARB( prog->a_texCoord );

	for( glpoly2_t *p = warp->polys; p; p = p->next )
	{
		int n = p->numverts;
		if( n < 3 ) continue;
		if( n > WATER_MAX_POLY_VERTS ) n = WATER_MAX_POLY_VERTS;

		float vertices [WATER_MAX_POLY_VERTS * 3];
		float texcoords[WATER_MAX_POLY_VERTS * 2];

		for( int i = 0; i < n; i++ )
		{
			float *v = p->verts[i];
			vertices[i * 3 + 0] = v[0];
			vertices[i * 3 + 1] = v[1];
			vertices[i * 3 + 2] = v[2];
			texcoords[i * 2 + 0] = v[3] * (1.0f / SUBDIVIDE_SIZE);
			texcoords[i * 2 + 1] = v[4] * (1.0f / SUBDIVIDE_SIZE);
		}

		pglVertexAttribPointerARB( prog->a_position, 3, GL_FLOAT, GL_FALSE, 0, vertices );
		pglVertexAttribPointerARB( prog->a_texCoord, 2, GL_FLOAT, GL_FALSE, 0, texcoords );
		pglDrawArrays( GL_TRIANGLE_FAN, 0, n );
	}

	pglDisableVertexAttribArrayARB( prog->a_position );
	pglDisableVertexAttribArrayARB( prog->a_texCoord );

	pglDisable( GL_BLEND );
	pglDepthMask( GL_TRUE );

	pglUseProgramObjectARB( 0 );

	pglActiveTextureARB( GL_TEXTURE0_ARB );
	pglBindTexture( GL_TEXTURE_2D, 0 );

	if( r_water_debug.value >= 1.0f )
		gEngfuncs.Con_Reportf( "R_WaterShader: drew warp %p (%s)\n",
		                       (void *)warp, underwater ? "underwater" : "above" );

	return true;
}

#endif  /* !XASH_NANOGL && !XASH_WES && !XASH_REGAL */
