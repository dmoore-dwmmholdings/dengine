#include "render/probes/probes_internal.h"
#include <string.h>
u64 de__probe_payload_size(u32 resolution) {
    u64 n = 0;
    for (u32 r = resolution; r; r >>= 1)
        n += (u64)r * r * 6u * 8u;
    return n;
}
de_result de__probe_store_init(de__probe_store *s, de_gpu_device *dev, const de_allocator *a,
                               u32 resolution, u32 capacity) {
    if (!s || !dev || !a || (resolution != 64 && resolution != 128 && resolution != 256) ||
        !capacity || capacity > DE__PROBE_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "probe store: invalid descriptor");
    memset(s, 0, sizeof *s);
    s->dev = dev;
    s->alloc = a;
    s->resolution = resolution;
    s->capacity = capacity;
    for (u32 r = resolution; r; r >>= 1)
        ++s->mips;
    s->cube = de_gpu_texture_create(
        dev,
        &(de_gpu_texture_desc){.dim = DE_TEX_CUBE,
                               .format = DE_FMT_RGBA16_FLOAT,
                               .width = resolution,
                               .height = resolution,
                               .depth_or_layers = capacity * 6u,
                               .mip_count = s->mips,
                               .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC,
                               .name = "probe.cube_pool"});
    if (!DE_HANDLE_VALID(s->cube)) {
        memset(s, 0, sizeof *s);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "probe store: cube pool");
    }
    return DE_OK;
}
void de__probe_store_free(de__probe_store *s, u32 slot) {
    if (!s || slot >= s->capacity)
        return;
    de__probe_slot *p = &s->slots[slot];
    de_gpu_texture_destroy(s->dev, p->strip);
    u32 generation = p->generation + 1u;
    memset(p, 0, sizeof *p);
    p->generation = generation ? generation : 1u;
}
void de__probe_store_destroy(de__probe_store *s) {
    if (!s)
        return;
    for (u32 i = 0; i < s->capacity; ++i)
        de__probe_store_free(s, i);
    de_gpu_texture_destroy(s->dev, s->cube);
    memset(s, 0, sizeof *s);
}
de__probe_handle de__probe_store_alloc_resolution(de__probe_store *s, u64 owner, u64 frame,
                                                  u32 resolution) {
    if (!s || !owner || !s->capacity ||
        (resolution != 64 && resolution != 128 && resolution != 256) || resolution > s->resolution)
        return (de__probe_handle){DE__PROBE_NONE, 0};
    for (u32 i = 0; i < s->capacity; ++i)
        if (s->slots[i].owner == owner) {
            if (s->slots[i].resolution == resolution) {
                s->slots[i].used = frame;
                return (de__probe_handle){i, s->slots[i].generation};
            }
            de__probe_store_free(s, i);
            break;
        }
    u32 slot = 0;
    for (u32 i = 0; i < s->capacity; ++i) {
        if (!s->slots[i].owner) {
            slot = i;
            break;
        }
        if (s->slots[i].used < s->slots[slot].used)
            slot = i;
    }
    de__probe_store_free(s, slot);
    de__probe_slot *p = &s->slots[slot];
    p->resolution = resolution;
    for (u32 n = resolution; n; n >>= 1)
        ++p->mips;
    for (u32 n = s->resolution; n > resolution; n >>= 1)
        ++p->mip_base;
    p->strip = de_gpu_texture_create(
        s->dev, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                       .width = resolution,
                                       .height = resolution * 6u,
                                       .mip_count = p->mips,
                                       .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE |
                                                DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                                       .name = "probe.filtered_strip"});
    if (!DE_HANDLE_VALID(p->strip))
        return (de__probe_handle){DE__PROBE_NONE, 0};
    p->owner = owner;
    p->used = frame;
    return (de__probe_handle){slot, p->generation};
}

bool de__probe_store_valid(const de__probe_store *s, de__probe_handle h) {
    return s && h.slot < s->capacity && s->slots[h.slot].owner &&
           s->slots[h.slot].generation == h.generation;
}

de__probe_handle de__probe_store_alloc(de__probe_store *s, u64 owner, u64 frame) {
    return de__probe_store_alloc_resolution(s, owner, frame, s ? s->resolution : 0);
}
