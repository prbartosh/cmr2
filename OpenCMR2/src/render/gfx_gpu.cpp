// The SDL_GPU executor: runs a recorded frame (gfx_core.cpp) with one set of
// shaders that emulate Direct3D 7's fixed-function pipeline.

#include "platform/platform.h"
#include "port/sys.h"
#include "render/presentation.h"
#include "render/gfx_internal.h"

#include "shaders_spirv.h"

// SPIR-V and, when built, MSL code of one shader for Executor::Shader.
#ifdef OPENCMR2_SHADERS_MSL
#define SHADER_CODE(name) g_##name, sizeof(g_##name), g_##name##_msl, sizeof(g_##name##_msl)
#else
#define SHADER_CODE(name) g_##name, sizeof(g_##name), nullptr, 0
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <unordered_map>

namespace {

// ---- uniform blocks (std140, see shaders/common.glsl and fixed.frag) ----

struct LightUniform {
    float diffuse[4], specular[4], ambient[4], position[4], direction[4], attenuation[4], spot[4];
};

struct VertexUniforms {
    float worldView[16];
    float projection[16];
    float textureMatrix[2][16];
    float materialDiffuse[4], materialAmbient[4], materialSpecular[4], materialEmissive[4];
    float globalAmbient[4];
    LightUniform lights[8];
    int32_t lightFlags[4];
    int32_t sources[4];
    int32_t fogFlags[4];
    float fogParams[4];
    int32_t texCoordIndex[4];
    float target[4];
};

struct FragmentUniforms {
    int32_t stage0[4], stage0b[4], stage1[4], stage1b[4];
    float textureFactor[4];
    int32_t alphaTest[4];
    float alphaRef[4];
    float fogColor[4];
    int32_t fogMode[4];
    float fogParams[4];
    float bumpMatrix[4];
    float bumpLuminance[4];
    float distanceFade[4];
};

struct FragmentLightingUniforms {
    float textureMatrix[2][16];
    float materialDiffuse[4], materialAmbient[4], materialSpecular[4], materialEmissive[4];
    float globalAmbient[4];
    LightUniform lights[8];
    int32_t lightFlags[4];
    int32_t sources[4];
    int32_t texCoordIndex[4];
    float viewUp[4];
    float enhanceParams[4];
    float eyeToLightClip[2][16];
    float shadowParams[4];
    float shadowParams2[4];
    float sunDirEye[4];
};

struct ClearUniforms {
    float color[4];
    float depth[4];
};

struct PostUniforms {
    float params[4];
};

struct PostUniforms2 {
    float a[4];
    float b[4];
};

struct ShadowVertexUniforms {
    float lightWorldViewProj[16];
};

struct ShadowFragmentUniforms {
    float alphaParams[4];
};

void Multiply(const GfxMatrix &a, const GfxMatrix &b, float *out)
{
    const float *pa = &a._11, *pb = &b._11;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += pa[r * 4 + k] * pb[k * 4 + c];
            out[r * 4 + c] = s;
        }
}

void TransformPoint(const GfxVector &v, float w, const GfxMatrix &m, float out[3])
{
    const float *p = &m._11;
    for (int c = 0; c < 3; c++)
        out[c] = v.x * p[c] + v.y * p[4 + c] + v.z * p[8 + c] + w * p[12 + c];
}

GfxVector VecNormalize(GfxVector v)
{
    float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len > 1e-6f)
        return { v.x / len, v.y / len, v.z / len };
    return { 0.0f, 1.0f, 0.0f };
}

GfxVector VecCross(GfxVector a, GfxVector b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

float VecDot(GfxVector a, GfxVector b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

bool InvertAffineMatrix(const GfxMatrix &m, GfxMatrix &out)
{
    const float a00 = m._11, a01 = m._12, a02 = m._13;
    const float a10 = m._21, a11 = m._22, a12 = m._23;
    const float a20 = m._31, a21 = m._32, a22 = m._33;
    const float c00 = a11 * a22 - a12 * a21;
    const float c01 = -(a10 * a22 - a12 * a20);
    const float c02 = a10 * a21 - a11 * a20;
    const float det = a00 * c00 + a01 * c01 + a02 * c02;
    if (fabsf(det) < 1e-7f)
        return false;
    const float invDet = 1.0f / det;
    const float c10 = -(a01 * a22 - a02 * a21);
    const float c11 = a00 * a22 - a02 * a20;
    const float c12 = -(a00 * a21 - a01 * a20);
    const float c20 = a01 * a12 - a02 * a11;
    const float c21 = -(a00 * a12 - a02 * a10);
    const float c22 = a00 * a11 - a01 * a10;

    memset(&out, 0, sizeof(out));
    out._11 = c00 * invDet; out._12 = c10 * invDet; out._13 = c20 * invDet;
    out._21 = c01 * invDet; out._22 = c11 * invDet; out._23 = c21 * invDet;
    out._31 = c02 * invDet; out._32 = c12 * invDet; out._33 = c22 * invDet;
    const float tx = m._41, ty = m._42, tz = m._43;
    out._41 = -(tx * out._11 + ty * out._21 + tz * out._31);
    out._42 = -(tx * out._12 + ty * out._22 + tz * out._32);
    out._43 = -(tx * out._13 + ty * out._23 + tz * out._33);
    out._44 = 1.0f;
    return true;
}

struct ShadowCascade {
    GfxMatrix lightVP;
    GfxVector center;
    float radius;
    float zRange;
};

struct ShadowSetup {
    int count = 0;
    ShadowCascade cascades[2] = {};
    GfxVector toSunWorld = { 0.0f, 1.0f, 0.0f };
    GfxVector lightRight = { 1.0f, 0.0f, 0.0f };
    GfxVector lightUp = { 0.0f, 1.0f, 0.0f };
    GfxVector lightFwd = { 0.0f, 0.0f, 1.0f };
};

void BuildSingleCascade(const GfxVector &focus, float radius, float zRange, int mapSize,
                        const GfxVector &lightRight, const GfxVector &lightUp, const GfxVector &lightFwd,
                        ShadowCascade &out)
{
    float texelSize = (2.0f * radius) / (float)std::max(mapSize, 256);
    float fx = floorf(VecDot(focus, lightRight) / texelSize) * texelSize;
    float fy = floorf(VecDot(focus, lightUp) / texelSize) * texelSize;
    float fz = VecDot(focus, lightFwd);
    GfxVector center = {
        lightRight.x * fx + lightUp.x * fy + lightFwd.x * fz,
        lightRight.y * fx + lightUp.y * fy + lightFwd.y * fz,
        lightRight.z * fx + lightUp.z * fy + lightFwd.z * fz,
    };

    GfxMatrix lightView = {};
    lightView._11 = lightRight.x; lightView._12 = lightUp.x; lightView._13 = lightFwd.x;
    lightView._21 = lightRight.y; lightView._22 = lightUp.y; lightView._23 = lightFwd.y;
    lightView._31 = lightRight.z; lightView._32 = lightUp.z; lightView._33 = lightFwd.z;
    lightView._41 = -VecDot(center, lightRight);
    lightView._42 = -VecDot(center, lightUp);
    lightView._43 = -VecDot(center, lightFwd);
    lightView._44 = 1.0f;

    GfxMatrix lightOrtho = {};
    lightOrtho._11 = 1.0f / radius;
    lightOrtho._22 = 1.0f / radius;
    lightOrtho._33 = 1.0f / (2.0f * zRange);
    lightOrtho._43 = 0.5f;
    lightOrtho._44 = 1.0f;

    Multiply(lightView, lightOrtho, &out.lightVP._11);
    out.center = center;
    out.radius = radius;
    out.zRange = zRange;
}

bool BuildSunCascades(const GfxMatrix &camView, const GfxVector &sunLightDir, float shadowDist, int mapSize,
                      int cascadeCount, ShadowSetup &out)
{
    GfxMatrix invCam;
    if (!InvertAffineMatrix(camView, invCam))
        return false;
    GfxVector camPos = { invCam._41, invCam._42, invCam._43 };
    GfxVector camFwd = VecNormalize({ camView._13, camView._23, camView._33 });

    GfxVector toSun = VecNormalize({ -sunLightDir.x, -sunLightDir.y, -sunLightDir.z });
    if (toSun.y < 0.24f) {
        toSun.y = 0.24f;
        toSun = VecNormalize(toSun);
    }
    GfxVector lightFwd = { -toSun.x, -toSun.y, -toSun.z };
    GfxVector upRef = fabsf(lightFwd.y) > 0.98f ? GfxVector{ 0.0f, 0.0f, 1.0f } : GfxVector{ 0.0f, 1.0f, 0.0f };
    GfxVector lightRight = VecNormalize(VecCross(upRef, lightFwd));
    GfxVector lightUp = VecNormalize(VecCross(lightFwd, lightRight));

    out.toSunWorld = toSun;
    out.lightRight = lightRight;
    out.lightUp = lightUp;
    out.lightFwd = lightFwd;
    out.count = (cascadeCount >= 2) ? 2 : 1;

    if (out.count == 2) {
        // Cascade 0: 0..20m tight high-res frustum around the rally car (~1.75 cm/texel at 2048).
        const float radius0 = 18.0f;
        const float zRange0 = 160.0f;
        GfxVector focus0 = {
            camPos.x + camFwd.x * 9.0f,
            camPos.y + camFwd.y * 9.0f,
            camPos.z + camFwd.z * 9.0f,
        };
        BuildSingleCascade(focus0, radius0, zRange0, mapSize, lightRight, lightUp, lightFwd, out.cascades[0]);

        // Cascade 1: 20..110m extended frustum for roadside trees, buildings, and hills.
        const float farDist = std::clamp(shadowDist, 55.0f, 200.0f);
        const float radius1 = farDist * 0.60f;
        const float zRange1 = std::max(radius1 * 2.5f, 220.0f);
        GfxVector focus1 = {
            camPos.x + camFwd.x * (farDist * 0.40f),
            camPos.y + camFwd.y * (farDist * 0.40f),
            camPos.z + camFwd.z * (farDist * 0.40f),
        };
        BuildSingleCascade(focus1, radius1, zRange1, mapSize, lightRight, lightUp, lightFwd, out.cascades[1]);
    } else {
        const float radius = std::clamp(shadowDist, 25.0f, 200.0f);
        const float zRange = std::max(radius * 2.5f, 200.0f);
        GfxVector focus = {
            camPos.x + camFwd.x * (radius * 0.35f),
            camPos.y + camFwd.y * (radius * 0.35f),
            camPos.z + camFwd.z * (radius * 0.35f),
        };
        BuildSingleCascade(focus, radius, zRange, mapSize, lightRight, lightUp, lightFwd, out.cascades[0]);
        out.cascades[1] = out.cascades[0];
    }
    return true;
}

struct CachedSphere {
    uint32_t version = 0;
    GfxVector center = { 0.0f, 0.0f, 0.0f };
    float radius = 0.0f;
};

struct DrawBoundsKey {
    const GfxVertexBuffer *buffer;
    uint32_t firstVertexByte;
    uint32_t vertexCount;
    bool operator==(const DrawBoundsKey &o) const
    {
        return buffer == o.buffer && firstVertexByte == o.firstVertexByte && vertexCount == o.vertexCount;
    }
};

struct DrawBoundsKeyHash {
    size_t operator()(const DrawBoundsKey &k) const noexcept
    {
        size_t h = (size_t)(uintptr_t)k.buffer * 11400714819323198485ull;
        h ^= (size_t)k.firstVertexByte * 2654435761u;
        h ^= (size_t)k.vertexCount * 2246822519u;
        return h;
    }
};

CachedSphere ComputeVertexSphere(const GfxVertex *verts, uint32_t count, uint32_t stride, float pad)
{
    CachedSphere s = {};
    if (verts == NULL || count == 0)
        return s;
    float minX = verts[0].x, maxX = verts[0].x;
    float minY = verts[0].y, maxY = verts[0].y;
    float minZ = verts[0].z, maxZ = verts[0].z;
    for (uint32_t i = stride; i < count; i += stride) {
        const GfxVertex &v = verts[i];
        minX = std::min(minX, v.x); maxX = std::max(maxX, v.x);
        minY = std::min(minY, v.y); maxY = std::max(maxY, v.y);
        minZ = std::min(minZ, v.z); maxZ = std::max(maxZ, v.z);
    }
    const GfxVertex &last = verts[count - 1];
    minX = std::min(minX, last.x); maxX = std::max(maxX, last.x);
    minY = std::min(minY, last.y); maxY = std::max(maxY, last.y);
    minZ = std::min(minZ, last.z); maxZ = std::max(maxZ, last.z);
    s.center = { (minX + maxX) * 0.5f, (minY + maxY) * 0.5f, (minZ + maxZ) * 0.5f };
    float dx = maxX - s.center.x, dy = maxY - s.center.y, dz = maxZ - s.center.z;
    s.radius = sqrtf(dx * dx + dy * dy + dz * dz) + pad;
    return s;
}

void Color(DWORD c, float out[4])
{
    out[0] = ((c >> 16) & 0xff) / 255.0f;
    out[1] = ((c >> 8) & 0xff) / 255.0f;
    out[2] = (c & 0xff) / 255.0f;
    out[3] = ((c >> 24) & 0xff) / 255.0f;
}

void Copy4(const GfxColorValue &c, float out[4])
{
    out[0] = c.r;
    out[1] = c.g;
    out[2] = c.b;
    out[3] = c.a;
}

SDL_GPUBlendFactor BlendFactor(int d3d)
{
    switch (d3d) {
    case GFX_BLEND_ZERO: return SDL_GPU_BLENDFACTOR_ZERO;
    case GFX_BLEND_ONE: return SDL_GPU_BLENDFACTOR_ONE;
    case GFX_BLEND_SRCCOLOR: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case GFX_BLEND_INVSRCCOLOR: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case GFX_BLEND_SRCALPHA: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case GFX_BLEND_INVSRCALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case GFX_BLEND_DESTALPHA: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case GFX_BLEND_INVDESTALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case GFX_BLEND_DESTCOLOR: return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case GFX_BLEND_INVDESTCOLOR: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case GFX_BLEND_SRCALPHASAT: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    default: return SDL_GPU_BLENDFACTOR_ONE;
    }
}

SDL_GPUCompareOp CompareOp(int d3d)
{
    switch (d3d) {
    case GFX_CMP_NEVER: return SDL_GPU_COMPAREOP_NEVER;
    case GFX_CMP_LESS: return SDL_GPU_COMPAREOP_LESS;
    case GFX_CMP_EQUAL: return SDL_GPU_COMPAREOP_EQUAL;
    case GFX_CMP_LESSEQUAL: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case GFX_CMP_GREATER: return SDL_GPU_COMPAREOP_GREATER;
    case GFX_CMP_NOTEQUAL: return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case GFX_CMP_GREATEREQUAL: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    default: return SDL_GPU_COMPAREOP_ALWAYS;
    }
}

SDL_GPUPrimitiveType PrimitiveType(int d3d)
{
    switch (d3d) {
    case GFX_POINTLIST: return SDL_GPU_PRIMITIVETYPE_POINTLIST;
    case GFX_LINELIST: return SDL_GPU_PRIMITIVETYPE_LINELIST;
    case GFX_LINESTRIP: return SDL_GPU_PRIMITIVETYPE_LINESTRIP;
    case GFX_TRIANGLESTRIP: return SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    default: return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    }
}

struct GpuTexture {
    SDL_GPUTexture *texture = nullptr;
    SDL_GPUTexture *depth = nullptr;       // render targets
    // A cube render target's faces are drawn in these 2D targets and copied
    // into the cube, which is then only sampled. Drawing in the cube's layers
    // directly broke the whole picture on the Adreno 840 (#46).
    SDL_GPUTexture *faces[6] = {};
    SDL_GPUTextureFormat format;
    bool cpuDecode = false;                // BC data decoded on the CPU (no BC support or compressed_mipmaps)
    int mipLevels = 1;                     // actual mip levels created on the GPU
    bool generatedMips = false;            // mip levels 1..N generated on upload from decoded level 0
};

// The GPU copy of a GfxVertexBuffer, as of a version of it.
struct GpuVertexBuffer {
    SDL_GPUBuffer *buffer = nullptr;
    uint32_t capacity = 0;
    uint32_t version = 0;
};

// The device the renderer asks SDL_GPU for: its shader formats, and no
// optional feature it does not use.
SDL_PropertiesID DeviceProperties()
{
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV;
#ifdef OPENCMR2_SHADERS_MSL
    formats |= SDL_GPU_SHADERFORMAT_MSL;
#endif
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN,
                           (formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN,
                           (formats & SDL_GPU_SHADERFORMAT_MSL) != 0);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN,
                           Platform_GetSettingInt("video.gpu_debug", 0) != 0);
    // SDL asks Vulkan for these features by default. The renderer does not
    // use them (anisotropy only when video.anisotropy is set, see Sampler),
    // and many Android GPUs lack some, for which SDL would refuse the device.
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_CLIP_DISTANCE_BOOLEAN, false);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_DEPTH_CLAMPING_BOOLEAN, false);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_INDIRECT_DRAW_FIRST_INSTANCE_BOOLEAN, false);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_ANISOTROPY_BOOLEAN,
                           Platform_GetSettingInt("video.anisotropy", 16) > 1);
    return props;
}

