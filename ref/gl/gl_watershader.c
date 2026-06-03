/*
 * PrimeXT-inspired water shader for xash3d-fwgs (gl4es / GLSL ES 1.00).
 *
 * Replaces the earlier broken r_water_shader implementation.
 *
 * The original code tried to perform real reflection/refraction passes
 * through FBOs but never actually rendered the scene into them, so the
 * shader was sampling uninitialised colour textures. This rewrite keeps
 * the externally-visible cvar (r_water_shader) but borrows the technique
 * used by PrimeXT (game_dir/glsl/forward/scene_bmodel_fp.glsl + fresnel.h):
 *
 *   - animated water normalmaps (gfx/water/water_normal_<N>)
 *   - Schlick Fresnel with WATER_F0_VALUE = 0.15 and FRESNEL_FACTOR = 5.0
 *   - depth-based water tint (waterBorderFactor analogue)
 *   - separate above-water / underwater fragment programs (LIQUID_UNDERWATER)
 *
 * No FBOs are used; the "reflection" is a Fresnel-tinted sky colour plus a
 * sun-like specular term, which is what mod authors usually configure in
 * PrimeXT's water material when running without planar reflections.
 */

#include "gl_local.h"
#include "gl_watershader.h"

CVAR_DEFINE_AUTO( r_water_shader,             "0",     FCVAR_GLCONFIG, "enable PrimeXT-style water shader" );
CVAR_DEFINE_AUTO( r_water_alpha,              "0.70",  FCVAR_GLCONFIG, "above-water alpha multiplier (0=transparent, 1=opaque)" );
CVAR_DEFINE_AUTO( r_water_ambient,            "0.55",  FCVAR_GLCONFIG, "water body brightness - lowers fullbright look (0..1)" );
CVAR_DEFINE_AUTO( r_water_density,            "0.50",  FCVAR_GLCONFIG, "above-water depth-based tint strength (0..1)" );
CVAR_DEFINE_AUTO( r_water_normalscale,        "1.0",   FCVAR_GLCONFIG, "bump-map effect strength (0..2)" );
CVAR_DEFINE_AUTO( r_water_choppy,             "0.04",  FCVAR_GLCONFIG, "vertex choppy wave offset (0..0.2)" );
CVAR_DEFINE_AUTO( r_water_wave,               "1",     FCVAR_GLCONFIG, "enable wave/ripple animation (0=static, 1=animated)" );
CVAR_DEFINE_AUTO( r_water_animspeed,          "1.0",   FCVAR_GLCONFIG, "wave animation speed multiplier (0=frozen, 2=double speed)" );
CVAR_DEFINE_AUTO( r_water_waveheight,         "0.5",   FCVAR_GLCONFIG, "vertex displacement amplitude in game units (0=flat, 2=storm)" );
CVAR_DEFINE_AUTO( r_water_wavefreq,           "0.05",  FCVAR_GLCONFIG, "vertex wave spatial frequency (0.01=long swells, 0.2=chop)" );
CVAR_DEFINE_AUTO( r_water_specular_min,       "0.15",  FCVAR_GLCONFIG, "sun specular floor in dark wave troughs (0..1)" );
CVAR_DEFINE_AUTO( r_water_specular,           "1.0",   FCVAR_GLCONFIG, "sun specular highlight intensity (0..2)" );
CVAR_DEFINE_AUTO( r_water_specular_color_r,   "255",   FCVAR_GLCONFIG, "sun specular highlight red (0-255)" );
CVAR_DEFINE_AUTO( r_water_specular_color_g,   "246",   FCVAR_GLCONFIG, "sun specular highlight green (0-255)" );
CVAR_DEFINE_AUTO( r_water_specular_color_b,   "217",   FCVAR_GLCONFIG, "sun specular highlight blue (0-255)" );
CVAR_DEFINE_AUTO( r_water_skyblend,           "1.0",   FCVAR_GLCONFIG, "sky reflection contribution (0..2)" );
CVAR_DEFINE_AUTO( r_water_skycolor_r,         "200",   FCVAR_GLCONFIG, "fake sky reflection red (0-255)" );
CVAR_DEFINE_AUTO( r_water_skycolor_g,         "230",   FCVAR_GLCONFIG, "fake sky reflection green (0-255)" );
CVAR_DEFINE_AUTO( r_water_skycolor_b,         "255",   FCVAR_GLCONFIG, "fake sky reflection blue (0-255)" );
CVAR_DEFINE_AUTO( r_water_underwater_alpha,   "1.0",   FCVAR_GLCONFIG, "underwater pass alpha (0..1)" );
CVAR_DEFINE_AUTO( r_water_underwater_color_r, "24",    FCVAR_GLCONFIG, "underwater base color red (0-255)" );
CVAR_DEFINE_AUTO( r_water_underwater_color_g, "48",    FCVAR_GLCONFIG, "underwater base color green (0-255)" );
CVAR_DEFINE_AUTO( r_water_underwater_color_b, "64",    FCVAR_GLCONFIG, "underwater base color blue (0-255)" );
CVAR_DEFINE_AUTO( r_water_underwater_density, "0.70",  FCVAR_GLCONFIG, "underwater depth-based tint strength (0..1)" );
CVAR_DEFINE_AUTO( r_water_fresnel,            "5.0",   FCVAR_GLCONFIG, "Fresnel exponent (PrimeXT uses 5.0)" );
CVAR_DEFINE_AUTO( r_water_fogblend,           "1.0",   FCVAR_GLCONFIG, "fog influence on water (0..1)" );
CVAR_DEFINE_AUTO( r_water_color_r,            "32",    FCVAR_GLCONFIG, "default water color red (0-255)" );
CVAR_DEFINE_AUTO( r_water_color_g,            "64",    FCVAR_GLCONFIG, "default water color green (0-255)" );
CVAR_DEFINE_AUTO( r_water_color_b,            "80",    FCVAR_GLCONFIG, "default water color blue (0-255)" );
CVAR_DEFINE_AUTO( r_water_debug,              "0",     0,              "debug water shader (1=log, 2=tint red)" );

