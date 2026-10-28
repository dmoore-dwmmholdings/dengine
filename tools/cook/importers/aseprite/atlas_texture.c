#include "atlas_pack.h"
#include "cook/importers/texture/texture_internal.h"
#include <string.h>

de_result de_aseprite_atlas_encode(const de_aseprite_atlas *atlas, const de_allocator *allocator,
                                   bool pixel_art, void **data, u64 *bytes) {
    if (data)
        *data = NULL;
    if (bytes)
        *bytes = 0;
    if (!atlas || !allocator || !data || !bytes || !atlas->rgba || !atlas->width ||
        !atlas->height || atlas->width > DE_COOKED_TEXTURE_MAX_SIZE ||
        atlas->height > DE_COOKED_TEXTURE_MAX_SIZE ||
        (u64)atlas->width * atlas->height * 4u != atlas->rgba_bytes)
        return DE_ERR_INVALID_ARGUMENT;
    bool alpha = false;
    for (size_t i = 3; i < atlas->rgba_bytes; i += 4u)
        if (atlas->rgba[i] != 255u) {
            alpha = true;
            break;
        }
    de_tex_dtex_desc desc = {.dim = DE_TEX_2D,
                             .format = DE_FMT_RGBA8_SRGB,
                             .width = atlas->width,
                             .height = atlas->height,
                             .layers = 1,
                             .mip_count = 1,
                             .flags = (pixel_art ? DE_COOKED_TEXTURE_PIXEL_ART : 0u) |
                                      (alpha ? DE_COOKED_TEXTURE_HAS_ALPHA : 0u),
                             .usage = DE_COOKED_TEXTURE_USAGE_COLOR};
    de_tex_dtex texture;
    de_result r = de_tex_dtex_begin(allocator, &desc, &texture);
    if (r != DE_OK)
        return r;
    for (u32 y = 0; y < atlas->height; ++y)
        memcpy(texture.data + texture.offset[0] + (size_t)y * texture.row_pitch[0],
               atlas->rgba + (size_t)y * atlas->width * 4u, (size_t)atlas->width * 4u);
    *data = texture.data;
    *bytes = texture.size;
    texture.data = NULL;
    de_tex_dtex_free(allocator, &texture);
    return DE_OK;
}
