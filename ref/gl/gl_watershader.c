/*
 * HL2 water shader port for xash3d-fwgs (gl4es / GLES 2.0).
 *
 * Ported from fteqw's HL2 water shader:
 *   fteqw/plugins/hl2/glsl/vmt/water.glsl
 *   fteqw/engine/shaders/glsl/altwater.glsl
 *
 * Replaces the earlier broken PrimeXT-style r_water_shader implementation.
 *
 * Uses pglCopyTexSubImage2D to grab the framebuffer for screen-space
 * refraction (no FBOs needed), and a procedurally-generated cubemap for
 * reflection.  This gives the HL2 water look without external FBO resources.
 */

#include "gl_local.h"
#include "gl_watershader.h"

CVAR_DEFINE_AUTO( r_water_shader,         "0",     FCVAR_GLCONFIG, "enable HL2-style water shader" );
CVAR_DEFINE_AUTO( r_water_alpha,          "0.85",  FCVAR_GLCONFIG, "water opacity (0=transparent, 1=opaque)" );
CVAR_DEFINE_AUTO( r_water_fresnel,        "4.0",   FCVAR_GLCONFIG, "Fresnel exponent (HL2 default ~4)" );
CVAR_DEFINE_AUTO( r_water_strength,       "0.05",  FCVAR_GLCONFIG, "refraction distortion strength" );
CVAR_DEFINE_AUTO( r_water_skyblend,       "0.70",  FCVAR_GLCONFIG, "reflection blend factor (0=none, 1=full)" );
CVAR_DEFINE_AUTO( r_water_skycolor_r,     "100",   FCVAR_GLCONFIG, "reflection cubemap tint red (0-255)" );
CVAR_DEFINE_AUTO( r_water_skycolor_g,     "150",   FCVAR_GLCONFIG, "reflection cubemap tint green (0-255)" );
CVAR_DEFINE_AUTO( r_water_skycolor_b,     "200",   FCVAR_GLCONFIG, "reflection cubemap tint blue (0-255)" );
CVAR_DEFINE_AUTO( r_water_tintrefr_r,     "220",   FCVAR_GLCONFIG, "refraction tint red (0-255)" );
CVAR_DEFINE_AUTO( r_water_tintrefr_g,     "235",   FCVAR_GLCONFIG, "refraction tint green (0-255)" );
CVAR_DEFINE_AUTO( r_water_tintrefr_b,     "255",   FCVAR_GLCONFIG, "refraction tint blue (0-255)" );
CVAR_DEFINE_AUTO( r_water_tintrefl_r,     "255",   FCVAR_GLCONFIG, "reflection tint red (0-255)" );
CVAR_DEFINE_AUTO( r_water_tintrefl_g,     "255",   FCVAR_GLCONFIG, "reflection tint green (0-255)" );
CVAR_DEFINE_AUTO( r_water_tintrefl_b,     "255",   FCVAR_GLCONFIG, "reflection tint blue (0-255)" );
CVAR_DEFINE_AUTO( r_water_color_r,        "30",    FCVAR_GLCONFIG, "water body color red (0-255)" );
CVAR_DEFINE_AUTO( r_water_color_g,        "60",    FCVAR_GLCONFIG, "water body color green (0-255)" );
CVAR_DEFINE_AUTO( r_water_color_b,        "80",    FCVAR_GLCONFIG, "water body color blue (0-255)" );
CVAR_DEFINE_AUTO( r_water_distscale,      "0.002", FCVAR_GLCONFIG, "distance-based depth tint scale" );
CVAR_DEFINE_AUTO( r_water_fogblend,       "1.0",   FCVAR_GLCONFIG, "fog influence on water (0..1)" );
CVAR_DEFINE_AUTO( r_water_debug,          "0",     0,              "debug (1=log, 2=tint red)" );
CVAR_DEFINE_AUTO( r_water_waveheight,     "3.0",   FCVAR_GLCONFIG, "vertex wave displacement amplitude" );
CVAR_DEFINE_AUTO( r_water_wavefreq,       "0.04",  FCVAR_GLCONFIG, "vertex wave spatial frequency" );
CVAR_DEFINE_AUTO( r_water_uw_alpha,       "0.50",  FCVAR_GLCONFIG, "underwater overlay opacity" );
CVAR_DEFINE_AUTO( r_water_uw_color_r,     "20",    FCVAR_GLCONFIG, "underwater tint red (0-255)" );
CVAR_DEFINE_AUTO( r_water_uw_color_g,     "50",    FCVAR_GLCONFIG, "underwater tint green (0-255)" );
CVAR_DEFINE_AUTO( r_water_uw_color_b,     "70",    FCVAR_GLCONFIG, "underwater tint blue (0-255)" );
CVAR_DEFINE_AUTO( r_water_uw_density,     "0.015", FCVAR_GLCONFIG, "underwater depth tint strength" );
CVAR_DEFINE_AUTO( r_water_uw_scattering,  "0.20",  FCVAR_GLCONFIG, "underwater light scattering" );

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
	gEngfuncs.Cvar_RegisterVariable( &r_water_strength );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skyblend );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_tintrefr_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_tintrefr_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_tintrefr_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_tintrefl_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_tintrefl_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_tintrefl_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_distscale );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fogblend );
	gEngfuncs.Cvar_RegisterVariable( &r_water_debug );
	gEngfuncs.Cvar_RegisterVariable( &r_water_waveheight );
	gEngfuncs.Cvar_RegisterVariable( &r_water_wavefreq );
	gEngfuncs.Cvar_RegisterVariable( &r_water_uw_alpha );
	gEngfuncs.Cvar_RegisterVariable( &r_water_uw_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_uw_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_uw_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_uw_density );
	gEngfuncs.Cvar_RegisterVariable( &r_water_uw_scattering );
}

