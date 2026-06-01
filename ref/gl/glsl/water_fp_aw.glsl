/*
 * PrimeXT-inspired above-water fragment shader (GLSL ES 1.00 / gl4es).
 *
 * Adapted from PrimeXT's LIQUID_SURFACE branch in
 * game_dir/glsl/forward/scene_bmodel_fp.glsl and game_dir/glsl/fresnel.h.
 *
 * Uses the Schlick Fresnel approximation:
 *     F = F0 + (1 - F0) * pow(1 - cosTheta, power)
 * with PrimeXT's WATER_F0_VALUE (0.15) and FRESNEL_FACTOR (default 5.0).
 *
 * Every visual knob is exposed through a uniform and a matching r_water_*
 * cvar (see ref/gl/gl_watershader.c).
 *
 * NOTE: this file mirrors the source string embedded in
 * ref/gl/gl_watershader.c (water_frag_above_source).
 */

#ifdef GL_ES
precision mediump float;
#endif

uniform sampler2D u_normalMap;
uniform vec3      u_cameraPos;
uniform vec3      u_waterColor;
uniform vec3      u_skyColor;
uniform vec3      u_specularColor;
uniform float     u_time;
uniform float     u_fresnelFactor;
uniform float     u_alpha;
uniform float     u_ambient;
uniform float     u_density;
uniform float     u_normalscale;
uniform float     u_choppy;
uniform float     u_specular;
uniform float     u_skyblend;
uniform float     u_fogBlend;
uniform vec3      u_fogColor;
uniform float     u_fogStart;
uniform float     u_fogEnd;
uniform float     u_fogEnabled;

varying vec3 v_worldPos;
varying vec3 v_viewPos;
varying vec2 v_texCoord;

const float WATER_F0   = 0.15;
const float WAVE_SCALE = 0.0035;
const float BUMP_BASE  = 1.2;
const vec2  WIND_DIR   = vec2(0.6, -0.8);
const float WIND_SPEED = 0.18;

vec2 waveUV(vec2 uv, float scale, float speed, float t, vec3 prev, float choppy)
{
    float z = max(abs(prev.z), 0.05);
    return uv * scale + WIND_DIR * t * (WIND_SPEED * speed)
           - (prev.xy / z) * choppy;
}

void main()
{
    vec2  uv = v_worldPos.xy * WAVE_SCALE;
    float t  = u_time;

    /* layered normal-map fetches (PrimeXT-style chop chaining) */
    vec3 n0 = texture2D(u_normalMap, waveUV(uv, 1.0, 0.10, t, vec3(0.0), u_choppy)).rgb * 2.0 - 1.0;
    vec3 n1 = texture2D(u_normalMap, waveUV(uv, 2.0, 0.18, t, n0, u_choppy)).rgb * 2.0 - 1.0;
    vec3 n2 = texture2D(u_normalMap, waveUV(uv, 4.0, 0.30, t, n1, u_choppy)).rgb * 2.0 - 1.0;
    vec3 n3 = texture2D(u_normalMap, waveUV(uv, 8.0, 0.55, t, n2, u_choppy)).rgb * 2.0 - 1.0;
    vec3 n  = normalize(n0 * 0.30 + n1 * 0.25 + n2 * 0.25 + n3 * 0.20);
    float bump = BUMP_BASE * u_normalscale;
    vec3  N    = normalize(vec3(-n.x * bump, -n.y * bump, n.z));

    vec3  V        = normalize(u_cameraPos - v_worldPos);
    float cosTheta = clamp(dot(N, V), 0.0, 1.0);

    /* Schlick Fresnel from PrimeXT/fresnel.h */
    float fresnel = WATER_F0 + (1.0 - WATER_F0) * pow(1.0 - cosTheta, u_fresnelFactor);
    fresnel = clamp(fresnel, 0.0, 0.95);

    /* depth-based water tint (waterBorderFactor analogue) */
    float dist        = length(v_viewPos);
    float depthFactor = clamp(dist * (0.0025 * u_density), 0.0, 1.0);
    vec3  deepColor    = u_waterColor * 0.55;
    vec3  shallowColor = u_waterColor + vec3(0.05, 0.08, 0.10);
    vec3  baseColor    = mix(shallowColor, deepColor, depthFactor);
    baseColor *= u_ambient; /* tame the fullbright look */

    /* fake sky reflection (no FBO planar reflection in this backend) */
    vec3 skyTint    = u_skyColor;
    vec3 finalColor = mix(baseColor, skyTint, clamp(fresnel * u_skyblend, 0.0, 0.98));

    /* sun specular term (cheap rim highlight on wave peaks) */
    vec3  sunDir = normalize(vec3(0.4, 0.4, 0.82));
    vec3  R      = reflect(-V, N);
    float spec   = pow(max(dot(R, sunDir), 0.0), 192.0) * 1.6 * u_specular;
    finalColor  += u_specularColor * spec;

    if (u_fogEnabled > 0.5)
    {
        float fogF = clamp((dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);
        finalColor = mix(finalColor, u_fogColor, fogF * u_fogBlend);
    }
    gl_FragColor = vec4(finalColor, clamp(u_alpha, 0.0, 1.0));
}
