// External HD texture override system (overrides/) and companion material maps
// (*_n.png normal maps, *_s.png specular/reflection masks) + dump_textures export.
#ifndef OPENCMR2_TEXTURE_OVERRIDE_H
#define OPENCMR2_TEXTURE_OVERRIDE_H

#include "port/gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called after a game DDS/TGA texture has been loaded into surface.
   - If video.dump_textures = 1, dumps the original texture to <user_dir>/dumped_textures/.
   - If video.texture_overrides = 1, looks for a matching PNG/DDS/TGA replacement
     in [paths] overrides, <user_dir>/overrides, or ./overrides, attaching it with
     GFX_TEXTURE_OVERRIDE so F10 toggles it live.
   - If video.override_material_maps = 1, attaches optional <name>_n.png and <name>_s.png. */
void TextureOverride_Apply(const char *name, GfxTexture *texture);

/* Normalizes a game texture path into lowercase relative stem (forward slashes,
   no extension) and basename stem. Exposed for unit tests and tools. */
void TextureOverride_NormalizeName(const char *name, char *relOut, int relSize, char *baseOut, int baseSize);

/* Test helpers: register an extra highest-priority search root and invalidate the index. */
void TextureOverride_ResetIndex(void);
void TextureOverride_AddSearchRoot(const char *dir);

#ifdef __cplusplus
}
#endif

#endif