void R_WaterShader_Init( void )    { R_WaterShader_RegisterCvars(); memset( &gWaterShader, 0, sizeof( gWaterShader )); }
void R_WaterShader_Shutdown( void ) {}
void R_WaterShader_VidInit( void )  {}

qboolean R_WaterShader_EmitPolys( msurface_t *warp ) { (void)warp; return false; }

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

/*
 * Cubemap face targets.  We use the numeric values directly in case the
 * GL headers do not define the non-ARB variants for GLES 2.0.
 */
#ifndef GL_TEXTURE_CUBE_MAP_POSITIVE_X
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X      0x8515
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_X      0x8516
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Y      0x8517
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Y      0x8518
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Z      0x8519
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z      0x851A
#endif
#ifndef GL_TEXTURE_CUBE_MAP
#define GL_TEXTURE_CUBE_MAP                 0x8513
#endif

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
	"    float t       = u_time;\n"
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
 * Faithful port of fteqw's water.glsl / altwater.glsl:
 *   - screen-space UVs from clip-space position
 *   - Q1-style texture coordinate warping
 *   - two-layer scrolling normalmap
 *   - Fresnel: pow(1 - abs(dot(N, normalize(V))), exponent)
 *   - refraction: screen-grab texture with normal distortion
 *   - reflection: cubemap sampled by normal
 *   - Fresnel blend
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
	"uniform samplerCube u_reflectCube;\n"
	"uniform highp float u_time;\n"
	"uniform float u_fresnelExp;\n"
	"uniform float u_strengthRefr;\n"
	"uniform vec3  u_tintRefr;\n"
	"uniform vec3  u_tintRefl;\n"
	"uniform float u_alpha;\n"
	"uniform float u_distScale;\n"
	"uniform float u_fogBlend;\n"
	"uniform vec3  u_fogColor;\n"
	"uniform float u_fogStart;\n"
	"uniform float u_fogEnd;\n"
	"uniform float u_fogEnabled;\n"
	"uniform float u_refractEnabled;\n"
	"varying vec2  v_texCoord;\n"
	"varying vec4  v_clipPos;\n"
	"varying vec3  v_normal;\n"
	"varying vec3  v_eye;\n"
	"\n"
	"void main()\n"
	"{\n"
	"    /* screen-space texcoords (matching fteqw's (1.0+tf.xy/tf.w)*0.5) */\n"
	"    vec2 stc = (1.0 + (v_clipPos.xy / v_clipPos.w)) * 0.5;\n"
	"\n"
	"    /* Q1-style warp for normalmap coords (fteqw's ntc) */\n"
	"    vec2 ntc;\n"
	"    ntc.s = v_texCoord.s + sin( v_texCoord.t + u_time ) * 0.125;\n"
	"    ntc.t = v_texCoord.t + sin( v_texCoord.s + u_time ) * 0.125;\n"
	"\n"
	"    /* two-layer scrolling normalmap (fteqw's altwater approach) */\n"
	"    vec3 n  = texture2D( u_normalMap, ntc * 1.0 + vec2( u_time * 0.10, 0.0 )).xyz;\n"
	"    n      += texture2D( u_normalMap, ntc * 0.5 - vec2( 0.0, u_time * 0.097 )).xyz;\n"
	"    n      -= 1.0 - 4.0 / 256.0;\n"
	"    n       = normalize( n );\n"
	"\n"
	"    /* Fresnel term (fteqw: pow(1-abs(dot(n,normalize(eye))), EXP)) */\n"
	"    float fres = pow( 1.0 - abs( dot( n, normalize( v_eye ))), u_fresnelExp );\n"
	"\n"
	"    /* refraction = screen grab with distortion, tinted */\n"
	"    vec3 refr;\n"
	"    if( u_refractEnabled > 0.5 )\n"
	"        refr = texture2D( u_refractMap, stc + n.st * u_strengthRefr ).rgb * u_tintRefr;\n"
	"    else\n"
	"        refr = vec3( 0.12, 0.25, 0.33 ) * u_tintRefr;\n"
	"\n"
	"    /* distance-based depth darkening */\n"
	"    float dist = length( v_eye );\n"
	"    float depthF = clamp( dist * u_distScale, 0.0, 1.0 );\n"
	"    refr = mix( refr, refr * 0.4, depthF );\n"
	"\n"
	"    /* reflection = cubemap sampled by normal, tinted */\n"
	"    vec3 refl = textureCube( u_reflectCube, n ).rgb * u_tintRefl;\n"
	"\n"
	"    /* Fresnel blend */\n"
	"    vec3 color = mix( refr, refl, clamp( fres, 0.0, 0.95 ));\n"
	"\n"
	"    /* diffuse (warp) texture overlay — like fteqw's ALPHA path */\n"
	"    vec4 ts = texture2D( u_diffuseMap, ntc );\n"
	"    color = mix( color, ts.rgb, 0.25 * ts.a );\n"
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
 * Simple deep-water tint with caustics and light shafts.
 * --------------------------------------------------------------------- */
