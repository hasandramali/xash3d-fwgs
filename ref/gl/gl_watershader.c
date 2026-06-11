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

#include "gl_local.h"
#include "gl_watershader.h"

CVAR_DEFINE_AUTO( r_water_shader,           "0",     FCVAR_GLCONFIG, "enable HL2-style water shader" );
CVAR_DEFINE_AUTO( r_water_alpha,            "0.85",  FCVAR_GLCONFIG, "water opacity (0=transparent, 1=opaque)" );
CVAR_DEFINE_AUTO( r_water_fresnel,          "4.0",   FCVAR_GLCONFIG, "Fresnel exponent (HL2 default ~4)" );
CVAR_DEFINE_AUTO( r_water_fresnel_min,      "0.0",   FCVAR_GLCONFIG, "minimum Fresnel reflectivity (0..1)" );
CVAR_DEFINE_AUTO( r_water_fresnel_range,    "1.0",   FCVAR_GLCONFIG, "Fresnel range (0..1)" );
CVAR_DEFINE_AUTO( r_water_strength,         "0.05",  FCVAR_GLCONFIG, "refraction distortion strength (above water)" );
CVAR_DEFINE_AUTO( r_water_strength_under,   "0.10",  FCVAR_GLCONFIG, "refraction distortion strength (underwater)" );
CVAR_DEFINE_AUTO( r_water_watercolor_r,     "30",    FCVAR_GLCONFIG, "water body color red (0-255)" );
CVAR_DEFINE_AUTO( r_water_watercolor_g,     "60",    FCVAR_GLCONFIG, "water body color green (0-255)" );
CVAR_DEFINE_AUTO( r_water_watercolor_b,     "80",    FCVAR_GLCONFIG, "water body color blue (0-255)" );
CVAR_DEFINE_AUTO( r_water_distscale,        "0.002", FCVAR_GLCONFIG, "distance-based depth tint scale" );
CVAR_DEFINE_AUTO( r_water_distcolor_r,      "255",   FCVAR_GLCONFIG, "distance depth tint color red (0-255) 0=black" );
CVAR_DEFINE_AUTO( r_water_distcolor_g,      "255",   FCVAR_GLCONFIG, "distance depth tint color green (0-255) 0=black" );
CVAR_DEFINE_AUTO( r_water_distcolor_b,      "255",   FCVAR_GLCONFIG, "distance depth tint color blue (0-255) 0=black" );
CVAR_DEFINE_AUTO( r_water_fogblend,         "1.0",   FCVAR_GLCONFIG, "fog influence on water (0..1)" );
CVAR_DEFINE_AUTO( r_water_caustic_intensity,"0.5",   FCVAR_GLCONFIG, "underwater caustic brightness (0=off)" );
CVAR_DEFINE_AUTO( r_water_caustic_color_r,  "25",    FCVAR_GLCONFIG, "underwater caustic color red (0-255)" );
CVAR_DEFINE_AUTO( r_water_caustic_color_g,  "38",    FCVAR_GLCONFIG, "underwater caustic color green (0-255)" );
CVAR_DEFINE_AUTO( r_water_caustic_color_b,  "20",    FCVAR_GLCONFIG, "underwater caustic color blue (0-255)" );
CVAR_DEFINE_AUTO( r_water_diffuse_overlay,  "0.25",  FCVAR_GLCONFIG, "diffuse texture overlay strength (0=off)" );
CVAR_DEFINE_AUTO( r_water_refract,          "1",     FCVAR_GLCONFIG, "refraction: 0=off, 1=normal, 2=opaque water (hide non-refracted)" );
CVAR_DEFINE_AUTO( r_water_underwaterwarp,   "1",     FCVAR_GLCONFIG, "underwater fullscreen warp effect (0=off)" );
CVAR_DEFINE_AUTO( r_water_debug,            "0",     FCVAR_GLCONFIG, "debug (1=log, 2=tint red)" );
CVAR_DEFINE_AUTO( r_water_waveheight,       "3.0",   FCVAR_GLCONFIG, "vertex wave displacement amplitude" );
CVAR_DEFINE_AUTO( r_water_wavefreq,         "0.04",  FCVAR_GLCONFIG, "vertex wave spatial frequency" );
CVAR_DEFINE_AUTO( r_water_wavespeed,        "1.0",   FCVAR_GLCONFIG, "vertex wave speed multiplier" );
CVAR_DEFINE_AUTO( r_water_refraction_speed, "1.0",   FCVAR_GLCONFIG, "refraction normalmap scroll speed" );
CVAR_DEFINE_AUTO( r_water_gamma,            "1.0",   FCVAR_GLCONFIG, "water brightness multiplier" );

gl_water_shader_state_t gWaterShader;

/* ---------- nanogl / wes / regal stubs -------------------------------- */
#if XASH_NANOGL || XASH_WES || XASH_REGAL

static void R_WaterShader_RegisterCvars( void )
{
	static qboolean reg = false;
	if( reg ) return;
	reg = true;
	gEngfuncs.Cvar_RegisterVariable( &r_water_shader );
	gEngfuncs.Cvar_RegisterVariable( &r_water_alpha );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fresnel );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fresnel_min );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fresnel_range );
	gEngfuncs.Cvar_RegisterVariable( &r_water_strength );
	gEngfuncs.Cvar_RegisterVariable( &r_water_strength_under );
	gEngfuncs.Cvar_RegisterVariable( &r_water_watercolor_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_watercolor_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_watercolor_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_distscale );
	gEngfuncs.Cvar_RegisterVariable( &r_water_distcolor_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_distcolor_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_distcolor_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fogblend );
	gEngfuncs.Cvar_RegisterVariable( &r_water_caustic_intensity );
	gEngfuncs.Cvar_RegisterVariable( &r_water_caustic_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_caustic_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_caustic_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_diffuse_overlay );
	gEngfuncs.Cvar_RegisterVariable( &r_water_refract );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwaterwarp );
	gEngfuncs.Cvar_RegisterVariable( &r_water_debug );
	gEngfuncs.Cvar_RegisterVariable( &r_water_waveheight );
	gEngfuncs.Cvar_RegisterVariable( &r_water_wavefreq );
	gEngfuncs.Cvar_RegisterVariable( &r_water_wavespeed );
	gEngfuncs.Cvar_RegisterVariable( &r_water_refraction_speed );
	gEngfuncs.Cvar_RegisterVariable( &r_water_gamma );
}

