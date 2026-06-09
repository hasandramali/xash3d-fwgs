/*
 * HL2 underwater fragment shader (GLSL ES 1.00 / gl4es).
 *
 * Simple deep-water tint with caustics and light shafts.
 *
 * NOTE: mirrors the source string embedded in ref/gl/gl_watershader.c
 */

#ifdef GL_ES
precision mediump float;
#endif

uniform highp float u_time;
uniform vec3  u_uwColor;
uniform float u_uwAlpha;
uniform float u_uwDensity;
uniform float u_uwScattering;
uniform float u_fogBlend;
uniform vec3  u_fogColor;
uniform float u_fogStart;
uniform float u_fogEnd;
uniform float u_fogEnabled;

varying vec3  v_eye;
varying vec3  v_normal;

float caustic(vec2 p, float t)
{
    vec2 uv = p * 0.002;
    float c1 = sin(uv.x * 3.0 + t * 0.8) * cos(uv.y * 2.5 - t * 0.6);
    float c2 = sin((uv.x + uv.y) * 4.0 + t * 1.2) * 0.5;
    float c3 = cos((uv.x - uv.y) * 5.0 - t * 0.9) * 0.3;
    return clamp(c1 * 0.5 + c2 + c3, 0.0, 1.0);
}

void main()
{
    float dist  = length(v_eye);
    float depthF = clamp(dist * u_uwDensity, 0.0, 1.0);
    float c = caustic(gl_FragCoord.xy, u_time);
    float sunF = clamp(1.0 - depthF, 0.0, 1.0);
    vec3 color = u_uwColor * (1.0 - depthF * 0.7);
    color += vec3(0.15, 0.25, 0.10) * c * (1.0 - depthF * 0.5);
    color += vec3(0.3, 0.4, 0.5) * sunF * u_uwScattering;
    if (u_fogEnabled > 0.5)
    {
        float fogF = clamp((dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);
        color = mix(color, u_fogColor, fogF * u_fogBlend);
    }
    gl_FragColor = vec4(color, clamp(u_uwAlpha, 0.0, 1.0));
}
