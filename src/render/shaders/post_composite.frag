#version 450
// 3D-only composite: subtle multi-scale bloom + Faithful+ ACES Filmic tone mapping.
// Runs before 2D HUD/menus so text and UI stay 100% sharp and unshifted.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D hdrTex;
layout(set = 2, binding = 1) uniform sampler2D bloomHalfTex;
layout(set = 2, binding = 2) uniform sampler2D bloomQuarterTex;

layout(std140, set = 3, binding = 0) uniform PostCompositeUniforms {
    vec4 params;    // x: toneMapping (0/1), y: bloomIntensity, z: exposure, w: unused
};

vec3 AcesRaw(vec3 x)
{
    return (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
}

void main()
{
    vec4 src = texture(hdrTex, inUV);
    vec3 lin = pow(max(src.rgb, vec3(0.0)), vec3(2.2));

    if (params.y > 0.0) {
        vec3 bHalf = texture(bloomHalfTex, inUV).rgb;
        vec3 bQuarter = texture(bloomQuarterTex, inUV).rgb;
        vec3 bloom = bHalf * 0.45 + bQuarter * 0.55;
        lin += bloom * params.y;
    }

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

    vec3 srgb = pow(max(mapped, vec3(0.0)), vec3(1.0 / 2.2));
    outColor = vec4(srgb, src.a);
}
