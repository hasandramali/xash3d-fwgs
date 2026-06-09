/*
 * HL2 above-water fragment shader (GLSL ES 1.00 / gl4es).
 *
 * Pure refraction shader:
 *   - screen-space UVs from clip-space position
 *   - Q1-style texture coordinate warping
 *   - two-layer scrolling normalmap
 *   - Fresnel: pow(1 - abs(dot(N, normalize(V))), exponent)
 *   - refraction: screen-grab texture with normal distortion
 *   - Fresnel blends refracted scene with water body colour
 *   - diffuse (warp) texture overlay
 *   - fog
 */

#ifdef GL_ES
precision mediump float;
#endif

uniform sampler2D u_normalMap;
uniform sampler2D u_diffuseMap;
uniform sampler2D u_refractMap;

uniform highp float u_time;
uniform float u_fresnelExp;
uniform float u_strengthRefr;
uniform vec3  u_waterColor;
uniform float u_alpha;
uniform float u_distScale;
uniform float u_fogBlend;
uniform vec3  u_fogColor;
uniform float u_fogStart;
uniform float u_fogEnd;
uniform float u_fogEnabled;
uniform float u_refractEnabled;

varying vec2  v_texCoord;
varying vec4  v_clipPos;
varying vec3  v_normal;
varying vec3  v_eye;

void main()
{
    vec2 stc = (1.0 + (v_clipPos.xy / v_clipPos.w)) * 0.5;

    vec2 ntc;
    ntc.s = v_texCoord.s + sin(v_texCoord.t + u_time) * 0.125;
    ntc.t = v_texCoord.t + sin(v_texCoord.s + u_time) * 0.125;

    vec3 n  = texture2D(u_normalMap, ntc * 1.0 + vec2(u_time * 0.10, 0.0)).xyz;
    n      += texture2D(u_normalMap, ntc * 0.5 - vec2(0.0, u_time * 0.097)).xyz;
    n      -= 1.0 - 4.0 / 256.0;
    n       = normalize(n);

    float fres = pow(1.0 - abs(dot(n, normalize(v_eye))), u_fresnelExp);

    vec3 refr;
    if (u_refractEnabled > 0.5)
        refr = texture2D(u_refractMap, stc + n.st * u_strengthRefr).rgb;
    else
        refr = vec3(0.12, 0.25, 0.33);

    float dist = length(v_eye);
    float depthF = clamp(dist * u_distScale, 0.0, 1.0);
    refr = mix(refr, refr * 0.4, depthF);

    vec3 color = mix(refr, u_waterColor, clamp(fres, 0.0, 0.95));

    vec4 ts = texture2D(u_diffuseMap, ntc);
    color = mix(color, ts.rgb, 0.25 * ts.a);

    if (u_fogEnabled > 0.5)
    {
        float fogF = clamp((dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);
        color = mix(color, u_fogColor, fogF * u_fogBlend);
    }

    gl_FragColor = vec4(color, clamp(u_alpha, 0.0, 1.0));
}
