/*
 * de_cooked_le.h: little-endian field helpers shared by the cooked render formats
 * (de_cooked_mesh.h, de_cooked_material.h, de_cooked_model.h; DENG-101). Internal; include as
 * "render/formats/de_cooked_le.h".
 *
 * Cooked formats are written by the cooker (tools/cook/importers/) and read by the runtime loaders
 * (DENG-105). Every multi-byte field is little-endian and goes through these helpers, never a
 * struct memcpy, so the bytes are the same on every host and a cook is byte-identical everywhere.
 * Floats are stored as their IEEE-754 bit patterns.
 */
#ifndef DE_RENDER_FORMATS_COOKED_LE_H
#define DE_RENDER_FORMATS_COOKED_LE_H

#include <dengine/de_base.h>

#include <string.h>

DE_EXTERN_C_BEGIN

static inline u32 de_cooked_get_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static inline u64 de_cooked_get_u64(const u8 *p) {
    return (u64)de_cooked_get_u32(p) | ((u64)de_cooked_get_u32(p + 4) << 32);
}

static inline f32 de_cooked_get_f32(const u8 *p) {
    u32 b = de_cooked_get_u32(p);
    f32 f;
    memcpy(&f, &b, 4);
    return f;
}

static inline void de_cooked_put_u32(u8 *p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static inline void de_cooked_put_u64(u8 *p, u64 v) {
    de_cooked_put_u32(p, (u32)v);
    de_cooked_put_u32(p + 4, (u32)(v >> 32));
}

static inline void de_cooked_put_f32(u8 *p, f32 f) {
    u32 b;
    memcpy(&b, &f, 4);
    de_cooked_put_u32(p, b);
}

/** True if [off, off + len) lies inside a blob of `size` bytes (no overflow). */
static inline bool de_cooked_range_ok(u64 size, u64 off, u64 len) {
    return off <= size && len <= size - off;
}

DE_EXTERN_C_END

#endif /* DE_RENDER_FORMATS_COOKED_LE_H */
