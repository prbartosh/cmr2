#version 450
// Half-resolution Ground-Truth Ambient Occlusion (GTAO: Jimenez et al. 2016).
// 2 directions x 4 steps rotated by Interleaved Gradient Noise for crisp contact
// shadows under tyres, rally car body, kerbs, trees, and barriers at minimal GPU cost.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outAO; // r = visibility [0..1], g = linear eye depth (metres)

layout(set = 2, binding = 0) uniform sampler2D sceneDepthTex;

layout(std140, set = 3, binding = 0) uniform PostGtaoUniforms {
    vec4 projParams;    // x: invProjX, y: invProjY, z: projScaleY (half-res px), w: worldRadius (m)
    vec4 texelParams;   // xy: 1 / halfSize, zw: 1 / fullSize
};

const float PI = 3.14159265;
const float HALF_PI = 1.57079633;

float InterleavedGradientNoise(vec2 p)
{
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(p, magic.xy)));
}

float SampleDepth(vec2 uv)
{
    return texture(sceneDepthTex, clamp(uv, vec2(0.0), vec2(1.0))).r;
}

vec3 ReconstructViewPos(vec2 uv, float z)
{
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return vec3(ndc.x * z * projParams.x, ndc.y * z * projParams.y, z);
}

// Analytical GTAO hemisphere arc visibility integral for one slice.
float IntegrateArc(float h1, float h2, float n)
{
    float cosN = cos(n);
    float sinN = sin(n);
    float a1 = -cos(2.0 * h1 - n) + cosN + 2.0 * h1 * sinN;
    float a2 = -cos(2.0 * h2 - n) + cosN + 2.0 * h2 * sinN;
    return 0.25 * (a1 + a2);
}

