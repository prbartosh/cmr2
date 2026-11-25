#version 450

layout(location = 0) in vec4 inDiffuse;
layout(location = 1) in vec4 inSpecular;
layout(location = 2) in vec4 inTex0;
layout(location = 3) in vec4 inTex1;
layout(location = 4) in float inFog;
layout(location = 5) in float inEyeDepth;
layout(location = 6) in vec3 inEyePosition;
layout(location = 7) in vec3 inEyeNormal;
layout(location = 8) in vec4 inRawColor1;
layout(location = 9) in vec4 inRawColor2;

layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform samplerCube cube0;
layout(set = 2, binding = 2) uniform sampler2D tex1;
layout(set = 2, binding = 3) uniform samplerCube cube1;
layout(set = 2, binding = 4) uniform sampler2D backdrop;   // Gfx_CaptureBackdrop
layout(set = 2, binding = 5) uniform sampler2D native0;
layout(set = 2, binding = 6) uniform sampler2D native1;

layout(std140, set = 3, binding = 0) uniform FragmentUniforms {
    ivec4 stage0;       // colour op, colour arg1, colour arg2, alpha op
    ivec4 stage0b;      // alpha arg1, alpha arg2, texture kind (0 none, 1 2D, 2 cube, 3 bump, 4 distance field, 5 menu dot, 6 vector icon), scaled point filter
    ivec4 stage1;
    ivec4 stage1b;
    vec4 textureFactor;
    ivec4 alphaTest;    // enabled, compare function, -, -
    vec4 alphaRef;      // reference / 255
    vec4 fogColor;
    ivec4 fogMode;      // fog enabled, table mode (3 linear), specular enabled, -
    vec4 fogParams;     // start, end
    vec4 bumpMatrix;    // m00, m01, m10, m11 (of the bump stage)
    vec4 bumpLuminance; // scale, offset
    vec4 distanceFade;  // distance where the fade into the backdrop starts, ends; on (z != 0)
};

struct FragLight {
    vec4 diffuse;
    vec4 specular;
    vec4 ambient;
    vec4 position;      // eye space; w: type (1 point, 2 spot, 3 directional)
    vec4 direction;     // eye space; w: range
    vec4 attenuation;   // a0, a1, a2, falloff
    vec4 spot;          // cos(theta / 2), cos(phi / 2), enabled, -
};

layout(std140, set = 3, binding = 1) uniform FragmentLightingUniforms {
    mat4 textureMatrix[2];
    vec4 materialDiffuse;
    vec4 materialAmbient;
    vec4 materialSpecular;
    vec4 materialEmissive;
    vec4 globalAmbient;     // w: material power
    FragLight lights[8];
    ivec4 lightFlags;       // lighting, colour vertex, specular, local viewer
    ivec4 sources;          // diffuse, specular, ambient, emissive
    ivec4 texCoordIndex;    // stage 0, stage 1, texture transform 0, texture transform 1
    vec4 viewUp;            // world up in eye space
    vec4 enhanceParams;     // x: perPixelLighting, y: hemisphereAmbient, z: carFresnelStrength, w: visualOverhaul
};

const int TOP_DISABLE = 1, TOP_SELECTARG1 = 2, TOP_SELECTARG2 = 3, TOP_MODULATE = 4, TOP_MODULATE2X = 5,
          TOP_MODULATE4X = 6, TOP_ADD = 7, TOP_ADDSIGNED = 8, TOP_ADDSIGNED2X = 9, TOP_SUBTRACT = 10,
          TOP_BUMPENVMAP = 22, TOP_BUMPENVMAPLUMINANCE = 23;

vec4 MaterialColor(int source, vec4 material, vec4 color1, vec4 color2)
{
    if (lightFlags.y != 0) {
        if (source == 1)
            return color1;
        if (source == 2)
            return color2;
    }
    return material;
}