static const char *water_frag_under_source =
	"#ifdef GL_ES\n"
	"precision mediump float;\n"
	"#endif\n"
	"uniform highp float u_time;\n"
	"uniform vec3  u_uwColor;\n"
	"uniform float u_uwAlpha;\n"
	"uniform float u_uwDensity;\n"
	"uniform float u_uwScattering;\n"
	"uniform float u_fogBlend;\n"
	"uniform vec3  u_fogColor;\n"
	"uniform float u_fogStart;\n"
	"uniform float u_fogEnd;\n"
	"uniform float u_fogEnabled;\n"
	"varying vec3  v_eye;\n"
	"varying vec3  v_normal;\n"
	"\n"
	"float caustic( vec2 p, float t )\n"
	"{\n"
	"    vec2 uv = p * 0.002;\n"
	"    float c1 = sin( uv.x * 3.0 + t * 0.8 ) * cos( uv.y * 2.5 - t * 0.6 );\n"
	"    float c2 = sin((uv.x + uv.y) * 4.0 + t * 1.2 ) * 0.5;\n"
	"    float c3 = cos((uv.x - uv.y) * 5.0 - t * 0.9 ) * 0.3;\n"
	"    return clamp( c1 * 0.5 + c2 + c3, 0.0, 1.0 );\n"
	"}\n"
	"\n"
	"void main()\n"
	"{\n"
	"    float dist  = length( v_eye );\n"
	"    float depthF = clamp( dist * u_uwDensity, 0.0, 1.0 );\n"
	"    float c = caustic( gl_FragCoord.xy, u_time );\n"
	"    float sunF = clamp( 1.0 - depthF, 0.0, 1.0 );\n"
	"    vec3 color = u_uwColor * (1.0 - depthF * 0.7);\n"
	"    color += vec3( 0.15, 0.25, 0.10 ) * c * (1.0 - depthF * 0.5);\n"
	"    color += vec3( 0.3, 0.4, 0.5 ) * sunF * u_uwScattering;\n"
	"    if( u_fogEnabled > 0.5 )\n"
	"    {\n"
	"        float fogF = clamp((dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);\n"
	"        color = mix( color, u_fogColor, fogF * u_fogBlend );\n"
	"    }\n"
	"    gl_FragColor = vec4( color, clamp( u_uwAlpha, 0.0, 1.0 ));\n"
	"}\n";

