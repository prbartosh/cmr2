// External HD texture override loader (PNG/DDS/TGA) with alpha-weighted mipmaps,
// companion normal (*_n.png) and specular/reflection (*_s.png) material maps,
// and optional texture dumping (video.dump_textures = 1).

#include "port/texture_override.h"
#include "platform/platform.h"
#include "port/sys.h"
#include "render/gfx_internal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_PSD
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

namespace fs = std::filesystem;

namespace {

struct IndexEntry {
    std::string path;
    int priority; // higher wins (.dds=3, .png=2, .tga=1)
    int rootRank; // lower wins (earlier root)
};

struct OverrideIndex {
    bool built = false;
    std::vector<std::string> extraRoots;
    std::unordered_map<std::string, IndexEntry> byRel;
    std::unordered_map<std::string, IndexEntry> byBase;
    std::unordered_set<std::string> dumped;
    size_t bc7Loaded = 0;
    size_t cpuLoaded = 0;
    double totalLoadMs = 0.0;
};

OverrideIndex s_index;

std::string ToLowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

int ExtPriority(const std::string &extLower)
{
    if (extLower == ".dds")
        return 3;
    if (extLower == ".png")
        return 2;
    if (extLower == ".tga")
        return 1;
    return 0;
}

void SplitNormalized(const char *name, std::string &relStem, std::string &baseStem)
{
    relStem.clear();
    baseStem.clear();
    if (name == NULL || name[0] == '\0')
        return;

    std::string s(name);
    for (char &c : s) {
        if (c == '\\')
            c = '/';
        else
            c = (char)std::tolower((unsigned char)c);
    }
    while (s.size() >= 2 && s[0] == '.' && s[1] == '/')
        s.erase(0, 2);
    while (!s.empty() && s[0] == '/')
        s.erase(0, 1);

    size_t slash = s.find_last_of('/');
    std::string base = (slash == std::string::npos) ? s : s.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) {
        size_t extLen = base.size() - dot;
        s.resize(s.size() - extLen);
        base.resize(dot);
    }
    relStem = s;
    baseStem = base;
}

void InsertEntry(std::unordered_map<std::string, IndexEntry> &map, const std::string &key, const std::string &path,
                 int prio, int rootRank)
{
    auto it = map.find(key);
    if (it == map.end()) {
        map.emplace(key, IndexEntry{ path, prio, rootRank });
        return;
    }
    if (rootRank < it->second.rootRank || (rootRank == it->second.rootRank && prio > it->second.priority)) {
        it->second = IndexEntry{ path, prio, rootRank };
    }
}

void ScanRoot(const fs::path &root, int rootRank)
{
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec))
        return;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        fs::path p = it->path();
        std::string ext = ToLowerAscii(p.extension().string());
        int prio = ExtPriority(ext);
        if (prio == 0)
            continue;
        fs::path rel = fs::relative(p, root, ec);
        if (ec)
            continue;
        std::string relStem, baseStem;
        SplitNormalized(rel.generic_string().c_str(), relStem, baseStem);
        if (relStem.empty() || baseStem.empty())
            continue;
        std::string fullPath = p.string();
        InsertEntry(s_index.byRel, relStem, fullPath, prio, rootRank);
        InsertEntry(s_index.byBase, baseStem, fullPath, prio, rootRank);
    }
}

void EnsureIndex()
{
    if (s_index.built)
        return;
    s_index.built = true;
    s_index.byRel.clear();
    s_index.byBase.clear();

    std::vector<fs::path> roots;
    for (const std::string &extra : s_index.extraRoots)
        if (!extra.empty())
            roots.emplace_back(extra);

    const char *cfgOverrides = Platform_GetSetting("paths.overrides", "");
    if (cfgOverrides != NULL && cfgOverrides[0] != '\0')
        roots.emplace_back(cfgOverrides);

    const char *userDir = Platform_GetUserDir();
    if (userDir != NULL && userDir[0] != '\0')
        roots.emplace_back(fs::path(userDir) / "overrides");

    roots.emplace_back(fs::path("overrides"));

    int rank = 0;
    for (const fs::path &r : roots)
        ScanRoot(r, rank++);

    if (!s_index.byRel.empty()) {
        Sys_Log("overrides: indexed %zu texture override files", s_index.byRel.size());
    }
}