vec4 Arg(int arg, vec4 current, vec4 texel, vec4 litDiffuse, vec4 litSpecular)
{
    vec4 v;
    switch (arg & 0xf) {
    case 0: v = litDiffuse; break;
    case 1: v = current; break;
    case 2: v = texel; break;
    case 3: v = textureFactor; break;
    case 4: v = litSpecular; break;
    default: v = current; break;
    }
    if ((arg & 0x10) != 0)
        v = vec4(1.0) - v;
    if ((arg & 0x20) != 0)
        v = v.aaaa;
    return v;
}

vec4 Op(int op, vec4 a, vec4 b)
{
    switch (op) {
    case TOP_SELECTARG1: return a;
    case TOP_SELECTARG2: return b;
    case TOP_MODULATE: return a * b;
    case TOP_MODULATE2X: return clamp(a * b * 2.0, 0.0, 1.0);
    case TOP_MODULATE4X: return clamp(a * b * 4.0, 0.0, 1.0);
    case TOP_ADD: return clamp(a + b, 0.0, 1.0);
    case TOP_ADDSIGNED: return clamp(a + b - 0.5, 0.0, 1.0);
    case TOP_ADDSIGNED2X: return clamp((a + b - 0.5) * 2.0, 0.0, 1.0);
    case TOP_SUBTRACT: return clamp(a - b, 0.0, 1.0);
    default: return a;
    }
}

// A point-filtered texture through a linear sampler: filtered smoothly where
// it is scaled, the nearest texel, as the point filter reads it, where it
// keeps its size.
vec4 SampleScaledPoint(sampler2D t2, vec2 uv)
{
    vec2 size = vec2(textureSize(t2, 0));
    vec2 t = uv * size;
    vec2 d = fwidth(t);
    t = mix(floor(t) + 0.5, t, bvec2(abs(d.x - 1.0) > 0.01, abs(d.y - 1.0) > 0.01));
    return textureLod(t2, t / size, 0.0);
}

// A distance field in alpha (0.5 on the edge): its coverage of the pixel,
// with an edge one pixel wide at any scale.
vec4 SampleDistanceField(sampler2D t2, vec2 uv)
{
    vec4 t = texture(t2, uv);
    float w = max(fwidth(t.a), 1e-5);
    return vec4(t.rgb, clamp((t.a - 0.5) / w + 0.5, 0.0, 1.0));
}

// The large matrix sprite is a white circle in a padded cell: 11 pixels
// across in the 16x16 set, 18 in the 32x32 set. Keep its original coverage
// at native size and below; enlarged, evaluate its edge at screen resolution.
// The surrounding game code still controls every dot's position and colour.
vec4 SampleMenuDot(sampler2D t2, vec2 uv)
{
    vec2 size = vec2(textureSize(t2, 0));
    vec2 p = uv * size;
    if (all(greaterThanEqual(fwidth(p), vec2(0.999))))
        return SampleScaledPoint(t2, uv);
    float radius = size.x < 24.0 ? 5.5 : 9.0;
    float distance = length(p - vec2(radius)) - radius;
    float coverage = clamp(0.5 - distance / max(fwidth(distance), 1e-5), 0.0, 1.0);
    return vec4(1.0, 1.0, 1.0, coverage);
}

float IconCoverage(float distance)
{
    return clamp((distance - 0.5) / max(fwidth(distance), 1e-5) + 0.5, 0.0, 1.0);
}

