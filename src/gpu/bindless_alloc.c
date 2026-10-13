/* bindless_alloc.c — bindless descriptor index allocation (DENG-281).
 *
 * One allocator per index space (sampled textures, storage textures, buffers, samplers,
 * acceleration structures), sized from the device caps. Index 0 of every space is reserved
 * (DE_GPU_BINDLESS_INVALID): backends put a null descriptor there, so zeroed push constants read
 * a harmless default instead of an unwritten slot. Released indices go on a LIFO stack and are
 * handed out again first; they reach it only through deferred destruction, after the GPU is done
 * with every submission that could read them.
 *
 * Shared numbering (DENG-285): a backend whose shaders index one descriptor heap with every
 * resource space (D3D12 ResourceDescriptorHeap) sets the vtable's bindless_shared_resource_heap;
 * then each resource space hands out its own range of one numbering ([first, capacity)), so an
 * index is a heap slot and two spaces never share one.
 */
#include "gpu/gpu_backend.h"

#include <string.h>

static const char *const k_space_names[DE__GPU_BINDLESS_SPACE_COUNT] = {
    "sampled texture", "storage texture", "buffer", "sampler", "acceleration structure",
};

void de__gpu_bindless_init(de__gpu_bindless *b, const de_allocator *alloc,
                           const de_gpu_caps *caps) {
    memset(b, 0, sizeof *b);
    b->alloc = alloc ? alloc : de_heap_allocator();
    de_mutex_init(&b->lock);
    const u32 caps_of[DE__GPU_BINDLESS_SPACE_COUNT] = {
        caps->max_bindless_textures, caps->max_bindless_storage_textures,
        caps->max_bindless_buffers,  caps->max_bindless_samplers,
        caps->max_bindless_accels,
    };
    for (u32 i = 0; i < DE__GPU_BINDLESS_SPACE_COUNT; ++i) {
        b->spaces[i].capacity = caps_of[i];
        b->spaces[i].next = 1; /* 0 is reserved */
        b->spaces[i].first = 1;
    }
}

void de__gpu_bindless_share_resource_numbering(de__gpu_bindless *b) {
    static const de__gpu_bindless_space order[] = {DE__GPU_BINDLESS_SAMPLED_TEXTURE,
                                                   DE__GPU_BINDLESS_STORAGE_TEXTURE,
                                                   DE__GPU_BINDLESS_BUFFER, DE__GPU_BINDLESS_ACCEL};
    /* The first space keeps 1 .. T-1 (slot 0 is the reserved null descriptor); each later space
     * starts where the previous one ends and uses its whole size. */
    u32 end = b->spaces[order[0]].capacity;
    for (u32 i = 1; i < sizeof order / sizeof order[0]; ++i) {
        de__gpu_index_space *s = &b->spaces[order[i]];
        DE_ASSERT(s->next == s->first && s->live == 0);
        const u32 size = s->capacity;
        s->first = s->next = end;
        s->capacity = end + size;
        end = s->capacity;
    }
}

void de__gpu_bindless_destroy(de__gpu_bindless *b) {
    for (u32 i = 0; i < DE__GPU_BINDLESS_SPACE_COUNT; ++i) {
        de__gpu_index_space *s = &b->spaces[i];
        if (s->free)
            DE_FREE(b->alloc, s->free, sizeof(u32) * s->free_cap);
    }
    de_mutex_destroy(&b->lock);
    memset(b, 0, sizeof *b);
}

u32 de__gpu_bindless_alloc(de__gpu_bindless *b, de__gpu_bindless_space space) {
    de__gpu_index_space *s = &b->spaces[space];
    u32 index = DE_GPU_BINDLESS_INVALID;
    de_mutex_lock(&b->lock);
    if (s->free_count > 0)
        index = s->free[--s->free_count];
    else if (s->next < s->capacity)
        index = s->next++;
    if (index != DE_GPU_BINDLESS_INVALID)
        ++s->live;
    u32 live = s->live, first = s->first, end = s->capacity;
    de_mutex_unlock(&b->lock);
    if (index == DE_GPU_BINDLESS_INVALID)
        DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                "bindless %s table is full (%u live, indices %u to %u; index 0 is "
                "reserved)",
                k_space_names[space], live, first, end ? end - 1 : 0);
    return index;
}

void de__gpu_bindless_free(de__gpu_bindless *b, de__gpu_bindless_space space, u32 index) {
    if (index == DE_GPU_BINDLESS_INVALID)
        return;
    de__gpu_index_space *s = &b->spaces[space];
    de_mutex_lock(&b->lock);
    DE_ASSERT(index >= s->first && index < s->next && s->live > 0);
    if (s->free_count == s->free_cap) {
        /* Grows to at most the number of indices ever handed out; amortised O(1). */
        u32 cap = s->free_cap ? s->free_cap * 2u : 256u;
        u32 *p = (u32 *)DE_REALLOC(b->alloc, s->free, sizeof(u32) * s->free_cap, sizeof(u32) * cap,
                                   DE_ALIGNOF(u32), DE_MEM_RENDER);
        if (!p) {
            /* The index leaks (stays reserved); nothing else breaks. */
            --s->live;
            de_mutex_unlock(&b->lock);
            return;
        }
        s->free = p;
        s->free_cap = cap;
    }
    s->free[s->free_count++] = index;
    --s->live;
    de_mutex_unlock(&b->lock);
}

u32 de__gpu_bindless_live(de__gpu_bindless *b, de__gpu_bindless_space space) {
    de_mutex_lock(&b->lock);
    u32 n = b->spaces[space].live;
    de_mutex_unlock(&b->lock);
    return n;
}
