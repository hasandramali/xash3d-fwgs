/*
 * HL2-inspired water vertex shader (GLSL ES 1.00 / gl4es).
 *
 * Adapted from fteqw's HL2 water shader approach.
 * Optional wave displacement with analytical geometric normal.
 *
 * NOTE: mirrors the source string embedded in ref/gl/gl_watershader.c
 */

#ifdef GL_ES
precision highp float;
#endif

attribute vec4 a_position;
attribute vec2 a_texCoord;
uniform mat4 u_modelView;
uniform mat4 u_projection;
uniform highp float u_time;
uniform highp float u_waveheight;
uniform highp float u_wavefreq;

varying vec3 v_worldPos;
varying vec3 v_viewPos;
varying vec2 v_texCoord;
varying vec3 v_geoNormal;

float waveHeight(vec2 p, float t, float freq, float amp)
{
    float f1 = freq;
    float f2 = freq * 0.83;
    float f3 = freq * 1.31;
    float f4 = freq * 0.57;
    float h1 = sin(p.x * f1 + t * 1.1) * 0.50;
    float h2 = cos(p.y * f2 + t * 0.7) * 0.30;
    float h3 = sin((p.x + p.y) * f3 + t * 1.5) * 0.15;
    float h4 = cos(p.x * 0.7 - p.y * 0.7 + t * 0.9) * 0.15;
    return (h1 + h2 + h3 + h4) * amp;
}

void main()
{
    vec3  pos  = a_position.xyz;
    float t    = u_time;
    float freq = max(u_wavefreq, 0.001);
    float amp  = u_waveheight;
    if (amp > 0.001)
        pos.z += waveHeight(pos.xy, t, freq, amp);

    float f1 = freq;
    float f2 = freq * 0.83;
    float f3 = freq * 1.31;
    float f4 = freq * 0.57;
    float dHdx =  cos(pos.x * f1 + t * 1.1) * 0.50 * f1
               +  cos((pos.x + pos.y) * f3 + t * 1.5) * 0.15 * f3
               -  sin(pos.x * 0.7 - pos.y * 0.7 + t * 0.9) * 0.15 * f4 * 0.7;
    float dHdy = -sin(pos.y * f2 + t * 0.7) * 0.30 * f2
               +  cos((pos.x + pos.y) * f3 + t * 1.5) * 0.15 * f3
               +  sin(pos.x * 0.7 - pos.y * 0.7 + t * 0.9) * 0.15 * f4 * (-0.7);
    if (amp > 0.001) { dHdx *= amp; dHdy *= amp; }
    else { dHdx = 0.0; dHdy = 0.0; }
    v_geoNormal = normalize(vec3(-dHdx, -dHdy, 1.0));

    v_worldPos = pos;
    v_texCoord = a_texCoord;
    vec4 viewPos = u_modelView * vec4(pos, 1.0);
    v_viewPos = viewPos.xyz;
    gl_Position = u_projection * viewPos;
}