void R_WaterShader_Init( void )    { R_WaterShader_RegisterCvars(); memset( &gWaterShader, 0, sizeof( gWaterShader )); }
void R_WaterShader_Shutdown( void ) {}
void R_WaterShader_VidInit( void )  {}

qboolean R_WaterShader_EmitPolys( msurface_t *warp ) { (void)warp; return false; }
void R_WaterShader_UnderwaterWarp( void ) {}

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
 * Vertex shader
 *
 * Based on fteqw's water.glsl vertex shader but extended with optional
 * wave displacement so the water surface physically moves.
 *
 * Varyings match fteqw conventions:
 *   v_texCoord  = tc        (texture coords)
 *   v_clipPos   = tf        (clip-space position, used for screen UVs)
 *   v_normal    = norm      (surface normal)
 *   v_eye       = eye       (view direction)
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
	"uniform highp float u_waveSpeed;\n"
	"uniform vec3 u_cameraPos;\n"
	"varying vec2 v_texCoord;\n"
	"varying vec4 v_clipPos;\n"
	"varying vec3 v_normal;\n"
	"varying vec3 v_eye;\n"
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
	"    float t       = u_time * u_waveSpeed;\n"
	"    float freq    = max( u_wavefreq, 0.001 );\n"
	"    float amp     = u_waveheight;\n"
	"    if( amp > 0.001 )\n"
	"        pos.z += waveHeight( pos.xy, t, freq, amp );\n"
	"\n"
	"    vec4 viewPos = u_modelView * vec4( pos, 1.0 );\n"
	"    gl_Position = u_projection * viewPos;\n"
	"\n"
	"    v_texCoord = a_texCoord;\n"
	"    v_clipPos  = gl_Position;\n"
	"    v_normal   = vec3( 0.0, 0.0, 1.0 );\n"
	"    v_eye      = u_cameraPos - pos;\n"
	"}\n";

/* -----------------------------------------------------------------------
 * Above-water fragment shader
 *
 * Pure refraction shader:
 *   - screen-space UVs from clip-space position
 *   - Q1-style texture coordinate warping
 *   - two-layer scrolling normalmap (speed controlled by u_refractionSpeed)
 *   - Fresnel: pow(1-abs(dot(N,V)), EXP) * RANGE + MIN
 *   - refraction: screen-grab texture with normal distortion
 *   - Fresnel blends refracted scene with water body colour
 *   - diffuse (warp) texture overlay
 *   - fog
 * --------------------------------------------------------------------- */
static const char *water_frag_above_source =
	"#ifdef GL_ES\n"
	"precision mediump float;\n"
	"#endif\n"
	"uniform sampler2D u_normalMap;\n"
	"uniform sampler2D u_diffuseMap;\n"
	"uniform sampler2D u_refractMap;\n"
	"uniform highp float u_time;\n"
	"uniform float u_fresnelExp;\n"
	"uniform float u_fresnelMin;\n"
	"uniform float u_fresnelRange;\n"
	"uniform float u_strengthRefr;\n"
	"uniform vec3  u_waterColor;\n"
	"uniform float u_waterGamma;\n"
	"uniform float u_alpha;\n"
	"uniform float u_distScale;\n"
	"uniform vec3  u_distColor;\n"
	"uniform float u_fogBlend;\n"
	"uniform vec3  u_fogColor;\n"
	"uniform float u_fogStart;\n"
	"uniform float u_fogEnd;\n"
	"uniform float u_fogEnabled;\n"
	"uniform float u_refractEnabled;\n"
	"uniform float u_diffuseOverlay;\n"
	"uniform highp float u_refractionSpeed;\n"
	"uniform highp float u_waveSpeed;\n"
	"varying vec2  v_texCoord;\n"
	"varying vec4  v_clipPos;\n"
	"varying vec3  v_normal;\n"
	"varying vec3  v_eye;\n"
	"\n"
	"void main()\n"
	"{\n"
	"    vec2 stc = (1.0 + (v_clipPos.xy / v_clipPos.w)) * 0.5;\n"
	"\n"
	"    float t = u_time;\n"
	"    float rs = u_refractionSpeed;\n"
	"    float ws = u_waveSpeed;\n"
	"\n"
	"    /* Q1-style warp for normalmap coords */\n"
	"    vec2 ntc;\n"
	"    ntc.s = v_texCoord.s + sin( v_texCoord.t + t * ws ) * 0.125;\n"
	"    ntc.t = v_texCoord.t + sin( v_texCoord.s + t * ws ) * 0.125;\n"
	"\n"
	"    /* two-layer scrolling normalmap with refraction speed control */\n"
	"    vec3 n  = texture2D( u_normalMap, ntc * 1.0 + vec2( t * 0.10 * rs, 0.0 )).xyz;\n"
	"    n      += texture2D( u_normalMap, ntc * 0.5 - vec2( 0.0, t * 0.097 * rs )).xyz;\n"
	"    n      -= 1.0 - 4.0 / 256.0;\n"
	"    n       = normalize( n );\n"
	"\n"
	"    /* Fresnel term with min/range from fteqw */\n"
	"    float fres = pow( 1.0 - abs( dot( n, normalize( v_eye ))), u_fresnelExp )\n"
	"                 * u_fresnelRange + u_fresnelMin;\n"
	"\n"
	"    /* refraction = screen grab with distortion */\n"
	"    vec3 refr;\n"
	"    if( u_refractEnabled > 0.5 )\n"
	"        refr = texture2D( u_refractMap, stc + n.st * u_strengthRefr ).rgb;\n"
	"    else\n"
	"        refr = vec3( 0.12, 0.25, 0.33 );\n"
	"\n"
	"    /* distance-based depth tint */\n"
	"    float dist = length( v_eye );\n"
	"    float depthF = clamp( dist * u_distScale, 0.0, 1.0 );\n"
	"    refr = mix( refr, refr * u_distColor, depthF );\n"
	"\n"
	"    /* Fresnel blend between refraction and water body colour */\n"
	"    vec3 wc = u_waterColor * u_waterGamma;\n"
	"    vec3 color = mix( refr, wc, clamp( fres, 0.0, 0.95 ));\n"
	"\n"
	"    /* diffuse (warp) texture overlay */\n"
	"    if( u_diffuseOverlay > 0.001 )\n"
	"    {\n"
	"        vec4 ts = texture2D( u_diffuseMap, ntc );\n"
	"        color = mix( color, ts.rgb, min( u_diffuseOverlay, 1.0 ) * ts.a );\n"
	"    }\n"
	"\n"
	"    /* fog */\n"
	"    if( u_fogEnabled > 0.5 )\n"
	"    {\n"
	"        float fogF = clamp((dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);\n"
	"        color = mix( color, u_fogColor, fogF * u_fogBlend );\n"
	"    }\n"
	"\n"
	"    gl_FragColor = vec4( color, clamp( u_alpha, 0.0, 1.0 ));\n"
	"}\n";