vec4 SampleVectorIcon(sampler2D field, sampler2D original, vec2 uv, int scaledPoint)
{
    // The original image remains bound, including its compressed format.
    // Native-size and smaller draws therefore preserve the original pixels.
    vec2 pixels = uv * vec2(textureSize(original, 0));
    if (all(greaterThanEqual(fwidth(pixels), vec2(0.999))))
        return scaledPoint != 0 ? SampleScaledPoint(original, uv) : texture(original, uv);
    // Keep the field's filter footprint inside its band. Padded matrix
    // points touch the texture edges; sampling a palette band there would
    // turn transparent padding into thin lines across adjacent map cells.
    vec2 halfTexel = 0.5 / vec2(textureSize(field, 0));
    vec2 fieldUV = clamp(uv * vec2(1.0, 1.0 / 3.0), halfTexel,
                         vec2(1.0, 1.0 / 3.0) - halfTexel);
    vec4 distances = textureLod(field, fieldUV, 0.0);
    vec3 colour = textureLod(field, fieldUV + vec2(0.0, 1.0 / 3.0), 0.0).rgb;
    vec3 detail = textureLod(field, fieldUV + vec2(0.0, 2.0 / 3.0), 0.0).rgb;
    float ink = distances.r > 0.5 ? IconCoverage(distances.g) : 1.0;
    return vec4(mix(detail, colour, ink), IconCoverage(distances.b));
}

vec4 Sample(int kind, int scaledPoint, sampler2D t2, samplerCube tc, sampler2D original, vec4 coord)
{
    if (kind == 2)
        return texture(tc, coord.xyz);
    if (kind == 0)
        return vec4(1.0);
    if (kind == 4)
        return SampleDistanceField(t2, coord.xy);
    if (kind == 5)
        return SampleMenuDot(t2, coord.xy);
    if (kind == 6)
        return SampleVectorIcon(t2, original, coord.xy, scaledPoint);
    if (scaledPoint != 0)
        return SampleScaledPoint(t2, coord.xy);
    return texture(t2, coord.xy);
}

float SignedByte(float v)
{
    float b = floor(v * 255.0 + 0.5);
    return (b >= 128.0 ? b - 256.0 : b) / 128.0;
}

bool Compare(int func, float a, float b)
{
    switch (func) {
    case 1: return false;
    case 2: return a < b;
    case 3: return a == b;
    case 4: return a <= b;
    case 5: return a > b;
    case 6: return a != b;
    case 7: return a >= b;
    default: return true;
    }
}

vec4 ComputeStageCoord(int stage, vec4 fallbackCoord, vec3 eyePos, vec3 eyeN, bool hasNormal)
{
    if (enhanceParams.w > 0.5 && hasNormal && (texCoordIndex[stage] & 0xffff0000) == 0x30000) {
        vec3 e = normalize(-eyePos);
        vec3 refl = 2.0 * dot(e, eyeN) * eyeN - e;
        if (texCoordIndex[stage + 2] != 0)
            refl = (textureMatrix[stage] * vec4(refl, 1.0)).xyz;
        return vec4(refl, 1.0);
    }
    return fallbackCoord;
}

