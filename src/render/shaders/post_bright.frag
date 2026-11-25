#version 450
// Bright-pass extraction in linear space with a soft knee for subtle rally bloom.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D hdrTex;

layout(std140, set = 3, binding = 0) uniform PostBrightUniforms {
    vec4 params;    // x: threshold, y: softKnee, z: maxBrightness, w: unused
};

vec3 ToLinear(vec3 c)
{
    return pow(max(c, vec3(0.0)), vec3(2.2));
}

void main()
{
    vec2 texel = 1.0 / vec2(textureSize(hdrTex, 0));
    // 4-tap box downsample reduces single-pixel specular fireflies.
    vec3 s0 = ToLinear(texture(hdrTex, inUV + texel * vec2(-0.5, -0.5)).rgb);
    vec3 s1 = ToLinear(texture(hdrTex, inUV + texel * vec2( 0.5, -0.5)).rgb);
    vec3 s2 = ToLinear(texture(hdrTex, inUV + texel * vec2(-0.5,  0.5)).rgb);
    vec3 s3 = ToLinear(texture(hdrTex, inUV + texel * vec2( 0.5,  0.5)).rgb);
    vec3 c = min((s0 + s1 + s2 + s3) * 0.25, vec3(params.z));

    float br = max(c.r, max(c.g, c.b));
    float threshold = params.x;
    float knee = max(params.y, 1e-4);
    float rq = clamp(br - threshold + knee, 0.0, 2.0 * knee);
    rq = (rq * rq) / (4.0 * knee);
    float weight = max(rq, br - threshold) / max(br, 1e-4);
    outColor = vec4(c * max(weight, 0.0), 1.0);
}