/* -----------------------------------------------------------------------
 * Underwater fragment shader
 *
 * Used when the camera is underwater, looking up at the water surface:
 *   - screen-space UVs from clip-space position
 *   - Q1-style warp
 *   - two-layer scrolling normalmap
 *   - screen-grab distortion for refraction (objects above water appear to move)
 *   - caustic-like modulation
 *   - deep-water colour tint
 *   - fog
 * --------------------------------------------------------------------- */
static const char *water_frag_under_source =
	"#ifdef GL_ES\n"
	"precision mediump float;\n"
	"#endif\n"
	"uniform sampler2D u_normalMap;\n"
	"uniform sampler2D u_diffuseMap;\n"
	"uniform sampler2D u_refractMap;\n"
	"uniform highp float u_time;\n"
	"uniform float u_strengthRefr;\n"
	"uniform float u_strengthRefrUnder;\n"
	"uniform vec3  u_waterColor;\n"
	"uniform float u_waterGamma;\n"
	"uniform float u_alpha;\n"
	"uniform float u_fogBlend;\n"
	"uniform vec3  u_fogColor;\n"
	"uniform float u_fogStart;\n"
	"uniform float u_fogEnd;\n"
	"uniform float u_fogEnabled;\n"
	"uniform highp float u_refractionSpeed;\n"
	"uniform highp float u_waveSpeed;\n"
	"uniform float u_distScale;\n"
	"uniform vec3  u_distColor;\n"
	"uniform float u_causticIntensity;\n"
	"uniform vec3  u_causticColor;\n"
	"uniform float u_refractEnabled;\n"
	"varying vec2  v_texCoord;\n"
	"varying vec4  v_clipPos;\n"
	"varying vec3  v_normal;\n"
	"varying vec3  v_eye;\n"
	"\n"
	"float caustic( vec2 p, float t )\n"
	"{\n"
	"    float c1 = sin( p.x * 3.0 + p.y * 2.0 + t * 0.8 ) * cos( p.y * 2.5 - t * 0.6 );\n"
	"    float c2 = sin((p.x + p.y) * 4.0 + t * 1.2 ) * 0.5;\n"
	"    float c3 = cos((p.x - p.y) * 5.0 - t * 0.9 ) * 0.3;\n"
	"    float c4 = sin( p.x * 6.0 - p.y * 3.0 + t * 1.5 ) * 0.2;\n"
	"    return clamp( c1 * 0.5 + c2 + c3 + c4, 0.0, 1.0 );\n"
	"}\n"
	"\n"
	"void main()\n"
	"{\n"
	"    vec2 stc = (1.0 + (v_clipPos.xy / v_clipPos.w)) * 0.5;\n"
	"\n"
	"    float t = u_time;\n"
	"    float rs = u_refractionSpeed;\n"
	"    float ws = u_waveSpeed;\n"
	"\n"
	"    /* water colour and distance */\n"
	"    float dist = length( v_eye );\n"
	"    float depthF = clamp( dist * u_distScale * 0.5, 0.0, 1.0 );\n"
	"    vec3 wc = u_waterColor * u_waterGamma;\n"
	"\n"
	"    /* Q1-style warp */\n"
	"    vec2 ntc;\n"
	"    ntc.s = v_texCoord.s + sin( v_texCoord.t + t * ws ) * 0.125;\n"
	"    ntc.t = v_texCoord.t + sin( v_texCoord.s + t * ws ) * 0.125;\n"
	"\n"
	"    /* two-layer scrolling normalmap */\n"
	"    vec3 n  = texture2D( u_normalMap, ntc * 1.0 + vec2( t * 0.10 * rs, 0.0 )).xyz;\n"
	"    n      += texture2D( u_normalMap, ntc * 0.5 - vec2( 0.0, t * 0.097 * rs )).xyz;\n"
	"    n      -= 1.0 - 4.0 / 256.0;\n"
	"    n       = normalize( n );\n"
	"\n"
	"    /* normal-based surface animation visible even when fog is heavy */\n"
	"    float surfAnim = sin( ntc.s * 8.0 + t * 0.5 ) * cos( ntc.t * 6.0 - t * 0.7 ) * 0.5 + 0.5;\n"
	"\n"
	"    /* caustic pattern */\n"
	"    float c = caustic( gl_FragCoord.xy, t * rs );\n"
	"    c = c * 0.5 + surfAnim * 0.5;\n"
	"\n"
	"    /* refraction: distort the screen grab with normal + separate underwater strength */\n"
	"    vec3 refr;\n"
	"    if( u_refractEnabled > 0.5 )\n"
	"    {\n"
	"        float twitch = sin( t * 0.5 + ntc.s * 10.0 ) * 0.003;\n"
	"        refr = texture2D( u_refractMap, stc + n.st * u_strengthRefrUnder + vec2( twitch, twitch * 0.7 )).rgb;\n"
	"    }\n"
	"    else\n"
	"    {\n"
	"        refr = wc * 0.6;\n"
	"    }\n"
	"\n"
	"    /* distance-based tint */\n"
	"    refr = mix( refr, refr * u_distColor, depthF );\n"
	"\n"
	"    /* underwater colour: blend screen with water color, caustics, and animated surface */\n"
	"    float wcBlend = depthF * 0.4;\n"
	"    vec3 color = mix( refr, wc * 0.6, wcBlend );\n"
	"\n"
	"    /* apply caustic light with configurable color and intensity */\n"
	"    float cStrength = u_causticIntensity * (1.0 - depthF * 0.5);\n"
	"    color += u_causticColor * c * cStrength;\n"
	"\n"
	"    /* subtle surface shimmer from normal map */\n"
	"    vec3 shimmer = vec3( 0.02, 0.03, 0.02 ) * (n.x + n.y) * (1.0 - depthF);\n"
	"    color += shimmer;\n"
	"\n"
	"    /* fog */\n"
	"    if( u_fogEnabled > 0.5 )\n"
	"    {\n"
	"        float fogF = clamp((dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);\n"
	"        color = mix( color, u_fogColor, fogF * u_fogBlend );\n"
	"    }\n"
	"\n"
	"    gl_FragColor = vec4( color, clamp( u_alpha, 0.0, 1.0 ));\n"
	"}\n";