gl_water_shader_state_t gWaterShader;

/* ---------- nanogl / wes / regal stubs (no shader support there) ------- */
#if XASH_NANOGL || XASH_WES || XASH_REGAL

void R_WaterShader_Init( void )
{
	/* The cvars persist for the whole engine session. R_WaterShader_Init
	 * can be called more than once (e.g. vid_restart) and the engine's
	 * Cvar_RegisterVariable refuses to re-link an existing name. Only
	 * register them once, then leave the rest of the no-op stub. */
	static qboolean s_waterCvarsRegistered = false;
	if( s_waterCvarsRegistered ) return;
	s_waterCvarsRegistered = true;

	memset( &gWaterShader, 0, sizeof( gWaterShader ));
	gEngfuncs.Cvar_RegisterVariable( &r_water_shader );
	gEngfuncs.Cvar_RegisterVariable( &r_water_alpha );
	gEngfuncs.Cvar_RegisterVariable( &r_water_ambient );
	gEngfuncs.Cvar_RegisterVariable( &r_water_density );
	gEngfuncs.Cvar_RegisterVariable( &r_water_normalscale );
	gEngfuncs.Cvar_RegisterVariable( &r_water_choppy );
	gEngfuncs.Cvar_RegisterVariable( &r_water_wave );
	gEngfuncs.Cvar_RegisterVariable( &r_water_animspeed );
	gEngfuncs.Cvar_RegisterVariable( &r_water_waveheight );
	gEngfuncs.Cvar_RegisterVariable( &r_water_wavefreq );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular_min );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skyblend );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_alpha );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_density );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fresnel );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fogblend );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_debug );
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

/* Maximum vertices in one water polygon. BSP subdivides at SUBDIVIDE_SIZE
 * so a single warp poly never exceeds a handful of verts; 64 is safe. */
#define WATER_MAX_POLY_VERTS  64

/* Custom attribute locations. Using fixed names (a_position / a_texCoord)
 * keeps gl4es' FPE shader rewriter out of our way. */
#define WATER_ATTRIB_POSITION  0
#define WATER_ATTRIB_TEXCOORD  8

/* -----------------------------------------------------------------------
 * GLSL ES 1.00 shader sources (also mirrored in glsl/water_*.glsl for
 * documentation / external editing, but embedded here so the renderer
 * does not need to ship loose files at runtime).
 * --------------------------------------------------------------------- */