void main()
{
    // Sample closest solid depth in the 2x2 full-res quad to preserve thin silhouettes.
    vec2 fh = texelParams.zw * 0.5;
    float z0 = SampleDepth(inUV + vec2(-fh.x, -fh.y));
    float z1 = SampleDepth(inUV + vec2( fh.x, -fh.y));
    float z2 = SampleDepth(inUV + vec2(-fh.x,  fh.y));
    float z3 = SampleDepth(inUV + vec2( fh.x,  fh.y));
    float centerZ = min(min(z0, z1), min(z2, z3));

    // Skip sky/backdrop and very distant scenery cleanly.
    if (centerZ <= 0.05 || centerZ > 220.0) {
        outAO = vec4(1.0, centerZ, 0.0, 1.0);
        return;
    }

    vec3 P = ReconstructViewPos(inUV, centerZ);
    vec3 V = normalize(-P);

    // Reconstruct sharp view-space normal using minimum-discontinuity finite differences.
    vec2 stepUV = texelParams.xy;
    float zL = SampleDepth(inUV - vec2(stepUV.x, 0.0));
    float zR = SampleDepth(inUV + vec2(stepUV.x, 0.0));
    float zU = SampleDepth(inUV - vec2(0.0, stepUV.y));
    float zD = SampleDepth(inUV + vec2(0.0, stepUV.y));

    vec3 P_l = ReconstructViewPos(inUV - vec2(stepUV.x, 0.0), zL);
    vec3 P_r = ReconstructViewPos(inUV + vec2(stepUV.x, 0.0), zR);
    vec3 P_u = ReconstructViewPos(inUV - vec2(0.0, stepUV.y), zU);
    vec3 P_d = ReconstructViewPos(inUV + vec2(0.0, stepUV.y), zD);

    vec3 dPdx = (abs(zR - centerZ) < abs(centerZ - zL)) ? (P_r - P) : (P - P_l);
    vec3 dPdy = (abs(zD - centerZ) < abs(centerZ - zU)) ? (P_d - P) : (P - P_u);
    vec3 N = cross(dPdx, dPdy);
    float nLen2 = dot(N, N);
    N = nLen2 > 1e-10 ? N * inversesqrt(nLen2) : V;
    if (dot(N, V) < 0.0)
        N = -N;

    float worldRadius = projParams.w;
    float screenRadius = clamp((worldRadius * projParams.z) / max(centerZ, 0.3), 2.5, 44.0);
    float invRadiusSq = 1.0 / max(worldRadius * worldRadius, 1e-4);

    // Per-pixel spatial rotation & step jitter via Interleaved Gradient Noise.
    float noisePhi = InterleavedGradientNoise(gl_FragCoord.xy);
    float noiseStep = InterleavedGradientNoise(gl_FragCoord.xy + vec2(5.588238, 1.314159));

    float visibility = 0.0;
    const int NUM_DIRS = 2;
    const int NUM_STEPS = 4;

    for (int d = 0; d < NUM_DIRS; d++) {
        float phi = (noisePhi + float(d) / float(NUM_DIRS)) * PI;
        vec2 omega = vec2(cos(phi), sin(phi));
        // Screen-space UV y points downward while eye-space Y points upward.
        vec3 dirVec = vec3(omega.x, -omega.y, 0.0);
        vec3 orthoDir = dirVec - dot(dirVec, V) * V;
        vec3 axisVec = normalize(cross(orthoDir, V));
        vec3 projN = N - axisVec * dot(N, axisVec);
        float projNLen = length(projN);
        if (projNLen < 1e-4) {
            visibility += 1.0;
            continue;
        }
        vec3 normProjN = projN / projNLen;
        vec3 tangent = cross(axisVec, V);
        float cosN = clamp(dot(normProjN, V), -1.0, 1.0);
        float signN = dot(normProjN, tangent) >= 0.0 ? 1.0 : -1.0;
        float nAngle = signN * acos(cosN);

        float maxCos1 = -1.0;
        float maxCos2 = -1.0;

        for (int s = 1; s <= NUM_STEPS; s++) {
            // Quadratic step spacing concentrates taps close to tyre/ground contacts.
            float frac = (float(s) - 0.5 + (noiseStep - 0.5) * 0.65) / float(NUM_STEPS);
            float sampleDistPx = max(1.25, frac * frac * screenRadius);
            vec2 offUV = omega * sampleDistPx * texelParams.xy;

            // +omega direction
            vec2 uv1 = inUV + offUV;
            if (uv1.x >= 0.0 && uv1.x <= 1.0 && uv1.y >= 0.0 && uv1.y <= 1.0) {
                float zs1 = SampleDepth(uv1);
                if (zs1 > 0.05 && zs1 < 500.0) {
                    vec3 hv1 = ReconstructViewPos(uv1, zs1) - P;
                    float distSq1 = dot(hv1, hv1);
                    if (distSq1 > 1e-5) {
                        float falloff1 = clamp(1.0 - distSq1 * invRadiusSq, 0.0, 1.0);
                        float hc1 = dot(hv1, V) * inversesqrt(distSq1);
                        // Thin-surface thickness attenuation prevents foreground dark halos.
                        float thick1 = clamp(1.0 - max(distSq1 * invRadiusSq - 0.65, 0.0) * 2.5, 0.0, 1.0);
                        maxCos1 = max(maxCos1, mix(-1.0, hc1, falloff1 * thick1));
                    }
                }
            }

            // -omega direction
            vec2 uv2 = inUV - offUV;
            if (uv2.x >= 0.0 && uv2.x <= 1.0 && uv2.y >= 0.0 && uv2.y <= 1.0) {
                float zs2 = SampleDepth(uv2);
                if (zs2 > 0.05 && zs2 < 500.0) {
                    vec3 hv2 = ReconstructViewPos(uv2, zs2) - P;
                    float distSq2 = dot(hv2, hv2);
                    if (distSq2 > 1e-5) {
                        float falloff2 = clamp(1.0 - distSq2 * invRadiusSq, 0.0, 1.0);
                        float hc2 = dot(hv2, V) * inversesqrt(distSq2);
                        float thick2 = clamp(1.0 - max(distSq2 * invRadiusSq - 0.65, 0.0) * 2.5, 0.0, 1.0);
                        maxCos2 = max(maxCos2, mix(-1.0, hc2, falloff2 * thick2));
                    }
                }
            }
        }

        float h1 = -acos(clamp(maxCos1, -1.0, 1.0));
        float h2 =  acos(clamp(maxCos2, -1.0, 1.0));
        h1 = nAngle + max(h1 - nAngle, -HALF_PI);
        h2 = nAngle + min(h2 - nAngle,  HALF_PI);

        visibility += projNLen * IntegrateArc(h1, h2, nAngle);
    }

    float ao = clamp(visibility / float(NUM_DIRS), 0.0, 1.0);
    // Smoothly fade SSAO toward the horizon so distant fogged hills remain clean.
    float distFade = 1.0 - smoothstep(75.0, 175.0, centerZ);
    ao = mix(1.0, ao, distFade);

    outAO = vec4(ao, centerZ, 0.0, 1.0);
}