/* -----------------------------------------------------------------------
 * Underwater fullscreen warp (post-processing) shaders
 *
 * Drawn as a full-screen quad when the view is underwater to simulate
 * refractive distortion of the entire screen (weapons, walls, etc.).
 *
 * Based on fteqw's underwaterwarp.glsl but simplified: uses the normal
 * map for distortion instead of a dedicated warp texture, so no
 * additional texture assets are needed.
 * --------------------------------------------------------------------- */
static const char *water_warp_vertex_source =
	"#ifdef GL_ES\n"
	"precision highp float;\n"
	"#endif\n"
	"attribute vec2 a_position;\n"
	"attribute vec2 a_texCoord;\n"
	"varying vec2 v_texCoord;\n"
	"void main()\n"
	"{\n"
	"    gl_Position = vec4( a_position, 0.0, 1.0 );\n"
	"    v_texCoord = a_texCoord;\n"
	"}\n";

static const char *water_warp_frag_source =
	"#ifdef GL_ES\n"
	"precision mediump float;\n"
	"#endif\n"
	"uniform sampler2D u_refractMap;\n"
	"uniform sampler2D u_normalMap;\n"
	"uniform highp float u_time;\n"
	"uniform float u_warpStrength;\n"
	"varying vec2 v_texCoord;\n"
	"\n"
	"void main()\n"
	"{\n"
	"    vec2 uv = v_texCoord;\n"
	"\n"
	"    /* sample normal map for distortion */\n"
	"    vec2 ntc = uv * 2.0 + vec2( u_time * 0.05, u_time * 0.03 );\n"
	"    vec3 n  = texture2D( u_normalMap, ntc ).xyz;\n"
	"    n      += texture2D( u_normalMap, ntc * 0.5 - vec2( 0.0, u_time * 0.04 )).xyz;\n"
	"    n      -= 1.0 - 4.0 / 256.0;\n"
	"\n"
	"    /* apply distortion with strength control */\n"
	"    float strength = u_warpStrength * 0.025;\n"
	"    uv += n.st * strength;\n"
	"\n"
	"    /* fallback sin/cos wobble when normal map is flat */\n"
	"    uv.x += sin( uv.y * 40.0 + u_time * 1.5 ) * strength * 0.3;\n"
	"    uv.y += cos( uv.x * 35.0 + u_time * 1.2 ) * strength * 0.3;\n"
	"\n"
	"    gl_FragColor = texture2D( u_screenMap, uv );\n"
	"}\n";

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

	p->u_modelView      = pglGetUniformLocationARB( p->program, "u_modelView" );
	p->u_projection     = pglGetUniformLocationARB( p->program, "u_projection" );
	p->u_normalMap      = pglGetUniformLocationARB( p->program, "u_normalMap" );
	p->u_diffuseMap     = pglGetUniformLocationARB( p->program, "u_diffuseMap" );
	p->u_refractMap     = pglGetUniformLocationARB( p->program, "u_refractMap" );
	p->u_cameraPos      = pglGetUniformLocationARB( p->program, "u_cameraPos" );
	p->u_time           = pglGetUniformLocationARB( p->program, "u_time" );
	p->u_fresnelExp     = pglGetUniformLocationARB( p->program, "u_fresnelExp" );
	p->u_fresnelMin     = pglGetUniformLocationARB( p->program, "u_fresnelMin" );
	p->u_fresnelRange   = pglGetUniformLocationARB( p->program, "u_fresnelRange" );
	p->u_strengthRefr      = pglGetUniformLocationARB( p->program, "u_strengthRefr" );
	p->u_strengthRefrUnder = pglGetUniformLocationARB( p->program, "u_strengthRefrUnder" );
	p->u_waterColor        = pglGetUniformLocationARB( p->program, "u_waterColor" );
	p->u_waterGamma        = pglGetUniformLocationARB( p->program, "u_waterGamma" );
	p->u_alpha             = pglGetUniformLocationARB( p->program, "u_alpha" );
	p->u_distScale         = pglGetUniformLocationARB( p->program, "u_distScale" );
	p->u_distColor         = pglGetUniformLocationARB( p->program, "u_distColor" );
	p->u_causticIntensity  = pglGetUniformLocationARB( p->program, "u_causticIntensity" );
	p->u_causticColor      = pglGetUniformLocationARB( p->program, "u_causticColor" );
	p->u_diffuseOverlay    = pglGetUniformLocationARB( p->program, "u_diffuseOverlay" );
	p->u_fogBlend       = pglGetUniformLocationARB( p->program, "u_fogBlend" );
	p->u_fogColor       = pglGetUniformLocationARB( p->program, "u_fogColor" );
	p->u_fogStart       = pglGetUniformLocationARB( p->program, "u_fogStart" );
	p->u_fogEnd         = pglGetUniformLocationARB( p->program, "u_fogEnd" );
	p->u_fogEnabled     = pglGetUniformLocationARB( p->program, "u_fogEnabled" );
	p->u_waveheight     = pglGetUniformLocationARB( p->program, "u_waveheight" );
	p->u_wavefreq       = pglGetUniformLocationARB( p->program, "u_wavefreq" );
	p->u_waveSpeed      = pglGetUniformLocationARB( p->program, "u_waveSpeed" );
	p->u_refractionSpeed = pglGetUniformLocationARB( p->program, "u_refractionSpeed" );
	p->u_refractEnabled = pglGetUniformLocationARB( p->program, "u_refractEnabled" );
	p->u_warpStrength   = pglGetUniformLocationARB( p->program, "u_warpStrength" );

	p->a_position = WATER_ATTRIB_POSITION;
	p->a_texCoord = WATER_ATTRIB_TEXCOORD;
	return true;
}

