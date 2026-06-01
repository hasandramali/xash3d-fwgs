/*
 * PrimeXT-inspired underwater fragment shader (GLSL ES 1.00 / gl4es).
 *
 * Mirrors PrimeXT's LIQUID_UNDERWATER branch in
 * game_dir/glsl/forward/scene_bmodel_fp.glsl: heavier water tint, no
 * Fresnel sky reflection, cheap caustic shimmer.
 *
 * Tuning knobs are exposed through uniforms and matching r_water_underwater_*
 * cvars (see ref/gl/gl_watershader.c).
 *
 * NOTE: this file mirrors the source string embedded in
 * ref/gl/gl_watershader.c (water_frag_underwater_source).
 */

#ifdef GL_ES
precision mediump float;
#endif

uniform sampler2D u_normalMap;
uniform vec3      u_cameraPos;
uniform vec3      u_underwaterColor;
uniform float     u_time;
uniform float     u_underwaterAlpha;
uniform float     u_underwaterDensity;
uniform float     u_fogBlend;
uniform vec3      u_fogColor;
uniform float     u_fogStart;
uniform float     u_fogEnd;
uniform float     u_fogEnabled;

varying vec3 v_worldPos;
varying vec3 v_viewPos;
varying vec2 v_texCoord;

void main()
{
    vec2  uv = v_worldPos.xy * 0.0035;
    float t  = u_time;

    vec3 n0 = texture2D(u_normalMap, uv * 1.0 + vec2( t * 0.025,  t * 0.035)).rgb * 2.0 - 1.0;
    vec3 n1 = texture2D(u_normalMap, uv * 2.0 + vec2(-t * 0.040,  t * 0.030)).rgb * 2.0 - 1.0;
    vec3 N  = normalize(n0 + n1);

    float caustic     = pow(max(N.z, 0.0), 2.0);
    float dist        = length(v_viewPos);
    float depthFactor = clamp(dist * (0.004 * u_underwaterDensity), 0.0, 1.0);

    vec3 baseColor    = u_underwaterColor;
    vec3 causticColor = u_underwaterColor * 1.5 + vec3(0.10, 0.20, 0.10);
    vec3 finalColor   = mix(causticColor, baseColor, 1.0 - caustic * 0.25);
    finalColor = mix(finalColor, u_underwaterColor * 0.20, depthFactor);

    if (u_fogEnabled > 0.5)
    {
        float fogF = clamp((dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);
        finalColor = mix(finalColor, u_fogColor, fogF * u_fogBlend);
    }
    gl_FragColor = vec4(finalColor, clamp(u_underwaterAlpha, 0.0, 1.0));
}
