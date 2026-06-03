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
 * The sun specular term uses the LOW-FREQUENCY geometric normal coming
 * from the vertex displacement (v_geoNormal), so the highlight is a
 * broad, transparent rim that fades in/out with the wave slope.  The
 * blend with v_geoNormal.z is a cheap "light proxy":
 *   - flat water (N.z ~ 1)   -> 1.0 multiplier  (full sun)
 *   - tilted wave peak        -> N.z less than 1 -> dimmer
 *   - very steep trough face  -> N.z near 0     -> u_specularMin floor
 * This way the sun glint is invisible in the dark wave troughs and
 * bright on the flat tops, exactly the look the user asked for.
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
uniform vec3      u_sunDir;
uniform highp float u_time;
uniform float     u_fresnelFactor;
uniform float     u_alpha;
uniform float     u_ambient;
uniform float     u_density;
uniform float     u_normalscale;
uniform float     u_choppy;
uniform float     u_specular;
uniform float     u_specularMin;
uniform float     u_skyblend;
uniform float     u_scattering;
uniform float     u_rainIntensity;
uniform float     u_fogBlend;
uniform vec3      u_fogColor;
uniform float     u_fogStart;
uniform float     u_fogEnd;
uniform float     u_fogEnabled;

varying vec3 v_worldPos;
varying vec3 v_viewPos;
varying vec2 v_texCoord;
varying vec3 v_geoNormal;

const float WATER_F0   = 0.15;
const float WAVE_SCALE = 0.0035;
const float BUMP_BASE  = 1.2;
const vec2  WIND_DIR   = vec2(0.6, -0.8);
const float WIND_SPEED = 0.18;

/* OpenMW-style normal coords with choppy offset */
vec2 normalCoords(vec2 uv, float scale, float speed, float t, vec3 prev, float choppy)
{
    float z = max(abs(prev.z), 0.05);
    return uv * scale + WIND_DIR * t * (WIND_SPEED * speed)
           - (prev.xy / z) * choppy;
}

/* Paranoia2-style animUV: time-based micro-offset for extra motion */
vec2 animUV(vec2 uv, float t)
{
    vec2 dir = vec2(sin(t * 0.7 + uv.y * 3.0), cos(t * 0.5 + uv.x * 2.5));
    return uv + dir * 0.012;
}

/* Procedural raindrop/impact ripple */
float rainRipple(vec2 pos, float t)
{
    float best = 1.0;
    for (int i = 0; i < 4; i++)
    {
        float fi = float(i);
        vec2  origin = vec2(
            sin(fi * 3.14159 + 1.0) * 400.0,
            cos(fi * 2.71828 + 2.0) * 400.0);
        float phase  = fract(t * 1.5 + fi * 0.7);
        float dist   = distance(pos, origin);
        float ring   = abs(dist - phase * 180.0);
        float width  = 12.0;
        float ripple = exp(-ring * ring / (width * width));
        best = min(best, 1.0 - ripple * 0.5);
    }
    return best;
}