/* Vertex shader with Paranoia2-style layered wave displacement.
 *
 * The base plane is deformed by three superposed sin/cos waves at slightly
 * different frequencies, producing the "~~~" up/down motion. The geometric
 * normal is rebuilt from the analytical derivatives of that displacement so
 * the fragment shader can use it as a "light proxy" (flat water = lit,
 * tilted wave peak = dark).
 *
 * The vertex shader is shared between above-water and underwater programs;
 * the only difference between the two fragment programs is how they colour
 * the surface.
 */
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
	"/* Same wave function as in the fragment, so the geometric normal here\n"
	" * matches the high-frequency normal map detail there. */\n"
	"float waveHeight( vec2 p, float t, float freq, float amp )\n"
	"{\n"
	"    float f1 = freq;\n"
	"    float f2 = freq * 0.83;\n"
	"    float f3 = freq * 1.31;\n"
	"    float h1 = sin( p.x * f1 + t * 1.1 ) * 0.55;\n"
	"    float h2 = cos( p.y * f2 + t * 0.7 ) * 0.30;\n"
	"    float h3 = sin((p.x + p.y) * f3 + t * 1.5 ) * 0.20;\n"
	"    return (h1 + h2 + h3) * amp;\n"
	"}\n"
	"\n"
	"void main()\n"
	"{\n"
	"    vec3  pos     = a_position.xyz;\n"
	"    float t       = u_time;\n"
	"    float freq    = max( u_wavefreq, 0.001 );\n"
	"    float amp     = u_waveheight;\n"
	"    pos.z        += waveHeight( pos.xy, t, freq, amp );\n"
	"\n"
	"    /* analytical derivatives of waveHeight w.r.t. x and y */\n"
	"    float f1 = freq;\n"
	"    float f2 = freq * 0.83;\n"
	"    float f3 = freq * 1.31;\n"
	"    float dHdx =  cos( pos.x * f1 + t * 1.1 ) * 0.55 * f1\n"
	"               +   cos((pos.x + pos.y) * f3 + t * 1.5 ) * 0.20 * f3;\n"
	"    float dHdy = -sin( pos.y * f2 + t * 0.7 ) * 0.30 * f2\n"
	"               +   cos((pos.x + pos.y) * f3 + t * 1.5 ) * 0.20 * f3;\n"
	"    dHdx *= amp;\n"
	"    dHdy *= amp;\n"
	"    v_geoNormal = normalize( vec3( -dHdx, -dHdy, 1.0 ));\n"
	"\n"
	"    v_worldPos = pos;\n"
	"    v_texCoord = a_texCoord;\n"
	"    vec4 viewPos = u_modelView * vec4( pos, 1.0 );\n"
	"    v_viewPos = viewPos.xyz;\n"
	"    gl_Position = u_projection * viewPos;\n"
	"}\n";

/* Above-water fragment shader.
 *
 * Adapted from PrimeXT's LIQUID_SURFACE branch in
 * game_dir/glsl/forward/scene_bmodel_fp.glsl + fresnel.h, simplified
 * for GLSL ES 1.00 (no #include, no texture(), no centroid varying).
 *
 * Final colour is mix(waterBody, skyTint, fresnel) + sun specular,
 * where `fresnel` follows PrimeXT's Schlick approximation
 *     F = F0 + (1 - F0) * pow(1 - cosTheta, power)
 * with F0 = WATER_F0_VALUE (0.15) and power supplied via u_fresnelFactor.
 *
 * All tuning knobs are exposed as uniforms so the user can drive them
 * through r_water_* cvars (see gl_watershader.c CVAR_DEFINE_AUTO block).
 */