struct Executor : GfxExecutor {
    SDL_GPUDevice *device = nullptr;
    SDL_Window *window = nullptr;
    SDL_GPUShader *vs3d = nullptr, *vsTL = nullptr, *fs = nullptr, *fsMrt = nullptr;
    SDL_GPUShader *vsClear = nullptr, *fsClear = nullptr, *fsClearMrt = nullptr;
    SDL_GPUShader *vsPost = nullptr, *fsPostBright = nullptr, *fsPostBlur = nullptr;
    SDL_GPUShader *fsPostGtao = nullptr, *fsPostAoBlur = nullptr, *fsPostComposite = nullptr;
    SDL_GPUShader *vsShadow = nullptr, *fsShadow = nullptr;
    SDL_GPUGraphicsPipeline *postBrightPipeline = nullptr, *postBlurPipeline = nullptr;
    SDL_GPUGraphicsPipeline *postGtaoPipeline = nullptr, *postAoBlurPipeline = nullptr, *postCompositePipeline = nullptr;
    SDL_GPUSampler *postSampler = nullptr, *depthSampler = nullptr, *shadowSampler = nullptr;
    SDL_GPUTexture *shadowMap = nullptr;
    int shadowMapSize = 0;
    int shadowMapCascades = 0;
    bool shadowSupported = false;
    SDL_GPUTextureFormat shadowDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    std::unordered_map<uint32_t, SDL_GPUGraphicsPipeline *> shadowPipelines;
    std::unordered_map<DrawBoundsKey, CachedSphere, DrawBoundsKeyHash> staticBoundsCache;
    SDL_GPUTexture *backColor = nullptr, *backDepth = nullptr;
    SDL_GPUTexture *backColorMsaa = nullptr, *backDepthMsaa = nullptr;
    SDL_GPUTexture *backdrop = nullptr;    // the copy of backColor the distance fade reads
    SDL_GPUTexture *hdrColor = nullptr, *hdrColorMsaa = nullptr, *hdrBackdrop = nullptr;
    SDL_GPUTexture *hdrDepth = nullptr, *hdrDepthMsaa = nullptr;
    SDL_GPUTexture *aoHalfA = nullptr, *aoHalfB = nullptr;
    SDL_GPUTexture *bloomHalfA = nullptr, *bloomHalfB = nullptr;
    SDL_GPUTexture *bloomQuarterA = nullptr, *bloomQuarterB = nullptr;
    int backWidth = 0, backHeight = 0;
    int bloomHalfW = 0, bloomHalfH = 0, bloomQuarterW = 0, bloomQuarterH = 0;
    SDL_GPUSampleCount msaaSamples = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUSampleCount hdrMsaaSamples = SDL_GPU_SAMPLECOUNT_1;
    bool hdrSupported = false;
    bool ssaoSupported = false;
    SDL_GPUTextureFormat colorFormat = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    SDL_GPUTextureFormat hdrFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    SDL_GPUTextureFormat linearDepthFormat = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    SDL_GPUTextureFormat aoFormat = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
    SDL_GPUTextureFormat depthFormat = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    SDL_GPUBuffer *vertexBuffer = nullptr, *indexBuffer = nullptr;
    uint32_t vertexCapacity = 0, indexCapacity = 0;
    SDL_GPUTransferBuffer *uploadBuffer = nullptr;  // the frame's vertices and indices
    uint32_t uploadCapacity = 0;
    SDL_GPUTexture *white2D = nullptr, *whiteCube = nullptr, *flatNormal2D = nullptr;
    std::unordered_map<uint32_t, SDL_GPUGraphicsPipeline *> pipelines;
    std::unordered_map<uint32_t, SDL_GPUSampler *> samplers;
    bool bcSupported = true;
    bool bc7Supported = false;
    bool presentModeVsync = true;
    int anisotropy = 16, smoothScaling = 1;   // video.anisotropy, video.smooth_scaling
    bool compressedMipmaps = true;            // video.compressed_mipmaps
    Uint32 presentWidth = 0, presentHeight = 0; // the window's pixels, as last logged
    // Back buffer reads of recently presented frames (the lens flare test).
    // The game gets the newest one the GPU has finished, a frame or two old:
    // waiting for the last frame instead stopped the CPU from working ahead
    // of the GPU, which halved the frame rate on phones with the sun in view.
    struct Readback {
        SDL_GPUTransferBuffer *buffer = nullptr;
        uint32_t capacity = 0;
        SDL_GPUFence *fence = nullptr;
        uint64_t serial = 0;
        int width = 0, height = 0;
    };
    Readback readbacks[3];
    uint64_t readbackSerial = 0;

    ~Executor() override
    {
        if (device == NULL)
            return;
        SDL_WaitForGPUIdle(device);
        for (Readback &r : readbacks) {
            SDL_ReleaseGPUFence(device, r.fence);
            SDL_ReleaseGPUTransferBuffer(device, r.buffer);
        }
        for (auto &p : pipelines)
            SDL_ReleaseGPUGraphicsPipeline(device, p.second);
        for (auto &p : shadowPipelines)
            SDL_ReleaseGPUGraphicsPipeline(device, p.second);
        for (auto &s : samplers)
            SDL_ReleaseGPUSampler(device, s.second);
        SDL_ReleaseGPUTexture(device, backColor);
        SDL_ReleaseGPUTexture(device, backDepth);
        SDL_ReleaseGPUTexture(device, backColorMsaa);
        SDL_ReleaseGPUTexture(device, backDepthMsaa);
        SDL_ReleaseGPUTexture(device, backdrop);
        SDL_ReleaseGPUTexture(device, hdrColor);
        SDL_ReleaseGPUTexture(device, hdrColorMsaa);
        SDL_ReleaseGPUTexture(device, hdrBackdrop);
        SDL_ReleaseGPUTexture(device, hdrDepth);
        SDL_ReleaseGPUTexture(device, hdrDepthMsaa);
        SDL_ReleaseGPUTexture(device, aoHalfA);
        SDL_ReleaseGPUTexture(device, aoHalfB);
        SDL_ReleaseGPUTexture(device, bloomHalfA);
        SDL_ReleaseGPUTexture(device, bloomHalfB);
        SDL_ReleaseGPUTexture(device, bloomQuarterA);
        SDL_ReleaseGPUTexture(device, bloomQuarterB);
        SDL_ReleaseGPUGraphicsPipeline(device, postBrightPipeline);
        SDL_ReleaseGPUGraphicsPipeline(device, postBlurPipeline);
        SDL_ReleaseGPUGraphicsPipeline(device, postGtaoPipeline);
        SDL_ReleaseGPUGraphicsPipeline(device, postAoBlurPipeline);
        SDL_ReleaseGPUGraphicsPipeline(device, postCompositePipeline);
        SDL_ReleaseGPUSampler(device, postSampler);
        SDL_ReleaseGPUSampler(device, depthSampler);
        SDL_ReleaseGPUSampler(device, shadowSampler);
        SDL_ReleaseGPUTexture(device, shadowMap);
        SDL_ReleaseGPUTexture(device, white2D);
        SDL_ReleaseGPUTexture(device, whiteCube);
        SDL_ReleaseGPUTexture(device, flatNormal2D);
        SDL_ReleaseGPUBuffer(device, vertexBuffer);
        SDL_ReleaseGPUBuffer(device, indexBuffer);
        SDL_ReleaseGPUTransferBuffer(device, uploadBuffer);
        SDL_ReleaseGPUShader(device, vs3d);
        SDL_ReleaseGPUShader(device, vsTL);
        SDL_ReleaseGPUShader(device, fs);
        SDL_ReleaseGPUShader(device, fsMrt);
        SDL_ReleaseGPUShader(device, vsClear);
        SDL_ReleaseGPUShader(device, fsClear);
        SDL_ReleaseGPUShader(device, fsClearMrt);
        SDL_ReleaseGPUShader(device, vsPost);
        SDL_ReleaseGPUShader(device, fsPostBright);
        SDL_ReleaseGPUShader(device, fsPostBlur);
        SDL_ReleaseGPUShader(device, fsPostGtao);
        SDL_ReleaseGPUShader(device, fsPostAoBlur);
        SDL_ReleaseGPUShader(device, fsPostComposite);
        SDL_ReleaseGPUShader(device, vsShadow);
        SDL_ReleaseGPUShader(device, fsShadow);
        SDL_ReleaseWindowFromGPUDevice(device, window);
        SDL_DestroyGPUDevice(device);
    }

    // The shader format the device takes: SPIR-V (Vulkan) or MSL (Metal).
    SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;