/* ---------------------------------------------------------------------- */
/* Normal-map loading / generation                                        */
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

static GLuint R_WaterShader_UploadTexture( const byte *data, int size,
                                            GLenum target, GLint level,
                                            GLint internal, GLenum format )
{
	GLuint tex = 0;
	pglGenTextures( 1, &tex );
	pglBindTexture( target, tex );
	pglTexParameteri( target, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	pglTexParameteri( target, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	pglTexParameteri( target, GL_TEXTURE_WRAP_S, GL_REPEAT );
	pglTexParameteri( target, GL_TEXTURE_WRAP_T, GL_REPEAT );
	pglTexImage2D( target, level, internal, size, size, 0, format, GL_UNSIGNED_BYTE, data );
	return tex;
}

static GLuint R_WaterShader_UploadProceduralNormal( void )
{
	const int size = WATER_PROC_NORMAL_SIZE;
	byte *buf = (byte *)malloc( size * size * 3 );
	if( !buf ) return 0;
	R_WaterShader_GenerateProceduralNormal( buf, size );
	GLuint tex = R_WaterShader_UploadTexture( buf, size, GL_TEXTURE_2D, 0, GL_RGB, GL_RGB );
	free( buf );
	return tex;
}

/* ---------------------------------------------------------------------- */
/* Screen grab texture (refraction source)                                */
/* ---------------------------------------------------------------------- */

static void R_WaterShader_CreateScreenGrabTexture( int width, int height )
{
	if( !gWaterShader.screenGrabTexture )
	{
		pglGenTextures( 1, &gWaterShader.screenGrabTexture );
	}

	pglBindTexture( GL_TEXTURE_2D, gWaterShader.screenGrabTexture );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	/* allocate storage (will be filled by pglCopyTexSubImage2D each frame) */
	pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL );

	gWaterShader.framebufferWidth  = width;
	gWaterShader.framebufferHeight = height;
	gWaterShader.lastFrameCaptured = -1;
}

/* ---------------------------------------------------------------------- */
/* Normal-map loading                                                     */
/* ---------------------------------------------------------------------- */

static void R_WaterShader_LoadNormalTexture( void )
{
	int idx = GL_LoadTexture( "gfx/water/water_normal_0.tga", NULL, 0, TF_NORMALMAP );
	if( !idx )
		idx = GL_LoadTexture( "gfx/textures/water_normal_0.tga", NULL, 0, TF_NORMALMAP );
	if( idx )
	{
		const gl_texture_t *t = R_GetTexture( idx );
		if( t && t->texnum )
		{
			gWaterShader.normalTexture = t->texnum;
			gWaterShader.normalFromFile = 1;
			gEngfuncs.Con_Reportf( "R_WaterShader: loaded normalmap\n" );
			return;
		}
	}
	gWaterShader.normalTexture = R_WaterShader_UploadProceduralNormal();
	gWaterShader.normalFromFile = 0;
	if( gWaterShader.normalTexture )
		gEngfuncs.Con_Reportf( "R_WaterShader: using procedural normalmap\n" );
}

/* ---------------------------------------------------------------------- */
/* Init / shutdown                                                        */
/* ---------------------------------------------------------------------- */

static void R_WaterShader_RegisterCvars( void )
{
	static qboolean reg = false;
	if( reg ) return;
	reg = true;
	gEngfuncs.Cvar_RegisterVariable( &r_water_shader );
	gEngfuncs.Cvar_RegisterVariable( &r_water_alpha );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fresnel );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fresnel_min );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fresnel_range );
	gEngfuncs.Cvar_RegisterVariable( &r_water_strength );
	gEngfuncs.Cvar_RegisterVariable( &r_water_strength_under );
	gEngfuncs.Cvar_RegisterVariable( &r_water_watercolor_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_watercolor_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_watercolor_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_distscale );
	gEngfuncs.Cvar_RegisterVariable( &r_water_distcolor_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_distcolor_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_distcolor_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fogblend );
	gEngfuncs.Cvar_RegisterVariable( &r_water_caustic_intensity );
	gEngfuncs.Cvar_RegisterVariable( &r_water_caustic_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_caustic_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_caustic_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_diffuse_overlay );
	gEngfuncs.Cvar_RegisterVariable( &r_water_refract );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwaterwarp );
	gEngfuncs.Cvar_RegisterVariable( &r_water_debug );
	gEngfuncs.Cvar_RegisterVariable( &r_water_waveheight );
	gEngfuncs.Cvar_RegisterVariable( &r_water_wavefreq );
	gEngfuncs.Cvar_RegisterVariable( &r_water_wavespeed );
	gEngfuncs.Cvar_RegisterVariable( &r_water_refraction_speed );
	gEngfuncs.Cvar_RegisterVariable( &r_water_gamma );
}