void main()
{
    bool hasNormal = dot(inEyeNormal, inEyeNormal) > 1e-6;
    vec3 N = hasNormal ? normalize(inEyeNormal) : vec3(0.0, 1.0, 0.0);
    vec3 V = length(inEyePosition) > 1e-5 ? normalize(-inEyePosition) : vec3(0.0, 0.0, -1.0);
    bool isCubePass = (stage0.x != TOP_DISABLE && stage0b.z == 2) ||
                      (stage0.x != TOP_DISABLE && stage1.x != TOP_DISABLE && stage1b.z == 2);

    vec4 litDiffuse = inDiffuse;
    vec4 litSpecular = inSpecular;
    vec3 clearcoatSpec = vec3(0.0);

    if (enhanceParams.w > 0.5 && enhanceParams.x > 0.5 && lightFlags.x != 0 && hasNormal) {
        vec4 matDiffuse = MaterialColor(sources.x, materialDiffuse, inRawColor1, inRawColor2);
        vec4 matSpecular = MaterialColor(sources.y, materialSpecular, inRawColor1, inRawColor2);
        vec4 matAmbient = MaterialColor(sources.z, materialAmbient, inRawColor1, inRawColor2);
        vec4 matEmissive = MaterialColor(sources.w, materialEmissive, inRawColor1, inRawColor2);
        vec3 ambient = globalAmbient.rgb;
        if (enhanceParams.y > 0.5 && dot(viewUp.xyz, viewUp.xyz) > 1e-6) {
            float hemi = clamp(dot(N, normalize(viewUp.xyz)) * 0.5 + 0.5, 0.0, 1.0);
            ambient *= mix(vec3(0.86, 0.85, 0.82), vec3(1.10, 1.12, 1.16), hemi);
        }
        vec3 diffuseSum = vec3(0.0);
        vec3 specularSum = vec3(0.0);
        vec3 toEye = lightFlags.w != 0 ? V : vec3(0.0, 0.0, -1.0);
        float ndotv = clamp(dot(N, V), 0.0, 1.0);
        float coatFresnel = mix(0.15, 1.0, pow(1.0 - ndotv, 4.0));

        for (int i = 0; i < 8; i++) {
            FragLight l = lights[i];
            if (l.spot.z == 0.0)
                continue;
            int type = int(l.position.w);
            vec3 L;
            float atten = 1.0;
            float spotFactor = 1.0;
            if (type == 3) {
                L = normalize(-l.direction.xyz);
            } else {
                vec3 toLight = l.position.xyz - inEyePosition;
                float d = length(toLight);
                if (d > l.direction.w)
                    continue;
                L = d > 0.0 ? toLight / d : vec3(0.0, 0.0, 1.0);
                float denom = l.attenuation.x + l.attenuation.y * d + l.attenuation.z * d * d;
                atten = denom > 0.0 ? 1.0 / denom : 1.0;
                if (type == 2) {
                    float rho = dot(-L, normalize(l.direction.xyz));
                    if (rho <= l.spot.y)
                        spotFactor = 0.0;
                    else if (rho < l.spot.x)
                        spotFactor = pow((rho - l.spot.y) / max(l.spot.x - l.spot.y, 1e-6), l.attenuation.w);
                }
            }
            float scale = atten * spotFactor;
            ambient += l.ambient.rgb * scale;
            float nDotL = max(dot(N, L), 0.0);
            diffuseSum += l.diffuse.rgb * nDotL * scale;
            if (nDotL > 0.0) {
                vec3 h = normalize(toEye + L);
                if (lightFlags.z != 0)
                    specularSum += l.specular.rgb * pow(max(dot(N, h), 0.0), max(globalAmbient.w, 1.0)) * scale;
                if (isCubePass && enhanceParams.z > 0.0 && type == 3)
                    clearcoatSpec += l.diffuse.rgb * pow(max(dot(N, h), 0.0), 48.0) * (0.28 * enhanceParams.z * coatFresnel) * scale;
            }
        }
        litDiffuse = vec4(clamp(matEmissive.rgb + ambient * matAmbient.rgb + diffuseSum * matDiffuse.rgb, 0.0, 1.0),
                          matDiffuse.a);
        litSpecular = vec4(clamp(specularSum * matSpecular.rgb, 0.0, 1.0), inRawColor2.a);
    }

    vec4 current = litDiffuse;
    vec2 bumpOffset = vec2(0.0);
    float luminance = 1.0;

    float cubeFresnel = 1.0;
    if (enhanceParams.w > 0.5 && enhanceParams.z > 0.0 && hasNormal) {
        float ndotv = clamp(dot(N, V), 0.0, 1.0);
        float f = mix(0.28, 1.35, pow(1.0 - ndotv, 3.2));
        cubeFresnel = mix(1.0, f, enhanceParams.z);
    }

    // Stage 0.
    if (stage0.x != TOP_DISABLE) {
        if (stage0.x == TOP_BUMPENVMAP || stage0.x == TOP_BUMPENVMAPLUMINANCE) {
            vec4 bump = texture(tex0, inTex0.xy);
            float du = SignedByte(bump.r), dv = SignedByte(bump.g);
            bumpOffset = vec2(du * bumpMatrix.x + dv * bumpMatrix.z, du * bumpMatrix.y + dv * bumpMatrix.w);
            if (stage0.x == TOP_BUMPENVMAPLUMINANCE)
                luminance = clamp(bump.b * bumpLuminance.x + bumpLuminance.y, 0.0, 1.0);
        } else {
            vec4 coord0 = ComputeStageCoord(0, inTex0, inEyePosition, N, hasNormal);
            vec4 texel = Sample(stage0b.z, stage0b.w, tex0, cube0, native0, coord0);
            if (stage0b.z == 2)
                texel.rgb = clamp(texel.rgb * cubeFresnel, 0.0, 1.0);
            vec4 color = Op(stage0.x, Arg(stage0.y, current, texel, litDiffuse, litSpecular),
                            Arg(stage0.z, current, texel, litDiffuse, litSpecular));
            float alpha = stage0.w == TOP_DISABLE
                              ? current.a
                              : Op(stage0.w, Arg(stage0b.x, current, texel, litDiffuse, litSpecular),
                                   Arg(stage0b.y, current, texel, litDiffuse, litSpecular))
                                    .a;
            current = vec4(color.rgb, alpha);
        }
        // Stage 1.
        if (stage1.x != TOP_DISABLE) {
            vec4 baseCoord1 = ComputeStageCoord(1, inTex1, inEyePosition, N, hasNormal);
            vec4 coord = baseCoord1 + vec4(bumpOffset, 0.0, 0.0);
            vec4 texel = Sample(stage1b.z, stage1b.w, tex1, cube1, native1, coord);
            texel.rgb *= luminance;
            if (stage1b.z == 2)
                texel.rgb = clamp(texel.rgb * cubeFresnel, 0.0, 1.0);
            vec4 color = Op(stage1.x, Arg(stage1.y, current, texel, litDiffuse, litSpecular),
                            Arg(stage1.z, current, texel, litDiffuse, litSpecular));
            float alpha = stage1.w == TOP_DISABLE
                              ? current.a
                              : Op(stage1.w, Arg(stage1b.x, current, texel, litDiffuse, litSpecular),
                                   Arg(stage1b.y, current, texel, litDiffuse, litSpecular))
                                    .a;
            current = vec4(color.rgb, alpha);
        }
    }

    if (fogMode.z != 0)
        current.rgb = fogMode.w != 0 ? clamp(current.rgb + litSpecular.rgb, 0.0, 4.0)
                                     : clamp(current.rgb + litSpecular.rgb, 0.0, 1.0);
    if (clearcoatSpec.r + clearcoatSpec.g + clearcoatSpec.b > 0.0)
        current.rgb = fogMode.w != 0 ? clamp(current.rgb + clearcoatSpec, 0.0, 4.0)
                                     : clamp(current.rgb + clearcoatSpec, 0.0, 1.0);

    if (alphaTest.x != 0 && !Compare(alphaTest.y, floor(current.a * 255.0 + 0.5), floor(alphaRef.x * 255.0 + 0.5)))
        discard;

    if (fogMode.x != 0) {
        float f = inFog;
        if (fogMode.y == 3) {
            float range = fogParams.y - fogParams.x;
            f = range != 0.0 ? clamp((fogParams.y - inEyeDepth) / range, 0.0, 1.0) : 1.0;
        }
        current.rgb = mix(fogColor.rgb, current.rgb, f);
    }

    // Toward the far plane the scenery dissolves into the sky and backdrop
    // behind it, the back buffer's pixel as it was before the scenery. By the
    // distance from the eye, not the depth along the view, so that a point
    // fades the same however the view turns.
    if (distanceFade.z != 0.0) {
        float f = smoothstep(distanceFade.x, distanceFade.y, length(inEyePosition));
        if (f > 0.0)
            current.rgb = mix(current.rgb, texelFetch(backdrop, ivec2(gl_FragCoord.xy), 0).rgb, f);
    }
    outColor = current;
}
