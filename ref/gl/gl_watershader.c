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
	memset( &gWaterShader, 0, sizeof( gWaterShader ));
	gEngfuncs.Cvar_RegisterVariable( &r_water_shader );
	gEngfuncs.Cvar_RegisterVariable( &r_water_alpha );
	gEngfuncs.Cvar_RegisterVariable( &r_water_ambient );
	gEngfuncs.Cvar_RegisterVariable( &r_water_density );
	gEngfuncs.Cvar_RegisterVariable( &r_water_normalscale );
	gEngfuncs.Cvar_RegisterVariable( &r_water_choppy );
	gEngfuncs.Cvar_RegisterVariable( &r_water_wave );
	gEngfuncs.Cvar_RegisterVariable( &r_water_animspeed );
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

	memset( &gWaterShader, 0, sizeof( gWaterShader ));

	gEngfuncs.Cvar_RegisterVariable( &r_water_shader );
	gEngfuncs.Cvar_RegisterVariable( &r_water_alpha );
	gEngfuncs.Cvar_RegisterVariable( &r_water_ambient );
	gEngfuncs.Cvar_RegisterVariable( &r_water_density );
	gEngfuncs.Cvar_RegisterVariable( &r_water_normalscale );
	gEngfuncs.Cvar_RegisterVariable( &r_water_choppy );
	gEngfuncs.Cvar_RegisterVariable( &r_water_wave );
	gEngfuncs.Cvar_RegisterVariable( &r_water_animspeed );
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

	/* Per-entity water tint: prefer rendercolor when the brush entity uses
	 * kRenderTrans*, fall back to the cvars (PrimeXT exposes u_RenderColor
	 * which is the same thing). */
	if( prog->u_waterColor >= 0 )
	{
		float r = r_water_color_r.value / 255.0f;
		float g = r_water_color_g.value / 255.0f;
		float b = r_water_color_b.value / 255.0f;

		cl_entity_t *e = RI.currententity;
		if( e && e->curstate.rendermode != kRenderNormal )
		{
			if( e->curstate.rendercolor.r || e->curstate.rendercolor.g || e->curstate.rendercolor.b )
			{
				r = e->curstate.rendercolor.r / 255.0f;
				g = e->curstate.rendercolor.g / 255.0f;
				b = e->curstate.rendercolor.b / 255.0f;
			}
		}

		if( r_water_debug.value >= 2.0f )
		{
			r = 1.0f; g = 0.0f; b = 0.0f;  /* debug tint */
		}

		pglUniform3fARB( prog->u_waterColor, r, g, b );
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

	/* above-water tuning uniforms */
	if( prog->u_alpha >= 0 )
		pglUniform1fARB( prog->u_alpha, r_water_alpha.value );
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
