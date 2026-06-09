/*
 * HL2-inspired above-water fragment shader (GLSL ES 1.00 / gl4es).
 *
 * Adapted from fteqw's altwater.glsl / water.glsl approach:
 *   - q1-style texture coordinate warping
 *   - two-layer scrolling normalmap sampling
 *   - Schlick Fresnel
 *   - fake refraction = water color + depth tint
 *   - fake reflection = sky color (Fresnel-blended)
 *   - specular highlight
 *   - optional diffuse warp texture overlay
 *   - fog
 *
 * NOTE: mirrors the source string embedded in ref/gl/gl_watershader.c
 */

#ifdef GL_ES
precision mediump float;
#endif

uniform sampler2D u_normalMap;
uniform sampler2D u_waterTex;
uniform vec3      u_cameraPos;
uniform vec3      u_waterColor;
uniform vec3      u_skyColor;
uniform vec3      u_specularColor;
uniform vec3      u_sunDir;
uniform highp float u_time;
uniform float     u_fresnelFactor;
uniform float     u_alpha;
uniform float     u_density;
uniform float     u_specular;
uniform float     u_skyblend;
uniform float     u_sunlightScattering;
uniform float     u_fogBlend;
uniform vec3      u_fogColor;
uniform float     u_fogStart;
uniform float     u_fogEnd;
uniform float     u_fogEnabled;

varying vec3 v_worldPos;
varying vec3 v_viewPos;
varying vec2 v_texCoord;
varying vec3 v_geoNormal;

const float WATER_F0 = 0.15;
const float WAVE_SCALE = 0.004;

void main()
{
    vec2 uv = v_worldPos.xy * WAVE_SCALE;
    float t = u_time;

    /* q1-style warp for normalmap coords (HL2 water style) */
    vec2 ntc;
    ntc.s = uv.s + sin(uv.t * 3.0 + t) * 0.1;
    ntc.t = uv.t + cos(uv.s * 3.0 + t * 0.8) * 0.1;

    /* two-layer scrolling normalmap (HL2 altwater approach) */
    vec3 n1 = texture2D(u_normalMap, ntc * 1.2 + vec2(t * 0.08, 0.0)).xyz;
    vec3 n2 = texture2D(u_normalMap, ntc * 0.6 - vec2(0.0, t * 0.06)).xyz;
    vec3 N = normalize((n1 + n2) * 2.0 - 1.0);

    vec3 V = normalize(u_cameraPos - v_worldPos);
    float NdotV = max(dot(N, V), 0.0);

    /* Schlick Fresnel */
    float fresnel = WATER_F0 + (1.0 - WATER_F0) * pow(1.0 - NdotV, u_fresnelFactor);
    fresnel = clamp(fresnel, 0.0, 0.95);

    /* fake refraction = water color with depth-based darkening */
    float dist = length(v_viewPos);
    float depthFactor = clamp(dist * u_density, 0.0, 1.0);
    vec3 refr = u_waterColor * (1.0 - depthFactor * 0.6);

    /* subsurface sunlight scattering */
    float scatter = u_sunlightScattering * pow(max(dot(N, u_sunDir), 0.0), 8.0);
    refr += vec3(0.2, 0.4, 0.6) * scatter;

    /* fake reflection = sky color */
    vec3 refl = u_skyColor;

    /* Fresnel blend */
    vec3 color = mix(refr, refl, fresnel * u_skyblend);

    /* specular */
    vec3 R = reflect(-V, N);
    float spec = pow(max(dot(R, u_sunDir), 0.0), 64.0) * u_specular;
    color += u_specularColor * spec;

    /* diffuse warp texture overlay (like HL2 water texture on top) */
    vec4 waterTexel = texture2D(u_waterTex, v_texCoord);
    color = mix(color, waterTexel.rgb, 0.15);

    if (u_fogEnabled > 0.5)
    {
        float fogF = clamp((dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);
        color = mix(color, u_fogColor, fogF * u_fogBlend);
    }

    gl_FragColor = vec4(color, clamp(u_alpha, 0.0, 1.0));
}