    SDL_GPUShader *Shader(const unsigned char *spirv, size_t spirvSize, const unsigned char *msl, size_t mslSize,
                          SDL_GPUShaderStage stage, int samplers, int uniforms)
    {
        SDL_GPUShaderCreateInfo info = {};
        if (shaderFormat == SDL_GPU_SHADERFORMAT_MSL) {
            info.code = msl;
            info.code_size = mslSize - 1; // without the NUL the embedding adds
            info.entrypoint = "main0";    // SPIRV-Cross renames main for MSL
        } else {
            info.code = spirv;
            info.code_size = spirvSize;
            info.entrypoint = "main";
        }
        info.format = shaderFormat;
        info.stage = stage;
        info.num_samplers = samplers;
        info.num_uniform_buffers = uniforms;
        SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);
        if (shader == NULL)
            Sys_Log("gfx: shader: %s", SDL_GetError());
        return shader;
    }

    SDL_GPUTexture *CreateTexture(SDL_GPUTextureType type, SDL_GPUTextureFormat format, int w, int h, int layers,
                                  int levels, SDL_GPUTextureUsageFlags usage,
                                  SDL_GPUSampleCount sampleCount = SDL_GPU_SAMPLECOUNT_1)
    {
        SDL_GPUTextureCreateInfo info = {};
        info.type = type;
        info.format = format;
        info.usage = usage;
        info.width = w;
        info.height = h;
        info.layer_count_or_depth = layers;
        info.num_levels = levels;
        info.sample_count = sampleCount;
        SDL_GPUTexture *t = SDL_CreateGPUTexture(device, &info);
        if (t == NULL)
            Sys_Log("gfx: texture %dx%d: %s", w, h, SDL_GetError());
        return t;
    }

    SDL_GPUSampleCount ChooseMsaaSamples(int requested)
    {
        static const struct {
            int count;
            SDL_GPUSampleCount flag;
        } levels[] = {
            { 8, SDL_GPU_SAMPLECOUNT_8 },
            { 4, SDL_GPU_SAMPLECOUNT_4 },
            { 2, SDL_GPU_SAMPLECOUNT_2 },
        };
        for (const auto &level : levels) {
            if (requested >= level.count &&
                SDL_GPUTextureSupportsSampleCount(device, colorFormat, level.flag) &&
                SDL_GPUTextureSupportsSampleCount(device, depthFormat, level.flag)) {
                return level.flag;
            }
        }
        return SDL_GPU_SAMPLECOUNT_1;
    }

    bool CreateDevice()
    {
        window = Platform_GetWindow();
        if (window == NULL) {
            Sys_Log("gfx: no window");
            return false;
        }
        anisotropy = std::clamp(Platform_GetSettingInt("video.anisotropy", 16), 1, 16);
        smoothScaling = Platform_GetSettingInt("video.smooth_scaling", 1);
        compressedMipmaps = Platform_GetSettingInt("video.compressed_mipmaps", 1) != 0;
        SDL_PropertiesID props = DeviceProperties();
        device = SDL_CreateGPUDeviceWithProperties(props);
        SDL_DestroyProperties(props);
        if (device == NULL) {
            Sys_Log("gfx: SDL_CreateGPUDevice: %s", SDL_GetError());
            return false;
        }
        if (!(SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_SPIRV))
            shaderFormat = SDL_GPU_SHADERFORMAT_MSL;
        if (!SDL_ClaimWindowForGPUDevice(device, window)) {
            Sys_Log("gfx: SDL_ClaimWindowForGPUDevice: %s", SDL_GetError());
            return false;
        }
        Sys_Log("gfx: SDL_GPU driver %s", SDL_GetGPUDeviceDriver(device));
        if (!SDL_GPUTextureSupportsFormat(device, depthFormat, SDL_GPU_TEXTURETYPE_2D,
                                          SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
            depthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        msaaSamples = ChooseMsaaSamples(Platform_GetSettingInt("video.msaa", 4));
        if (msaaSamples != SDL_GPU_SAMPLECOUNT_1) {
            int count = msaaSamples == SDL_GPU_SAMPLECOUNT_8 ? 8 : msaaSamples == SDL_GPU_SAMPLECOUNT_4 ? 4 : 2;
            Sys_Log("gfx: MSAA %dx enabled", count);
        }
        bcSupported = SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM,
                                                   SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        bc7Supported = SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM,
                                                    SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        if (bc7Supported)
            Sys_Log("gfx: native GPU BC7 textures enabled");

        vs3d = Shader(SHADER_CODE(fixed3d_vert), SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
        vsTL = Shader(SHADER_CODE(fixedtl_vert), SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
        fs = Shader(SHADER_CODE(fixed_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 10, 2);
        vsClear = Shader(SHADER_CODE(clear_vert), SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
        fsClear = Shader(SHADER_CODE(clear_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
        if (!vs3d || !vsTL || !fs || !vsClear || !fsClear)
            return false;

        hdrSupported = SDL_GPUTextureSupportsFormat(device, hdrFormat, SDL_GPU_TEXTURETYPE_2D,
                                                    SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        if (hdrSupported) {
            vsPost = Shader(SHADER_CODE(post_vert), SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
            fsPostBright = Shader(SHADER_CODE(post_bright_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
            fsPostBlur = Shader(SHADER_CODE(post_blur_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
            fsPostComposite = Shader(SHADER_CODE(post_composite_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 1);
            SDL_GPUSamplerCreateInfo psi = {};
            psi.min_filter = SDL_GPU_FILTER_LINEAR;
            psi.mag_filter = SDL_GPU_FILTER_LINEAR;
            psi.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
            psi.address_mode_u = psi.address_mode_v = psi.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            postSampler = SDL_CreateGPUSampler(device, &psi);
            if (vsPost && fsPostBright && fsPostBlur && fsPostComposite && postSampler) {
                postBrightPipeline = CreatePostPipeline(fsPostBright, hdrFormat);
                postBlurPipeline = CreatePostPipeline(fsPostBlur, hdrFormat);
                postCompositePipeline = CreatePostPipeline(fsPostComposite, colorFormat);
            }
            if (postBrightPipeline && postBlurPipeline && postCompositePipeline) {
                hdrMsaaSamples = (msaaSamples != SDL_GPU_SAMPLECOUNT_1 &&
                                  SDL_GPUTextureSupportsSampleCount(device, hdrFormat, msaaSamples))
                                     ? msaaSamples
                                     : SDL_GPU_SAMPLECOUNT_1;
                Sys_Log("gfx: HDR 3D post-processing + FidelityFX CAS enabled");

                // Stage 12: MRT linear eye-depth (hardware MSAA resolved) + Half-Res GTAO + Bilateral Blur.
                auto depthFmtSupported = [&](SDL_GPUTextureFormat fmt) {
                    if (!SDL_GPUTextureSupportsFormat(device, fmt, SDL_GPU_TEXTURETYPE_2D,
                                                      SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))
                        return false;
                    if (hdrMsaaSamples != SDL_GPU_SAMPLECOUNT_1 &&
                        !SDL_GPUTextureSupportsSampleCount(device, fmt, hdrMsaaSamples))
                        return false;
                    return true;
                };
                bool depthFmtOk = false;
                if (depthFmtSupported(SDL_GPU_TEXTUREFORMAT_R32_FLOAT)) {
                    linearDepthFormat = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
                    depthFmtOk = true;
                } else if (depthFmtSupported(SDL_GPU_TEXTUREFORMAT_R16_FLOAT)) {
                    linearDepthFormat = SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
                    depthFmtOk = true;
                }
                bool aoFmtOk = false;
                if (SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, SDL_GPU_TEXTURETYPE_2D,
                                                 SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
                    aoFormat = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
                    aoFmtOk = true;
                } else if (SDL_GPUTextureSupportsFormat(device, hdrFormat, SDL_GPU_TEXTURETYPE_2D,
                                                        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                                                            SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
                    aoFormat = hdrFormat;
                    aoFmtOk = true;
                }
                if (depthFmtOk && aoFmtOk) {
                    fsMrt = Shader(SHADER_CODE(fixed_mrt_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 10, 2);
                    fsClearMrt = Shader(SHADER_CODE(clear_mrt_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
                    fsPostGtao = Shader(SHADER_CODE(post_gtao_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
                    fsPostAoBlur = Shader(SHADER_CODE(post_ao_blur_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
                    SDL_GPUSamplerCreateInfo dsi = {};
                    dsi.min_filter = SDL_GPU_FILTER_NEAREST;
                    dsi.mag_filter = SDL_GPU_FILTER_NEAREST;
                    dsi.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
                    dsi.address_mode_u = dsi.address_mode_v = dsi.address_mode_w =
                        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
                    depthSampler = SDL_CreateGPUSampler(device, &dsi);
                    if (fsMrt && fsClearMrt && fsPostGtao && fsPostAoBlur && depthSampler) {
                        postGtaoPipeline = CreatePostPipeline(fsPostGtao, aoFormat);
                        postAoBlurPipeline = CreatePostPipeline(fsPostAoBlur, aoFormat);
                    }
                    if (postGtaoPipeline && postAoBlurPipeline) {
                        ssaoSupported = true;
                        Sys_Log("gfx: Half-res GTAO + bilateral blur enabled");
                    }
                }
            } else {
                hdrSupported = false;
                Sys_Log("gfx: HDR 3D post-processing unavailable, falling back to SDR");
            }
        }

        shadowSupported = SDL_GPUTextureSupportsFormat(device, shadowDepthFormat, SDL_GPU_TEXTURETYPE_2D,
                                                       SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
                                                           SDL_GPU_TEXTUREUSAGE_SAMPLER);
        if (shadowSupported) {
            vsShadow = Shader(SHADER_CODE(shadow_vert), SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
            fsShadow = Shader(SHADER_CODE(shadow_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
            SDL_GPUSamplerCreateInfo ssi = {};
            ssi.min_filter = SDL_GPU_FILTER_NEAREST;
            ssi.mag_filter = SDL_GPU_FILTER_NEAREST;
            ssi.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
            ssi.address_mode_u = ssi.address_mode_v = ssi.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            shadowSampler = SDL_CreateGPUSampler(device, &ssi);
            if (!vsShadow || !fsShadow || !shadowSampler) {
                shadowSupported = false;
            } else {
                int initCascades = std::clamp(Platform_GetSettingInt("video.shadow_cascades", 2), 1, 2);
                EnsureShadowMap(Platform_GetSettingInt("video.shadow_map_size", 2048), initCascades);
                if (shadowMap != NULL)
                    Sys_Log("gfx: real-time sun CSM enabled (%d cascade%s, %dx%d per cascade)", shadowMapCascades,
                            shadowMapCascades > 1 ? "s" : "", shadowMapSize, shadowMapSize);
                else
                    shadowSupported = false;
            }
        }

        white2D = CreateTexture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, 1, 1, 1, 1,
                                SDL_GPU_TEXTUREUSAGE_SAMPLER);
        whiteCube = CreateTexture(SDL_GPU_TEXTURETYPE_CUBE, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, 1, 1, 6, 1,
                                  SDL_GPU_TEXTUREUSAGE_SAMPLER);
        flatNormal2D = CreateTexture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, 1, 1, 1, 1,
                                     SDL_GPU_TEXTUREUSAGE_SAMPLER);
        // BGRA8: white = (255,255,255,255), neutral tangent normal (128,128,255) = B=255,G=128,R=128,A=255 = 0xff8080ff.
        uint32_t defaults[8] = {
            0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xff8080ff
        };
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, sizeof(defaults), 0 };
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device, &tbi);
        memcpy(SDL_MapGPUTransferBuffer(device, tb, false), defaults, sizeof(defaults));
        SDL_UnmapGPUTransferBuffer(device, tb);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        for (int layer = 0; layer < 8; layer++) {
            SDL_GPUTextureTransferInfo src = { tb, (Uint32)(layer * 4), 0, 0 };
            SDL_GPUTextureRegion dst = {};
            dst.texture = layer < 6 ? whiteCube : (layer == 6 ? white2D : flatNormal2D);
            dst.layer = layer < 6 ? layer : 0;
            dst.w = dst.h = dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
        }
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(device, tb);

        // The pipelines the game draws with (keys of PipelineKey: menus,
        // stages, replays, results), made now: the first use of each could
        // take a frame's time to compile when the driver has not cached it.
        static const uint32_t common[] = {
            0x001a0459, 0x001a045b, 0x001a04b9, 0x001a0cb8, 0x001a0cb9, 0x001a0cbb, 0x001a2458, 0x001a2cb8,
            0x001a64b8, 0x001a6ca8, 0x001a6cb4, 0x001a6cb8, 0x000a2458, 0x000a2cb8, 0x000a6ca8, 0x000a6cb8,
        };
        for (uint32_t key : common) {
            Pipeline(key);
            if (msaaSamples != SDL_GPU_SAMPLECOUNT_1)
                Pipeline(key | ((uint32_t)msaaSamples << 21));
            if (hdrSupported) {
                Pipeline(key | (1u << 23));
                if (hdrMsaaSamples != SDL_GPU_SAMPLECOUNT_1)
                    Pipeline(key | ((uint32_t)hdrMsaaSamples << 21) | (1u << 23));
                if (ssaoSupported) {
                    Pipeline(key | (1u << 23) | (1u << 24));
                    if (hdrMsaaSamples != SDL_GPU_SAMPLECOUNT_1)
                        Pipeline(key | ((uint32_t)hdrMsaaSamples << 21) | (1u << 23) | (1u << 24));
                }
            }
        }
        for (int clear = 1; clear < 4; clear++) {
            ClearPipeline((clear & 1) != 0, (clear & 2) != 0, SDL_GPU_SAMPLECOUNT_1, false, false);
            if (msaaSamples != SDL_GPU_SAMPLECOUNT_1)
                ClearPipeline((clear & 1) != 0, (clear & 2) != 0, msaaSamples, false, false);
            if (hdrSupported) {
                ClearPipeline((clear & 1) != 0, (clear & 2) != 0, SDL_GPU_SAMPLECOUNT_1, true, false);
                if (hdrMsaaSamples != SDL_GPU_SAMPLECOUNT_1)
                    ClearPipeline((clear & 1) != 0, (clear & 2) != 0, hdrMsaaSamples, true, false);
                if (ssaoSupported) {
                    ClearPipeline((clear & 1) != 0, (clear & 2) != 0, SDL_GPU_SAMPLECOUNT_1, true, true);
                    if (hdrMsaaSamples != SDL_GPU_SAMPLECOUNT_1)
                        ClearPipeline((clear & 1) != 0, (clear & 2) != 0, hdrMsaaSamples, true, true);
                }
            }
        }
        return true;
    }

    bool SetVideoMode(int width, int height, bool fullscreen) override
    {
        if (device == NULL && !CreateDevice())
            return false;
        if (backColor == NULL || backWidth != width || backHeight != height) {
            SDL_WaitForGPUIdle(device);
            SDL_ReleaseGPUTexture(device, backColor);
            SDL_ReleaseGPUTexture(device, backDepth);
            SDL_ReleaseGPUTexture(device, backColorMsaa);
            SDL_ReleaseGPUTexture(device, backDepthMsaa);
            SDL_ReleaseGPUTexture(device, backdrop);
            SDL_ReleaseGPUTexture(device, hdrColor);
            SDL_ReleaseGPUTexture(device, hdrColorMsaa);
            SDL_ReleaseGPUTexture(device, hdrBackdrop);
            SDL_ReleaseGPUTexture(device, hdrDepth);
            SDL_ReleaseGPUTexture(device, hdrDepthMsaa);
            SDL_ReleaseGPUTexture(device, aoHalfA);
            SDL_ReleaseGPUTexture(device, aoHalfB);
            SDL_ReleaseGPUTexture(device, bloomHalfA);
            SDL_ReleaseGPUTexture(device, bloomHalfB);
            SDL_ReleaseGPUTexture(device, bloomQuarterA);
            SDL_ReleaseGPUTexture(device, bloomQuarterB);
            backColorMsaa = nullptr;
            backDepthMsaa = nullptr;
            hdrColor = hdrColorMsaa = hdrBackdrop = nullptr;
            hdrDepth = hdrDepthMsaa = aoHalfA = aoHalfB = nullptr;
            bloomHalfA = bloomHalfB = bloomQuarterA = bloomQuarterB = nullptr;
            backColor = CreateTexture(SDL_GPU_TEXTURETYPE_2D, colorFormat, width, height, 1, 1,
                                      SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
            backDepth = CreateTexture(SDL_GPU_TEXTURETYPE_2D, depthFormat, width, height, 1, 1,
                                      SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
            if (msaaSamples != SDL_GPU_SAMPLECOUNT_1) {
                backColorMsaa = CreateTexture(SDL_GPU_TEXTURETYPE_2D, colorFormat, width, height, 1, 1,
                                              SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, msaaSamples);
                backDepthMsaa = CreateTexture(SDL_GPU_TEXTURETYPE_2D, depthFormat, width, height, 1, 1,
                                              SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, msaaSamples);
                if (backColorMsaa == NULL || backDepthMsaa == NULL) {
                    SDL_ReleaseGPUTexture(device, backColorMsaa);
                    SDL_ReleaseGPUTexture(device, backDepthMsaa);
                    backColorMsaa = backDepthMsaa = nullptr;
                    msaaSamples = SDL_GPU_SAMPLECOUNT_1;
                    hdrMsaaSamples = SDL_GPU_SAMPLECOUNT_1;
                }
            }
            backdrop = CreateTexture(SDL_GPU_TEXTURETYPE_2D, colorFormat, width, height, 1, 1,
                                     SDL_GPU_TEXTUREUSAGE_SAMPLER);
            if (hdrSupported) {
                bloomHalfW = std::max(1, width / 2);
                bloomHalfH = std::max(1, height / 2);
                bloomQuarterW = std::max(1, width / 4);
                bloomQuarterH = std::max(1, height / 4);
                hdrColor = CreateTexture(SDL_GPU_TEXTURETYPE_2D, hdrFormat, width, height, 1, 1,
                                         SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
                hdrBackdrop = CreateTexture(SDL_GPU_TEXTURETYPE_2D, hdrFormat, width, height, 1, 1,
                                            SDL_GPU_TEXTUREUSAGE_SAMPLER);
                bloomHalfA = CreateTexture(SDL_GPU_TEXTURETYPE_2D, hdrFormat, bloomHalfW, bloomHalfH, 1, 1,
                                           SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
                bloomHalfB = CreateTexture(SDL_GPU_TEXTURETYPE_2D, hdrFormat, bloomHalfW, bloomHalfH, 1, 1,
                                           SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
                bloomQuarterA = CreateTexture(SDL_GPU_TEXTURETYPE_2D, hdrFormat, bloomQuarterW, bloomQuarterH, 1, 1,
                                              SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
                bloomQuarterB = CreateTexture(SDL_GPU_TEXTURETYPE_2D, hdrFormat, bloomQuarterW, bloomQuarterH, 1, 1,
                                              SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
                if (hdrMsaaSamples != SDL_GPU_SAMPLECOUNT_1 && backDepthMsaa != NULL) {
                    hdrColorMsaa = CreateTexture(SDL_GPU_TEXTURETYPE_2D, hdrFormat, width, height, 1, 1,
                                                 SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, hdrMsaaSamples);
                    if (hdrColorMsaa == NULL)
                        hdrMsaaSamples = SDL_GPU_SAMPLECOUNT_1;
                }
                if (!hdrColor || !hdrBackdrop || !bloomHalfA || !bloomHalfB || !bloomQuarterA || !bloomQuarterB) {
                    SDL_ReleaseGPUTexture(device, hdrColor);
                    SDL_ReleaseGPUTexture(device, hdrColorMsaa);
                    SDL_ReleaseGPUTexture(device, hdrBackdrop);
                    SDL_ReleaseGPUTexture(device, bloomHalfA);
                    SDL_ReleaseGPUTexture(device, bloomHalfB);
                    SDL_ReleaseGPUTexture(device, bloomQuarterA);
                    SDL_ReleaseGPUTexture(device, bloomQuarterB);
                    hdrColor = hdrColorMsaa = hdrBackdrop = nullptr;
                    bloomHalfA = bloomHalfB = bloomQuarterA = bloomQuarterB = nullptr;
                    hdrSupported = false;
                    ssaoSupported = false;
                } else if (ssaoSupported) {
                    hdrDepth = CreateTexture(SDL_GPU_TEXTURETYPE_2D, linearDepthFormat, width, height, 1, 1,
                                             SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
                    aoHalfA = CreateTexture(SDL_GPU_TEXTURETYPE_2D, aoFormat, bloomHalfW, bloomHalfH, 1, 1,
                                            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
                    aoHalfB = CreateTexture(SDL_GPU_TEXTURETYPE_2D, aoFormat, bloomHalfW, bloomHalfH, 1, 1,
                                            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
                    if (hdrMsaaSamples != SDL_GPU_SAMPLECOUNT_1 && hdrColorMsaa != NULL) {
                        hdrDepthMsaa = CreateTexture(SDL_GPU_TEXTURETYPE_2D, linearDepthFormat, width, height, 1, 1,
                                                     SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, hdrMsaaSamples);
                    }
                    bool msaaMismatch = (hdrMsaaSamples != SDL_GPU_SAMPLECOUNT_1 && hdrColorMsaa != NULL &&
                                         hdrDepthMsaa == NULL);
                    if (!hdrDepth || !aoHalfA || !aoHalfB || msaaMismatch) {
                        SDL_ReleaseGPUTexture(device, hdrDepth);
                        SDL_ReleaseGPUTexture(device, hdrDepthMsaa);
                        SDL_ReleaseGPUTexture(device, aoHalfA);
                        SDL_ReleaseGPUTexture(device, aoHalfB);
                        hdrDepth = hdrDepthMsaa = aoHalfA = aoHalfB = nullptr;
                        ssaoSupported = false;
                    }
                }
            }
            backWidth = width;
            backHeight = height;
            if (backColor == NULL || backDepth == NULL || backdrop == NULL)
                return false;
        }
        SDL_SetWindowResizable(window, true);
        SetFullscreen(fullscreen);
        SDL_ShowWindow(window);
        return true;
    }

    void SetFullscreen(bool fullscreen) override
    {
        Gfx_SetWindowMode(window, fullscreen, backWidth, backHeight);
    }

    bool SupportsBC7() const override { return bc7Supported; }

    // ---- textures ----

    SDL_GPUTextureFormat GpuFormat(int format, bool &cpuDecode)
    {
        cpuDecode = false;
        switch (format) {
        case GFX_FORMAT_BC1:
            if (bcSupported)
                return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
            cpuDecode = true;
            return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        case GFX_FORMAT_BC3:
            if (bcSupported)
                return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
            cpuDecode = true;
            return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        case GFX_FORMAT_BC7:
            return SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM;
        case GFX_FORMAT_BUMP_DUDVL:
            return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        default:
            return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        }
    }

    static void DownsampleBGRA(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh)
    {
        for (int y = 0; y < dh; y++) {
            for (int x = 0; x < dw; x++) {
                int weighted[3] = { 0, 0, 0 };
                int sum[4] = { 0, 0, 0, 0 };
                int alphaSum = 0;
                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        int sx = std::min(sw - 1, x * 2 + dx);
                        int sy = std::min(sh - 1, y * 2 + dy);
                        const uint8_t *p = src + ((size_t)sy * sw + sx) * 4;
                        int a = p[3];
                        alphaSum += a;
                        for (int c = 0; c < 4; c++)
                            sum[c] += p[c];
                        for (int c = 0; c < 3; c++)
                            weighted[c] += p[c] * a;
                    }
                }
                uint8_t *q = dst + ((size_t)y * dw + x) * 4;
                if (alphaSum > 0) {
                    for (int c = 0; c < 3; c++)
                        q[c] = (uint8_t)((weighted[c] + alphaSum / 2) / alphaSum);
                    q[3] = (uint8_t)((sum[3] + 2) / 4);
                } else {
                    for (int c = 0; c < 4; c++)
                        q[c] = (uint8_t)((sum[c] + 2) / 4);
                }
            }
        }
    }

    GpuTexture *Ensure(GfxTexture *t)
    {
        if (t->gpu != NULL)
            return (GpuTexture *)t->gpu;
        GpuTexture *g = new GpuTexture;
        g->format = GpuFormat(t->format, g->cpuDecode);
        bool cube = (t->flags & GFX_TEXTURE_CUBE) != 0;
        bool target = (t->flags & GFX_TEXTURE_RENDER_TARGET) != 0;
        if (t->format == GFX_FORMAT_BC7 && !bc7Supported) {
            t->gpu = g;
            t->dirty = false;
            return g;
        }
        bool isCompressed = (t->format == GFX_FORMAT_BC1 || t->format == GFX_FORMAT_BC3);
        g->mipLevels = t->mipLevels;
        g->generatedMips = false;
        if (compressedMipmaps && isCompressed && !cube && !target && !(t->flags & GFX_TEXTURE_STAGING) &&
            t->width >= 4 && t->height >= 4) {
            g->cpuDecode = true;
            g->format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            int mips = 1;
            while ((t->width >> mips) > 0 || (t->height >> mips) > 0)
                mips++;
            g->mipLevels = std::min(mips, 16);
            g->generatedMips = g->mipLevels > 1;
        }
        SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        if (target)
            usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        if (cube && target) {
            usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            for (int f = 0; f < 6; f++)
                g->faces[f] = CreateTexture(SDL_GPU_TEXTURETYPE_2D, g->format, t->width, t->height, 1, 1,
                                            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
        }
        g->texture = CreateTexture(cube ? SDL_GPU_TEXTURETYPE_CUBE : SDL_GPU_TEXTURETYPE_2D, g->format, t->width,
                                   t->height, cube ? 6 : 1, g->mipLevels, usage);
        if (target)
            g->depth = CreateTexture(SDL_GPU_TEXTURETYPE_2D, depthFormat, t->width, t->height, 1, 1,
                                     SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
        t->gpu = g;
        t->dirty = true;
        return g;
    }

    // Bytes of a level as uploaded (after conversion).
    size_t UploadSize(GfxTexture *t, GpuTexture *g, int w, int h)
    {
        if (t->format == GFX_FORMAT_BC7 ||
            ((t->format == GFX_FORMAT_BC1 || t->format == GFX_FORMAT_BC3) && !g->cpuDecode))
            return Gfx_LevelSize(t->format, w, h, NULL);
        return (size_t)w * h * 4;
    }

    void ConvertLevel(GfxTexture *t, GpuTexture *g, const uint8_t *src, int w, int h, uint8_t *dst)
    {
        size_t n = (size_t)w * h;
        switch (t->format) {
        case GFX_FORMAT_BGRX8:
            for (size_t i = 0; i < n; i++)
                ((uint32_t *)dst)[i] = ((const uint32_t *)src)[i] | 0xff000000;
            break;
        case GFX_FORMAT_BUMP_DUDVL:
            for (size_t i = 0; i < n; i++) {
                dst[i * 4] = src[i * 3];
                dst[i * 4 + 1] = src[i * 3 + 1];
                dst[i * 4 + 2] = src[i * 3 + 2];
                dst[i * 4 + 3] = 0xff;
            }
            break;
        case GFX_FORMAT_BC7:
            memcpy(dst, src, Gfx_LevelSize(t->format, w, h, NULL));
            break;
        case GFX_FORMAT_BC1:
        case GFX_FORMAT_BC3:
            if (g->cpuDecode) {
                int blockBytes = t->format == GFX_FORMAT_BC1 ? 8 : 16;
                int bw = (w + 3) / 4, bh = (h + 3) / 4;
                uint32_t block[16];
                for (int by = 0; by < bh; by++)
                    for (int bx = 0; bx < bw; bx++) {
                        Gfx_DecodeBlock(t->format, src + (by * bw + bx) * blockBytes, block);
                        for (int y = 0; y < 4; y++)
                            for (int x = 0; x < 4; x++)
                                if (bx * 4 + x < w && by * 4 + y < h)
                                    ((uint32_t *)dst)[(size_t)(by * 4 + y) * w + bx * 4 + x] = block[y * 4 + x];
                    }
            } else {
                memcpy(dst, src, Gfx_LevelSize(t->format, w, h, NULL));
            }
            break;
        default:
            memcpy(dst, src, n * 4);
            break;
        }
    }

    void UploadTextures(SDL_GPUCommandBuffer *cmd, std::vector<GfxTexture *> &textures)
    {
        size_t total = 0;
        for (GfxTexture *t : textures) {
            if (!t->dirty)
                continue;
            GpuTexture *g = Ensure(t);
            if (g->texture == NULL) {
                t->dirty = false;
                continue;
            }
            int faces = (t->flags & GFX_TEXTURE_CUBE) ? 6 : 1;
            for (int f = 0; f < faces; f++)
                for (int m = 0; m < g->mipLevels; m++) {
                    if ((!g->generatedMips || m == 0) && t->levels[f][m].empty())
                        continue;
                    total = (total + 15u) & ~size_t(15u);
                    total += UploadSize(t, g, std::max(1, t->width >> m), std::max(1, t->height >> m));
                }
        }
        if (total == 0)
            return;
        SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, (Uint32)total, 0 };
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device, &tbi);
        uint8_t *map = (uint8_t *)SDL_MapGPUTransferBuffer(device, tb, false);
        struct Pending {
            SDL_GPUTexture *texture;
            int face, level, w, h;
            size_t offset;
        };
        std::vector<Pending> pending;
        size_t offset = 0;
        for (GfxTexture *t : textures) {
            if (!t->dirty)
                continue;
            GpuTexture *g = (GpuTexture *)t->gpu;
            int faces = (t->flags & GFX_TEXTURE_CUBE) ? 6 : 1;
            for (int f = 0; f < faces; f++) {
                size_t prevOffset = offset;
                int prevW = t->width, prevH = t->height;
                for (int m = 0; m < g->mipLevels; m++) {
                    if ((!g->generatedMips || m == 0) && t->levels[f][m].empty())
                        continue;
                    offset = (offset + 15u) & ~size_t(15u);
                    int w = std::max(1, t->width >> m), h = std::max(1, t->height >> m);
                    if (g->generatedMips && m > 0) {
                        DownsampleBGRA(map + prevOffset, prevW, prevH, map + offset, w, h);
                    } else {
                        ConvertLevel(t, g, t->levels[f][m].data(), w, h, map + offset);
                    }
                    pending.push_back({ g->texture, f, m, w, h, offset });
                    prevOffset = offset;
                    prevW = w;
                    prevH = h;
                    offset += UploadSize(t, g, w, h);
                }
            }
            t->dirty = false;
            // Immutable native BC7 overrides never need CPU readback: drop CPU mip copies once staged.
            if ((t->flags & GFX_TEXTURE_OVERRIDE) && t->format == GFX_FORMAT_BC7) {
                for (int m = 0; m < t->mipLevels; m++)
                    std::vector<uint8_t>().swap(t->levels[0][m]);
            }
        }
        SDL_UnmapGPUTransferBuffer(device, tb);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        for (const Pending &p : pending) {
            SDL_GPUTextureTransferInfo src = { tb, (Uint32)p.offset, 0, 0 };
            SDL_GPUTextureRegion dst = {};
            dst.texture = p.texture;
            dst.mip_level = p.level;
            dst.layer = p.face;
            dst.w = p.w;
            dst.h = p.h;
            dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
        }
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(device, tb);
    }

    void DestroyTexture(GfxTexture *t) override
    {
        GpuTexture *g = (GpuTexture *)t->gpu;
        if (g == NULL || device == NULL)
            return;
        SDL_ReleaseGPUTexture(device, g->texture);
        SDL_ReleaseGPUTexture(device, g->depth);
        for (SDL_GPUTexture *face : g->faces)
            SDL_ReleaseGPUTexture(device, face);
        delete g;
        t->gpu = NULL;
    }

    void ReadTexture(GfxTexture *t) override
    {
        GpuTexture *g = (GpuTexture *)t->gpu;
        if (g == NULL || t->format != GFX_FORMAT_BGRX8 && t->format != GFX_FORMAT_BGRA8)
            return;
        int faces = (t->flags & GFX_TEXTURE_CUBE) ? 6 : 1;
        size_t level = (size_t)t->width * t->height * 4;
        SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, (Uint32)(level * faces), 0 };
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device, &tbi);
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        for (int f = 0; f < faces; f++) {
            SDL_GPUTextureRegion src = {};
            src.texture = g->texture;
            src.layer = f;
            src.w = t->width;
            src.h = t->height;
            src.d = 1;
            SDL_GPUTextureTransferInfo dst = { tb, (Uint32)(level * f), 0, 0 };
            SDL_DownloadFromGPUTexture(copy, &src, &dst);
        }
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        SDL_WaitForGPUFences(device, true, &fence, 1);
        SDL_ReleaseGPUFence(device, fence);
        const uint8_t *map = (const uint8_t *)SDL_MapGPUTransferBuffer(device, tb, false);
        for (int f = 0; f < faces; f++)
            memcpy(t->levels[f][0].data(), map + level * f, level);
        SDL_UnmapGPUTransferBuffer(device, tb);
        SDL_ReleaseGPUTransferBuffer(device, tb);
    }

    // ---- pipelines and samplers ----

    SDL_GPUGraphicsPipeline *CreatePostPipeline(SDL_GPUShader *fragShader, SDL_GPUTextureFormat targetFormat)
    {
        SDL_GPUColorTargetDescription target = {};
        target.format = targetFormat;
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = vsPost;
        info.fragment_shader = fragShader;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.enable_depth_clip = true;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.target_info.color_target_descriptions = &target;
        info.target_info.num_color_targets = 1;
        info.target_info.has_depth_stencil_target = false;
        SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(device, &info);
        if (p == NULL)
            Sys_Log("gfx: post pipeline: %s", SDL_GetError());
        return p;
    }

    static int SanitizeShadowMapSize(int requested)
    {
        if (requested >= 4096)
            return 4096;
        if (requested >= 2048)
            return 2048;
        return 1024;
    }

    void EnsureShadowMap(int requestedSize, int requestedCascades)
    {
        if (!shadowSupported)
            return;
        int sz = SanitizeShadowMapSize(requestedSize);
        int cascades = std::clamp(requestedCascades, 1, 2);
        if (shadowMap != NULL && shadowMapSize == sz && shadowMapCascades == cascades)
            return;
        if (shadowMap != NULL) {
            SDL_WaitForGPUIdle(device);
            SDL_ReleaseGPUTexture(device, shadowMap);
            shadowMap = nullptr;
        }
        int atlasW = sz * cascades;
        shadowMap = CreateTexture(SDL_GPU_TEXTURETYPE_2D, shadowDepthFormat, atlasW, sz, 1, 1,
                                  SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        if (shadowMap == NULL && cascades > 1) {
            // Fall back to smaller per-cascade size if atlas exceeds GPU limits (e.g. 8192px).
            sz = std::min(sz, 2048);
            atlasW = sz * cascades;
            shadowMap = CreateTexture(SDL_GPU_TEXTURETYPE_2D, shadowDepthFormat, atlasW, sz, 1, 1,
                                      SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        }
        shadowMapSize = shadowMap != NULL ? sz : 0;
        shadowMapCascades = shadowMap != NULL ? cascades : 0;
    }

    SDL_GPUGraphicsPipeline *ShadowPipeline(int primitive)
    {
        uint32_t key = (uint32_t)(primitive & 7);
        auto it = shadowPipelines.find(key);
        if (it != shadowPipelines.end())
            return it->second;

        SDL_GPUVertexBufferDescription vb = {};
        vb.slot = 0;
        vb.pitch = sizeof(GfxVertex);
        vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        SDL_GPUVertexAttribute attrs[6] = {
            { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 },
            { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12 },
            { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 24 },
            { 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 28 },
            { 4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 32 },
            { 5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 40 },
        };

        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = vsShadow;
        info.fragment_shader = fsShadow;
        info.vertex_input_state.vertex_buffer_descriptions = &vb;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attrs;
        info.vertex_input_state.num_vertex_attributes = 6;
        info.primitive_type = PrimitiveType(primitive);
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.enable_depth_clip = true;
        info.rasterizer_state.enable_depth_bias = true;
        info.rasterizer_state.depth_bias_constant_factor = 1.5f;
        info.rasterizer_state.depth_bias_slope_factor = 2.0f;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.target_info.num_color_targets = 0;
        info.target_info.depth_stencil_format = shadowDepthFormat;
        info.target_info.has_depth_stencil_target = true;
        SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(device, &info);
        if (p == NULL)
            Sys_Log("gfx: shadow pipeline: %s", SDL_GetError());
        shadowPipelines[key] = p;
        return p;
    }

    // A draw's pipeline: everything of it the state can change.
    static uint32_t PipelineKey(const GfxCommand &cmd, const GfxDrawState &s,
                                SDL_GPUSampleCount samples = SDL_GPU_SAMPLECOUNT_1, bool hdr = false,
                                bool mrtDepth = false)
    {
        bool depthTest = s.depthTest;
        return (cmd.transformed ? 1u : 0u) | (uint32_t)(cmd.primitive & 7) << 1 | (s.alphaBlend ? 1u : 0u) << 4 |
               (uint32_t)(s.srcBlend & 15) << 5 | (uint32_t)(s.destBlend & 15) << 9 | (depthTest ? 1u : 0u) << 13 |
               ((depthTest && s.depthWrite) ? 1u : 0u) << 14 | (uint32_t)(s.depthFunc & 15) << 15 |
               (uint32_t)(s.cull & 3) << 19 | (uint32_t)(samples & 3) << 21 | (hdr ? 1u : 0u) << 23 |
               (mrtDepth ? 1u : 0u) << 24;
    }

    SDL_GPUGraphicsPipeline *Pipeline(uint32_t key)
    {
        auto it = pipelines.find(key);
        if (it != pipelines.end())
            return it->second;
        bool transformed = (key & 1) != 0, alphaBlend = (key >> 4 & 1) != 0, depthTest = (key >> 13 & 1) != 0;
        bool depthWrite = (key >> 14 & 1) != 0;
        int primitive = key >> 1 & 7, srcBlend = key >> 5 & 15, destBlend = key >> 9 & 15, depthFunc = key >> 15 & 15;
        int cull = key >> 19 & 3;
        SDL_GPUSampleCount samples = (SDL_GPUSampleCount)((key >> 21) & 3);
        bool hdr = (key >> 23 & 1) != 0;
        bool mrtDepth = (key >> 24 & 1) != 0;

        SDL_GPUVertexBufferDescription vb = {};
        vb.slot = 0;
        vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        SDL_GPUVertexAttribute attrs[6] = {};
        int attrCount;
        if (transformed) {
            vb.pitch = sizeof(GfxTLVertex);
            attrs[0] = { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0 };
            attrs[1] = { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 16 };
            attrs[2] = { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 20 };
            attrs[3] = { 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 24 };
            attrCount = 4;
        } else {
            vb.pitch = sizeof(GfxVertex);
            attrs[0] = { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 };
            attrs[1] = { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12 };
            attrs[2] = { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 24 };
            attrs[3] = { 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 28 };
            attrs[4] = { 4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 32 };
            attrs[5] = { 5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 40 };
            attrCount = 6;
        }

        SDL_GPUColorTargetDescription targets[2] = {};
        targets[0].format = hdr ? hdrFormat : colorFormat;
        targets[0].blend_state.enable_blend = alphaBlend;
        targets[0].blend_state.src_color_blendfactor = BlendFactor(srcBlend);
        targets[0].blend_state.dst_color_blendfactor = BlendFactor(destBlend);
        targets[0].blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        targets[0].blend_state.src_alpha_blendfactor = BlendFactor(srcBlend);
        targets[0].blend_state.dst_alpha_blendfactor = BlendFactor(destBlend);
        targets[0].blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        if (mrtDepth) {
            targets[1].format = linearDepthFormat;
            targets[1].blend_state.enable_blend = false;
            targets[1].blend_state.enable_color_write_mask = true;
            targets[1].blend_state.color_write_mask = (depthTest && depthWrite && !transformed) ? 0x1 : 0;
        }

        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = transformed ? vsTL : vs3d;
        info.fragment_shader = (mrtDepth && fsMrt != NULL) ? fsMrt : fs;
        info.vertex_input_state.vertex_buffer_descriptions = &vb;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attrs;
        info.vertex_input_state.num_vertex_attributes = attrCount;
        info.primitive_type = PrimitiveType(primitive);
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        // Direct3D's front faces are clockwise; CULL_CCW (its default)
        // removes the back faces.
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
        info.rasterizer_state.cull_mode = cull == GFX_CULL_CW    ? SDL_GPU_CULLMODE_FRONT
                                          : cull == GFX_CULL_CCW ? SDL_GPU_CULLMODE_BACK
                                                                   : SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.enable_depth_clip = true;
        info.depth_stencil_state.enable_depth_test = depthTest;
        info.depth_stencil_state.enable_depth_write = depthTest && depthWrite;
        info.depth_stencil_state.compare_op = CompareOp(depthFunc);
        info.multisample_state.sample_count = samples;
        info.target_info.color_target_descriptions = targets;
        info.target_info.num_color_targets = mrtDepth ? 2 : 1;
        info.target_info.depth_stencil_format = depthFormat;
        info.target_info.has_depth_stencil_target = true;
        SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(device, &info);
        if (p == NULL)
            Sys_Log("gfx: pipeline: %s", SDL_GetError());
        pipelines[key] = p;
        return p;
    }

    SDL_GPUGraphicsPipeline *ClearPipeline(bool color, bool depth, SDL_GPUSampleCount samples = SDL_GPU_SAMPLECOUNT_1,
                                           bool hdr = false, bool mrtDepth = false)
    {
        uint32_t key = 0x80000000u | (color ? 1u : 0u) | (depth ? 2u : 0u) | (uint32_t)(samples & 3) << 2 |
                       (hdr ? 1u : 0u) << 4 | (mrtDepth ? 1u : 0u) << 5;
        auto it = pipelines.find(key);
        if (it != pipelines.end())
            return it->second;
        SDL_GPUColorTargetDescription targets[2] = {};
        targets[0].format = hdr ? hdrFormat : colorFormat;
        targets[0].blend_state.enable_color_write_mask = true;
        targets[0].blend_state.color_write_mask = color ? 0xf : 0;
        if (mrtDepth) {
            targets[1].format = linearDepthFormat;
            targets[1].blend_state.enable_color_write_mask = true;
            targets[1].blend_state.color_write_mask = depth ? 0x1 : 0;
        }
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = vsClear;
        info.fragment_shader = (mrtDepth && fsClearMrt != NULL) ? fsClearMrt : fsClear;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        // CreateDevice leaves depth clamping off, which needs depth clipping.
        info.rasterizer_state.enable_depth_clip = true;
        info.multisample_state.sample_count = samples;
        info.depth_stencil_state.enable_depth_test = depth;
        info.depth_stencil_state.enable_depth_write = depth;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
        info.target_info.color_target_descriptions = targets;
        info.target_info.num_color_targets = mrtDepth ? 2 : 1;
        info.target_info.depth_stencil_format = depthFormat;
        info.target_info.has_depth_stencil_target = true;
        SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(device, &info);
        if (p == NULL)
            Sys_Log("gfx: clear pipeline: %s", SDL_GetError());
        pipelines[key] = p;
        return p;
    }

    SDL_GPUSampler *Sampler(const GfxStageState &stage)
    {
        int aniso = Platform_IsVisualOverhaulEnabled() ? anisotropy : 1;
        uint32_t key = (uint32_t)(stage.minFilter & 3) | (uint32_t)(stage.magFilter & 3) << 2 |
                       (uint32_t)(stage.mipFilter & 3) << 4 | (uint32_t)(stage.address & 3) << 6 |
                       (uint32_t)(aniso & 31) << 8;
        auto it = samplers.find(key);
        if (it != samplers.end())
            return it->second;
        SDL_GPUSamplerCreateInfo info = {};
        info.min_filter = stage.minFilter == GFX_FILTER_LINEAR ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        info.mag_filter = stage.magFilter == GFX_FILTER_LINEAR ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        info.mipmap_mode = stage.mipFilter == GFX_MIPFILTER_LINEAR ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR
                                                                   : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        SDL_GPUSamplerAddressMode mode = stage.address == GFX_ADDRESS_CLAMP    ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE
                                         : stage.address == GFX_ADDRESS_MIRROR ? SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT
                                                                               : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        info.address_mode_u = info.address_mode_v = info.address_mode_w = mode;
        info.min_lod = 0.0f;
        info.max_lod = stage.mipFilter == GFX_MIPFILTER_NONE ? 0.0f : 1000.0f;
        if (aniso > 1 && stage.mipFilter != GFX_MIPFILTER_NONE) {
            info.enable_anisotropy = true;
            info.max_anisotropy = (float)aniso;
        }
        SDL_GPUSampler *s = SDL_CreateGPUSampler(device, &info);
        samplers[key] = s;
        return s;
    }

    // ---- uniforms ----

    // Whether two states give the same VertexUniforms (FillVertexUniforms).
    static bool SameVertexUniforms(const GfxDrawState &a, const GfxDrawState &b)
    {
        for (int i = 0; i < 2; i++)
            if (a.stages[i].texCoordIndex != b.stages[i].texCoordIndex ||
                a.stages[i].textureTransform != b.stages[i].textureTransform)
                return false;
        return a.viewport.width == b.viewport.width && a.viewport.height == b.viewport.height &&
               a.screenW == b.screenW && a.screenH == b.screenH && a.lighting == b.lighting && a.colorVertex == b.colorVertex && a.specular == b.specular &&
               a.localViewer == b.localViewer && a.normalizeNormals == b.normalizeNormals &&
               a.diffuseSource == b.diffuseSource && a.specularSource == b.specularSource &&
               a.ambientSource == b.ambientSource && a.emissiveSource == b.emissiveSource && a.fog == b.fog &&
               a.fogTableMode == b.fogTableMode && a.fogVertexMode == b.fogVertexMode &&
               a.fogStart == b.fogStart && a.fogEnd == b.fogEnd && a.ambient == b.ambient &&
               memcmp(&a.world, &b.world, sizeof(a.world)) == 0 && memcmp(&a.view, &b.view, sizeof(a.view)) == 0 &&
               memcmp(&a.projection, &b.projection, sizeof(a.projection)) == 0 &&
               memcmp(a.texture, b.texture, sizeof(a.texture)) == 0 &&
               memcmp(&a.material, &b.material, sizeof(a.material)) == 0 &&
               memcmp(a.lightEnabled, b.lightEnabled, sizeof(a.lightEnabled)) == 0 &&
               memcmp(a.lights, b.lights, sizeof(a.lights)) == 0;
    }

    // A rectangle of the screen the game draws in (Gfx_SetVirtualScreen) in
    // the back buffer's pixels, within it; elsewhere as it is.
    static GfxViewport ToTarget(const GfxViewport &v, int screenW, int screenH, bool backBuffer, int targetW,
                                int targetH)
    {
        if (!backBuffer || screenW <= 0 || screenH <= 0)
            return v;
        float kx = (float)targetW / screenW, ky = (float)targetH / screenH;
        int x0 = std::clamp((int)lroundf(v.x * kx), 0, targetW);
        int y0 = std::clamp((int)lroundf(v.y * ky), 0, targetH);
        int x1 = std::clamp((int)lroundf((v.x + v.width) * kx), x0, targetW);
        int y1 = std::clamp((int)lroundf((v.y + v.height) * ky), y0, targetH);
        GfxViewport r = v;
        r.x = x0;
        r.y = y0;
        r.width = x1 - x0;
        r.height = y1 - y0;
        return r;
    }

    void FillVertexUniforms(const GfxCommand &cmd, const GfxDrawState &s, int targetW, int targetH,
                            const GfxViewport &viewport, bool scaled, VertexUniforms &u)
    {
        memset(&u, 0, sizeof(u));
        Multiply(s.world, s.view, u.worldView);
        memcpy(u.projection, &s.projection, sizeof(u.projection));
        memcpy(u.textureMatrix[0], &s.texture[0], 64);
        memcpy(u.textureMatrix[1], &s.texture[1], 64);
        Copy4(s.material.diffuse, u.materialDiffuse);
        Copy4(s.material.ambient, u.materialAmbient);
        Copy4(s.material.specular, u.materialSpecular);
        Copy4(s.material.emissive, u.materialEmissive);
        Color(s.ambient, u.globalAmbient);
        u.globalAmbient[3] = s.material.power;
        for (int i = 0; i < 8; i++) {
            const GfxLight &l = s.lights[i];
            LightUniform &lu = u.lights[i];
            Copy4(l.diffuse, lu.diffuse);
            Copy4(l.specular, lu.specular);
            Copy4(l.ambient, lu.ambient);
            TransformPoint(l.position, 1.0f, s.view, lu.position);
            lu.position[3] = (float)l.type;
            TransformPoint(l.direction, 0.0f, s.view, lu.direction);
            lu.direction[3] = l.range > 0.0f ? l.range : 1e30f;
            lu.attenuation[0] = l.attenuation0;
            lu.attenuation[1] = l.attenuation1;
            lu.attenuation[2] = l.attenuation2;
            lu.attenuation[3] = l.falloff;
            lu.spot[0] = cosf(l.theta * 0.5f);
            lu.spot[1] = cosf(l.phi * 0.5f);
            lu.spot[2] = s.lightEnabled[i] ? 1.0f : 0.0f;
        }
        u.lightFlags[0] = s.lighting;
        u.lightFlags[1] = s.colorVertex;
        u.lightFlags[2] = s.specular;
        u.lightFlags[3] = s.localViewer;
        u.sources[0] = s.diffuseSource;
        u.sources[1] = s.specularSource;
        u.sources[2] = s.ambientSource;
        u.sources[3] = s.emissiveSource;
        u.fogFlags[0] = (s.fog && s.fogTableMode == GFX_FOG_NONE) ? s.fogVertexMode : 0;
        u.fogFlags[1] = s.normalizeNormals;
        // Direct3D takes the fog factor from the vertex's specular alpha when
        // fog is on with neither vertex nor table fog: the backdrop of a
        // foggy stage is drawn so, its vertices' alpha 0 filling it with fog.
        u.fogFlags[2] = s.fog && s.fogTableMode == GFX_FOG_NONE && s.fogVertexMode == GFX_FOG_NONE;
        u.fogParams[0] = s.fogStart;
        u.fogParams[1] = s.fogEnd;
        u.texCoordIndex[0] = (int32_t)s.stages[0].texCoordIndex;
        u.texCoordIndex[1] = (int32_t)s.stages[1].texCoordIndex;
        u.texCoordIndex[2] = s.stages[0].textureTransform;
        u.texCoordIndex[3] = s.stages[1].textureTransform;
        // Pre-transformed vertices are in pixels of the game's screen, scaled
        // to the back buffer's.
        u.target[0] = (float)(scaled ? s.screenW : targetW);
        u.target[1] = (float)(scaled ? s.screenH : targetH);
        // Direct3D 7's half-pixel offset of the viewport transform.
        u.target[2] = viewport.width ? 1.0f / viewport.width : 0.0f;
        u.target[3] = viewport.height ? -1.0f / viewport.height : 0.0f;
        (void)cmd;
    }

    int TextureKind(const GfxStageState &stage)
    {
        GfxTexture *t = Gfx_DrawnTexture(stage.texture, Platform_IsVisualOverhaulEnabled());
        if (t == NULL)
            return 0;
        if (t->flags & GFX_TEXTURE_CUBE)
            return 2;
        if (t->format == GFX_FORMAT_BUMP_DUDVL)
            return 3;
        if (t->flags & GFX_TEXTURE_DISTANCE_FIELD)
            return 4;
        if (t->flags & GFX_TEXTURE_MENU_DOT)
            return 5;
        if (t->flags & GFX_TEXTURE_VECTOR_ICON)
            return 6;
        return 1;
    }

    // A 2D texture drawn with point filtering and no mipmaps (the game's
    // sprites, text and HUD). Scaled, nearest texels would come out one or
    // two pixels wide, or be skipped; the shader filters them smoothly
    // through a linear sampler. At their size it reads them as the point
    // filter does.
    bool ScaledPoint(const GfxStageState &stage)
    {
        int kind = TextureKind(stage);
        return (kind == 1 || kind == 6) && stage.magFilter == GFX_FILTER_POINT && stage.mipFilter == GFX_MIPFILTER_NONE;
    }

    void FillFragmentLightingUniforms(const GfxDrawState &s, const VertexUniforms &vu, bool overhaul,
                                      bool perPixel, bool hemisphere, float fresnelStrength, bool shadowActive,
                                      const ShadowSetup &shadowSetup, float shadowStrength, int pcfQuality,
                                      FragmentLightingUniforms &fl)
    {
        memset(&fl, 0, sizeof(fl));
        memcpy(fl.textureMatrix, vu.textureMatrix, sizeof(fl.textureMatrix));
        memcpy(fl.materialDiffuse, vu.materialDiffuse, sizeof(fl.materialDiffuse));
        memcpy(fl.materialAmbient, vu.materialAmbient, sizeof(fl.materialAmbient));
        memcpy(fl.materialSpecular, vu.materialSpecular, sizeof(fl.materialSpecular));
        memcpy(fl.materialEmissive, vu.materialEmissive, sizeof(fl.materialEmissive));
        memcpy(fl.globalAmbient, vu.globalAmbient, sizeof(fl.globalAmbient));
        memcpy(fl.lights, vu.lights, sizeof(fl.lights));
        memcpy(fl.lightFlags, vu.lightFlags, sizeof(fl.lightFlags));
        memcpy(fl.sources, vu.sources, sizeof(fl.sources));
        memcpy(fl.texCoordIndex, vu.texCoordIndex, sizeof(fl.texCoordIndex));
        GfxVector worldUp = { 0.0f, 1.0f, 0.0f };
        TransformPoint(worldUp, 0.0f, s.view, fl.viewUp);
        fl.viewUp[3] = 0.0f;
        fl.enhanceParams[0] = perPixel ? 1.0f : 0.0f;
        fl.enhanceParams[1] = hemisphere ? 1.0f : 0.0f;
        fl.enhanceParams[2] = fresnelStrength;
        fl.enhanceParams[3] = overhaul ? 1.0f : 0.0f;
        if (shadowActive && shadowMapSize > 0 && shadowSetup.count > 0) {
            GfxMatrix invView;
            if (InvertAffineMatrix(s.view, invView)) {
                Multiply(invView, shadowSetup.cascades[0].lightVP, fl.eyeToLightClip[0]);
                Multiply(invView, shadowSetup.cascades[shadowSetup.count > 1 ? 1 : 0].lightVP, fl.eyeToLightClip[1]);
                fl.shadowParams[0] = 1.0f;
                fl.shadowParams[1] = shadowStrength;
                fl.shadowParams[2] = 1.0f / (float)shadowMapSize;
                fl.shadowParams[3] = (float)shadowSetup.count;
                fl.shadowParams2[0] = (float)std::clamp(pcfQuality, 0, 2);
                fl.shadowParams2[1] = shadowSetup.count > 1 ? 1.75f : 1.50f;
                fl.shadowParams2[2] = 1.35f;
                fl.shadowParams2[3] = 0.0f;
                TransformPoint(shadowSetup.toSunWorld, 0.0f, s.view, fl.sunDirEye);
                float slen = sqrtf(fl.sunDirEye[0] * fl.sunDirEye[0] + fl.sunDirEye[1] * fl.sunDirEye[1] +
                                   fl.sunDirEye[2] * fl.sunDirEye[2]);
                if (slen > 1e-5f) {
                    fl.sunDirEye[0] /= slen;
                    fl.sunDirEye[1] /= slen;
                    fl.sunDirEye[2] /= slen;
                }
            }
        }
    }

    // fade: the draw is a 3D one into the back buffer, which the distance
    // fade applies to.
    void FillFragmentUniforms(const GfxDrawState &s, bool fade, bool hdr, bool materialMaps, bool receiveShadows,
                              FragmentUniforms &u)
    {
        memset(&u, 0, sizeof(u));
        const GfxStageState &a = s.stages[0], &b = s.stages[1];
        u.stage0[0] = a.colorOp;
        u.stage0[1] = a.colorArg1;
        u.stage0[2] = a.colorArg2;
        u.stage0[3] = a.alphaOp;
        u.stage0b[0] = a.alphaArg1;
        u.stage0b[1] = a.alphaArg2;
        u.stage0b[2] = TextureKind(a);
        u.stage0b[3] = ScaledPoint(a);
        u.stage1[0] = b.colorOp;
        u.stage1[1] = b.colorArg1;
        u.stage1[2] = b.colorArg2;
        u.stage1[3] = b.alphaOp;
        u.stage1b[0] = b.alphaArg1;
        u.stage1b[1] = b.alphaArg2;
        u.stage1b[2] = TextureKind(b);
        u.stage1b[3] = ScaledPoint(b);
        Color(s.textureFactor, u.textureFactor);
        u.alphaTest[0] = s.alphaTest;
        u.alphaTest[1] = s.alphaFunc;
        GfxTexture *root0 = a.texture != NULL && a.texture->cube != NULL ? a.texture->cube : a.texture;
        u.alphaTest[2] = (materialMaps && root0 != NULL && root0->normalMap != NULL) ? 1 : 0;
        u.alphaTest[3] = (materialMaps && root0 != NULL && root0->specularMap != NULL) ? 1 : 0;
        u.alphaRef[0] = (s.alphaRef & 0xff) / 255.0f;
        Color(s.fogColor, u.fogColor);
        u.fogMode[0] = s.fog;
        u.fogMode[1] = s.fogTableMode;
        u.fogMode[2] = s.specular;
        u.fogMode[3] = hdr ? 1 : 0;
        u.fogParams[0] = s.fogStart;
        u.fogParams[1] = s.fogEnd;
        u.fogParams[2] = receiveShadows ? 1.0f : 0.0f;
        memcpy(u.bumpMatrix, a.bumpMat, sizeof(u.bumpMatrix));
        u.bumpLuminance[0] = a.bumpLumScale;
        u.bumpLuminance[1] = a.bumpLumOffset;
        if (fade && s.fadeEnd > s.fadeStart) {
            u.distanceFade[0] = s.fadeStart;
            u.distanceFade[1] = s.fadeEnd;
            u.distanceFade[2] = 1.0f;
        }
        u.distanceFade[3] = 0.85f;
    }

    // The stages' textures (2D and cube for each), backdrop, native icons, material maps, and shadow map.
    void TextureBindings(const GfxDrawState &s, bool is3D, bool hdr, bool materialMaps,
                         SDL_GPUTextureSamplerBinding bindings[10])
    {
        bool overhaul = Platform_IsVisualOverhaulEnabled();
        SDL_GPUSampler *stage0Sampler = NULL;
        for (int i = 0; i < 2; i++) {
            const GfxStageState &stage = s.stages[i];
            GfxTexture *t = Gfx_DrawnTexture(stage.texture, overhaul);
            GpuTexture *g = t != NULL ? Ensure(t) : NULL;
            GfxStageState sampled = stage;
            int kind = TextureKind(stage);
            if (ScaledPoint(stage) || kind == 5 || kind == 6)
                sampled.minFilter = sampled.magFilter = GFX_FILTER_LINEAR;
            // A distance field is read between its texels, and from its mip
            // levels where it is drawn smaller.
            if (kind == 4) {
                sampled.minFilter = sampled.magFilter = GFX_FILTER_LINEAR;
                sampled.mipFilter = GFX_MIPFILTER_LINEAR;
            }
            if (is3D && kind == 1 && g != NULL && g->mipLevels > 1) {
                if (overhaul) {
                    sampled.minFilter = sampled.magFilter = GFX_FILTER_LINEAR;
                    sampled.mipFilter = GFX_MIPFILTER_LINEAR;
                } else if (g->generatedMips) {
                    sampled.mipFilter = GFX_MIPFILTER_NONE;
                }
            }
            SDL_GPUSampler *sampler = Sampler(sampled);
            if (i == 0)
                stage0Sampler = sampler;
            bool cube = t != NULL && (t->flags & GFX_TEXTURE_CUBE);
            SDL_GPUTexture *texture = g != NULL ? g->texture : NULL;
            bindings[i * 2] = { (texture != NULL && !cube) ? texture : white2D, sampler };
            bindings[i * 2 + 1] = { (texture != NULL && cube) ? texture : whiteCube, sampler };
            GpuTexture *native = t != NULL && (t->flags & GFX_TEXTURE_VECTOR_ICON) ? Ensure(stage.texture) : NULL;
            GfxStageState source = stage;
            if (ScaledPoint(stage)) source.minFilter = source.magFilter = GFX_FILTER_LINEAR;
            bindings[5 + i] = { native != NULL ? native->texture : white2D, Sampler(source) };
        }
        bindings[4] = { (hdr && hdrBackdrop != NULL) ? hdrBackdrop : backdrop, Sampler(GfxStageState()) };
        GfxTexture *root0 = s.stages[0].texture != NULL && s.stages[0].texture->cube != NULL
                                ? s.stages[0].texture->cube
                                : s.stages[0].texture;
        GpuTexture *normGpu = (materialMaps && root0 != NULL && root0->normalMap != NULL) ? Ensure(root0->normalMap)
                                                                                           : NULL;
        GpuTexture *specGpu = (materialMaps && root0 != NULL && root0->specularMap != NULL)
                                  ? Ensure(root0->specularMap)
                                  : NULL;
        SDL_GPUSampler *matSampler = stage0Sampler != NULL ? stage0Sampler : Sampler(GfxStageState());
        bindings[7] = { (normGpu != NULL && normGpu->texture != NULL) ? normGpu->texture : flatNormal2D, matSampler };
        bindings[8] = { (specGpu != NULL && specGpu->texture != NULL) ? specGpu->texture : white2D, matSampler };
        bindings[9] = { shadowMap != NULL ? shadowMap : white2D,
                        shadowSampler != NULL ? shadowSampler : Sampler(GfxStageState()) };
    }

    // What a render pass has bound: each draw sets only what differs from the
    // previous one. Render passes start with nothing bound.
    struct PassBindings {
        SDL_GPUGraphicsPipeline *pipeline = nullptr;
        bool viewportSet = false, texturesSet = false, indicesSet = false;
        SDL_GPUViewport viewport = {};
        SDL_Rect scissor = {};
        SDL_GPUTextureSamplerBinding textures[10] = {};
        SDL_GPUBuffer *vertices = nullptr;
    };

    void SetViewport(SDL_GPURenderPass *pass, PassBindings &bound, const SDL_GPUViewport &vp, const SDL_Rect &scissor)
    {
        if (bound.viewportSet && memcmp(&bound.viewport, &vp, sizeof(vp)) == 0 &&
            memcmp(&bound.scissor, &scissor, sizeof(scissor)) == 0)
            return;
        SDL_SetGPUViewport(pass, &vp);
        SDL_SetGPUScissor(pass, &scissor);
        bound.viewport = vp;
        bound.scissor = scissor;
        bound.viewportSet = true;
    }

    // ---- frame execution ----

    void EnsureBuffers(uint32_t vertexBytes, uint32_t indexBytes)
    {
        if (vertexBytes > vertexCapacity) {
            SDL_ReleaseGPUBuffer(device, vertexBuffer);
            vertexCapacity = std::max(vertexBytes, vertexCapacity * 2);
            SDL_GPUBufferCreateInfo info = { SDL_GPU_BUFFERUSAGE_VERTEX, vertexCapacity, 0 };
            vertexBuffer = SDL_CreateGPUBuffer(device, &info);
        }
        if (indexBytes > indexCapacity) {
            SDL_ReleaseGPUBuffer(device, indexBuffer);
            indexCapacity = std::max(indexBytes, indexCapacity * 2);
            SDL_GPUBufferCreateInfo info = { SDL_GPU_BUFFERUSAGE_INDEX, indexCapacity, 0 };
            indexBuffer = SDL_CreateGPUBuffer(device, &info);
        }
    }

    void UploadGeometry(SDL_GPUCommandBuffer *cmd, GfxFrame &frame)
    {
        uint32_t vertexBytes = (uint32_t)frame.vertices.size();
        uint32_t indexBytes = (uint32_t)(frame.indices.size() * 2);
        if (vertexBytes == 0 && indexBytes == 0)
            return;
        EnsureBuffers(std::max(vertexBytes, 4u), std::max(indexBytes, 4u));
        if (vertexBytes + indexBytes > uploadCapacity) {
            SDL_ReleaseGPUTransferBuffer(device, uploadBuffer);
            uploadCapacity = std::max(vertexBytes + indexBytes, uploadCapacity * 2);
            SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, uploadCapacity, 0 };
            uploadBuffer = SDL_CreateGPUTransferBuffer(device, &tbi);
        }
        SDL_GPUTransferBuffer *tb = uploadBuffer;
        // Cycled: a fresh area while the GPU still reads an earlier frame's.
        uint8_t *map = (uint8_t *)SDL_MapGPUTransferBuffer(device, tb, true);
        memcpy(map, frame.vertices.data(), vertexBytes);
        if (indexBytes)
            memcpy(map + vertexBytes, frame.indices.data(), indexBytes);
        SDL_UnmapGPUTransferBuffer(device, tb);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        if (vertexBytes) {
            SDL_GPUTransferBufferLocation src = { tb, 0 };
            SDL_GPUBufferRegion dst = { vertexBuffer, 0, vertexBytes };
            SDL_UploadToGPUBuffer(copy, &src, &dst, true);
        }
        if (indexBytes) {
            SDL_GPUTransferBufferLocation isrc = { tb, vertexBytes };
            SDL_GPUBufferRegion idst = { indexBuffer, 0, indexBytes };
            SDL_UploadToGPUBuffer(copy, &isrc, &idst, true);
        }
        SDL_EndGPUCopyPass(copy);
    }

    // The GPU copies of the vertex buffers the frame draws from, where the
    // game has locked them since.
    void UploadVertexBuffers(SDL_GPUCommandBuffer *cmd, GfxFrame &frame)
    {
        SDL_GPUCopyPass *copy = NULL;
        for (GfxVertexBuffer *b : frame.buffers) {
            GpuVertexBuffer *g = (GpuVertexBuffer *)b->gpu;
            if (g == NULL)
                b->gpu = g = new GpuVertexBuffer;
            uint32_t bytes = (uint32_t)(b->vertices.size() * sizeof(GfxVertex));
            if ((g->version == b->version && g->buffer != NULL) || bytes == 0)
                continue;
            if (bytes > g->capacity) {
                SDL_ReleaseGPUBuffer(device, g->buffer);
                SDL_GPUBufferCreateInfo info = { SDL_GPU_BUFFERUSAGE_VERTEX, bytes, 0 };
                g->buffer = SDL_CreateGPUBuffer(device, &info);
                g->capacity = bytes;
            }
            SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0 };
            SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device, &tbi);
            memcpy(SDL_MapGPUTransferBuffer(device, tb, false), b->vertices.data(), bytes);
            SDL_UnmapGPUTransferBuffer(device, tb);
            if (copy == NULL)
                copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTransferBufferLocation src = { tb, 0 };
            SDL_GPUBufferRegion dst = { g->buffer, 0, bytes };
            // Cycling keeps the contents earlier frames still draw from.
            SDL_UploadToGPUBuffer(copy, &src, &dst, true);
            SDL_ReleaseGPUTransferBuffer(device, tb);
            g->version = b->version;
        }
        if (copy != NULL)
            SDL_EndGPUCopyPass(copy);
    }

    void DestroyVertexBuffer(GfxVertexBuffer *b) override
    {
        for (auto it = staticBoundsCache.begin(); it != staticBoundsCache.end(); ) {
            if (it->first.buffer == b)
                it = staticBoundsCache.erase(it);
            else
                ++it;
        }
        GpuVertexBuffer *g = (GpuVertexBuffer *)b->gpu;
        if (g == NULL)
            return;
        if (device != NULL)
            SDL_ReleaseGPUBuffer(device, g->buffer);
        delete g;
        b->gpu = NULL;
    }

    // Begins a pass on the target at commands[first]. Clears of the whole
    // target there, before any draw, become the pass's load operations (the
    // GPU's fast clears) instead of drawn quads; *cleared counts them.
    SDL_GPURenderPass *BeginPass(SDL_GPUCommandBuffer *cmd, GfxTexture *target, SDL_GPUSampleCount passSamples,
                                 bool hdrTarget, bool mrtDepth, bool &hdrDepthCleared, int &w, int &h,
                                 const std::vector<GfxCommand> &commands, size_t first, size_t *cleared)
    {
        SDL_GPUColorTargetInfo colors[2] = {};
        SDL_GPUColorTargetInfo &color = colors[0];
        SDL_GPUDepthStencilTargetInfo depth = {};
        color.load_op = SDL_GPU_LOADOP_LOAD;
        color.store_op = SDL_GPU_STOREOP_STORE;
        depth.load_op = SDL_GPU_LOADOP_LOAD;
        depth.store_op = SDL_GPU_STOREOP_STORE;
        // The stencil is kept too: with a combined depth-stencil format a
        // discarded stencil may take the depth with it on some tilers.
        depth.stencil_load_op = SDL_GPU_LOADOP_LOAD;
        depth.stencil_store_op = SDL_GPU_STOREOP_STORE;
        Uint32 numColorTargets = 1;
        if (target == NULL) {
            if (hdrTarget && hdrColor != NULL) {
                if (passSamples != SDL_GPU_SAMPLECOUNT_1 && hdrColorMsaa != NULL && backDepthMsaa != NULL) {
                    color.texture = hdrColorMsaa;
                    color.resolve_texture = hdrColor;
                    color.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
                    depth.texture = backDepthMsaa;
                } else {
                    color.texture = hdrColor;
                    depth.texture = backDepth;
                }
                if (mrtDepth && hdrDepth != NULL) {
                    SDL_GPUColorTargetInfo &dcol = colors[1];
                    if (passSamples != SDL_GPU_SAMPLECOUNT_1 && hdrDepthMsaa != NULL) {
                        dcol.texture = hdrDepthMsaa;
                        dcol.resolve_texture = hdrDepth;
                        dcol.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
                    } else {
                        dcol.texture = hdrDepth;
                        dcol.store_op = SDL_GPU_STOREOP_STORE;
                    }
                    dcol.load_op = hdrDepthCleared ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
                    dcol.clear_color = { 10000.0f, 0.0f, 0.0f, 1.0f };
                    numColorTargets = 2;
                }
            } else if (passSamples != SDL_GPU_SAMPLECOUNT_1 && backColorMsaa != NULL) {
                color.texture = backColorMsaa;
                color.resolve_texture = backColor;
                color.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
                depth.texture = backDepthMsaa;
            } else {
                color.texture = backColor;
                depth.texture = backDepth;
            }
            w = backWidth;
            h = backHeight;
        } else {
            GfxTexture *root = target->cube != NULL ? target->cube : target;
            GpuTexture *g = Ensure(root);
            // Face 0 of a cube is the cube itself.
            int face = target->cube != NULL ? target->face : 0;
            if (g->faces[face] != NULL) {
                color.texture = g->faces[face];
            } else {
                color.texture = g->texture;
                color.layer_or_depth_plane = target->cube != NULL ? target->face : 0;
            }
            depth.texture = g->depth;
            w = root->width;
            h = root->height;
        }
        *cleared = 0;
        for (size_t i = first; i < commands.size() && commands[i].type == GFX_CMD_CLEAR; i++) {
            const GfxCommand &c = commands[i];
            GfxViewport v = ToTarget(c.clearViewport, c.clearScreenW, c.clearScreenH, target == NULL, w, h);
            if (v.x != 0 || v.y != 0 || v.width != (DWORD)w || v.height != (DWORD)h)
                break;
            if (c.clearFlags & GFX_CLEAR_TARGET) {
                float rgba[4];
                Color(c.clearColor, rgba);
                color.load_op = SDL_GPU_LOADOP_CLEAR;
                color.clear_color = { rgba[0], rgba[1], rgba[2], rgba[3] };
            }
            if (c.clearFlags & GFX_CLEAR_ZBUFFER) {
                depth.load_op = SDL_GPU_LOADOP_CLEAR;
                depth.clear_depth = c.clearZ;
                if (numColorTargets == 2) {
                    colors[1].load_op = SDL_GPU_LOADOP_CLEAR;
                    colors[1].clear_color = { 10000.0f, 0.0f, 0.0f, 1.0f };
                }
            }
            (*cleared)++;
        }
        if (numColorTargets == 2)
            hdrDepthCleared = true;
        return SDL_BeginGPURenderPass(cmd, colors, numColorTargets, &depth);
    }

    // Ends the render pass on target; a cube face's goes on into the cube.
    void EndPass(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, GfxTexture *target)
    {
        SDL_EndGPURenderPass(pass);
        if (target == NULL)
            return;
        // Face 0 of a cube is the cube itself.
        GfxTexture *root = target->cube != NULL ? target->cube : target;
        int face = target->cube != NULL ? target->face : 0;
        GpuTexture *g = (GpuTexture *)root->gpu;
        if (g == NULL || g->faces[face] == NULL || g->texture == NULL)
            return;
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureLocation src = {}, dst = {};
        src.texture = g->faces[face];
        dst.texture = g->texture;
        dst.layer = (Uint32)face;
        SDL_CopyGPUTextureToTexture(copy, &src, &dst, root->width, root->height, 1, false);
        SDL_EndGPUCopyPass(copy);
    }

    void DrawPostPass(SDL_GPUCommandBuffer *cmd, SDL_GPUGraphicsPipeline *pipeline, SDL_GPUTexture *dst, int w, int h,
                      const SDL_GPUTextureSamplerBinding *bindings, int numBindings, const void *uData, size_t uSize)
    {
        SDL_GPUColorTargetInfo color = {};
        color.texture = dst;
        color.load_op = SDL_GPU_LOADOP_DONT_CARE;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color, 1, NULL);
        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        SDL_GPUViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
        SDL_Rect scissor = { 0, 0, w, h };
        SDL_SetGPUViewport(pass, &vp);
        SDL_SetGPUScissor(pass, &scissor);
        SDL_PushGPUFragmentUniformData(cmd, 0, uData, (Uint32)uSize);
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, numBindings);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }

    void RunPostProcess(SDL_GPUCommandBuffer *cmd, bool toneMap, bool bloomOn, float bloomIntensity, bool ssaoActive,
                        float ssaoStrength, bool casOn, float casStrength, const GfxMatrix &mainProj, bool hasMainProj)
    {
        bool ssaoRan = false;
        if (ssaoActive && ssaoStrength > 0.001f && hasMainProj && hdrDepth != NULL && aoHalfA != NULL &&
            aoHalfB != NULL && postGtaoPipeline != NULL && postAoBlurPipeline != NULL &&
            fabsf(mainProj._11) > 1e-4f && fabsf(mainProj._22) > 1e-4f) {
            SDL_GPUTextureSamplerBinding depthBind = { hdrDepth, depthSampler };
            PostUniforms2 gtaoU = {
                { 1.0f / mainProj._11, 1.0f / mainProj._22, mainProj._22 * (float)bloomHalfH * 0.5f, 0.85f },
                { 1.0f / (float)bloomHalfW, 1.0f / (float)bloomHalfH, 1.0f / (float)backWidth, 1.0f / (float)backHeight },
            };
            DrawPostPass(cmd, postGtaoPipeline, aoHalfA, bloomHalfW, bloomHalfH, &depthBind, 1, &gtaoU, sizeof(gtaoU));

            SDL_GPUTextureSamplerBinding aoA = { aoHalfA, depthSampler };
            PostUniforms blurAoH = { { 1.0f / (float)bloomHalfW, 0.0f, 0.0f, 0.0f } };
            DrawPostPass(cmd, postAoBlurPipeline, aoHalfB, bloomHalfW, bloomHalfH, &aoA, 1, &blurAoH, sizeof(blurAoH));

            SDL_GPUTextureSamplerBinding aoB = { aoHalfB, depthSampler };
            PostUniforms blurAoV = { { 0.0f, 1.0f / (float)bloomHalfH, 0.0f, 0.0f } };
            DrawPostPass(cmd, postAoBlurPipeline, aoHalfA, bloomHalfW, bloomHalfH, &aoB, 1, &blurAoV, sizeof(blurAoV));
            ssaoRan = true;
        }

        if (bloomOn && bloomIntensity > 0.0f) {
            SDL_GPUTextureSamplerBinding src = { hdrColor, postSampler };
            PostUniforms brightU = { { 0.92f, 0.35f, 4.0f, 0.0f } };
            DrawPostPass(cmd, postBrightPipeline, bloomHalfA, bloomHalfW, bloomHalfH, &src, 1, &brightU, sizeof(brightU));

            SDL_GPUTextureSamplerBinding halfA = { bloomHalfA, postSampler };
            PostUniforms blurHalfH = { { 1.0f / (float)bloomHalfW, 0.0f, 0.0f, 0.0f } };
            DrawPostPass(cmd, postBlurPipeline, bloomHalfB, bloomHalfW, bloomHalfH, &halfA, 1, &blurHalfH,
                         sizeof(blurHalfH));

            SDL_GPUTextureSamplerBinding halfB = { bloomHalfB, postSampler };
            PostUniforms blurHalfV = { { 0.0f, 1.0f / (float)bloomHalfH, 0.0f, 0.0f } };
            DrawPostPass(cmd, postBlurPipeline, bloomHalfA, bloomHalfW, bloomHalfH, &halfB, 1, &blurHalfV,
                         sizeof(blurHalfV));

            // Downsample + blur to quarter resolution for wider, softer glow.
            PostUniforms blurQtrH1 = { { 1.0f / (float)bloomHalfW, 0.0f, 0.0f, 0.0f } };
            DrawPostPass(cmd, postBlurPipeline, bloomQuarterB, bloomQuarterW, bloomQuarterH, &halfA, 1, &blurQtrH1,
                         sizeof(blurQtrH1));

            SDL_GPUTextureSamplerBinding qtrB = { bloomQuarterB, postSampler };
            PostUniforms blurQtrV1 = { { 0.0f, 1.0f / (float)bloomQuarterH, 0.0f, 0.0f } };
            DrawPostPass(cmd, postBlurPipeline, bloomQuarterA, bloomQuarterW, bloomQuarterH, &qtrB, 1, &blurQtrV1,
                         sizeof(blurQtrV1));

            SDL_GPUTextureSamplerBinding qtrA = { bloomQuarterA, postSampler };
            PostUniforms blurQtrH2 = { { 1.25f / (float)bloomQuarterW, 0.0f, 0.0f, 0.0f } };
            DrawPostPass(cmd, postBlurPipeline, bloomQuarterB, bloomQuarterW, bloomQuarterH, &qtrA, 1, &blurQtrH2,
                         sizeof(blurQtrH2));

            PostUniforms blurQtrV2 = { { 0.0f, 1.25f / (float)bloomQuarterH, 0.0f, 0.0f } };
            DrawPostPass(cmd, postBlurPipeline, bloomQuarterA, bloomQuarterW, bloomQuarterH, &qtrB, 1, &blurQtrV2,
                         sizeof(blurQtrV2));
        }

        SDL_GPUTextureSamplerBinding compBindings[4] = {
            { hdrColor, postSampler },
            { (bloomOn && bloomIntensity > 0.0f) ? bloomHalfA : white2D, postSampler },
            { (bloomOn && bloomIntensity > 0.0f) ? bloomQuarterA : white2D, postSampler },
            { ssaoRan ? aoHalfA : white2D, postSampler },
        };
        PostUniforms2 compU = {
            { toneMap ? 1.0f : 0.0f, bloomOn ? bloomIntensity : 0.0f, 0.92f, ssaoRan ? ssaoStrength : 0.0f },
            { casOn ? 1.0f : 0.0f, casOn ? casStrength : 0.0f, 1.0f / (float)backWidth, 1.0f / (float)backHeight },
        };
        DrawPostPass(cmd, postCompositePipeline, backColor, backWidth, backHeight, compBindings, 4, &compU,
                     sizeof(compU));
    }

    CachedSphere GetDrawWorldBounds(const GfxFrame &frame, const GfxCommand &c, const GfxDrawState &s)
    {
        CachedSphere local = {};
        if (c.buffer != NULL) {
            DrawBoundsKey key = { c.buffer, c.firstVertexByte, c.vertexCount };
            auto it = staticBoundsCache.find(key);
            if (it != staticBoundsCache.end() && it->second.version == c.buffer->version) {
                local = it->second;
            } else {
                uint32_t firstVert = c.firstVertexByte / sizeof(GfxVertex);
                if (firstVert + c.vertexCount <= c.buffer->vertices.size()) {
                    local = ComputeVertexSphere(c.buffer->vertices.data() + firstVert, c.vertexCount, 1, 0.5f);
                }
                local.version = c.buffer->version;
                staticBoundsCache[key] = local;
            }
        } else {
            size_t availBytes = frame.vertices.size();
            size_t endByte = (size_t)c.firstVertexByte + (size_t)c.vertexCount * sizeof(GfxVertex);
            if (endByte <= availBytes && c.vertexCount > 0) {
                const GfxVertex *verts = (const GfxVertex *)(frame.vertices.data() + c.firstVertexByte);
                uint32_t stride = c.vertexCount <= 96 ? 1u : std::max(1u, c.vertexCount / 48u);
                float pad = stride > 1u ? 3.5f : 0.5f;
                local = ComputeVertexSphere(verts, c.vertexCount, stride, pad);
            }
        }
        CachedSphere world = {};
        float wc[3];
        TransformPoint(local.center, 1.0f, s.world, wc);
        world.center = { wc[0], wc[1], wc[2] };
        const GfxMatrix &m = s.world;
        float sx = sqrtf(m._11 * m._11 + m._12 * m._12 + m._13 * m._13);
        float sy = sqrtf(m._21 * m._21 + m._22 * m._22 + m._23 * m._23);
        float sz = sqrtf(m._31 * m._31 + m._32 * m._32 + m._33 * m._33);
        float maxScale = std::max(sx, std::max(sy, sz));
        world.radius = local.radius * (maxScale > 1e-4f ? maxScale : 1.0f);
        return world;
    }

    void RenderShadowPass(SDL_GPUCommandBuffer *cmd, const GfxFrame &frame, size_t backdropIndex,
                          const ShadowSetup &setup, bool overhaul)
    {
        if (shadowMap == NULL || shadowMapSize <= 0 || setup.count <= 0)
            return;

        // Gather shadow-casting 3D draws + their world bounding spheres once for all cascades.
        struct ShadowCaster {
            size_t cmdIndex;
            GfxVector center;
            float radius;
        };
        std::vector<ShadowCaster> casters;
        casters.reserve(256);
        GfxTexture *scanTarget = NULL;
        for (size_t i = 0; i < frame.commands.size(); i++) {
            const GfxCommand &c = frame.commands[i];
            if (c.type == GFX_CMD_TARGET) {
                scanTarget = c.target;
                continue;
            }
            if (scanTarget != NULL || c.type != GFX_CMD_DRAW || c.transformed)
                continue;
            if (backdropIndex != SIZE_MAX && i <= backdropIndex)
                continue;
            const GfxDrawState &s = frame.states[c.state];
            if (!s.depthTest || !s.depthWrite)
                continue;
            if (s.alphaBlend && !s.alphaTest)
                continue;
            CachedSphere wb = GetDrawWorldBounds(frame, c, s);
            casters.push_back({ i, wb.center, wb.radius });
        }

        // Single render pass over the 2x1 CSM atlas (zero pass-switch overhead on Apple Silicon TBDR).
        SDL_GPUDepthStencilTargetInfo depth = {};
        depth.texture = shadowMap;
        depth.load_op = SDL_GPU_LOADOP_CLEAR;
        depth.store_op = SDL_GPU_STOREOP_STORE;
        depth.clear_depth = 1.0f;
        SDL_GPURenderPass *spass = SDL_BeginGPURenderPass(cmd, NULL, 0, &depth);
        if (spass == NULL)
            return;

        SDL_GPUGraphicsPipeline *curPipe = NULL;
        SDL_GPUBuffer *curVB = NULL;
        SDL_GPUTexture *curTex = NULL;
        SDL_GPUSampler *curSamp = NULL;
        bool indicesBound = false;

        for (int ci = 0; ci < setup.count; ci++) {
            const ShadowCascade &casc = setup.cascades[ci];
            SDL_GPUViewport vp = {
                (float)(ci * shadowMapSize), 0.0f, (float)shadowMapSize, (float)shadowMapSize, 0.0f, 1.0f
            };
            SDL_Rect scissor = { ci * shadowMapSize, 0, shadowMapSize, shadowMapSize };
            SDL_SetGPUViewport(spass, &vp);
            SDL_SetGPUScissor(spass, &scissor);

            for (const ShadowCaster &sc : casters) {
                // Lightweight light-space ortho frustum culling on CPU: skips ~85% of draws for Cascade 0.
                if (sc.radius > 0.0f) {
                    GfxVector diff = {
                        sc.center.x - casc.center.x,
                        sc.center.y - casc.center.y,
                        sc.center.z - casc.center.z,
                    };
                    float dx = fabsf(VecDot(diff, setup.lightRight));
                    float dy = fabsf(VecDot(diff, setup.lightUp));
                    if (dx > casc.radius + sc.radius || dy > casc.radius + sc.radius)
                        continue;
                }

                const GfxCommand &c = frame.commands[sc.cmdIndex];
                const GfxDrawState &s = frame.states[c.state];

                SDL_GPUGraphicsPipeline *pipe = ShadowPipeline(c.primitive);
                if (pipe == NULL)
                    continue;
                if (pipe != curPipe) {
                    SDL_BindGPUGraphicsPipeline(spass, pipe);
                    curPipe = pipe;
                }

                ShadowVertexUniforms svu;
                Multiply(s.world, casc.lightVP, svu.lightWorldViewProj);
                SDL_PushGPUVertexUniformData(cmd, 0, &svu, sizeof(svu));

                ShadowFragmentUniforms sfu = {};
                const bool alphaTestOn = s.alphaTest && s.stages[0].texture != NULL;
                if (alphaTestOn) {
                    sfu.alphaParams[0] = 1.0f;
                    sfu.alphaParams[1] = (s.alphaRef & 0xff) / 255.0f;
                    sfu.alphaParams[2] = (float)s.alphaFunc;
                }
                SDL_PushGPUFragmentUniformData(cmd, 0, &sfu, sizeof(sfu));

                SDL_GPUTexture *tex = white2D;
                SDL_GPUSampler *samp = Sampler(GfxStageState());
                if (alphaTestOn) {
                    GfxTexture *t = Gfx_DrawnTexture(s.stages[0].texture, overhaul);
                    GpuTexture *g = t != NULL ? Ensure(t) : NULL;
                    if (g != NULL && g->texture != NULL && !(t->flags & GFX_TEXTURE_CUBE)) {
                        tex = g->texture;
                        samp = Sampler(s.stages[0]);
                    }
                }
                if (tex != curTex || samp != curSamp) {
                    SDL_GPUTextureSamplerBinding tb = { tex, samp };
                    SDL_BindGPUFragmentSamplers(spass, 0, &tb, 1);
                    curTex = tex;
                    curSamp = samp;
                }

                SDL_GPUBuffer *vb = c.buffer != NULL ? ((GpuVertexBuffer *)c.buffer->gpu)->buffer : vertexBuffer;
                uint32_t firstVertex = c.firstVertexByte / sizeof(GfxVertex);
                if (vb != curVB) {
                    SDL_GPUBufferBinding vbb = { vb, 0 };
                    SDL_BindGPUVertexBuffers(spass, 0, &vbb, 1);
                    curVB = vb;
                }
                if (c.firstIndex != ~0u) {
                    if (!indicesBound) {
                        SDL_GPUBufferBinding ib = { indexBuffer, 0 };
                        SDL_BindGPUIndexBuffer(spass, &ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);
                        indicesBound = true;
                    }
                    SDL_DrawGPUIndexedPrimitives(spass, c.indexCount, 1, c.firstIndex, (Sint32)firstVertex, 0);
                } else {
                    SDL_DrawGPUPrimitives(spass, c.vertexCount, 1, firstVertex, 0);
                }
            }
        }
        SDL_EndGPURenderPass(spass);
    }

    void Execute(GfxFrame &frame, std::vector<GfxTexture *> &textures, GfxReadback &readback, bool present,
                 bool vsync, bool stretch) override
    {
        if (device == NULL)
            return;
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
        if (cmd == NULL) {
            Sys_Log("gfx: %s", SDL_GetError());
            return;
        }
        // Textures used by this frame must exist before the passes start.
        const bool overhaulInit = Platform_IsVisualOverhaulEnabled();
        for (const GfxDrawState &s : frame.states)
            for (const GfxStageState &stage : s.stages)
                if (stage.texture != NULL) {
                    Ensure(Gfx_DrawnTexture(stage.texture, overhaulInit));
                    GfxTexture *root = stage.texture->cube != NULL ? stage.texture->cube : stage.texture;
                    if (root->normalMap != NULL)
                        Ensure(root->normalMap);
                    if (root->specularMap != NULL)
                        Ensure(root->specularMap);
                }
        for (const GfxCommand &c : frame.commands)
            if (c.type == GFX_CMD_TARGET && c.target != NULL)
                Ensure(c.target->cube != NULL ? c.target->cube : c.target);
        UploadTextures(cmd, textures);
        UploadGeometry(cmd, frame);
        UploadVertexBuffers(cmd, frame);

        SDL_GPURenderPass *pass = NULL;
        PassBindings bound;
        GfxTexture *target = NULL;
        int targetW = backWidth, targetH = backHeight;
        // The uniforms pushed to this command buffer (they stay for its
        // passes): the state and target size of the vertex ones, and the
        // fragment ones. Clears push their own.
        const GfxDrawState *vertexUniformState = NULL;
        int vertexUniformW = 0, vertexUniformH = 0;
        bool vertexUniformScaled = false;
        FragmentUniforms fragmentUniforms;
        bool fragmentUniformsSet = false;
        // The drawing into textures (the car reflections' cube faces) runs
        // first, then the back buffer's, so that the back buffer's render
        // pass is not split by passes on other depth buffers mid-scene. With
        // cubic environment mapping on, the Adreno 840 lost parts of the
        // scenery after such a split in some sessions (#46). Nothing drawn
        // on the back buffer is read by the drawing into textures.
        GfxTexture *passTarget = NULL;
        const bool overhaul = Platform_IsVisualOverhaulEnabled();
        const bool perPixel = overhaul && Platform_GetSettingInt("video.per_pixel_lighting", 1) != 0;
        const bool hemisphere = overhaul && Platform_GetSettingInt("video.hemisphere_ambient", 1) != 0;
        const bool carFresnel = overhaul && Platform_GetSettingInt("video.car_fresnel", 1) != 0;
        const bool materialMaps = overhaul && Platform_GetSettingInt("video.override_material_maps", 1) != 0;
        const float fresnelStrength =
            carFresnel ? std::clamp(Platform_GetSettingFloat("video.car_fresnel_strength", 0.65f), 0.0f, 1.0f) : 0.0f;
        size_t last3D = SIZE_MAX;
        size_t backdropIndex = SIZE_MAX;
        bool foundSun = false;
        GfxVector sunLightDir = { 0.3f, -0.8f, 0.4f };
        GfxMatrix mainCamView = {};
        GfxMatrix mainCamProj = {};
        bool foundMainCam = false;
        bool foundMainProj = false;
        float bestSunLuma = 0.0f;
        {
            GfxTexture *scanTarget = NULL;
            for (size_t i = 0; i < frame.commands.size(); i++) {
                const GfxCommand &c = frame.commands[i];
                if (c.type == GFX_CMD_TARGET) {
                    scanTarget = c.target;
                    continue;
                }
                if (scanTarget != NULL)
                    continue;
                if (c.type == GFX_CMD_BACKDROP) {
                    backdropIndex = i;
                    continue;
                }
                if (c.type == GFX_CMD_DRAW && !c.transformed) {
                    last3D = i;
                    const GfxDrawState &s = frame.states[c.state];
                    if (s.depthTest && s.depthWrite && (!foundMainCam || backdropIndex != SIZE_MAX)) {
                        mainCamView = s.view;
                        foundMainCam = true;
                        if (fabsf(s.projection._11) > 1e-4f && fabsf(s.projection._22) > 1e-4f) {
                            mainCamProj = s.projection;
                            foundMainProj = true;
                        }
                    }
                    if (s.lighting) {
                        for (int li = 0; li < 8; li++) {
                            if (!s.lightEnabled[li] || s.lights[li].type != GFX_LIGHT_DIRECTIONAL)
                                continue;
                            float luma = s.lights[li].diffuse.r * 0.299f + s.lights[li].diffuse.g * 0.587f +
                                         s.lights[li].diffuse.b * 0.114f;
                            if (luma > 0.15f && luma > bestSunLuma) {
                                bestSunLuma = luma;
                                sunLightDir = s.lights[li].direction;
                                foundSun = true;
                            }
                        }
                    }
                }
            }
        }
        bool shadowActive = false;
        ShadowSetup shadowSetup = {};
        const float shadowStrength =
            std::clamp(Platform_GetSettingFloat("video.shadow_strength", 0.65f), 0.0f, 1.0f);
        const int shadowCascades = std::clamp(Platform_GetSettingInt("video.shadow_cascades", 2), 1, 2);
        const int shadowPcfQuality = std::clamp(Platform_GetSettingInt("video.shadow_pcf_quality", 2), 0, 2);
        if (overhaul && shadowSupported && foundSun && foundMainCam && shadowStrength > 0.01f &&
            Platform_GetSettingInt("video.shadow_map", 1) != 0) {
            EnsureShadowMap(Platform_GetSettingInt("video.shadow_map_size", 2048), shadowCascades);
            float shadowDist = std::clamp(Platform_GetSettingFloat("video.shadow_distance", 110.0f), 25.0f, 200.0f);
            if (shadowMap != NULL &&
                BuildSunCascades(mainCamView, sunLightDir, shadowDist, shadowMapSize, shadowMapCascades, shadowSetup)) {
                RenderShadowPass(cmd, frame, backdropIndex, shadowSetup, overhaul);
                shadowActive = true;
                vertexUniformState = NULL;
                fragmentUniformsSet = false;
            }
        }
        const bool toneMap = overhaul && Platform_GetSettingInt("video.tone_mapping", 1) != 0;
        const bool bloomOn = overhaul && Platform_GetSettingInt("video.bloom", 1) != 0;
        const float bloomIntensity =
            bloomOn ? std::clamp(Platform_GetSettingFloat("video.bloom_intensity", 0.25f), 0.0f, 1.5f) : 0.0f;
        const bool ssaoOn = overhaul && ssaoSupported && hdrDepth != NULL &&
                            Platform_GetSettingInt("video.ssao", 1) != 0;
        const float ssaoStrength =
            ssaoOn ? std::clamp(Platform_GetSettingFloat("video.ssao_strength", 0.55f), 0.0f, 1.0f) : 0.0f;
        const bool casOn = overhaul && Platform_GetSettingInt("video.cas_sharpening", 1) != 0;
        const float casStrength =
            casOn ? std::clamp(Platform_GetSettingFloat("video.cas_strength", 0.40f), 0.0f, 1.0f) : 0.0f;
        const bool postEnabled = overhaul && hdrSupported && hdrColor != NULL &&
                                 Platform_GetSettingInt("video.post_process", 1) != 0 && last3D != SIZE_MAX &&
                                 (toneMap || bloomIntensity > 0.0f || ssaoStrength > 0.001f || casStrength > 0.001f);
        const bool useMrtDepth = postEnabled && ssaoOn && ssaoStrength > 0.001f && foundMainProj;
        SDL_GPUSampleCount backSamples =
            (msaaSamples != SDL_GPU_SAMPLECOUNT_1 && backColorMsaa != NULL && overhaul)
                ? msaaSamples
                : SDL_GPU_SAMPLECOUNT_1;
        SDL_GPUSampleCount hdrBackSamples =
            (hdrMsaaSamples != SDL_GPU_SAMPLECOUNT_1 && hdrColorMsaa != NULL && backDepthMsaa != NULL && overhaul)
                ? hdrMsaaSamples
                : SDL_GPU_SAMPLECOUNT_1;
        for (int phase = 0; phase < 2; phase++) {
            target = NULL;
            vertexUniformState = NULL;
            fragmentUniformsSet = false;
            bool inHdr3D = (phase == 1 && postEnabled);
            bool hdrDepthCleared = false;
            for (size_t i = 0; i < frame.commands.size(); i++) {
                const GfxCommand &c = frame.commands[i];
                if (c.type == GFX_CMD_TARGET) {
                    // The back buffer's pass stays open over the texture
                    // drawing skipped in its phase.
                    if (pass != NULL && phase == 0) {
                        EndPass(cmd, pass, passTarget);
                        pass = NULL;
                    }
                    target = c.target;
                    continue;
                }
                if ((target != NULL) != (phase == 0))
                    continue;
                SDL_GPUSampleCount passSamples = target != NULL ? SDL_GPU_SAMPLECOUNT_1
                                                 : inHdr3D      ? hdrBackSamples
                                                 : postEnabled  ? SDL_GPU_SAMPLECOUNT_1
                                                                : backSamples;
                const bool passMrtDepth = (target == NULL && inHdr3D && useMrtDepth);
                if (c.type == GFX_CMD_BACKDROP) {
                    // A copy pass between render passes; the next draw starts a
                    // render pass that loads the back buffer as it is.
                    if (target != NULL)
                        continue;
                    if (pass != NULL) {
                        SDL_EndGPURenderPass(pass);
                        pass = NULL;
                    }
                    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
                    SDL_GPUTextureLocation src = {}, dst = {};
                    src.texture = inHdr3D ? hdrColor : backColor;
                    dst.texture = inHdr3D ? hdrBackdrop : backdrop;
                    SDL_CopyGPUTextureToTexture(copy, &src, &dst, backWidth, backHeight, 1, false);
                    SDL_EndGPUCopyPass(copy);
                    continue;
                }
                if (pass == NULL) {
                    size_t cleared;
                    pass = BeginPass(cmd, target, passSamples, inHdr3D, passMrtDepth, hdrDepthCleared, targetW, targetH,
                                     frame.commands, i, &cleared);
                    passTarget = target;
                    bound = PassBindings();
                    if (cleared > 0) {
                        i += cleared - 1;
                        continue;
                    }
                }
                if (c.type == GFX_CMD_CLEAR) {
                    bool colorClear = (c.clearFlags & GFX_CLEAR_TARGET) != 0;
                    bool depthClear = (c.clearFlags & GFX_CLEAR_ZBUFFER) != 0;
                    if (!colorClear && !depthClear)
                        continue;
                    SDL_GPUGraphicsPipeline *pipeline =
                        ClearPipeline(colorClear, depthClear, passSamples, inHdr3D, passMrtDepth);
                    if (pipeline != bound.pipeline) {
                        SDL_BindGPUGraphicsPipeline(pass, pipeline);
                        bound.pipeline = pipeline;
                    }
                    GfxViewport v = ToTarget(c.clearViewport, c.clearScreenW, c.clearScreenH, target == NULL, targetW,
                                             targetH);
                    SDL_GPUViewport vp = { (float)v.x, (float)v.y, (float)v.width, (float)v.height, 0.0f, 1.0f };
                    SDL_Rect scissor = { (int)v.x, (int)v.y, (int)v.width, (int)v.height };
                    SetViewport(pass, bound, vp, scissor);
                    ClearUniforms u = {};
                    Color(c.clearColor, u.color);
                    u.depth[0] = c.clearZ;
                    SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof(u));
                    SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
                    vertexUniformState = NULL;
                    fragmentUniformsSet = false;
                    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
                    continue;
                }
                const GfxDrawState &s = frame.states[c.state];
                SDL_GPUGraphicsPipeline *pipeline = Pipeline(PipelineKey(c, s, passSamples, inHdr3D, passMrtDepth));
                if (pipeline == NULL)
                    continue;
                if (pipeline != bound.pipeline) {
                    SDL_BindGPUGraphicsPipeline(pass, pipeline);
                    bound.pipeline = pipeline;
                }
                const bool scaled = target == NULL && s.screenW > 0 && s.screenH > 0;
                const GfxViewport viewport = ToTarget(s.viewport, s.screenW, s.screenH, target == NULL, targetW, targetH);
                SDL_GPUViewport vp;
                if (c.transformed)
                    vp = { 0.0f, 0.0f, (float)targetW, (float)targetH, 0.0f, 1.0f };
                else
                    vp = { (float)viewport.x, (float)viewport.y, (float)viewport.width, (float)viewport.height,
                           viewport.minZ, viewport.maxZ };
                SDL_Rect scissor = { (int)viewport.x, (int)viewport.y, (int)viewport.width, (int)viewport.height };
                SetViewport(pass, bound, vp, scissor);
                if (vertexUniformState == NULL || vertexUniformW != targetW || vertexUniformH != targetH ||
                    vertexUniformScaled != scaled ||
                    (vertexUniformState != &s && !SameVertexUniforms(*vertexUniformState, s))) {
                    VertexUniforms vu;
                    FillVertexUniforms(c, s, targetW, targetH, viewport, scaled, vu);
                    SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));
                    FragmentLightingUniforms fl;
                    FillFragmentLightingUniforms(s, vu, overhaul, perPixel, hemisphere, fresnelStrength,
                                                 shadowActive && target == NULL, shadowSetup, shadowStrength,
                                                 shadowPcfQuality, fl);
                    SDL_PushGPUFragmentUniformData(cmd, 1, &fl, sizeof(fl));
                    vertexUniformState = &s;
                    vertexUniformW = targetW;
                    vertexUniformH = targetH;
                    vertexUniformScaled = scaled;
                }
                const bool receiveShadows = shadowActive && target == NULL && !c.transformed && s.depthTest &&
                                            !(s.alphaBlend && s.destBlend == GFX_BLEND_ONE) &&
                                            (backdropIndex == SIZE_MAX || i > backdropIndex);
                FragmentUniforms fu;
                FillFragmentUniforms(s, target == NULL && !c.transformed, inHdr3D && !c.transformed,
                                     materialMaps && !c.transformed, receiveShadows, fu);
                if (!fragmentUniformsSet || memcmp(&fu, &fragmentUniforms, sizeof(fu)) != 0) {
                    SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof(fu));
                    fragmentUniforms = fu;
                    fragmentUniformsSet = true;
                }
                SDL_GPUTextureSamplerBinding textures[10];
                TextureBindings(s, !c.transformed, inHdr3D, materialMaps && !c.transformed, textures);
                if (!bound.texturesSet || memcmp(textures, bound.textures, sizeof(textures)) != 0) {
                    SDL_BindGPUFragmentSamplers(pass, 0, textures, 10);
                    memcpy(bound.textures, textures, sizeof(textures));
                    bound.texturesSet = true;
                }
                // The buffer from its start; the draw gives its first vertex.
                SDL_GPUBuffer *vertices = c.buffer != NULL ? ((GpuVertexBuffer *)c.buffer->gpu)->buffer : vertexBuffer;
                uint32_t firstVertex = c.firstVertexByte / (c.transformed ? sizeof(GfxTLVertex) : sizeof(GfxVertex));
                if (vertices != bound.vertices) {
                    SDL_GPUBufferBinding vb = { vertices, 0 };
                    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
                    bound.vertices = vertices;
                }
                if (c.firstIndex != ~0u) {
                    if (!bound.indicesSet) {
                        SDL_GPUBufferBinding ib = { indexBuffer, 0 };
                        SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);
                        bound.indicesSet = true;
                    }
                    SDL_DrawGPUIndexedPrimitives(pass, c.indexCount, 1, c.firstIndex, (Sint32)firstVertex, 0);
                } else {
                    SDL_DrawGPUPrimitives(pass, c.vertexCount, 1, firstVertex, 0);
                }
                if (inHdr3D && i == last3D) {
                    if (pass != NULL) {
                        EndPass(cmd, pass, passTarget);
                        pass = NULL;
                    }
                    RunPostProcess(cmd, toneMap, bloomOn, bloomIntensity, useMrtDepth, ssaoStrength, casOn, casStrength,
                                   mainCamProj, foundMainProj);
                    vertexUniformState = NULL;
                    fragmentUniformsSet = false;
                    inHdr3D = false;
                }
            }
            if (pass != NULL) {
                EndPass(cmd, pass, passTarget);
                pass = NULL;
            }
        }

        if (!present) {
            SDL_SubmitGPUCommandBuffer(cmd);
            return;
        }

        // Read back before the frame is shown (the lens flare test).
        CollectReadback(readback);
        Readback *reading = NULL;
        if (readback.requested && readback.width > 0 && readback.height > 0) {
            int x = std::max(0, std::min(readback.x, backWidth - 1));
            int y = std::max(0, std::min(readback.y, backHeight - 1));
            int w = std::min(readback.width, backWidth - x), h = std::min(readback.height, backHeight - y);
            reading = FreeReadback(readback);
            if ((uint32_t)(w * h * 4) > reading->capacity) {
                SDL_ReleaseGPUTransferBuffer(device, reading->buffer);
                reading->capacity = (uint32_t)(w * h * 4);
                SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, reading->capacity, 0 };
                reading->buffer = SDL_CreateGPUTransferBuffer(device, &tbi);
            }
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureRegion src = {};
            src.texture = backColor;
            src.x = x;
            src.y = y;
            src.w = w;
            src.h = h;
            src.d = 1;
            SDL_GPUTextureTransferInfo dst = { reading->buffer, 0, 0, 0 };
            SDL_DownloadFromGPUTexture(copy, &src, &dst);
            SDL_EndGPUCopyPass(copy);
            reading->width = w;
            reading->height = h;
            readback.requested = false;
        }

        if (vsync != presentModeVsync) {
            SDL_GPUPresentMode mode = vsync ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE;
            if (!vsync && !SDL_WindowSupportsGPUPresentMode(device, window, mode))
                mode = SDL_GPU_PRESENTMODE_MAILBOX;
            if (SDL_WindowSupportsGPUPresentMode(device, window, mode))
                SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
            presentModeVsync = vsync;
        }

        SDL_GPUTexture *swapchain = NULL;
        Uint32 sw = 0, sh = 0;
        if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain, &sw, &sh) && swapchain != NULL) {
            if (sw != presentWidth || sh != presentHeight) {
                Sys_Log("gfx: %dx%d shown in %ux%u pixels", backWidth, backHeight, sw, sh);
                presentWidth = sw;
                presentHeight = sh;
            }
            // Movie frames fill the drawable; ordinary game frames retain their aspect ratio.
            PresentationRect rect = GetPresentationRect(backWidth, backHeight, sw, sh, stretch);
            SDL_GPUBlitInfo blit = {};
            blit.source.texture = backColor;
            blit.source.w = backWidth;
            blit.source.h = backHeight;
            blit.destination.texture = swapchain;
            blit.destination.x = rect.x;
            blit.destination.y = rect.y;
            blit.destination.w = rect.width;
            blit.destination.h = rect.height;
            blit.load_op = SDL_GPU_LOADOP_CLEAR;
            blit.clear_color = { 0.0f, 0.0f, 0.0f, 1.0f };
            blit.filter = smoothScaling ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(cmd, &blit);
        }

        if (reading != NULL) {
            reading->fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            reading->serial = ++readbackSerial;
        } else {
            SDL_SubmitGPUCommandBuffer(cmd);
        }
        HashFrame();
    }

    // Takes the reads the GPU has finished, oldest first, so the newest
    // stays. Frame hashing waits for them all: its frames must not depend on
    // the GPU's timing.
    void CollectReadback(GfxReadback &readback) override
    {
        bool wait = Gfx_FrameHashWanted();
        for (;;) {
            Readback *oldest = NULL;
            for (Readback &r : readbacks)
                if (r.fence != NULL && (oldest == NULL || r.serial < oldest->serial))
                    oldest = &r;
            if (oldest == NULL || (!wait && !SDL_QueryGPUFence(device, oldest->fence)))
                return;
            FinishReadback(*oldest, readback);
        }
    }

    void FinishReadback(Readback &r, GfxReadback &readback)
    {
        SDL_WaitForGPUFences(device, true, &r.fence, 1);
        SDL_ReleaseGPUFence(device, r.fence);
        r.fence = NULL;
        const uint32_t *map = (const uint32_t *)SDL_MapGPUTransferBuffer(device, r.buffer, false);
        readback.pixels.assign(map, map + (size_t)r.width * r.height);
        SDL_UnmapGPUTransferBuffer(device, r.buffer);
        readback.readyWidth = r.width;
        readback.readyHeight = r.height;
        readback.ready = true;
    }

    // A slot for a new read. With all of them in flight, the GPU is far
    // behind: the oldest is finished first.
    Readback *FreeReadback(GfxReadback &readback)
    {
        Readback *oldest = NULL;
        for (Readback &r : readbacks) {
            if (r.fence == NULL)
                return &r;
            if (oldest == NULL || r.serial < oldest->serial)
                oldest = &r;
        }
        FinishReadback(*oldest, readback);
        return oldest;
    }

    // The back buffer's pixels for Gfx_HashFrame.
    void HashFrame()
    {
        if (!Gfx_FrameHashWanted())
            return;
        uint32_t bytes = (uint32_t)backWidth * backHeight * 4;
        SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes, 0 };
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(device, &tbi);
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion src = {};
        src.texture = backColor;
        src.w = backWidth;
        src.h = backHeight;
        src.d = 1;
        SDL_GPUTextureTransferInfo dst = { tb, 0, 0, 0 };
        SDL_DownloadFromGPUTexture(copy, &src, &dst);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        SDL_WaitForGPUFences(device, true, &fence, 1);
        SDL_ReleaseGPUFence(device, fence);
        Gfx_HashFrame((const uint8_t *)SDL_MapGPUTransferBuffer(device, tb, false), backWidth, backHeight);
        SDL_UnmapGPUTransferBuffer(device, tb);
        SDL_ReleaseGPUTransferBuffer(device, tb);
    }
};

} // namespace

GfxExecutor *Gfx_CreateGpuExecutor(void)
{
    return new Executor;
}

bool Gfx_GpuIsAvailable(void)
{
    SDL_PropertiesID props = DeviceProperties();
    // A software Vulkan driver (lavapipe, often installed beside the GPU's
    // own driver) does not count: an old GPU draws faster on its OpenGL.
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_ACCELERATION_BOOLEAN, true);
    bool available = SDL_GPUSupportsProperties(props);
    SDL_DestroyProperties(props);
    return available;
}
