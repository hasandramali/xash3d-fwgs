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
uniform vec3      u_sunDir;
uniform vec3      u_underwaterColor;
uniform highp float u_time;
uniform float     u_underwaterAlpha;
uniform float     u_underwaterDensity;
uniform float     u_scattering;
uniform float     u_fogBlend;
uniform vec3      u_fogColor;
uniform float     u_fogStart;
uniform float     u_fogEnd;
uniform float     u_fogEnabled;

varying vec3 v_worldPos;
varying vec3 v_viewPos;
varying vec3 v_geoNormal;
varying vec2 v_texCoord;

/* Procedural caustic pattern using world position + time */
float caustic(vec2 pos, float t)
{
    vec2 uv = pos * 0.002;
    float c1 = sin(uv.x * 3.0 + t * 0.8) * cos(uv.y * 2.5 - t * 0.6);
    float c2 = sin((uv.x + uv.y) * 4.0 + t * 1.2) * 0.5;
    float c3 = cos((uv.x - uv.y) * 5.0 - t * 0.9) * 0.3;
    return clamp(c1 * 0.5 + c2 + c3, 0.0, 1.0);
}

/* Floating underwater particles (procedural) */
float particles(vec3 pos, float t)
{
    float sum = 0.0;
    for (int i = 0; i < 6; i++)
    {
        float fi = float(i);
        vec3  origin = vec3(
            sin(fi * 12.9898 + 1.0) * 800.0,
            cos(fi * 78.233  + 2.0) * 800.0,
            sin(fi * 43.1243 + 3.0) * 200.0);
        origin.z += t * 10.0 * (0.5 + fi * 0.2);
        float d = distance(pos, origin);
        sum += exp(-d * d * 0.01) * 0.3;
    }
    return sum;
}

void main()
{
    float dist       = length(v_viewPos);
    float depthFactor= clamp(dist * (0.003 * u_underwaterDensity), 0.0, 1.0);

    /* Caustic light pattern */
    float c = caustic(v_worldPos.xy, u_time);

    /* Sun shafts: brighter near surface */
    float sunFactor = clamp(1.0 - depthFactor, 0.0, 1.0);
    float sunAngle  = max(dot(v_geoNormal, u_sunDir), 0.0);

    /* Base water colour darkens with depth */
    vec3 color = u_underwaterColor * (1.0 - depthFactor * 0.7);

    /* Caustic highlights */
    color += vec3(0.15, 0.25, 0.10) * c * (1.0 - depthFactor * 0.5);

    /* Sun light shafts from the surface */
    color += vec3(0.3, 0.4, 0.5) * sunFactor * sunAngle * u_scattering;

    /* Floating particle motes */
    float p = particles(v_worldPos, u_time);
    color += vec3(0.4, 0.5, 0.6) * p * 0.3;

    if (u_fogEnabled > 0.5)
    {
        float fogF = clamp((dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);
        color = mix(color, u_fogColor, fogF * u_fogBlend);
    }
    gl_FragColor = vec4(color, clamp(u_underwaterAlpha, 0.0, 1.0));
}