static const char *water_frag_above_source =
	"#ifdef GL_ES\n"
	"precision mediump float;\n"
	"#endif\n"
	"uniform sampler2D u_normalMap;\n"
	"uniform vec3      u_cameraPos;\n"
	"uniform vec3      u_waterColor;\n"
	"uniform vec3      u_skyColor;\n"
	"uniform vec3      u_specularColor;\n"
	"uniform highp float u_time;\n"
	"uniform float     u_fresnelFactor;\n"
	"uniform float     u_alpha;\n"
	"uniform float     u_ambient;\n"
	"uniform float     u_density;\n"
	"uniform float     u_normalscale;\n"
	"uniform float     u_choppy;\n"
	"uniform float     u_specular;\n"
	"uniform float     u_specularMin;\n"
	"uniform float     u_skyblend;\n"
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
	"const float WATER_F0   = 0.15;\n"
	"const float WAVE_SCALE = 0.0035;\n"
	"const float BUMP_BASE  = 1.2;\n"
	"const vec2  WIND_DIR   = vec2( 0.6, -0.8 );\n"
	"const float WIND_SPEED = 0.18;\n"
	"\n"
	"vec2 waveUV( vec2 uv, float scale, float speed, float t, vec3 prev, float choppy )\n"
	"{\n"
	"    float z = max( abs(prev.z), 0.05 );\n"
	"    return uv * scale + WIND_DIR * t * (WIND_SPEED * speed)\n"
	"           - (prev.xy / z) * choppy;\n"
	"}\n"
	"\n"
	"void main()\n"
	"{\n"
	"    vec2  uv = v_worldPos.xy * WAVE_SCALE;\n"
	"    float t  = u_time;\n"
	"    /* layered normal-map fetches (PrimeXT-style chop chaining) */\n"
	"    vec3 n0 = texture2D( u_normalMap, waveUV(uv,  1.0, 0.10, t, vec3(0.0), u_choppy)).rgb * 2.0 - 1.0;\n"
	"    vec3 n1 = texture2D( u_normalMap, waveUV(uv,  2.0, 0.18, t, n0, u_choppy)).rgb * 2.0 - 1.0;\n"
	"    vec3 n2 = texture2D( u_normalMap, waveUV(uv,  4.0, 0.30, t, n1, u_choppy)).rgb * 2.0 - 1.0;\n"
	"    vec3 n3 = texture2D( u_normalMap, waveUV(uv,  8.0, 0.55, t, n2, u_choppy)).rgb * 2.0 - 1.0;\n"
	"    vec3 n  = normalize( n0*0.30 + n1*0.25 + n2*0.25 + n3*0.20 );\n"
	"    float bump = BUMP_BASE * u_normalscale;\n"
	"    /* Per-fragment detail normal: combine the normal-map detail with the\n"
	"     * low-frequency geometric normal coming from the vertex displacement,\n"
	"     * so the surface looks like a single coherent height field. */\n"
	"    vec3  Ndetail = vec3(-n.x * bump, -n.y * bump, n.z);\n"
	"    vec3  N       = normalize( v_geoNormal + Ndetail * 0.6 );\n"
	"\n"
	"    vec3  V  = normalize( u_cameraPos - v_worldPos );\n"
	"    float cosTheta = clamp( dot(N, V), 0.0, 1.0 );\n"
	"    /* Schlick Fresnel (PrimeXT fresnel.h) */\n"
	"    float fresnel = WATER_F0 + (1.0 - WATER_F0) * pow( 1.0 - cosTheta, u_fresnelFactor );\n"
	"    fresnel = clamp( fresnel, 0.0, 0.95 );\n"
	"\n"
	"    /* depth-based water tint (waterBorderFactor analogue) */\n"
	"    float dist        = length( v_viewPos );\n"
	"    float depthFactor = clamp( dist * (0.0025 * u_density), 0.0, 1.0 );\n"
	"    vec3  deepColor   = u_waterColor * 0.55;\n"
	"    vec3  shallowColor= u_waterColor + vec3( 0.05, 0.08, 0.10 );\n"
	"    vec3  baseColor   = mix( shallowColor, deepColor, depthFactor );\n"
	"    /* ambient multiplier: lets the user tame the fullbright look without\n"
	"     * dimming the sky reflection part of the mix. */\n"
	"    baseColor *= u_ambient;\n"
	"\n"
	"    /* fake sky reflection (no FBO planar reflection in this backend) */\n"
	"    vec3  skyTint   = u_skyColor;\n"
	"    vec3  finalColor = mix( baseColor, skyTint, clamp( fresnel * u_skyblend, 0.0, 0.98 ));\n"
	"\n"
	"    /* sun specular term.  We use the LOW-FREQUENCY geometric normal here\n"
	"     * (rather than the per-fragment detail normal) so the highlight is a\n"
	"     * broad, transparent rim that fades in/out with the wave slope.\n"
	"     *\n"
	"     * The blend with v_geoNormal.z is a cheap \"light proxy\":\n"
	"     *   - flat water (N.z ~ 1)   -> 1.0 multiplier  (full sun)\n"
	"     *   - tilted wave peak        -> N.z less than 1 -> dimmer\n"
	"     *   - very steep trough face  -> N.z near 0     -> u_specularMin floor\n"
	"     * This way the sun glint is invisible in the \"dark\" wave troughs and\n"
	"     * bright on the flat tops, exactly the look the user asked for. */\n"
	"    vec3  R = reflect( -V, v_geoNormal );\n"
	"    vec3  sunDir = normalize( vec3( 0.4, 0.4, 0.82 ));\n"
	"    float specRaw = pow( max( dot(R, sunDir), 0.0 ), 64.0 );\n"
	"    float lightAmount = mix( u_specularMin, 1.0, clamp( v_geoNormal.z, 0.0, 1.0 ));\n"
	"    float spec   = specRaw * 0.9 * u_specular * lightAmount;\n"
	"    finalColor += u_specularColor * spec;\n"
	"\n"
	"    if( u_fogEnabled > 0.5 )\n"
	"    {\n"
	"        float fogF = clamp((dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);\n"
	"        finalColor = mix( finalColor, u_fogColor, fogF * u_fogBlend );\n"
	"    }\n"
	"    gl_FragColor = vec4( finalColor, clamp( u_alpha, 0.0, 1.0 ));\n"
	"}\n";