void R_WaterShader_Init( void )
{
	R_WaterShader_RegisterCvars();

	if( gWaterShader.initialized )
		return;

	memset( &gWaterShader, 0, sizeof( gWaterShader ));
	gWaterShader.lastFrameCaptured = -1;

	if( !GL_Support( GL_SHADER_GLSL100_EXT ))
	{
		gEngfuncs.Con_Printf( "R_WaterShader: GLSL not supported, disabled\n" );
		gWaterShader.initialized = 1;	// mark as attempted so we don't retry every frame
		return;
	}

	if( !R_WaterShader_CompileProgram( &gWaterShader.program,
	                                   water_vertex_source, water_frag_above_source ))
	{
		gEngfuncs.Con_Printf( S_ERROR "R_WaterShader: failed to build above-water program\n" );
		gWaterShader.initialized = 1;
		return;
	}

	if( !R_WaterShader_CompileProgram( &gWaterShader.programUnderwater,
	                                   water_vertex_source, water_frag_under_source ))
	{
		gEngfuncs.Con_Printf( S_ERROR "R_WaterShader: failed to build underwater program\n" );
		R_WaterShader_DeleteProgram( &gWaterShader.program );
		gWaterShader.initialized = 1;
		return;
	}

	gWaterShader.shaderSupport = 1;
	gWaterShader.initialized   = 1;

	gEngfuncs.Con_Reportf( "R_WaterShader: ready (refraction + underwater)\n" );

	/* warp program is optional (non-fatal if it fails) */
	if( !R_WaterShader_CompileProgram( &gWaterShader.warpProgram,
	                                   water_warp_vertex_source, water_warp_frag_source ))
	{
		gEngfuncs.Con_Printf( S_ERROR "R_WaterShader: failed to build underwater warp program\n" );
	}
	else
	{
		gEngfuncs.Con_Reportf( "R_WaterShader: underwater warp ready\n" );
	}
}

void R_WaterShader_Shutdown( void )
{
	if( !gWaterShader.initialized )
		return;

	if( glw_state.initialized )
	{
		R_WaterShader_DeleteProgram( &gWaterShader.program );
		R_WaterShader_DeleteProgram( &gWaterShader.programUnderwater );
		R_WaterShader_DeleteProgram( &gWaterShader.warpProgram );

		if( gWaterShader.normalTexture )
			pglDeleteTextures( 1, &gWaterShader.normalTexture );
		if( gWaterShader.screenGrabTexture )
			pglDeleteTextures( 1, &gWaterShader.screenGrabTexture );
		if( gWaterShader.warpScreenTexture )
			pglDeleteTextures( 1, &gWaterShader.warpScreenTexture );
		if( gWaterShader.warpNormalTexture )
			pglDeleteTextures( 1, &gWaterShader.warpNormalTexture );
	}

	memset( &gWaterShader, 0, sizeof( gWaterShader ));
	gWaterShader.lastFrameCaptured = -1;
	gWaterShader.lastWarpCaptured = -1;
}

void R_WaterShader_VidInit( void )
{
	/* delete old textures before re-creating */
	if( gWaterShader.normalTexture )
	{
		pglDeleteTextures( 1, &gWaterShader.normalTexture );
		gWaterShader.normalTexture = 0;
	}
	if( gWaterShader.screenGrabTexture )
	{
		pglDeleteTextures( 1, &gWaterShader.screenGrabTexture );
		gWaterShader.screenGrabTexture = 0;
	}
	if( gWaterShader.warpScreenTexture )
	{
		pglDeleteTextures( 1, &gWaterShader.warpScreenTexture );
		gWaterShader.warpScreenTexture = 0;
	}

	R_WaterShader_LoadNormalTexture();

	if( gpGlobals->width > 0 && gpGlobals->height > 0 )
	{
		R_WaterShader_CreateScreenGrabTexture(
		    gpGlobals->width, gpGlobals->height );
	}

	/* warp screen capture texture */
	{
		int w = gpGlobals->width;
		int h = gpGlobals->height;
		if( w > 0 && h > 0 )
		{
			pglGenTextures( 1, &gWaterShader.warpScreenTexture );
			pglBindTexture( GL_TEXTURE_2D, gWaterShader.warpScreenTexture );
			pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
			pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
			pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
			pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
			pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL );
			gWaterShader.lastWarpCaptured = -1;
		}
	}
}

/* ---------------------------------------------------------------------- */
/* Per-surface drawing                                                    */
/* ---------------------------------------------------------------------- */