/* ---------------------------------------------------------------------- */
/* Shader compilation helper                                              */
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

	/* common uniforms */
	p->u_modelView      = pglGetUniformLocationARB( p->program, "u_modelView" );
	p->u_projection     = pglGetUniformLocationARB( p->program, "u_projection" );
	p->u_normalMap      = pglGetUniformLocationARB( p->program, "u_normalMap" );
	p->u_diffuseMap     = pglGetUniformLocationARB( p->program, "u_diffuseMap" );
	p->u_refractMap     = pglGetUniformLocationARB( p->program, "u_refractMap" );
	p->u_reflectCube    = pglGetUniformLocationARB( p->program, "u_reflectCube" );
	p->u_cameraPos      = pglGetUniformLocationARB( p->program, "u_cameraPos" );
	p->u_time           = pglGetUniformLocationARB( p->program, "u_time" );
	p->u_fresnelExp     = pglGetUniformLocationARB( p->program, "u_fresnelExp" );
	p->u_strengthRefr   = pglGetUniformLocationARB( p->program, "u_strengthRefr" );
	p->u_tintRefr       = pglGetUniformLocationARB( p->program, "u_tintRefr" );
	p->u_tintRefl       = pglGetUniformLocationARB( p->program, "u_tintRefl" );
	p->u_alpha          = pglGetUniformLocationARB( p->program, "u_alpha" );
	p->u_distScale      = pglGetUniformLocationARB( p->program, "u_distScale" );
	p->u_fogBlend       = pglGetUniformLocationARB( p->program, "u_fogBlend" );
	p->u_fogColor       = pglGetUniformLocationARB( p->program, "u_fogColor" );
	p->u_fogStart       = pglGetUniformLocationARB( p->program, "u_fogStart" );
	p->u_fogEnd         = pglGetUniformLocationARB( p->program, "u_fogEnd" );
	p->u_fogEnabled     = pglGetUniformLocationARB( p->program, "u_fogEnabled" );
	p->u_waveheight     = pglGetUniformLocationARB( p->program, "u_waveheight" );
	p->u_wavefreq       = pglGetUniformLocationARB( p->program, "u_wavefreq" );
	p->u_refractEnabled = pglGetUniformLocationARB( p->program, "u_refractEnabled" );

	/* underwater uniforms */
	p->u_uwColor        = pglGetUniformLocationARB( p->program, "u_uwColor" );
	p->u_uwAlpha        = pglGetUniformLocationARB( p->program, "u_uwAlpha" );
	p->u_uwDensity      = pglGetUniformLocationARB( p->program, "u_uwDensity" );
	p->u_uwScattering   = pglGetUniformLocationARB( p->program, "u_uwScattering" );

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
	if( target == GL_TEXTURE_CUBE_MAP )
	{
		/* for cubemap, allocate all 6 faces */
		for( int i = 0; i < 6; i++ )
			pglTexImage2D( GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, level,
			               internal, size, size, 0, format, GL_UNSIGNED_BYTE, data );
	}
	else
	{
		pglTexImage2D( target, level, internal, size, size, 0, format, GL_UNSIGNED_BYTE, data );
	}
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
/* Cubemap generation — creates a 6-face cubemap from the sky colour      */
/* ---------------------------------------------------------------------- */