std::string LookupFile(const std::string &relKey, const std::string &baseKey)
{
    auto it = s_index.byRel.find(relKey);
    if (it != s_index.byRel.end())
        return it->second.path;
    it = s_index.byBase.find(baseKey);
    if (it != s_index.byBase.end())
        return it->second.path;
    return {};
}

bool ReadWholeFile(const std::string &path, std::vector<uint8_t> &out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return false;
    std::streamsize sz = f.tellg();
    if (sz <= 0 || sz > 256 * 1024 * 1024)
        return false;
    out.resize((size_t)sz);
    f.seekg(0, std::ios::beg);
    return (bool)f.read((char *)out.data(), sz);
}

uint32_t ReadLE32(const uint8_t *p, size_t off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) | ((uint32_t)p[off + 2] << 16) |
           ((uint32_t)p[off + 3] << 24);
}

// Fast native GPU path: loads a DDS DX10 BC7 file (DXGI_FORMAT_BC7_UNORM / SRGB)
// directly with its pre-generated mipmap chain — zero CPU decoding or mip generation.
GfxTexture *TryCreateNativeBC7Texture(const std::string &path, DWORD flags)
{
    if (!Gfx_SupportsBC7())
        return NULL;

    std::vector<uint8_t> data;
    if (!ReadWholeFile(path, data))
        return NULL;
    // 4 ("DDS ") + 124 (DDS_HEADER) + 20 (DDS_HEADER_DXT10) = 148 bytes.
    if (data.size() < 148 || memcmp(data.data(), "DDS ", 4) != 0)
        return NULL;

    const uint8_t *p = data.data();
    if (ReadLE32(p, 4) != 124u)
        return NULL;

    uint32_t h = ReadLE32(p, 12);
    uint32_t w = ReadLE32(p, 16);
    uint32_t mipHeader = ReadLE32(p, 28);
    uint32_t pfFlags = ReadLE32(p, 80);
    uint32_t fourCC = ReadLE32(p, 84);

    if ((pfFlags & 0x4u) == 0 || fourCC != 0x30315844u) // 'DX10'
        return NULL;

    uint32_t dxgiFormat = ReadLE32(p, 128);
    uint32_t resourceDim = ReadLE32(p, 132);
    uint32_t arraySize = ReadLE32(p, 140);
    // DXGI_FORMAT_BC7_UNORM = 98, DXGI_FORMAT_BC7_UNORM_SRGB = 99
    if ((dxgiFormat != 98u && dxgiFormat != 99u) || resourceDim != 3u || arraySize < 1u)
        return NULL;
    if (w == 0 || h == 0 || w > 8192 || h > 8192)
        return NULL;

    int maxMips = 1;
    while ((w >> maxMips) > 0 || (h >> maxMips) > 0)
        maxMips++;
    maxMips = std::min(maxMips, 16);
    int requestedMips = mipHeader > 0 ? std::min((int)mipHeader, maxMips) : 1;

    // Verify available mip levels fit inside the file buffer.
    size_t avail = data.size() - 148;
    size_t needed = 0;
    int validMips = 0;
    for (int m = 0; m < requestedMips; m++) {
        int mw = std::max(1, (int)(w >> m));
        int mh = std::max(1, (int)(h >> m));
        size_t sz = Gfx_LevelSize(GFX_FORMAT_BC7, mw, mh, NULL);
        if (needed + sz > avail)
            break;
        needed += sz;
        validMips++;
    }
    if (validMips < 1)
        return NULL;

    GfxTexture *tex = Gfx_CreateTexture((int)w, (int)h, GFX_FORMAT_BC7, validMips, flags);
    if (tex == NULL)
        return NULL;

    const uint8_t *src = p + 148;
    for (int m = 0; m < validMips; m++) {
        int mw = std::max(1, (int)(w >> m));
        int mh = std::max(1, (int)(h >> m));
        size_t sz = Gfx_LevelSize(GFX_FORMAT_BC7, mw, mh, NULL);
        GfxLockedRect lock{};
        if (Gfx_LockTexture(tex, 0, m, &lock)) {
            memcpy(lock.pixels, src, sz);
            Gfx_UnlockTexture(tex, 0, m);
        }
        src += sz;
    }
    return tex;
}