/* Underwater fragment shader.
 * Mirrors PrimeXT's LIQUID_UNDERWATER branch: heavier tint, caustic-ish
 * highlights, separate alpha so the scene behind is fully replaced. */
static const char *water_frag_underwater_source =
	"#ifdef GL_ES\n"
	"precision mediump float;\n"
	"#endif\n"
	"uniform sampler2D u_normalMap;\n"
	"uniform vec3      u_cameraPos;\n"
	"uniform vec3      u_underwaterColor;\n"
	"uniform highp float u_time;\n"
	"uniform float     u_underwaterAlpha;\n"
	"uniform float     u_underwaterDensity;\n"
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
	"void main()\n"
	"{\n"
	"    vec2  uv = v_worldPos.xy * 0.0035;\n"
	"    float t  = u_time;\n"
	"    vec3  n0 = texture2D( u_normalMap, uv * 1.0 + vec2( t*0.025,  t*0.035)).rgb * 2.0 - 1.0;\n"
	"    vec3  n1 = texture2D( u_normalMap, uv * 2.0 + vec2(-t*0.040,  t*0.030)).rgb * 2.0 - 1.0;\n"
	"    vec3  N  = normalize( n0 + n1 );\n"
	"\n"
	"    float caustic    = pow( max( N.z, 0.0 ), 2.0 );\n"
	"    float dist       = length( v_viewPos );\n"
	"    float depthFactor= clamp( dist * (0.004 * u_underwaterDensity), 0.0, 1.0 );\n"
	"\n"
	"    vec3  baseColor   = u_underwaterColor;\n"
	"    vec3  causticColor= u_underwaterColor * 1.5 + vec3( 0.10, 0.20, 0.10 );\n"
	"    vec3  finalColor  = mix( causticColor, baseColor, 1.0 - caustic * 0.25 );\n"
	"    finalColor = mix( finalColor, u_underwaterColor * 0.20, depthFactor );\n"
	"\n"
	"    if( u_fogEnabled > 0.5 )\n"
	"    {\n"
	"        float fogF = clamp((dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);\n"
	"        finalColor = mix( finalColor, u_fogColor, fogF * u_fogBlend );\n"
	"    }\n"
	"    gl_FragColor = vec4( finalColor, clamp( u_underwaterAlpha, 0.0, 1.0 ));\n"
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

	/* Bind attribute locations BEFORE linking to bypass gl4es' FPE wrapper. */
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
	p->u_cameraPos     = pglGetUniformLocationARB( p->program, "u_cameraPos" );
	p->u_time          = pglGetUniformLocationARB( p->program, "u_time" );
	p->u_fresnelFactor = pglGetUniformLocationARB( p->program, "u_fresnelFactor" );
	p->u_fogColor      = pglGetUniformLocationARB( p->program, "u_fogColor" );
	p->u_fogStart      = pglGetUniformLocationARB( p->program, "u_fogStart" );
	p->u_fogEnd        = pglGetUniformLocationARB( p->program, "u_fogEnd" );
	p->u_fogEnabled    = pglGetUniformLocationARB( p->program, "u_fogEnabled" );

	/* above-water tuning uniforms (set to -1 on the underwater program) */
	p->u_waterColor      = pglGetUniformLocationARB( p->program, "u_waterColor" );
	p->u_alpha           = pglGetUniformLocationARB( p->program, "u_alpha" );
	p->u_ambient         = pglGetUniformLocationARB( p->program, "u_ambient" );
	p->u_density         = pglGetUniformLocationARB( p->program, "u_density" );
	p->u_normalScale     = pglGetUniformLocationARB( p->program, "u_normalscale" );
	p->u_choppy          = pglGetUniformLocationARB( p->program, "u_choppy" );
	p->u_specular        = pglGetUniformLocationARB( p->program, "u_specular" );
	p->u_specularMin     = pglGetUniformLocationARB( p->program, "u_specularMin" );
	p->u_specularColor   = pglGetUniformLocationARB( p->program, "u_specularColor" );
	p->u_skyblend        = pglGetUniformLocationARB( p->program, "u_skyblend" );
	p->u_skyColor        = pglGetUniformLocationARB( p->program, "u_skyColor" );
	p->u_fogBlend        = pglGetUniformLocationARB( p->program, "u_fogBlend" );

	/* underwater tuning uniforms (set to -1 on the above-water program) */
	p->u_underwaterColor    = pglGetUniformLocationARB( p->program, "u_underwaterColor" );
	p->u_underwaterAlpha    = pglGetUniformLocationARB( p->program, "u_underwaterAlpha" );
	p->u_underwaterDensity  = pglGetUniformLocationARB( p->program, "u_underwaterDensity" );

	/* vertex-shader uniforms (always present in both programs) */
	p->u_waveheight      = pglGetUniformLocationARB( p->program, "u_waveheight" );
	p->u_wavefreq        = pglGetUniformLocationARB( p->program, "u_wavefreq" );

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
			float nz = 1.0f / len;
			nx /= len;
			ny /= len;

			out[(y * size + x) * 3 + 0] = (byte)((nx * 0.5f + 0.5f) * 255.0f);
			out[(y * size + x) * 3 + 1] = (byte)((ny * 0.5f + 0.5f) * 255.0f);
			out[(y * size + x) * 3 + 2] = (byte)((nz * 0.5f + 0.5f) * 255.0f);
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

