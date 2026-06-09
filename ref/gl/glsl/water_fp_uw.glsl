/*
 * HL2-inspired underwater fragment shader (GLSL ES 1.00 / gl4es).
 *
 * Simple deep-water tint with caustic shimmer and light shafts.
 * Based on the underwater pass concept from fteqw/altwater.glsl.
 *
 * NOTE: mirrors the source string embedded in ref/gl/gl_watershader.c
 */

#ifdef GL_ES
precision mediump float;
#endif

uniform vec3      u_sunDir;
uniform vec3      u_underwaterColor;
uniform highp float u_time;
uniform float     u_underwaterAlpha;
uniform float     u_underwaterDensity;
uniform float     u_sunlightScattering;
uniform float     u_fogBlend;
uniform vec3      u_fogColor;
uniform float     u_fogStart;
uniform float     u_fogEnd;
uniform float     u_fogEnabled;

varying vec3 v_worldPos;
varying vec3 v_viewPos;
varying vec3 v_geoNormal;

float caustic(vec2 pos, float t)
{
    vec2 uv = pos * 0.002;
    float c1 = sin(uv.x * 3.0 + t * 0.8) * cos(uv.y * 2.5 - t * 0.6);
    float c2 = sin((uv.x + uv.y) * 4.0 + t * 1.2) * 0.5;
    float c3 = cos((uv.x - uv.y) * 5.0 - t * 0.9) * 0.3;
    return clamp(c1 * 0.5 + c2 + c3, 0.0, 1.0);
}

void main()
{
    float dist  = length(v_viewPos);
    float depthFactor = clamp(dist * u_underwaterDensity, 0.0, 1.0);

    float c = caustic(v_worldPos.xy, u_time);
    float sunFactor = clamp(1.0 - depthFactor, 0.0, 1.0);
    float sunAngle  = max(dot(v_geoNormal, u_sunDir), 0.0);

    vec3 color = u_underwaterColor * (1.0 - depthFactor * 0.7);
    color += vec3(0.15, 0.25, 0.10) * c * (1.0 - depthFactor * 0.5);
    color += vec3(0.3, 0.4, 0.5) * sunFactor * sunAngle * u_sunlightScattering;

    if (u_fogEnabled > 0.5)
    {
        float fogF = clamp((dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);
        color = mix(color, u_fogColor, fogF * u_fogBlend);
    }

    gl_FragColor = vec4(color, clamp(u_underwaterAlpha, 0.0, 1.0));
}