// Decodes a DDS file (DXT1/BC1, DXT5/BC3, or 32-bit BGRA/RGBA) into tightly packed BGRA8.
bool LoadDDSFileBGRA(const std::string &path, int &width, int &height, std::vector<uint8_t> &bgra)
{
    std::vector<uint8_t> data;
    if (!ReadWholeFile(path, data))
        return false;
    if (data.size() < 128 || memcmp(data.data(), "DDS ", 4) != 0)
        return false;

    auto rd32 = [&](size_t off) -> uint32_t { return ReadLE32(data.data(), off); };

    uint32_t h = rd32(12);
    uint32_t w = rd32(16);
    uint32_t pfFlags = rd32(80);
    uint32_t fourCC = rd32(84);
    uint32_t bitCount = rd32(88);
    uint32_t rMask = rd32(92);
    uint32_t gMask = rd32(96);
    uint32_t bMask = rd32(100);
    uint32_t aMask = rd32(104);

    if (w == 0 || h == 0 || w > 8192 || h > 8192)
        return false;

    const uint8_t *pixels = data.data() + 128;
    size_t avail = data.size() - 128;
    width = (int)w;
    height = (int)h;
    bgra.assign((size_t)w * h * 4, 0);

    if ((pfFlags & 0x4u) != 0) { // DDPF_FOURCC
        int fmt = 0;
        if (fourCC == 0x31545844u) // 'DXT1'
            fmt = GFX_FORMAT_BC1;
        else if (fourCC == 0x35545844u) // 'DXT5'
            fmt = GFX_FORMAT_BC3;
        else
            return false;
        int blockBytes = (fmt == GFX_FORMAT_BC1) ? 8 : 16;
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (avail < (size_t)bw * bh * (size_t)blockBytes)
            return false;
        uint32_t block[16];
        for (uint32_t by = 0; by < bh; by++) {
            for (uint32_t bx = 0; bx < bw; bx++) {
                Gfx_DecodeBlock(fmt, pixels + ((size_t)by * bw + bx) * (size_t)blockBytes, block);
                for (uint32_t y = 0; y < 4; y++) {
                    for (uint32_t x = 0; x < 4; x++) {
                        if (bx * 4 + x < w && by * 4 + y < h) {
                            ((uint32_t *)bgra.data())[(size_t)(by * 4 + y) * w + bx * 4 + x] = block[y * 4 + x];
                        }
                    }
                }
            }
        }
        return true;
    }

    if (bitCount == 32 && avail >= (size_t)w * h * 4) {
        bool isRGBA = (rMask == 0x000000ffu && gMask == 0x0000ff00u && bMask == 0x00ff0000u);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            const uint8_t *s = pixels + i * 4;
            uint8_t *d = bgra.data() + i * 4;
            if (isRGBA) {
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
                d[3] = aMask != 0 ? s[3] : 255;
            } else {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = aMask != 0 ? s[3] : 255;
            }
        }
        return true;
    }
    return false;
}

bool LoadImageFileBGRA(const std::string &path, int &width, int &height, std::vector<uint8_t> &bgra)
{
    std::string ext = ToLowerAscii(fs::path(path).extension().string());
    if (ext == ".dds")
        return LoadDDSFileBGRA(path, width, height, bgra);

    int w = 0, h = 0, comp = 0;
    stbi_uc *rgba = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (rgba == NULL || w <= 0 || h <= 0) {
        if (rgba)
            stbi_image_free(rgba);
        return false;
    }
    width = w;
    height = h;
    bgra.resize((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        bgra[i * 4 + 0] = rgba[i * 4 + 2]; // B
        bgra[i * 4 + 1] = rgba[i * 4 + 1]; // G
        bgra[i * 4 + 2] = rgba[i * 4 + 0]; // R
        bgra[i * 4 + 3] = rgba[i * 4 + 3]; // A
    }
    stbi_image_free(rgba);
    return true;
}

void DownsampleBGRAAlphaWeighted(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh, bool isNormalMap)
{
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            uint8_t *q = dst + ((size_t)y * dw + x) * 4;
            if (isNormalMap) {
                float nx = 0.0f, ny = 0.0f, nz = 0.0f;
                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        int sx = std::min(sw - 1, x * 2 + dx);
                        int sy = std::min(sh - 1, y * 2 + dy);
                        const uint8_t *p = src + ((size_t)sy * sw + sx) * 4;
                        nx += (p[2] / 255.0f) * 2.0f - 1.0f; // R = X
                        ny += (p[1] / 255.0f) * 2.0f - 1.0f; // G = Y
                        nz += (p[0] / 255.0f) * 2.0f - 1.0f; // B = Z
                    }
                }
                float len = sqrtf(nx * nx + ny * ny + nz * nz);
                if (len > 1e-5f) {
                    nx /= len;
                    ny /= len;
                    nz /= len;
                } else {
                    nx = 0.0f;
                    ny = 0.0f;
                    nz = 1.0f;
                }
                q[2] = (uint8_t)std::clamp((int)lroundf((nx * 0.5f + 0.5f) * 255.0f), 0, 255);
                q[1] = (uint8_t)std::clamp((int)lroundf((ny * 0.5f + 0.5f) * 255.0f), 0, 255);
                q[0] = (uint8_t)std::clamp((int)lroundf((nz * 0.5f + 0.5f) * 255.0f), 0, 255);
                q[3] = 255;
                continue;
            }
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

