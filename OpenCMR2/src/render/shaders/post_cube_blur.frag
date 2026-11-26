#version 450
// 9-tap Gaussian roughness prefilter downsample for dynamic car reflection cubemap faces.
// Generates mip 1..4 (128 -> 64 -> 32 -> 16) so GGX IBL can sample roughness via textureLod().

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D srcFace;

layout(std140, set = 3, binding = 0) uniform CubeBlurUniforms {
    vec4 params;    // xy: srcTexelSize (1/srcW, 1/srcH), z: spread, w: unused
};

void main()
{
    vec2 t = params.xy * params.z;
    vec3 c  = texture(srcFace, inUV).rgb * 0.25;
    vec3 c1 = texture(srcFace, clamp(inUV + vec2(-t.x, -t.y), vec2(0.0), vec2(1.0))).rgb;
    vec3 c2 = texture(srcFace, clamp(inUV + vec2( t.x, -t.y), vec2(0.0), vec2(1.0))).rgb;
    vec3 c3 = texture(srcFace, clamp(inUV + vec2(-t.x,  t.y), vec2(0.0), vec2(1.0))).rgb;
    vec3 c4 = texture(srcFace, clamp(inUV + vec2( t.x,  t.y), vec2(0.0), vec2(1.0))).rgb;
    vec3 e1 = texture(srcFace, clamp(inUV + vec2(-t.x * 1.5, 0.0), vec2(0.0), vec2(1.0))).rgb;
    vec3 e2 = texture(srcFace, clamp(inUV + vec2( t.x * 1.5, 0.0), vec2(0.0), vec2(1.0))).rgb;
    vec3 e3 = texture(srcFace, clamp(inUV + vec2(0.0, -t.y * 1.5), vec2(0.0), vec2(1.0))).rgb;
    vec3 e4 = texture(srcFace, clamp(inUV + vec2(0.0,  t.y * 1.5), vec2(0.0), vec2(1.0))).rgb;

    vec3 outRgb = c + (c1 + c2 + c3 + c4) * 0.125 + (e1 + e2 + e3 + e4) * 0.0625;
    outColor = vec4(outRgb, 1.0);
}