void main()
{
    vec2  uv = v_worldPos.xy * WAVE_SCALE;
    float t  = u_time;

    /* OpenMW-style 6-layer normal sampling */
    vec3 n0 = texture2D(u_normalMap, animUV(normalCoords(uv, 0.05, 0.04, t, vec3(0.0), u_choppy), t)).rgb * 2.0 - 1.0;
    vec3 n1 = texture2D(u_normalMap, animUV(normalCoords(uv, 0.10, 0.08, t, n0, u_choppy), t * 1.3)).rgb * 2.0 - 1.0;
    vec3 n2 = texture2D(u_normalMap, animUV(normalCoords(uv, 0.25, 0.15, t, n1, u_choppy), t * 0.7)).rgb * 2.0 - 1.0;
    vec3 n3 = texture2D(u_normalMap, animUV(normalCoords(uv, 0.50, 0.25, t, n2, u_choppy), t * 1.1)).rgb * 2.0 - 1.0;
    vec3 n4 = texture2D(u_normalMap, animUV(normalCoords(uv, 1.00, 0.40, t, n3, u_choppy), t * 0.5)).rgb * 2.0 - 1.0;
    vec3 n5 = texture2D(u_normalMap, animUV(normalCoords(uv, 2.00, 0.60, t, n4, u_choppy), t * 1.7)).rgb * 2.0 - 1.0;

    /* Blend all 6 layers with OpenMW-style weights */
    vec3 n = normalize(n0 * 0.20 + n1 * 0.18 + n2 * 0.16 + n3 * 0.15 + n4 * 0.13 + n5 * 0.08);

    /* Rain / impact ripple disturbance */
    float ripple = u_rainIntensity * (1.0 - rainRipple(v_worldPos.xy, t));
    n = normalize(n + vec3(ripple, ripple, 0.0));

    float bump = BUMP_BASE * u_normalscale;
    vec3  Ndetail = vec3(-n.x * bump, -n.y * bump, n.z);
    vec3  N       = normalize(v_geoNormal + Ndetail * 0.6);

    vec3  V  = normalize(u_cameraPos - v_worldPos);
    float cosTheta = clamp(dot(N, V), 0.0, 1.0);

    /* Schlick Fresnel (air-to-water) */
    float fresnel = WATER_F0 + (1.0 - WATER_F0) * pow(1.0 - cosTheta, u_fresnelFactor);
    fresnel = clamp(fresnel, 0.0, 0.95);

    /* depth-based water tint */
    float dist        = length(v_viewPos);
    float depthFactor = clamp(dist * (0.0025 * u_density), 0.0, 1.0);
    vec3  deepColor    = u_waterColor * 0.55;
    vec3  shallowColor = u_waterColor + vec3(0.05, 0.08, 0.10);
    vec3  baseColor    = mix(shallowColor, deepColor, depthFactor);
    baseColor *= u_ambient;

    /* fake sky reflection */
    vec3  skyTint    = u_skyColor;
    vec3  finalColor = mix(baseColor, skyTint, clamp(fresnel * u_skyblend, 0.0, 0.98));

    /* Dual specular: broad (geo normal) + sharp (detail normal) */
    vec3  R = reflect(-V, v_geoNormal);
    float specBroad = pow(max(dot(R, u_sunDir), 0.0), 32.0);
    vec3  R2 = reflect(-V, N);
    float specSharp = pow(max(dot(R2, u_sunDir), 0.0), 192.0);
    float lightAmount = mix(u_specularMin, 1.0, clamp(v_geoNormal.z, 0.0, 1.0));
    float spec   = (specBroad * 0.5 + specSharp * 1.2) * u_specular * lightAmount;
    finalColor += u_specularColor * spec;

    /* OpenMW-style sunlight scattering through wave crests */
    float scatterFactor = u_scattering * pow(max(dot(N, u_sunDir), 0.0), 8.0);
    vec3  scatterColor  = vec3(0.25, 0.50, 0.75) * scatterFactor;
    finalColor += scatterColor;

    if (u_fogEnabled > 0.5)
    {
        float fogF = clamp((dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);
        finalColor = mix(finalColor, u_fogColor, fogF * u_fogBlend);
    }
    gl_FragColor = vec4(finalColor, clamp(u_alpha, 0.0, 1.0));
}

/* Paranoia2-style: time-based UV offset creates wave motion from a
 * single normal map by shifting sample positions over time. */
vec2 animUV(vec2 uv, float t)
{
    vec2 dir = vec2(sin(t * 0.7 + uv.y * 3.0), cos(t * 0.5 + uv.x * 2.5));
    return uv + dir * 0.012;
}

void main()
{
    vec2  uv = v_worldPos.xy * WAVE_SCALE;
    float t  = u_time;

    /* Paranoia2-style layered normal-map with animated UV offsets.
     * Each layer samples at a different time-offset and UV scale,
     * producing the same "~~~" visual as 29-frame animated textures. */
    vec3 n0 = texture2D(u_normalMap, animUV(waveUV(uv, 1.0, 0.10, t, vec3(0.0), u_choppy), t)).rgb * 2.0 - 1.0;
    vec3 n1 = texture2D(u_normalMap, animUV(waveUV(uv, 2.0, 0.18, t, n0, u_choppy), t * 1.3)).rgb * 2.0 - 1.0;
    vec3 n2 = texture2D(u_normalMap, animUV(waveUV(uv, 4.0, 0.30, t, n1, u_choppy), t * 0.7)).rgb * 2.0 - 1.0;
    vec3 n3 = texture2D(u_normalMap, animUV(waveUV(uv, 8.0, 0.55, t, n2, u_choppy), t * 1.1)).rgb * 2.0 - 1.0;
    vec3 n  = normalize(n0 * 0.30 + n1 * 0.25 + n2 * 0.25 + n3 * 0.20);
    float bump = BUMP_BASE * u_normalscale;
    vec3  Ndetail = vec3(-n.x * bump, -n.y * bump, n.z);
    vec3  N       = normalize(v_geoNormal + Ndetail * 0.6);

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

    /* Paranoia2-style sun specular: use DETAIL normal for sharp glint
     * on wave crests, with geo normal for broad light proxy.
     * Two specular terms: broad (geo normal) + sharp (detail normal). */
    vec3  R          = reflect(-V, v_geoNormal);
    vec3  sunDir     = normalize(vec3(0.4, 0.4, 0.82));
    float specBroad  = pow(max(dot(R, sunDir), 0.0), 32.0);
    vec3  R2         = reflect(-V, N);
    float specSharp  = pow(max(dot(R2, sunDir), 0.0), 192.0);
    float lightAmount = mix(u_specularMin, 1.0, clamp(v_geoNormal.z, 0.0, 1.0));
    float spec       = (specBroad * 0.5 + specSharp * 1.2) * u_specular * lightAmount;
    finalColor      += u_specularColor * spec;

    if (u_fogEnabled > 0.5)
    {
        float fogF = clamp((dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);
        finalColor = mix(finalColor, u_fogColor, fogF * u_fogBlend);
    }
    gl_FragColor = vec4(finalColor, clamp(u_alpha, 0.0, 1.0));
}
