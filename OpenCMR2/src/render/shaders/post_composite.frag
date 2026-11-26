#version 450
// 3D-only composite: Half-res GTAO contact shadows + multi-scale bloom +
// Faithful+ ACES Filmic tone mapping + FidelityFX CAS sharpening.
// Runs strictly before 2D HUD/menus so text and UI stay 100% sharp and unshifted.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D hdrTex;
layout(set = 2, binding = 1) uniform sampler2D bloomHalfTex;
layout(set = 2, binding = 2) uniform sampler2D bloomQuarterTex;
layout(set = 2, binding = 3) uniform sampler2D aoTex;

layout(std140, set = 3, binding = 0) uniform PostCompositeUniforms {
    vec4 params;    // x: toneMapping (0/1), y: bloomIntensity, z: exposure, w: ssaoStrength
    vec4 casParams; // x: casEnabled (0/1), y: casStrength (0..1), z: invWidth, w: invHeight
};

vec3 AcesRaw(vec3 x)
{
    return (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
}

vec3 ApplyToneMap(vec3 lin)
{
    vec3 mapped;
    if (params.x > 0.5) {
        float exposure = params.z;
        vec3 whiteScale = vec3(1.0) / AcesRaw(vec3(3.2 * exposure));
        vec3 aces = clamp(AcesRaw(lin * exposure) * whiteScale, 0.0, 1.0);
        // Preserve CMR2's bright daylight/snow whites while keeping filmic highlight roll-off.
        vec3 softClip = lin / (1.0 + lin * 0.18);
        mapped = clamp(mix(softClip, aces, 0.72), 0.0, 1.0);
        // Gentle saturation recovery after filmic curve.
        float luma = dot(mapped, vec3(0.2126, 0.7152, 0.0722));
        mapped = clamp(mix(vec3(luma), mapped, 1.06), 0.0, 1.0);
    } else {
        mapped = clamp(lin, 0.0, 1.0);
    }
    return pow(max(mapped, vec3(0.0)), vec3(1.0 / 2.2));
}

vec3 EvalShadedPixel(vec2 uv, vec3 bloomAdd, float aoMul)
{
    vec3 src = texture(hdrTex, uv).rgb;
    vec3 lin = pow(max(src, vec3(0.0)), vec3(2.2));
    // Protect bright emissive lights (headlights/brake lights/sun) from SSAO darkening.
    float luma = dot(lin, vec3(0.2126, 0.7152, 0.0722));
    float emissiveProtect = smoothstep(0.95, 2.2, luma);
    lin *= mix(aoMul, 1.0, emissiveProtect);
    lin += bloomAdd;
    return ApplyToneMap(lin);
}

void main()
{
    vec4 srcCenter = texture(hdrTex, inUV);

    vec3 bloomAdd = vec3(0.0);
    if (params.y > 0.0) {
        vec3 bHalf = texture(bloomHalfTex, inUV).rgb;
        vec3 bQuarter = texture(bloomQuarterTex, inUV).rgb;
        bloomAdd = (bHalf * 0.45 + bQuarter * 0.55) * params.y;
    }

    float aoMul = 1.0;
    if (params.w > 0.001) {
        float rawAO = clamp(texture(aoTex, inUV).r, 0.0, 1.0);
        // Subtle multi-bounce curve keeps dark contact crevices rich without crushing midtones.
        aoMul = mix(1.0, pow(rawAO, 1.15), clamp(params.w, 0.0, 1.0));
    }

    vec3 center = EvalShadedPixel(inUV, bloomAdd, aoMul);

    // AMD FidelityFX CAS (Contrast Adaptive Sharpening) on 3D scene before 2D HUD.
    if (casParams.x > 0.5 && casParams.y > 0.001) {
        vec2 dx = vec2(casParams.z, 0.0);
        vec2 dy = vec2(0.0, casParams.w);
        vec3 n = EvalShadedPixel(inUV - dy, bloomAdd, aoMul);
        vec3 s = EvalShadedPixel(inUV + dy, bloomAdd, aoMul);
        vec3 w = EvalShadedPixel(inUV - dx, bloomAdd, aoMul);
        vec3 e = EvalShadedPixel(inUV + dx, bloomAdd, aoMul);

        vec3 mn = min(min(min(n, s), min(w, e)), center);
        vec3 mx = max(max(max(n, s), max(w, e)), center);

        // Adaptive sharpening weight attenuates on already high-contrast edges (zero halo).
        vec3 amp = clamp(min(mn, vec3(1.0) - mx) / max(mx, vec3(1e-4)), vec3(0.0), vec3(1.0));
        amp = sqrt(amp);
        float peak = -1.0 / mix(8.0, 5.0, clamp(casParams.y, 0.0, 1.0));
        vec3 weight = amp * peak;
        vec3 rcpWeight = vec3(1.0) / (vec3(1.0) + 4.0 * weight);
        center = clamp((center + (n + s + w + e) * weight) * rcpWeight, 0.0, 1.0);
    }

    outColor = vec4(center, srcCenter.a);
}