void RecordOverrideLoad(bool nativeBc7, double elapsedMs)
{
    if (nativeBc7)
        s_index.bc7Loaded++;
    else
        s_index.cpuLoaded++;
    s_index.totalLoadMs += elapsedMs;
    size_t total = s_index.bc7Loaded + s_index.cpuLoaded;
    if (total == 1 || (total <= 50 && total % 25 == 0) || (total % 100 == 0)) {
        Sys_Log("overrides: loaded %zu textures (%zu BC7 GPU, %zu CPU) in %.1f ms (avg %.2f ms/tex)", total,
                s_index.bc7Loaded, s_index.cpuLoaded, s_index.totalLoadMs,
                total > 0 ? s_index.totalLoadMs / (double)total : 0.0);
    }
}

GfxTexture *CreateMipmappedTexture(const std::string &path, DWORD flags, bool isNormalMap)
{
    auto t0 = std::chrono::steady_clock::now();
    std::string ext = ToLowerAscii(fs::path(path).extension().string());
    if (ext == ".dds") {
        GfxTexture *bc7 = TryCreateNativeBC7Texture(path, flags);
        if (bc7 != NULL) {
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            RecordOverrideLoad(true, ms);
            return bc7;
        }
    }

    int w = 0, h = 0;
    std::vector<uint8_t> level0;
    if (!LoadImageFileBGRA(path, w, h, level0))
        return NULL;

    int mips = 1;
    while ((w >> mips) > 0 || (h >> mips) > 0)
        mips++;
    mips = std::min(mips, 16);

    GfxTexture *tex = Gfx_CreateTexture(w, h, GFX_FORMAT_BGRA8, mips, flags);
    if (tex == NULL)
        return NULL;

    std::vector<uint8_t> prev = std::move(level0);
    int prevW = w, prevH = h;
    for (int m = 0; m < mips; m++) {
        int mw = std::max(1, w >> m);
        int mh = std::max(1, h >> m);
        std::vector<uint8_t> cur;
        const uint8_t *srcData = NULL;
        if (m == 0) {
            srcData = prev.data();
        } else {
            cur.resize((size_t)mw * mh * 4);
            DownsampleBGRAAlphaWeighted(prev.data(), prevW, prevH, cur.data(), mw, mh, isNormalMap);
            srcData = cur.data();
        }
        GfxLockedRect lock{};
        if (Gfx_LockTexture(tex, 0, m, &lock)) {
            for (int y = 0; y < mh; y++)
                memcpy((uint8_t *)lock.pixels + (size_t)y * lock.pitch, srcData + (size_t)y * mw * 4, (size_t)mw * 4);
            Gfx_UnlockTexture(tex, 0, m);
        }
        if (m > 0) {
            prev = std::move(cur);
            prevW = mw;
            prevH = mh;
        }
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    RecordOverrideLoad(false, ms);
    return tex;
}

void DumpTextureIfEnabled(const std::string &relStem, GfxTexture *texture)
{
    if (Platform_GetSettingInt("video.dump_textures", 0) == 0 || relStem.empty() || texture == NULL)
        return;
    if (s_index.dumped.count(relStem) != 0)
        return;
    s_index.dumped.insert(relStem);

    const char *userDir = Platform_GetUserDir();
    fs::path outRoot = (userDir != NULL && userDir[0] != '\0') ? (fs::path(userDir) / "dumped_textures")
                                                               : fs::path("dumped_textures");
    fs::path outPath = outRoot / (relStem + ".png");
    std::error_code ec;
    fs::create_directories(outPath.parent_path(), ec);

    int w = texture->width, h = texture->height;
    if (w <= 0 || h <= 0)
        return;
    GfxTexture *staging = Gfx_CreateTexture(w, h, GFX_FORMAT_BGRA8, 1, GFX_TEXTURE_STAGING);
    if (staging == NULL)
        return;
    Gfx_CopyTexture(staging, texture);
    GfxLockedRect lock{};
    if (Gfx_LockTexture(staging, 0, 0, &lock)) {
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        for (int y = 0; y < h; y++) {
            const uint8_t *row = (const uint8_t *)lock.pixels + (size_t)y * lock.pitch;
            uint8_t *dst = rgba.data() + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) {
                dst[x * 4 + 0] = row[x * 4 + 2];
                dst[x * 4 + 1] = row[x * 4 + 1];
                dst[x * 4 + 2] = row[x * 4 + 0];
                dst[x * 4 + 3] = row[x * 4 + 3];
            }
        }
        Gfx_UnlockTexture(staging, 0, 0);
        stbi_write_png(outPath.string().c_str(), w, h, 4, rgba.data(), w * 4);
    }
    Gfx_DestroyTexture(staging);
}

} // namespace

