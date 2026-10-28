#ifndef DE_ANIM_SPRITE_CLIP_FORMAT_H
#define DE_ANIM_SPRITE_CLIP_FORMAT_H
#include "render/formats/de_cooked_le.h"
#include <string.h>
/* DSCP: canonical little-endian data, no pointers or native padding.
 * v1 remains header64/frame32/tag24/event16. v2 adds a header96, frame64
 * geometry and keyed slice64 records; tag byte20 stores the traversal repeat count.
 * Tags sort by name hash; events and slice keys have canonical sorted identities. */
#define DE__SPRITE_CLIP_VERSION          1u
#define DE__SPRITE_CLIP_GEOMETRY_VERSION 2u
#define DE__SPRITE_FRAME_LIMIT           65535u
#define DE__SPRITE_TAG_LIMIT             4096u
#define DE__SPRITE_EVENT_LIMIT           65535u
#define DE__SPRITE_SLICE_LIMIT           65535u
typedef struct de__sprite_frame {
    f32 rect[4], pivot[2];
    u32 ms;
    u32 trim[4];
    f32 size[2], source_pivot[2];
} de__sprite_frame;
typedef struct de__sprite_tag {
    u64 name;
    u32 from, to, direction, repeats;
} de__sprite_tag;
typedef struct de__sprite_slice {
    u64 name;
    u32 frame, flags;
    i32 x, y, center_x, center_y, pivot_x, pivot_y;
    u32 width, height, center_width, center_height;
} de__sprite_slice;
typedef struct de__sprite_event {
    u64 name;
    u32 frame;
} de__sprite_event;
typedef struct de__sprite_clip_view {
    const u8 *frames, *tags, *events, *slices;
    u32 frame_count, tag_count, event_count, width, height;
    u32 version, frame_stride, canvas_width, canvas_height, slice_count;
    f32 pixels_per_meter;
    u64 atlas, total_ms;
} de__sprite_clip_view;
static inline de__sprite_frame de__sprite_frame_at(const de__sprite_clip_view *v, u32 i) {
    const u8 *p = v->frames + (size_t)i * v->frame_stride;
    de__sprite_frame f = {.ms = de_cooked_get_u32(p + 16)};
    for (u32 k = 0; k < 4; ++k)
        f.rect[k] = de_cooked_get_f32(p + k * 4);
    f.pivot[0] = de_cooked_get_f32(p + 20);
    f.pivot[1] = de_cooked_get_f32(p + 24);
    if (v->version == DE__SPRITE_CLIP_GEOMETRY_VERSION) {
        for (u32 k = 0; k < 4; ++k)
            f.trim[k] = de_cooked_get_u32(p + 32 + k * 4);
        for (u32 k = 0; k < 2; ++k) {
            f.size[k] = de_cooked_get_f32(p + 48 + k * 4);
            f.source_pivot[k] = de_cooked_get_f32(p + 56 + k * 4);
        }
    }
    return f;
}
static inline de__sprite_tag de__sprite_tag_at(const de__sprite_clip_view *v, u32 i) {
    const u8 *p = v->tags + (size_t)i * 24;
    return (de__sprite_tag){.name = de_cooked_get_u64(p),
                            .from = de_cooked_get_u32(p + 8),
                            .to = de_cooked_get_u32(p + 12),
                            .direction = de_cooked_get_u32(p + 16),
                            .repeats = de_cooked_get_u32(p + 20)};
}
static inline i32 de__sprite_i32(const u8 *p) {
    u32 bits = de_cooked_get_u32(p);
    i32 value;
    memcpy(&value, &bits, sizeof value);
    return value;
}
static inline de__sprite_slice de__sprite_slice_at(const de__sprite_clip_view *v, u32 i) {
    const u8 *p = v->slices + (size_t)i * 64;
    return (de__sprite_slice){.name = de_cooked_get_u64(p),
                              .frame = de_cooked_get_u32(p + 8),
                              .flags = de_cooked_get_u32(p + 12),
                              .x = de__sprite_i32(p + 16),
                              .y = de__sprite_i32(p + 20),
                              .width = de_cooked_get_u32(p + 24),
                              .height = de_cooked_get_u32(p + 28),
                              .center_x = de__sprite_i32(p + 32),
                              .center_y = de__sprite_i32(p + 36),
                              .center_width = de_cooked_get_u32(p + 40),
                              .center_height = de_cooked_get_u32(p + 44),
                              .pivot_x = de__sprite_i32(p + 48),
                              .pivot_y = de__sprite_i32(p + 52)};
}
static inline de__sprite_event de__sprite_event_at(const de__sprite_clip_view *v, u32 i) {
    const u8 *p = v->events + (size_t)i * 16;
    return (de__sprite_event){de_cooked_get_u64(p + 8), de_cooked_get_u32(p)};
}
static inline bool de__sprite_finite(f32 x) {
    u32 bits;
    memcpy(&bits, &x, 4);
    return (bits & 0x7f800000u) != 0x7f800000u;
}
static inline bool de__sprite_clip_parse(const void *bytes, u64 size, de__sprite_clip_view *out,
                                         const char **why) {
    if (out)
        memset(out, 0, sizeof *out);
    if (why)
        *why = "invalid sprite clip";
    if (!bytes || !out || size < 64 || size > UINT32_MAX)
        return false;
    const u8 *p = bytes;
    u32 version = de_cooked_get_u32(p + 4);
    bool geometry = version == DE__SPRITE_CLIP_GEOMETRY_VERSION;
    u32 header = geometry ? 96u : 64u, stride = geometry ? 64u : 32u;
    if (memcmp(p, "DSCP", 4) || (version != DE__SPRITE_CLIP_VERSION && !geometry) ||
        size < header || de_cooked_get_u32(p + 8) != size || de_cooked_get_u32(p + 44))
        return false;
    u32 nf = de_cooked_get_u32(p + 12), nt = de_cooked_get_u32(p + 16),
        ne = de_cooked_get_u32(p + 20);
    if (!nf || nf > DE__SPRITE_FRAME_LIMIT || nt > DE__SPRITE_TAG_LIMIT ||
        ne > DE__SPRITE_EVENT_LIMIT)
        return false;
    u64 tags = header + (u64)nf * stride, events = tags + (u64)nt * 24;
    u64 slices = events + (u64)ne * 16;
    u32 ns = geometry ? de_cooked_get_u32(p + 76) : 0;
    if (ns > DE__SPRITE_SLICE_LIMIT || de_cooked_get_u32(p + 24) != header ||
        de_cooked_get_u32(p + 28) != tags || de_cooked_get_u32(p + 32) != events ||
        slices + (u64)ns * 64 != size)
        return false;
    de__sprite_clip_view v = {.frames = p + header,
                              .tags = p + tags,
                              .events = p + events,
                              .frame_count = nf,
                              .tag_count = nt,
                              .event_count = ne,
                              .width = de_cooked_get_u32(p + 36),
                              .height = de_cooked_get_u32(p + 40),
                              .atlas = de_cooked_get_u64(p + 48),
                              .total_ms = de_cooked_get_u64(p + 56),
                              .version = version,
                              .frame_stride = stride,
                              .slices = p + slices,
                              .slice_count = ns};
    if (geometry) {
        v.canvas_width = de_cooked_get_u32(p + 64);
        v.canvas_height = de_cooked_get_u32(p + 68);
        v.pixels_per_meter = de_cooked_get_f32(p + 72);
        if (!v.canvas_width || !v.canvas_height || v.canvas_width > 65535u ||
            v.canvas_height > 65535u || !de__sprite_finite(v.pixels_per_meter) ||
            !(v.pixels_per_meter > 0) || de_cooked_get_u32(p + 80) != slices ||
            de_cooked_get_u32(p + 84) || de_cooked_get_u32(p + 88) || de_cooked_get_u32(p + 92))
            return false;
    }
    if (!v.width || !v.height || !v.atlas)
        return false;
    u64 duration = 0;
    for (u32 i = 0; i < nf; ++i) {
        de__sprite_frame f = de__sprite_frame_at(&v, i);
        if (!f.ms || !(f.rect[0] >= 0 && f.rect[1] >= 0 && f.rect[2] > 0 && f.rect[3] > 0) ||
            !(f.rect[0] + f.rect[2] <= 1.000001f && f.rect[1] + f.rect[3] <= 1.000001f) ||
            !de__sprite_finite(f.pivot[0]) || !de__sprite_finite(f.pivot[1]) ||
            de_cooked_get_u32(v.frames + (size_t)i * stride + 28))
            return false;
        if (geometry) {
            if (f.ms > 65535u || (u64)f.trim[0] + f.trim[2] > v.canvas_width ||
                (u64)f.trim[1] + f.trim[3] > v.canvas_height || ((!f.trim[2]) != (!f.trim[3])))
                return false;
            for (u32 k = 0; k < 2; ++k)
                if (!de__sprite_finite(f.size[k]) || !(f.size[k] >= 0) ||
                    (f.trim[k + 2] && !(f.size[k] > 0)) || !de__sprite_finite(f.source_pivot[k]) ||
                    f.size[k] != (f32)f.trim[k + 2] / v.pixels_per_meter)
                    return false;
        }
        duration += f.ms;
    }
    if (duration != v.total_ms)
        return false;
    u64 previous = 0;
    for (u32 i = 0; i < nt; ++i) {
        de__sprite_tag t = de__sprite_tag_at(&v, i);
        if (!t.name || t.name <= previous || t.from > t.to || t.to >= nf ||
            t.direction > (geometry ? 3u : 2u) || t.repeats > (geometry ? 65535u : 0u))
            return false;
        previous = t.name;
    }
    de__sprite_slice previous_slice = {0};
    for (u32 i = 0; i < ns; ++i) {
        de__sprite_slice s = de__sprite_slice_at(&v, i);
        if (!s.name || s.frame >= nf || (s.flags & ~3u) ||
            de_cooked_get_u64(v.slices + (size_t)i * 64 + 56) ||
            (i && (s.name < previous_slice.name ||
                   (s.name == previous_slice.name &&
                    (s.frame <= previous_slice.frame || s.flags != previous_slice.flags)))))
            return false;
        if (!(s.flags & 1u) && (s.center_x || s.center_y || s.center_width || s.center_height))
            return false;
        if (!(s.flags & 2u) && (s.pivot_x || s.pivot_y))
            return false;
        previous_slice = s;
    }
    de__sprite_event previous_event = {0};
    for (u32 i = 0; i < ne; ++i) {
        de__sprite_event e = de__sprite_event_at(&v, i);
        if (!e.name || e.frame >= nf ||
            (i && (e.frame < previous_event.frame ||
                   (e.frame == previous_event.frame && e.name <= previous_event.name))) ||
            de_cooked_get_u32(v.events + (size_t)i * 16 + 4))
            return false;
        previous_event = e;
    }
    *out = v;
    if (why)
        *why = NULL;
    return true;
}
#endif