static void R_WaterShader_SetUniforms( gl_water_program_t *prog,
                                        msurface_t *warp )
{
	/* ---- matrices ---- */
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

	/*
	 * Camera position in model-local space.
	 * For world water, RI.objectMatrix is identity, so this is a no-op.
	 * For entity water (func_water), transforms the camera from world space
	 * to the brush model's local space, matching the vertex positions.
	 */
	if( prog->u_cameraPos >= 0 )
	{
		vec3_t localOrigin;
		Matrix4x4_VectorITransform( RI.objectMatrix, RI.rvp.vieworigin, localOrigin );
		pglUniform3fARB( prog->u_cameraPos,
		                 localOrigin[0], localOrigin[1], localOrigin[2] );
	}

	float time = (float)gp_cl->time;

	/* ---- time ---- */
	if( prog->u_time >= 0 )
		pglUniform1fARB( prog->u_time, time );

	/* ---- wave speed ---- */
	if( prog->u_waveSpeed >= 0 )
		pglUniform1fARB( prog->u_waveSpeed, r_water_wavespeed.value );

	/* ---- refraction speed ---- */
	if( prog->u_refractionSpeed >= 0 )
		pglUniform1fARB( prog->u_refractionSpeed, r_water_refraction_speed.value );

	/* ---- Fresnel exponent (above-water only) ---- */
	if( prog->u_fresnelExp >= 0 )
	{
		float fexp = r_water_fresnel.value;
		if( fexp < 0.1f ) fexp = 4.0f;
		pglUniform1fARB( prog->u_fresnelExp, fexp );
	}
	if( prog->u_fresnelMin >= 0 )
		pglUniform1fARB( prog->u_fresnelMin, r_water_fresnel_min.value );
	if( prog->u_fresnelRange >= 0 )
		pglUniform1fARB( prog->u_fresnelRange, r_water_fresnel_range.value );

	/* ---- refraction strength (above) ---- */
	if( prog->u_strengthRefr >= 0 )
		pglUniform1fARB( prog->u_strengthRefr, r_water_strength.value );

	/* ---- refraction strength (underwater) ---- */
	if( prog->u_strengthRefrUnder >= 0 )
		pglUniform1fARB( prog->u_strengthRefrUnder, r_water_strength_under.value );

	/* ---- water colour ---- */
	if( prog->u_waterColor >= 0 )
		pglUniform3fARB( prog->u_waterColor,
		                 r_water_watercolor_r.value / 255.0f,
		                 r_water_watercolor_g.value / 255.0f,
		                 r_water_watercolor_b.value / 255.0f );
	if( prog->u_waterGamma >= 0 )
		pglUniform1fARB( prog->u_waterGamma, r_water_gamma.value );

	/* ---- alpha ---- */
	{
		float a = r_water_alpha.value;
		cl_entity_t *e = RI.currententity;
		if( e )
		{
			qboolean alphaModified = ( r_water_alpha.value != 0.85f );
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
			a = 1.0f;

		if( r_water_refract.value >= 2.0f )
			a = 1.0f;	// opaque: hide normal underwater objects, show only refracted

		if( prog->u_alpha >= 0 )
			pglUniform1fARB( prog->u_alpha, Q_min( 1.0f, Q_max( 0.0f, a )));
	}

	/* ---- distance scale ---- */
	if( prog->u_distScale >= 0 )
		pglUniform1fARB( prog->u_distScale, r_water_distscale.value );

	/* ---- distance tint color ---- */
	if( prog->u_distColor >= 0 )
		pglUniform3fARB( prog->u_distColor,
		                 r_water_distcolor_r.value / 255.0f,
		                 r_water_distcolor_g.value / 255.0f,
		                 r_water_distcolor_b.value / 255.0f );

	/* ---- caustic intensity ---- */
	if( prog->u_causticIntensity >= 0 )
		pglUniform1fARB( prog->u_causticIntensity, r_water_caustic_intensity.value );

	/* ---- caustic color ---- */
	if( prog->u_causticColor >= 0 )
		pglUniform3fARB( prog->u_causticColor,
		                 r_water_caustic_color_r.value / 255.0f,
		                 r_water_caustic_color_g.value / 255.0f,
		                 r_water_caustic_color_b.value / 255.0f );

	/* ---- diffuse overlay ---- */
	if( prog->u_diffuseOverlay >= 0 )
		pglUniform1fARB( prog->u_diffuseOverlay, r_water_diffuse_overlay.value );

	/* ---- fog ---- */
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

	/* ---- wave uniforms ---- */
	if( prog->u_waveheight >= 0 )
	{
		float wh = r_water_waveheight.value;
		cl_entity_t *ec = RI.currententity;
		if( ec && ec->curstate.scale > 0.001f && fabsf( ec->curstate.scale - 1.0f ) > 0.001f )
			wh *= ec->curstate.scale;
		pglUniform1fARB( prog->u_waveheight, wh );
	}
	if( prog->u_wavefreq >= 0 )
		pglUniform1fARB( prog->u_wavefreq, r_water_wavefreq.value );

	/* ------------------------------------------------------------------ */
	/* Screen grab: copy current framebuffer to texture once per frame     */
	/* ------------------------------------------------------------------ */
	if( r_water_refract.value > 0.5f &&
	    gWaterShader.screenGrabTexture &&
	    gWaterShader.framebufferWidth > 0 && gWaterShader.framebufferHeight > 0 )
	{
		if( gWaterShader.lastFrameCaptured != tr.framecount )
		{
			gWaterShader.lastFrameCaptured = tr.framecount;
			pglBindTexture( GL_TEXTURE_2D, gWaterShader.screenGrabTexture );
			pglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0,
			                      0, 0,
			                      gWaterShader.framebufferWidth,
			                      gWaterShader.framebufferHeight );
		}
		if( prog->u_refractEnabled >= 0 )
			pglUniform1fARB( prog->u_refractEnabled, 1.0f );
	}
	else
	{
		if( prog->u_refractEnabled >= 0 )
			pglUniform1fARB( prog->u_refractEnabled, 0.0f );
	}
}

