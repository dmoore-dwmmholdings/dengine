#ifndef DE_ANIM_SPRITE_CLIP_FORMAT_H
#define DE_ANIM_SPRITE_CLIP_FORMAT_H
#include "render/formats/de_cooked_le.h"
#include <string.h>
/* DSCP v1: canonical little-endian header64/frame32/tag24/event16 bytes.
 * Tags sort by name hash; events sort by frame then name. No stored pointers/padding. */
#define DE__SPRITE_CLIP_VERSION 1u
#define DE__SPRITE_FRAME_LIMIT  65535u
#define DE__SPRITE_TAG_LIMIT    4096u
#define DE__SPRITE_EVENT_LIMIT  65535u
typedef struct de__sprite_frame {
    f32 rect[4], pivot[2];
    u32 ms;
} de__sprite_frame;
typedef struct de__sprite_tag {
    u64 name;
    u32 from, to, direction;
} de__sprite_tag;
typedef struct de__sprite_event {
    u64 name;
    u32 frame;
} de__sprite_event;
typedef struct de__sprite_clip_view {
    const u8 *frames, *tags, *events;
    u32 frame_count, tag_count, event_count, width, height;
    u64 atlas, total_ms;
} de__sprite_clip_view;
static inline de__sprite_frame de__sprite_frame_at(const de__sprite_clip_view *v, u32 i) {
    const u8 *p = v->frames + (size_t)i * 32;
    de__sprite_frame f = {.ms = de_cooked_get_u32(p + 16)};
    for (u32 k = 0; k < 4; ++k)
        f.rect[k] = de_cooked_get_f32(p + k * 4);
    f.pivot[0] = de_cooked_get_f32(p + 20);
    f.pivot[1] = de_cooked_get_f32(p + 24);
    return f;
}
static inline de__sprite_tag de__sprite_tag_at(const de__sprite_clip_view *v, u32 i) {
    const u8 *p = v->tags + (size_t)i * 24;
    return (de__sprite_tag){de_cooked_get_u64(p), de_cooked_get_u32(p + 8),
                            de_cooked_get_u32(p + 12), de_cooked_get_u32(p + 16)};
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
    if (memcmp(p, "DSCP", 4) || de_cooked_get_u32(p + 4) != DE__SPRITE_CLIP_VERSION ||
        de_cooked_get_u32(p + 8) != size || de_cooked_get_u32(p + 44))
        return false;
    u32 nf = de_cooked_get_u32(p + 12), nt = de_cooked_get_u32(p + 16),
        ne = de_cooked_get_u32(p + 20);
    if (!nf || nf > DE__SPRITE_FRAME_LIMIT || nt > DE__SPRITE_TAG_LIMIT ||
        ne > DE__SPRITE_EVENT_LIMIT)
        return false;
    u64 tags = 64 + (u64)nf * 32, events = tags + (u64)nt * 24;
    if (de_cooked_get_u32(p + 24) != 64 || de_cooked_get_u32(p + 28) != tags ||
        de_cooked_get_u32(p + 32) != events || events + (u64)ne * 16 != size)
        return false;
    de__sprite_clip_view v = {.frames = p + 64,
                              .tags = p + tags,
                              .events = p + events,
                              .frame_count = nf,
                              .tag_count = nt,
                              .event_count = ne,
                              .width = de_cooked_get_u32(p + 36),
                              .height = de_cooked_get_u32(p + 40),
                              .atlas = de_cooked_get_u64(p + 48),
                              .total_ms = de_cooked_get_u64(p + 56)};
    if (!v.width || !v.height || !v.atlas)
        return false;
    u64 duration = 0;
    for (u32 i = 0; i < nf; ++i) {
        de__sprite_frame f = de__sprite_frame_at(&v, i);
        if (!f.ms || !(f.rect[0] >= 0 && f.rect[1] >= 0 && f.rect[2] > 0 && f.rect[3] > 0) ||
            !(f.rect[0] + f.rect[2] <= 1.000001f && f.rect[1] + f.rect[3] <= 1.000001f) ||
            !de__sprite_finite(f.pivot[0]) || !de__sprite_finite(f.pivot[1]) ||
            de_cooked_get_u32(v.frames + (size_t)i * 32 + 28))
            return false;
        duration += f.ms;
    }
    if (duration != v.total_ms)
        return false;
    u64 previous = 0;
    for (u32 i = 0; i < nt; ++i) {
        de__sprite_tag t = de__sprite_tag_at(&v, i);
        if (!t.name || t.name <= previous || t.from > t.to || t.to >= nf || t.direction > 2 ||
            de_cooked_get_u32(v.tags + (size_t)i * 24 + 20))
            return false;
        previous = t.name;
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