static GLuint R_WaterShader_GenerateCubemap( void )
{
	const int s = WATER_CUBEMAP_SIZE;
	byte *face = (byte *)malloc( s * s * 3 );
	if( !face ) return 0;

	float r = r_water_skycolor_r.value / 255.0f;
	float g = r_water_skycolor_g.value / 255.0f;
	float b = r_water_skycolor_b.value / 255.0f;

	/* fill each face with a gradient that gives a subtle sky feel */
	for( int y = 0; y < s; y++ )
	{
		float v = (float)y / (float)(s - 1);
		for( int x = 0; x < s; x++ )
		{
			float u = (float)x / (float)(s - 1);
			float l = 0.5f + 0.5f * (u * v);  /* subtle variation */
			face[(y * s + x) * 3 + 0] = (byte)Q_min( 255.0f, r * 255.0f * l );
			face[(y * s + x) * 3 + 1] = (byte)Q_min( 255.0f, g * 255.0f * l );
			face[(y * s + x) * 3 + 2] = (byte)Q_min( 255.0f, b * 255.0f * l );
		}
	}

	pglGenTextures( 1, &gWaterShader.reflectCubemap );
	pglBindTexture( GL_TEXTURE_CUBE_MAP, gWaterShader.reflectCubemap );
	pglTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	pglTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	for( int i = 0; i < 6; i++ )
		pglTexImage2D( GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0,
		               GL_RGB, s, s, 0, GL_RGB, GL_UNSIGNED_BYTE, face );

	free( face );
	return gWaterShader.reflectCubemap;
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
			gEngfuncs.Con_Reportf( "R_WaterShader: loaded normalmap\n" );
			return;
		}
	}
	gWaterShader.normalTexture = R_WaterShader_UploadProceduralNormal();
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
	gEngfuncs.Cvar_RegisterVariable( &r_water_strength );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skyblend );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_skycolor_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_tintrefr_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_tintrefr_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_tintrefr_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_tintrefl_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_tintrefl_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_tintrefl_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_distscale );
	gEngfuncs.Cvar_RegisterVariable( &r_water_fogblend );
	gEngfuncs.Cvar_RegisterVariable( &r_water_debug );
	gEngfuncs.Cvar_RegisterVariable( &r_water_waveheight );
	gEngfuncs.Cvar_RegisterVariable( &r_water_wavefreq );
	gEngfuncs.Cvar_RegisterVariable( &r_water_uw_alpha );
	gEngfuncs.Cvar_RegisterVariable( &r_water_uw_color_r );
	gEngfuncs.Cvar_RegisterVariable( &r_water_uw_color_g );
	gEngfuncs.Cvar_RegisterVariable( &r_water_uw_color_b );
	gEngfuncs.Cvar_RegisterVariable( &r_water_uw_density );
	gEngfuncs.Cvar_RegisterVariable( &r_water_uw_scattering );
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
		return;
	}

	if( !R_WaterShader_CompileProgram( &gWaterShader.programAbove,
	                                   water_vertex_source, water_frag_above_source ))
	{
		gEngfuncs.Con_Printf( S_ERROR "R_WaterShader: failed to build above-water program\n" );
		return;
	}

	if( !R_WaterShader_CompileProgram( &gWaterShader.programUnder,
	                                   water_vertex_source, water_frag_under_source ))
	{
		gEngfuncs.Con_Printf( S_ERROR "R_WaterShader: failed to build underwater program\n" );
		R_WaterShader_DeleteProgram( &gWaterShader.programAbove );
		return;
	}

	gWaterShader.shaderSupport = 1;
	gWaterShader.initialized   = 1;

	gEngfuncs.Con_Reportf( "R_WaterShader: ready (HL2-style, screen-grab refraction)\n" );
}