qboolean R_WaterShader_EmitPolys( msurface_t *warp )
{
	if( !r_water_shader.value )            return false;
	if( !warp || !warp->polys )            return false;

	/* lazy init: ensure programs are compiled */
	if( !gWaterShader.program.program || !gWaterShader.programUnderwater.program )
	{
		R_WaterShader_Init();
		if( !gWaterShader.program.program || !gWaterShader.programUnderwater.program )
			return false;

		/* ensure textures are created (VidInit might have been skipped) */
		if( !gWaterShader.screenGrabTexture )
			R_WaterShader_VidInit();
	}

	/* above-water vs underwater */
	/*
	 * When the player is physically in water (waterlevel >= 2), use the
	 * underwater shader for ALL water surfaces.  The vertex-height check
	 * works for BSP world water, but func_water entities may have
	 * inconsistent poly Z values when the camera is inside the brush.
	 */
	qboolean underwater;
	if( ENGINE_GET_PARM( PARM_WATER_LEVEL ) >= 2 )
		underwater = true;
	else
		underwater = ( warp->polys->verts[0][2] >= RI.rvp.vieworigin[2] );

	gl_water_program_t *prog = underwater
	    ? &gWaterShader.programUnderwater
	    : &gWaterShader.program;

	pglUseProgramObjectARB( prog->program );

	R_WaterShader_SetUniforms( prog, warp );

	/* ---- bind textures ---- */

	/* unit 0: normal map */
	pglActiveTextureARB( GL_TEXTURE0_ARB );
	pglBindTexture( GL_TEXTURE_2D, gWaterShader.normalTexture );
	if( prog->u_normalMap >= 0 )
		pglUniform1iARB( prog->u_normalMap, 0 );

	/* unit 1: diffuse (warp) texture */
	if( prog->u_diffuseMap >= 0 )
	{
		pglActiveTextureARB( GL_TEXTURE1_ARB );
		texture_t *wt = warp->texinfo ? warp->texinfo->texture : NULL;
		GLuint wtNum = ( wt && wt->fb_texturenum ) ? wt->fb_texturenum : gWaterShader.normalTexture;
		pglBindTexture( GL_TEXTURE_2D, wtNum );
		pglUniform1iARB( prog->u_diffuseMap, 1 );
	}

	/* unit 2: refraction (screen grab) */
	if( prog->u_refractMap >= 0 && gWaterShader.screenGrabTexture
	    && r_water_refract.value > 0.5f )
	{
		pglActiveTextureARB( GL_TEXTURE0_ARB + 2 );
		pglBindTexture( GL_TEXTURE_2D, gWaterShader.screenGrabTexture );
		pglUniform1iARB( prog->u_refractMap, 2 );
	}

	pglActiveTextureARB( GL_TEXTURE0_ARB );

	/* ---- render state ---- */
	pglDepthMask( GL_FALSE );
	pglEnable( GL_DEPTH_TEST );
	pglDepthFunc( GL_LEQUAL );
	pglEnable( GL_BLEND );
	pglBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	pglDisable( GL_CLIP_PLANE0 );
	pglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );

	pglEnableVertexAttribArrayARB( prog->a_position );
	pglEnableVertexAttribArrayARB( prog->a_texCoord );

	pglDisable( GL_CULL_FACE );

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

	pglEnable( GL_CULL_FACE );
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

/* ------------------------------------------------------------------ */
/* Underwater fullscreen warp (post-processing)                        */
/* ------------------------------------------------------------------ */
void R_WaterShader_UnderwaterWarp( void )
{
	if( !gWaterShader.shaderSupport )                    return;
	if( !r_water_underwaterwarp.value )                  return;
	if( !gWaterShader.warpProgram.program )               return;
	if( !gWaterShader.warpScreenTexture )                 return;
	if( ENGINE_GET_PARM( PARM_WATER_LEVEL ) < 2 )         return;

	int w = gpGlobals->width;
	int h = gpGlobals->height;
	if( w < 1 || h < 1 ) return;

	/* capture current framebuffer */
	pglBindTexture( GL_TEXTURE_2D, gWaterShader.warpScreenTexture );
	pglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h );

	gl_water_program_t *prog = &gWaterShader.warpProgram;
	pglUseProgramObjectARB( prog->program );

	/* uniforms */
	if( prog->u_time >= 0 )
		pglUniform1fARB( prog->u_time, (float)gp_cl->time );

	if( prog->u_warpStrength >= 0 )
		pglUniform1fARB( prog->u_warpStrength, r_water_underwaterwarp.value );

	/* textures */
	/* unit 0: screen capture */
	pglActiveTextureARB( GL_TEXTURE0_ARB );
	pglBindTexture( GL_TEXTURE_2D, gWaterShader.warpScreenTexture );
	if( prog->u_refractMap >= 0 )
		pglUniform1iARB( prog->u_refractMap, 0 );

	/* unit 1: normal map (for distortion) */
	pglActiveTextureARB( GL_TEXTURE1_ARB );
	pglBindTexture( GL_TEXTURE_2D, gWaterShader.normalTexture ? gWaterShader.normalTexture : gWaterShader.warpScreenTexture );
	if( prog->u_normalMap >= 0 )
		pglUniform1iARB( prog->u_normalMap, 1 );

	/* state */
	pglDisable( GL_DEPTH_TEST );
	pglDisable( GL_BLEND );
	pglDisable( GL_CULL_FACE );
	pglDepthMask( GL_FALSE );

	/* fullscreen quad vertices (NDC) */
	static const float verts[] = {
		-1.0f, -1.0f,
		 1.0f, -1.0f,
		-1.0f,  1.0f,
		 1.0f,  1.0f
	};
	static const float uvs[] = {
		0.0f, 0.0f,
		1.0f, 0.0f,
		0.0f, 1.0f,
		1.0f, 1.0f
	};

	pglEnableVertexAttribArrayARB( prog->a_position );
	pglEnableVertexAttribArrayARB( prog->a_texCoord );
	pglVertexAttribPointerARB( prog->a_position, 2, GL_FLOAT, GL_FALSE, 0, verts );
	pglVertexAttribPointerARB( prog->a_texCoord, 2, GL_FLOAT, GL_FALSE, 0, uvs );
	pglDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
	pglDisableVertexAttribArrayARB( prog->a_position );
	pglDisableVertexAttribArrayARB( prog->a_texCoord );

	pglEnable( GL_DEPTH_TEST );
	pglDepthMask( GL_TRUE );

	pglUseProgramObjectARB( 0 );
	pglActiveTextureARB( GL_TEXTURE0_ARB );
	pglBindTexture( GL_TEXTURE_2D, 0 );
}

#endif  /* !XASH_NANOGL && !XASH_WES && !XASH_REGAL */
