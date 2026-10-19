#ifndef DE_COOKED_SPRITE_PROXY_H
#define DE_COOKED_SPRITE_PROXY_H
#include <dengine/de_base.h>
#include <string.h>
#include <math.h>
#include "render/formats/de_cooked_le.h"
#define DE_SPRITE_PROXY_MAX_VERTICES 32u
#define DE_SPRITE_PROXY_MAX_INDICES  192u
#define DE_SPRITE_PROXY_BYTES                                                                      \
    (32u + DE_SPRITE_PROXY_MAX_VERTICES * 12u + DE_SPRITE_PROXY_MAX_INDICES * 4u)
typedef struct de_cooked_sprite_proxy {
    u32 vertex_count, index_count;
    f32 region[4]; /* normalized texture UV rectangle, before sprite flips */
    f32 positions[DE_SPRITE_PROXY_MAX_VERTICES][3];
    u32 indices[DE_SPRITE_PROXY_MAX_INDICES];
} de_cooked_sprite_proxy;
static inline void de_sprite_proxy_write(u8 *p, const de_cooked_sprite_proxy *proxy) {
    memset(p, 0, DE_SPRITE_PROXY_BYTES);
    memcpy(p, "PXRT", 4);
    de_cooked_put_u32(p + 4, 1);
    de_cooked_put_u32(p + 8, proxy->vertex_count);
    de_cooked_put_u32(p + 12, proxy->index_count);
    for (u32 i = 0; i < proxy->vertex_count; ++i)
        for (u32 j = 0; j < 3; ++j)
            de_cooked_put_f32(p + 16 + i * 12 + j * 4, proxy->positions[i][j]);
    for (u32 i = 0; i < proxy->index_count; ++i)
        de_cooked_put_u32(p + 400 + i * 4, proxy->indices[i]);
    for (u32 i = 0; i < 4; ++i)
        de_cooked_put_f32(p + 1168 + i * 4, proxy->region[i]);
}
static inline bool de_sprite_proxy_parse(const u8 *p, u64 bytes, de_cooked_sprite_proxy *out) {
    if (bytes != DE_SPRITE_PROXY_BYTES || memcmp(p, "PXRT", 4) || de_cooked_get_u32(p + 4) != 1)
        return false;
    memset(out, 0, sizeof *out);
    for (u32 i = 0; i < 4; ++i) {
        out->region[i] = de_cooked_get_f32(p + 1168 + i * 4);
        if (!isfinite(out->region[i]))
            return false;
    }
    if (out->region[0] < 0 || out->region[1] < 0 || out->region[2] <= 0 || out->region[3] <= 0 ||
        out->region[0] + out->region[2] > 1 || out->region[1] + out->region[3] > 1)
        return false;
    out->vertex_count = de_cooked_get_u32(p + 8);
    out->index_count = de_cooked_get_u32(p + 12);
    if (out->vertex_count < 6 || out->vertex_count > 32 || out->index_count > 192 ||
        out->index_count % 3)
        return false;
    for (u32 i = 0; i < out->vertex_count; ++i)
        for (u32 j = 0; j < 3; ++j) {
            out->positions[i][j] = de_cooked_get_f32(p + 16 + i * 12 + j * 4);
            if (!isfinite(out->positions[i][j]))
                return false;
        }
    for (u32 i = 0; i < out->index_count; ++i) {
        out->indices[i] = de_cooked_get_u32(p + 400 + i * 4);
        if (out->indices[i] >= out->vertex_count)
            return false;
    }
    return true;
}
#endif