void R_WaterShader_Shutdown( void )
{
	if( !gWaterShader.initialized )
		return;

	if( glw_state.initialized )
	{
		R_WaterShader_DeleteProgram( &gWaterShader.programAbove );
		R_WaterShader_DeleteProgram( &gWaterShader.programUnder );

		if( gWaterShader.normalTexture )
			pglDeleteTextures( 1, &gWaterShader.normalTexture );
		if( gWaterShader.screenGrabTexture )
			pglDeleteTextures( 1, &gWaterShader.screenGrabTexture );
		if( gWaterShader.reflectCubemap )
			pglDeleteTextures( 1, &gWaterShader.reflectCubemap );
	}

	memset( &gWaterShader, 0, sizeof( gWaterShader ));
	gWaterShader.lastFrameCaptured = -1;
}

void R_WaterShader_VidInit( void )
{
	if( !gWaterShader.shaderSupport )
		return;

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
	if( gWaterShader.reflectCubemap )
	{
		pglDeleteTextures( 1, &gWaterShader.reflectCubemap );
		gWaterShader.reflectCubemap = 0;
	}

	R_WaterShader_LoadNormalTexture();
	R_WaterShader_GenerateCubemap();

	R_WaterShader_CreateScreenGrabTexture(
	    glState.width, glState.height );
}

/* ---------------------------------------------------------------------- */
/* Per-surface drawing                                                    */
/* ---------------------------------------------------------------------- */

