// Unit tests for Stage 6 external texture override matching, PNG loading,
// companion normal/specular maps (*_n.png, *_s.png), F10 live toggle, and dump_textures.

#include "port/texture_override.h"
#include "port/gfx.h"
#include "render/gfx_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

namespace fs = std::filesystem;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond);                                           \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static bool WriteSolidPng(const fs::path &path, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        rgba[i * 4 + 0] = r;
        rgba[i * 4 + 1] = g;
        rgba[i * 4 + 2] = b;
        rgba[i * 4 + 3] = a;
    }
    return stbi_write_png(path.string().c_str(), w, h, 4, rgba.data(), w * 4) != 0;
}

static void PutLE32(std::vector<uint8_t> &buf, size_t off, uint32_t v)
{
    buf[off + 0] = (uint8_t)(v & 0xffu);
    buf[off + 1] = (uint8_t)((v >> 8) & 0xffu);
    buf[off + 2] = (uint8_t)((v >> 16) & 0xffu);
    buf[off + 3] = (uint8_t)((v >> 24) & 0xffu);
}

static bool WriteSyntheticBC7Dds(const fs::path &path, int w, int h, int mipCount, uint8_t fillTag)
{
    std::vector<uint8_t> data(148, 0);
    memcpy(data.data(), "DDS ", 4);
    PutLE32(data, 4, 124);                                    // dwSize
    PutLE32(data, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000); // dwFlags
    PutLE32(data, 12, (uint32_t)h);                           // dwHeight
    PutLE32(data, 16, (uint32_t)w);                           // dwWidth
    PutLE32(data, 20, (uint32_t)Gfx_LevelSize(GFX_FORMAT_BC7, w, h, NULL)); // dwPitchOrLinearSize
    PutLE32(data, 24, 1);                                     // dwDepth
    PutLE32(data, 28, (uint32_t)mipCount);                    // dwMipMapCount
    PutLE32(data, 76, 32);                                    // ddpf.dwSize
    PutLE32(data, 80, 0x4);                                   // DDPF_FOURCC
    memcpy(data.data() + 84, "DX10", 4);                      // dwFourCC
    PutLE32(data, 108, 0x1000 | 0x400008);                    // dwCaps
    // DDS_HEADER_DXT10
    PutLE32(data, 128, 98);                                   // DXGI_FORMAT_BC7_UNORM
    PutLE32(data, 132, 3);                                    // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    PutLE32(data, 136, 0);                                    // miscFlag
    PutLE32(data, 140, 1);                                    // arraySize
    PutLE32(data, 144, 0);                                    // miscFlags2

    for (int m = 0; m < mipCount; m++) {
        int mw = std::max(1, w >> m);
        int mh = std::max(1, h >> m);
        size_t sz = Gfx_LevelSize(GFX_FORMAT_BC7, mw, mh, NULL);
        size_t pos = data.size();
        data.resize(pos + sz);
        memset(data.data() + pos, (int)((fillTag + m) & 0xff), sz);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out.write((const char *)data.data(), (std::streamsize)data.size());
    return (bool)out;
}

int main()
{
    // 1. Name normalization
    char rel[256], base[256];
    TextureOverride_NormalizeName("Cars\\Ford\\FORDBODF.DDS", rel, sizeof(rel), base, sizeof(base));
    CHECK(strcmp(rel, "cars/ford/fordbodf") == 0);
    CHECK(strcmp(base, "fordbodf") == 0);

    TextureOverride_NormalizeName("\\NEWIMAGE\\OSD\\CODRIV\\ARROWS4.TGA", rel, sizeof(rel), base, sizeof(base));
    CHECK(strcmp(rel, "newimage/osd/codriv/arrows4") == 0);
    CHECK(strcmp(base, "arrows4") == 0);

    TextureOverride_NormalizeName("./Game/Tracks/UK_Tarmac01.DDS", rel, sizeof(rel), base, sizeof(base));
    CHECK(strcmp(rel, "game/tracks/uk_tarmac01") == 0);
    CHECK(strcmp(base, "uk_tarmac01") == 0);

    // 2. Override lookup, mipmap generation, material maps, and F10 toggle
    fs::path tmpDir = fs::temp_directory_path() / "opencmr2_override_test";
    std::error_code ec;
    fs::remove_all(tmpDir, ec);
    fs::create_directories(tmpDir, ec);

    CHECK(WriteSolidPng(tmpDir / "UK_Tarmac01.PNG", 16, 16, 40, 80, 120, 255));
    CHECK(WriteSolidPng(tmpDir / "uk_tarmac01_n.png", 16, 16, 128, 128, 255, 255));
    CHECK(WriteSolidPng(tmpDir / "uk_tarmac01_s.png", 16, 16, 180, 220, 255, 255));

    TextureOverride_ResetIndex();
    TextureOverride_AddSearchRoot(tmpDir.string().c_str());

    GfxTexture *orig = Gfx_CreateTexture(8, 8, GFX_FORMAT_BGRA8, 1, 0);
    CHECK(orig != NULL);

    TextureOverride_Apply("Tracks\\UK_Tarmac01.DDS", orig);

    GfxTexture *drawnOverhaul = Gfx_DrawnTexture(orig, true);
    GfxTexture *drawnVanilla = Gfx_DrawnTexture(orig, false);
    CHECK(drawnOverhaul != orig);
    CHECK(drawnVanilla == orig);
    CHECK((drawnOverhaul->flags & GFX_TEXTURE_OVERRIDE) != 0);
    CHECK(drawnOverhaul->width == 16 && drawnOverhaul->height == 16);
    CHECK(drawnOverhaul->mipLevels == 5); // 16 -> 8 -> 4 -> 2 -> 1

    // Check BGRA pixel order of loaded override (R=40, G=80, B=120, A=255 -> BGRA = 120, 80, 40, 255)
    CHECK(drawnOverhaul->levels[0][0].size() == 16 * 16 * 4);
    CHECK(drawnOverhaul->levels[0][0][0] == 120);
    CHECK(drawnOverhaul->levels[0][0][1] == 80);
    CHECK(drawnOverhaul->levels[0][0][2] == 40);
    CHECK(drawnOverhaul->levels[0][0][3] == 255);

    // Check companion normal & specular maps
    CHECK(orig->normalMap != NULL);
    CHECK(orig->specularMap != NULL);
    CHECK(orig->normalMap->width == 16 && orig->normalMap->mipLevels == 5);
    CHECK(orig->specularMap->width == 16 && orig->specularMap->mipLevels == 5);

    Gfx_DestroyTexture(orig);
    TextureOverride_ResetIndex();

    // 3. Stage 10: Native GPU DDS DX10 BC7 loading with pre-generated mipmap chain
    //    and .dds priority over .png
    fs::path bc7Dir = tmpDir / "bc7";
    fs::create_directories(bc7Dir, ec);
    CHECK(WriteSolidPng(bc7Dir / "fordbodf.png", 8, 8, 255, 0, 0, 255));
    CHECK(WriteSyntheticBC7Dds(bc7Dir / "fordbodf.dds", 16, 16, 5, 0x40));
    CHECK(WriteSyntheticBC7Dds(bc7Dir / "fordbodf_n.dds", 16, 16, 5, 0x80));
    CHECK(WriteSyntheticBC7Dds(bc7Dir / "fordbodf_s.dds", 16, 16, 5, 0xC0));

    TextureOverride_AddSearchRoot(bc7Dir.string().c_str());
    GfxTexture *carOrig = Gfx_CreateTexture(8, 8, GFX_FORMAT_BC1, 1, 0);
    CHECK(carOrig != NULL);
    TextureOverride_Apply("Cars\\Ford\\FORDBODF.DDS", carOrig);

    GfxTexture *carOverhaul = Gfx_DrawnTexture(carOrig, true);
    GfxTexture *carVanilla = Gfx_DrawnTexture(carOrig, false);
    CHECK(carOverhaul != carOrig);
    CHECK(carVanilla == carOrig);
    CHECK(carOverhaul->format == GFX_FORMAT_BC7);
    CHECK(carOverhaul->width == 16 && carOverhaul->height == 16);
    CHECK(carOverhaul->mipLevels == 5);
    CHECK((carOverhaul->flags & GFX_TEXTURE_OVERRIDE) != 0);
    // Mip 0: 16x16 = 4x4 blocks = 256 bytes; mip 4: 1x1 = 1 block = 16 bytes.
    CHECK(carOverhaul->levels[0][0].size() == 256);
    CHECK(carOverhaul->levels[0][0][0] == 0x40);
    CHECK(carOverhaul->levels[0][4].size() == 16);
    CHECK(carOverhaul->levels[0][4][0] == 0x44);

    // Companion _n.dds and _s.dds also load natively as BC7 with full mip chains
    CHECK(carOrig->normalMap != NULL && carOrig->normalMap->format == GFX_FORMAT_BC7);
    CHECK(carOrig->normalMap->mipLevels == 5 && carOrig->normalMap->levels[0][0][0] == 0x80);
    CHECK(carOrig->specularMap != NULL && carOrig->specularMap->format == GFX_FORMAT_BC7);
    CHECK(carOrig->specularMap->mipLevels == 5 && carOrig->specularMap->levels[0][0][0] == 0xC0);

    Gfx_DestroyTexture(carOrig);
    TextureOverride_ResetIndex();
    fs::remove_all(tmpDir, ec);
    return 0;
}
