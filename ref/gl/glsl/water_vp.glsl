/*
 * PrimeXT-inspired water vertex shader with Paranoia2-style layered
 * wave displacement (GLSL ES 1.00 / gl4es).
 *
 * Three superposed sin/cos waves at slightly different frequencies
 * produce the "~~~" up/down motion. The geometric normal is rebuilt
 * from the analytical derivatives of that displacement so the
 * fragment shader can use it as a "light proxy" (flat water = lit,
 * tilted wave peak = dark).
 *
 * The vertex shader is shared between above-water and underwater
 * programs; the only difference between the two fragment programs
 * is how they colour the surface.
 *
 * NOTE: this file mirrors the source string embedded in
 * ref/gl/gl_watershader.c (water_vertex_source).
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
    float h1 = sin( p.x * f1 + t * 1.1 ) * 0.55;
    float h2 = cos( p.y * f2 + t * 0.7 ) * 0.30;
    float h3 = sin((p.x + p.y) * f3 + t * 1.5 ) * 0.20;
    return (h1 + h2 + h3) * amp;
}

void main()
{
    vec3  pos  = a_position.xyz;
    float t    = u_time;
    float freq = max(u_wavefreq, 0.001);
    float amp  = u_waveheight;
    pos.z += waveHeight(pos.xy, t, freq, amp);

    /* analytical derivatives of waveHeight w.r.t. x and y */
    float f1 = freq;
    float f2 = freq * 0.83;
    float f3 = freq * 1.31;
    float dHdx =  cos( pos.x * f1 + t * 1.1 ) * 0.55 * f1
               +   cos((pos.x + pos.y) * f3 + t * 1.5 ) * 0.20 * f3;
    float dHdy = -sin( pos.y * f2 + t * 0.7 ) * 0.30 * f2
               +   cos((pos.x + pos.y) * f3 + t * 1.5 ) * 0.20 * f3;
    dHdx *= amp;
    dHdy *= amp;
    v_geoNormal = normalize(vec3(-dHdx, -dHdy, 1.0));

    v_worldPos = pos;
    v_texCoord = a_texCoord;
    vec4 viewPos = u_modelView * vec4(pos, 1.0);
    v_viewPos = viewPos.xyz;
    gl_Position = u_projection * viewPos;
}
