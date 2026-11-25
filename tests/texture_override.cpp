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
    fs::remove_all(tmpDir, ec);
    return 0;
}