/* Try to load PrimeXT's animated water normalmaps: gfx/water/water_normal_N.
 * We try each index until the file is missing. */
static void R_WaterShader_LoadNormalFrames( void )
{
	char path[64];

	gWaterShader.numNormalFrames = 0;
	memset( gWaterShader.normalFrames, 0, sizeof( gWaterShader.normalFrames ));

	for( int i = 0; i < WATER_MAX_FRAMES; i++ )
	{
		Q_snprintf( path, sizeof( path ), "gfx/water/water_normal_%i.tga", i );
		int idx = GL_LoadTexture( path, NULL, 0, TF_NORMALMAP );

		if( !idx )
		{
			/* PrimeXT-style fallback path */
			Q_snprintf( path, sizeof( path ), "gfx/textures/water_normal_%i.tga", i );
			idx = GL_LoadTexture( path, NULL, 0, TF_NORMALMAP );
		}

		if( !idx ) break;

		const gl_texture_t *t = R_GetTexture( idx );
		if( !t || !t->texnum ) break;

		gWaterShader.normalFrames[gWaterShader.numNormalFrames++] = t->texnum;
	}

	if( gWaterShader.numNormalFrames == 0 )
	{
		/* Fall back to a single procedural normal map. */
		gWaterShader.normalProcedural = R_WaterShader_UploadProceduralNormal();
		if( gWaterShader.normalProcedural )
		{
			gEngfuncs.Con_Reportf( "R_WaterShader: no gfx/water/water_normal_*.tga, using procedural normalmap\n" );
		}
	}
	else
	{
		gEngfuncs.Con_Reportf( "R_WaterShader: loaded %i animated water normalmap frame(s)\n",
		                       gWaterShader.numNormalFrames );
	}
}

static GLuint R_WaterShader_CurrentNormalFrame( void )
{
	if( gWaterShader.numNormalFrames > 0 )
	{
		int frame = (int)( gp_cl->time * WATER_ANIMTIME ) % gWaterShader.numNormalFrames;
		if( frame < 0 ) frame += gWaterShader.numNormalFrames;
		return gWaterShader.normalFrames[frame];
	}
	return gWaterShader.normalProcedural;
}

/* ---------------------------------------------------------------------- */
/* Init / shutdown                                                        */
/* ---------------------------------------------------------------------- */

void R_WaterShader_Init( void )
{
	/* The cvars persist for the whole engine session. R_WaterShader_Init
	 * can be called more than once (e.g. vid_restart) and the second call
	 * would spam "is already defined" warnings for every cvar, since the
	 * engine's Cvar_RegisterVariable refuses to re-link an existing name.
	 * We only register them once, but always re-evaluate shader compilation
	 * because R_WaterShader_Shutdown may have run in between. */
	static qboolean s_waterCvarsRegistered = false;

	if( !s_waterCvarsRegistered )
	{
		s_waterCvarsRegistered = true;
		memset( &gWaterShader, 0, sizeof( gWaterShader ));

		gEngfuncs.Cvar_RegisterVariable( &r_water_shader );
		gEngfuncs.Cvar_RegisterVariable( &r_water_alpha );
	gEngfuncs.Cvar_RegisterVariable( &r_water_ambient );
	gEngfuncs.Cvar_RegisterVariable( &r_water_density );
	gEngfuncs.Cvar_RegisterVariable( &r_water_normalscale );
	gEngfuncs.Cvar_RegisterVariable( &r_water_choppy );
	gEngfuncs.Cvar_RegisterVariable( &r_water_wave );
	gEngfuncs.Cvar_RegisterVariable( &r_water_animspeed );
	gEngfuncs.Cvar_RegisterVariable( &r_water_waveheight );
	gEngfuncs.Cvar_RegisterVariable( &r_water_wavefreq );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular_min );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_specular_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skyblend );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_alpha );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_underwater_density );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fresnel );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fogblend );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_debug );
	}

	/* Skip the expensive GL state work if we already finished a previous
	 * Init->Shutdown cycle's worth of compilation, but always re-run if
	 * Shutdown zeroed gWaterShader since. */
	if( gWaterShader.initialized )
		return;

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

	gEngfuncs.Con_Reportf( "R_WaterShader: ready (PrimeXT-style, FBO-less)\n" );
}

