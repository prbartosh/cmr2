#version 450
// Separable 9-tap depth-aware bilateral blur for half-res GTAO.
// Preserves crisp depth discontinuities around car silhouettes, tyres, kerbs, and trees.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outAO;

layout(set = 2, binding = 0) uniform sampler2D aoDepthTex;

layout(std140, set = 3, binding = 0) uniform PostAoBlurUniforms {
    vec4 texelDir;  // xy: step in UV space (dir / halfSize), zw: unused
};

const float kWeights[5] = float[5](0.227027, 0.194595, 0.121622, 0.054054, 0.016216);

void main()
{
    vec2 center = texture(aoDepthTex, inUV).rg;
    float centerAO = center.r;
    float centerZ = center.g;

    if (centerZ <= 0.05 || centerZ > 220.0) {
        outAO = vec4(1.0, centerZ, 0.0, 1.0);
        return;
    }

    float invDepthSigma = 1.0 / max(centerZ * 0.055, 0.06);
    float sumAO = centerAO * kWeights[0];
    float sumW = kWeights[0];

    for (int i = 1; i <= 4; i++) {
        vec2 off = texelDir.xy * float(i);
        float spatialW = kWeights[i];

        vec2 sPos = texture(aoDepthTex, clamp(inUV + off, vec2(0.0), vec2(1.0))).rg;
        float wPos = spatialW * exp(-abs(sPos.g - centerZ) * invDepthSigma);
        sumAO += sPos.r * wPos;
        sumW += wPos;

        vec2 sNeg = texture(aoDepthTex, clamp(inUV - off, vec2(0.0), vec2(1.0))).rg;
        float wNeg = spatialW * exp(-abs(sNeg.g - centerZ) * invDepthSigma);
        sumAO += sNeg.r * wNeg;
        sumW += wNeg;
    }

    outAO = vec4(clamp(sumAO / max(sumW, 1e-5), 0.0, 1.0), centerZ, 0.0, 1.0);
}
