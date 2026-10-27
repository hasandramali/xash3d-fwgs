/*
 * HL2 water vertex shader (GLSL ES 1.00 / gl4es).
 *
 * Ported from fteqw's water.glsl vertex shader with optional
 * wave displacement.  Varyings match fteqw conventions:
 *   v_texCoord = tc    (texture coords)
 *   v_clipPos  = tf    (clip-space position, for screen UVs)
 *   v_normal   = norm  (surface normal)
 *   v_eye      = eye   (view direction)
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
uniform highp float u_waveSpeed;
uniform vec3 u_cameraPos;

varying vec2 v_texCoord;
varying vec4 v_clipPos;
varying vec3 v_normal;
varying vec3 v_eye;

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
    float t    = u_time * u_waveSpeed;
    float freq = max(u_wavefreq, 0.001);
    float amp  = u_waveheight;
    if (amp > 0.001)
        pos.z += waveHeight(pos.xy, t, freq, amp);

    vec4 viewPos = u_modelView * vec4(pos, 1.0);
    gl_Position = u_projection * viewPos;

    v_texCoord = a_texCoord;
    v_clipPos  = gl_Position;
    v_normal   = vec3(0.0, 0.0, 1.0);
    v_eye      = u_cameraPos - pos;
}