void R_WaterShader_Shutdown( void )
{
	if( !gWaterShader.initialized )
		return;

	if( glw_state.initialized )
	{
		R_WaterShader_DeleteProgram( &gWaterShader.programAboveWater );
		R_WaterShader_DeleteProgram( &gWaterShader.programUnderwater );

		if( gWaterShader.normalProcedural )
			pglDeleteTextures( 1, &gWaterShader.normalProcedural );
	}

	memset( &gWaterShader, 0, sizeof( gWaterShader ));
}

void R_WaterShader_VidInit( void )
{
	if( !gWaterShader.shaderSupport )
		return;

	/* Drop the procedural fallback before reloading frames. */
	if( gWaterShader.normalProcedural )
	{
		pglDeleteTextures( 1, &gWaterShader.normalProcedural );
		gWaterShader.normalProcedural = 0;
	}

	R_WaterShader_LoadNormalFrames();
}

/* ---------------------------------------------------------------------- */
/* Per-surface drawing                                                    */
/* ---------------------------------------------------------------------- */

/* Returns true if we actually drew the surface and the caller should
 * skip the fixed-function water emit; false otherwise (so the legacy
 * water rendering still happens). */
qboolean R_WaterShader_EmitPolys( msurface_t *warp )
{
	if( !gWaterShader.shaderSupport ) return false;
	if( !r_water_shader.value )       return false;
	if( !warp || !warp->polys )       return false;

	GLuint normalTex = R_WaterShader_CurrentNormalFrame();
	if( !normalTex ) return false;  /* nothing useful to render with */

	/* Pick above-water vs underwater program based on camera height
	 * relative to the surface plane. */
	const qboolean underwater =
	    ( warp->polys->verts[0][2] >= RI.rvp.vieworigin[2] );

	gl_water_program_t *prog = underwater
	    ? &gWaterShader.programUnderwater
	    : &gWaterShader.programAboveWater;

	pglUseProgramObjectARB( prog->program );

	/* Push the engine's current MV / P matrices so the water lives in the
	 * same view space as the rest of the scene (works for both world and
	 * brush models without us having to know about RI.objectMatrix). */
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

	if( prog->u_cameraPos >= 0 )
	{
		pglUniform3fARB( prog->u_cameraPos,
		                 RI.rvp.vieworigin[0],
		                 RI.rvp.vieworigin[1],
		                 RI.rvp.vieworigin[2] );
	}

	/* Per-entity water tint and transparency from func_water (Half-Life):
	 *   - rendercolor (when non-black) overrides the cvars
	 *   - renderamt  drives the alpha so map-defined func_water transparencies
	 *     work out of the box without the mapper needing to know about our cvars
	 *   - rendermode kRenderTrans* OR renderamt != 255 activates the per-entity
	 *     path; the default engine state (kRenderNormal + renderamt 255) keeps
	 *     the global cvars in control. */
	{
		cl_entity_t *e = RI.currententity;
		const qboolean hasPerEntity =
			( e != NULL ) &&
			( e->curstate.rendermode != kRenderNormal ||
			  e->curstate.renderamt != 255 );

		float r = r_water_color_r.value / 255.0f;
		float g = r_water_color_g.value / 255.0f;
		float b = r_water_color_b.value / 255.0f;
		float a = r_water_alpha.value;

		if( hasPerEntity )
		{
			if( e->curstate.rendercolor.r || e->curstate.rendercolor.g || e->curstate.rendercolor.b )
			{
				r = e->curstate.rendercolor.r / 255.0f;
				g = e->curstate.rendercolor.g / 255.0f;
				b = e->curstate.rendercolor.b / 255.0f;
			}

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

		if( r_water_debug.value >= 2.0f )
		{
			r = 1.0f; g = 0.0f; b = 0.0f;  /* debug tint */
		}

		if( prog->u_waterColor >= 0 )
			pglUniform3fARB( prog->u_waterColor, r, g, b );
		if( prog->u_alpha >= 0 )
			pglUniform1fARB( prog->u_alpha, Q_min( 1.0f, Q_max( 0.0f, a )));
	}

	if( prog->u_time >= 0 )
	{
		const float t = r_water_wave.value
		    ? (float)gp_cl->time * r_water_animspeed.value
		    : 0.0f;
		pglUniform1fARB( prog->u_time, t );
	}

	if( prog->u_fresnelFactor >= 0 )
		pglUniform1fARB( prog->u_fresnelFactor, r_water_fresnel.value > 0.1f ? r_water_fresnel.value : WATER_FRESNEL_FACTOR );

	if( prog->u_fogColor >= 0 )
		pglUniform3fARB( prog->u_fogColor, RI.fogColor[0], RI.fogColor[1], RI.fogColor[2] );

	if( prog->u_fogStart >= 0 )
		pglUniform1fARB( prog->u_fogStart, RI.fogStart );

	if( prog->u_fogEnd >= 0 )
		pglUniform1fARB( prog->u_fogEnd, RI.fogEnd );

	if( prog->u_fogEnabled >= 0 )
		pglUniform1fARB( prog->u_fogEnabled, RI.fogEnabled ? 1.0f : 0.0f );

	/* above-water tuning uniforms (u_alpha/u_waterColor are set above
	 * from per-entity rendercolor + renderamt) */
	if( prog->u_ambient >= 0 )
		pglUniform1fARB( prog->u_ambient, r_water_ambient.value );
	if( prog->u_density >= 0 )
		pglUniform1fARB( prog->u_density, r_water_density.value );
	if( prog->u_normalScale >= 0 )
		pglUniform1fARB( prog->u_normalScale, r_water_normalscale.value );
	if( prog->u_choppy >= 0 )
		pglUniform1fARB( prog->u_choppy, r_water_choppy.value );
	if( prog->u_specular >= 0 )
		pglUniform1fARB( prog->u_specular, r_water_specular.value );
	if( prog->u_specularMin >= 0 )
		pglUniform1fARB( prog->u_specularMin, r_water_specular_min.value );
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
	if( prog->u_fogBlend >= 0 )
		pglUniform1fARB( prog->u_fogBlend, r_water_fogblend.value );

	/* underwater tuning uniforms */
	if( prog->u_underwaterAlpha >= 0 )
		pglUniform1fARB( prog->u_underwaterAlpha, r_water_underwater_alpha.value );
	if( prog->u_underwaterColor >= 0 )
		pglUniform3fARB( prog->u_underwaterColor,
		                 r_water_underwater_color_r.value / 255.0f,
		                 r_water_underwater_color_g.value / 255.0f,
		                 r_water_underwater_color_b.value / 255.0f );
	if( prog->u_underwaterDensity >= 0 )
		pglUniform1fARB( prog->u_underwaterDensity, r_water_underwater_density.value );

	/* vertex-shader uniforms (wave height and frequency).
	 * Per-entity scale acts as a multiplier on the global wave height, so
	 * a func_water with scale=0.5 has half-amplitude waves and a scale=2.0
	 * func_water is twice as choppy.  scale==1.0 (the engine default) is a
	 * no-op. */
	{
		float waveHeight = r_water_waveheight.value;
		cl_entity_t *ec = RI.currententity;
		if( ec && ec->curstate.scale > 0.001f && fabsf( ec->curstate.scale - 1.0f ) > 0.001f )
			waveHeight *= ec->curstate.scale;
		if( prog->u_waveheight >= 0 )
			pglUniform1fARB( prog->u_waveheight, waveHeight );
		if( prog->u_wavefreq >= 0 )
			pglUniform1fARB( prog->u_wavefreq, r_water_wavefreq.value );
	}

	/* Bind animated normal map on unit 0. We bind manually because GL_Bind
	 * works on engine texture indices and we already hold the raw handle. */
	pglActiveTextureARB( GL_TEXTURE0_ARB );
	pglBindTexture( GL_TEXTURE_2D, normalTex );
	if( prog->u_normalMap >= 0 )
		pglUniform1iARB( prog->u_normalMap, 0 );

	/* Render state: alpha-blended over the existing scene, no depth write,
	 * no clip plane (PrimeXT enables a clip plane when it does a planar
	 * reflection pass, but we don't). */
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

	/* Leave the texture stack the way GL_Bind expects to find it. */
	pglActiveTextureARB( GL_TEXTURE0_ARB );
	pglBindTexture( GL_TEXTURE_2D, 0 );

	if( r_water_debug.value >= 1.0f )
		gEngfuncs.Con_Reportf( "R_WaterShader: drew warp %p (%s)\n",
		                       (void *)warp, underwater ? "underwater" : "above" );

	return true;
}

#endif  /* !XASH_NANOGL && !XASH_WES && !XASH_REGAL */