qboolean R_WaterShader_EmitPolys( msurface_t *warp )
{
	if( !gWaterShader.shaderSupport )      return false;
	if( !r_water_shader.value )            return false;
	if( !warp || !warp->polys )            return false;
	if( !gWaterShader.normalTexture )      return false;

	/* above-water vs underwater */
	const qboolean underwater =
	    ( warp->polys->verts[0][2] >= RI.rvp.vieworigin[2] );

	gl_water_program_t *prog = underwater
	    ? &gWaterShader.programUnder
	    : &gWaterShader.programAbove;

	pglUseProgramObjectARB( prog->program );

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

	/* ---- camera position ---- */
	if( prog->u_cameraPos >= 0 )
	{
		pglUniform3fARB( prog->u_cameraPos,
		                 RI.rvp.vieworigin[0],
		                 RI.rvp.vieworigin[1],
		                 RI.rvp.vieworigin[2] );
	}

	/* ---- time ---- */
	if( prog->u_time >= 0 )
		pglUniform1fARB( prog->u_time, (float)gp_cl->time );

	/* ---- Fresnel exponent ---- */
	if( prog->u_fresnelExp >= 0 )
	{
		float fexp = r_water_fresnel.value;
		if( fexp < 0.1f ) fexp = 4.0f;
		pglUniform1fARB( prog->u_fresnelExp, fexp );
	}

	/* ---- refraction strength ---- */
	if( prog->u_strengthRefr >= 0 )
		pglUniform1fARB( prog->u_strengthRefr, r_water_strength.value );

	/* ---- tints ---- */
	if( prog->u_tintRefr >= 0 )
		pglUniform3fARB( prog->u_tintRefr,
		                 r_water_tintrefr_r.value / 255.0f,
		                 r_water_tintrefr_g.value / 255.0f,
		                 r_water_tintrefr_b.value / 255.0f );
	if( prog->u_tintRefl >= 0 )
		pglUniform3fARB( prog->u_tintRefl,
		                 r_water_tintrefl_r.value / 255.0f,
		                 r_water_tintrefl_g.value / 255.0f,
		                 r_water_tintrefl_b.value / 255.0f );

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

		if( underwater )
		{
			if( prog->u_uwAlpha >= 0 )
				pglUniform1fARB( prog->u_uwAlpha, Q_min( 1.0f, Q_max( 0.0f, a )));
		}
		else
		{
			if( prog->u_alpha >= 0 )
				pglUniform1fARB( prog->u_alpha, Q_min( 1.0f, Q_max( 0.0f, a )));
		}
	}

	/* ---- distance scale (depth darkening) ---- */
	if( !underwater )
	{
		if( prog->u_distScale >= 0 )
			pglUniform1fARB( prog->u_distScale, r_water_distscale.value );
	}

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

	/* ---- underwater uniforms ---- */
	if( underwater )
	{
		if( prog->u_uwColor >= 0 )
			pglUniform3fARB( prog->u_uwColor,
			                 r_water_uw_color_r.value / 255.0f,
			                 r_water_uw_color_g.value / 255.0f,
			                 r_water_uw_color_b.value / 255.0f );
		if( prog->u_uwDensity >= 0 )
			pglUniform1fARB( prog->u_uwDensity, r_water_uw_density.value );
		if( prog->u_uwScattering >= 0 )
			pglUniform1fARB( prog->u_uwScattering, r_water_uw_scattering.value );
	}

	/* ------------------------------------------------------------------ */
	/* Screen grab: copy current framebuffer to texture once per frame     */
	/* ------------------------------------------------------------------ */
	if( !underwater && gWaterShader.screenGrabTexture &&
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
	else if( prog->u_refractEnabled >= 0 )
	{
		pglUniform1fARB( prog->u_refractEnabled, 0.0f );
	}

	/* ---- bind textures ---- */

	/* unit 0: normal map */
	pglActiveTextureARB( GL_TEXTURE0_ARB );
	pglBindTexture( GL_TEXTURE_2D, gWaterShader.normalTexture );
	if( prog->u_normalMap >= 0 )
		pglUniform1iARB( prog->u_normalMap, 0 );

	/* unit 1: diffuse (warp) texture */
	if( prog->u_diffuseMap >= 0 && !underwater )
	{
		pglActiveTextureARB( GL_TEXTURE1_ARB );
		texture_t *wt = warp->texinfo ? warp->texinfo->texture : NULL;
		GLuint wtNum = ( wt && wt->fb_texturenum ) ? wt->fb_texturenum : gWaterShader.normalTexture;
		pglBindTexture( GL_TEXTURE_2D, wtNum );
		pglUniform1iARB( prog->u_diffuseMap, 1 );
	}

	/* unit 2: refraction (screen grab) */
	if( prog->u_refractMap >= 0 && !underwater && gWaterShader.screenGrabTexture )
	{
		pglActiveTextureARB( GL_TEXTURE2_ARB );
		pglBindTexture( GL_TEXTURE_2D, gWaterShader.screenGrabTexture );
		pglUniform1iARB( prog->u_refractMap, 2 );
	}

	/* unit 3: reflection cubemap */
	if( prog->u_reflectCube >= 0 && !underwater && gWaterShader.reflectCubemap )
	{
		pglActiveTextureARB( GL_TEXTURE3_ARB );
		pglBindTexture( GL_TEXTURE_CUBE_MAP, gWaterShader.reflectCubemap );
		pglUniform1iARB( prog->u_reflectCube, 3 );
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

	/* Since the vertex shader uses u_cameraPos for v_eye, and we also
	 * pass matrices, we need to disable culling for water (double-sided). */
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

#endif  /* !XASH_NANOGL && !XASH_WES && !XASH_REGAL */