extern "C" void TextureOverride_NormalizeName(const char *name, char *relOut, int relSize, char *baseOut, int baseSize)
{
    std::string rel, base;
    SplitNormalized(name, rel, base);
    if (relOut && relSize > 0) {
        snprintf(relOut, (size_t)relSize, "%s", rel.c_str());
    }
    if (baseOut && baseSize > 0) {
        snprintf(baseOut, (size_t)baseSize, "%s", base.c_str());
    }
}

extern "C" void TextureOverride_ResetIndex(void)
{
    s_index.built = false;
    s_index.byRel.clear();
    s_index.byBase.clear();
    s_index.extraRoots.clear();
    s_index.dumped.clear();
    s_index.bc7Loaded = 0;
    s_index.cpuLoaded = 0;
    s_index.totalLoadMs = 0.0;
}

extern "C" void TextureOverride_AddSearchRoot(const char *dir)
{
    if (dir != NULL && dir[0] != '\0') {
        s_index.extraRoots.emplace_back(dir);
        s_index.built = false;
    }
}

extern "C" void TextureOverride_Apply(const char *name, GfxTexture *texture)
{
    if (name == NULL || texture == NULL || (texture->flags & (GFX_TEXTURE_CUBE | GFX_TEXTURE_RENDER_TARGET)))
        return;

    std::string relStem, baseStem;
    SplitNormalized(name, relStem, baseStem);
    if (relStem.empty() || baseStem.empty())
        return;

    DumpTextureIfEnabled(relStem, texture);

    if (Platform_GetSettingInt("video.texture_overrides", 1) == 0)
        return;

    EnsureIndex();
    if (s_index.byRel.empty())
        return;

    std::string albedoPath = LookupFile(relStem, baseStem);
    if (!albedoPath.empty()) {
        GfxTexture *replacement = CreateMipmappedTexture(albedoPath, GFX_TEXTURE_OVERRIDE, false);
        if (replacement != NULL)
            Gfx_SetTextureReplacement(texture, replacement);
    }

    if (Platform_GetSettingInt("video.override_material_maps", 1) != 0) {
        std::string normalPath = LookupFile(relStem + "_n", baseStem + "_n");
        std::string specPath = LookupFile(relStem + "_s", baseStem + "_s");
        GfxTexture *normalTex = !normalPath.empty() ? CreateMipmappedTexture(normalPath, GFX_TEXTURE_OVERRIDE, true)
                                                    : NULL;
        GfxTexture *specTex = !specPath.empty() ? CreateMipmappedTexture(specPath, GFX_TEXTURE_OVERRIDE, false) : NULL;
        if (normalTex != NULL || specTex != NULL)
            Gfx_SetTextureMaterialMaps(texture, normalTex, specTex);
    }
}
