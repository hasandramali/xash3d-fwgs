/*
 * HL2 underwater fragment shader (GLSL ES 1.00 / gl4es).
 *
 * Used when the camera is underwater, looking up at the water surface:
 *   - screen-space UVs from clip-space position
 *   - Q1-style warp
 *   - two-layer scrolling normalmap
 *   - screen-grab distortion for refraction (objects above water appear to move)
 *   - caustic-like modulation
 *   - deep-water colour tint
 *   - fog
 */

#ifdef GL_ES
precision mediump float;
#endif

uniform sampler2D u_normalMap;
uniform sampler2D u_diffuseMap;
uniform sampler2D u_refractMap;

uniform highp float u_time;
uniform float u_strengthRefr;
uniform float u_strengthRefrUnder;
uniform vec3  u_waterColor;
uniform float u_waterGamma;
uniform float u_alpha;
uniform float u_fogBlend;
uniform vec3  u_fogColor;
uniform float u_fogStart;
uniform float u_fogEnd;
uniform float u_fogEnabled;
uniform highp float u_refractionSpeed;
uniform highp float u_waveSpeed;
uniform float u_distScale;
uniform vec3  u_distColor;
uniform float u_causticIntensity;
uniform vec3  u_causticColor;
uniform float u_refractEnabled;

varying vec2  v_texCoord;
varying vec4  v_clipPos;
varying vec3  v_normal;
varying vec3  v_eye;

float caustic(vec2 p, float t)
{
    float c1 = sin(p.x * 3.0 + p.y * 2.0 + t * 0.8) * cos(p.y * 2.5 - t * 0.6);
    float c2 = sin((p.x + p.y) * 4.0 + t * 1.2) * 0.5;
    float c3 = cos((p.x - p.y) * 5.0 - t * 0.9) * 0.3;
    float c4 = sin(p.x * 6.0 - p.y * 3.0 + t * 1.5) * 0.2;
    return clamp(c1 * 0.5 + c2 + c3 + c4, 0.0, 1.0);
}

void main()
{
    vec2 stc = (1.0 + (v_clipPos.xy / v_clipPos.w)) * 0.5;

    float t  = u_time;
    float rs = u_refractionSpeed;
    float ws = u_waveSpeed;

    /* water colour and distance */
    float dist = length(v_eye);
    float depthF = clamp(dist * u_distScale * 0.5, 0.0, 1.0);
    vec3 wc = u_waterColor * u_waterGamma;

    /* Q1-style warp */
    vec2 ntc;
    ntc.s = v_texCoord.s + sin(v_texCoord.t + t * ws) * 0.125;
    ntc.t = v_texCoord.t + sin(v_texCoord.s + t * ws) * 0.125;

    /* two-layer scrolling normalmap */
    vec3 n  = texture2D(u_normalMap, ntc * 1.0 + vec2(t * 0.10 * rs, 0.0)).xyz;
    n      += texture2D(u_normalMap, ntc * 0.5 - vec2(0.0, t * 0.097 * rs)).xyz;
    n      -= 1.0 - 4.0 / 256.0;
    n       = normalize(n);

    /* normal-based surface animation */
    float surfAnim = sin(ntc.s * 8.0 + t * 0.5) * cos(ntc.t * 6.0 - t * 0.7) * 0.5 + 0.5;

    /* caustic pattern */
    float c = caustic(gl_FragCoord.xy, t * rs);
    c = c * 0.5 + surfAnim * 0.5;

    /* refraction with refract-enabled check */
    vec3 refr;
    if (u_refractEnabled > 0.5)
    {
        float twitch = sin(t * 0.5 + ntc.s * 10.0) * 0.003;
        refr = texture2D(u_refractMap, stc + n.st * u_strengthRefrUnder + vec2(twitch, twitch * 0.7)).rgb;
    }
    else
    {
        refr = wc * 0.6;
    }

    /* distance-based tint */
    refr = mix(refr, refr * u_distColor, depthF);

    /* underwater colour: blend screen with water color, caustics, and animated surface */
    float wcBlend = depthF * 0.4;
    vec3 color = mix(refr, wc * 0.6, wcBlend);

    /* apply caustic light with configurable color and intensity */
    float cStrength = u_causticIntensity * (1.0 - depthF * 0.5);
    color += u_causticColor * c * cStrength;

    /* subtle surface shimmer from normal map */
    vec3 shimmer = vec3(0.02, 0.03, 0.02) * (n.x + n.y) * (1.0 - depthF);
    color += shimmer;

    /* fog */
    if (u_fogEnabled > 0.5)
    {
        float fogF = clamp((dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);
        color = mix(color, u_fogColor, fogF * u_fogBlend);
    }

    gl_FragColor = vec4(color, clamp(u_alpha, 0.0, 1.0));
}
