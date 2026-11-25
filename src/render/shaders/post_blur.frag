#version 450
// Separable 9-tap Gaussian blur for half-res and quarter-res bloom buffers.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D srcTex;

layout(std140, set = 3, binding = 0) uniform PostBlurUniforms {
    vec4 texelDir;  // xy: step in UV space (dir / textureSize), zw: unused
};

void main()
{
    vec2 stepUV = texelDir.xy;
    vec3 sum = texture(srcTex, inUV).rgb * 0.22702703;
    sum += texture(srcTex, inUV + stepUV * 1.38461538).rgb * 0.31621622;
    sum += texture(srcTex, inUV - stepUV * 1.38461538).rgb * 0.31621622;
    sum += texture(srcTex, inUV + stepUV * 3.23076923).rgb * 0.07027027;
    sum += texture(srcTex, inUV - stepUV * 3.23076923).rgb * 0.07027027;
    outColor = vec4(sum, 1.0);
}
