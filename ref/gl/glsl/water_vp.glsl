/*
 * PrimeXT-inspired water vertex shader (GLSL ES 1.00 / gl4es).
 *
 * NOTE: this file mirrors the source string embedded in
 * ref/gl/gl_watershader.c (water_vertex_source). It is kept here for
 * reference and editing convenience; the runtime does not load it.
 */

#ifdef GL_ES
precision highp float;
#endif

attribute vec4 a_position;
attribute vec2 a_texCoord;

uniform mat4 u_modelView;
uniform mat4 u_projection;

varying vec3 v_worldPos;
varying vec3 v_viewPos;
varying vec2 v_texCoord;

void main()
{
    v_worldPos = a_position.xyz;
    v_texCoord = a_texCoord;
    vec4 viewPos = u_modelView * a_position;
    v_viewPos = viewPos.xyz;
    gl_Position = u_projection * viewPos;
}
