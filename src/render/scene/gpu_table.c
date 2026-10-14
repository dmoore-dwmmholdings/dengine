/*
 * gpu_table.c: one persistent GPU table of the GPU scene (SPEC §12.4; DENG-177): a GPU_ONLY
 * storage buffer of fixed-stride entries with its bindless index, a LIFO slot allocator, the
 * dirty set and capacity doubling. gpu_scene.h describes the tables; gpu_scene.c uploads them.
 */
#include "render/scene/gpu_scene.h"

#include <dengine/de_debug.h>

#include <stdlib.h>
#include <string.h>

/* ---------- CPU arrays (grown all or nothing, only when the table grows) ---------- */

static size_t bit_words(u32 capacity) {
    return ((size_t)capacity + 63u) / 64u;
}

/* Grows the CPU arrays from t->capacity entries to `cap` (all or nothing; zero-filled). The
 * shadow grows only when it exists. */
static de_result grow_arrays(de__gpu_table *t, u32 cap) {
    const u32 old = t->capacity;
    const size_t sizes_old[4] = {bit_words(old) * sizeof(u64), (size_t)old * sizeof(u32),
                                 (size_t)old * sizeof(u32), (size_t)old * t->stride};
    const size_t sizes_new[4] = {bit_words(cap) * sizeof(u64), (size_t)cap * sizeof(u32),
                                 (size_t)cap * sizeof(u32), (size_t)cap * t->stride};
    void **slots[4] = {(void **)&t->dirty_bits, (void **)&t->dirty, (void **)&t->free,
                       (void **)&t->shadow};
    const u32 n = t->shadow != NULL || (old == 0 && t->want_shadow) ? 4u : 3u;
    void *blocks[4] = {NULL, NULL, NULL, NULL};
    for (u32 i = 0; i < n; ++i) {
        blocks[i] = DE_ALLOC(t->alloc, sizes_new[i], 16, DE_MEM_RENDER);
        if (blocks[i] == NULL) {
            for (u32 j = 0; j < i; ++j)
                DE_FREE(t->alloc, blocks[j], sizes_new[j]);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "gpu scene: no memory for %u %s entries", cap,
                           t->name);
        }
    }
    for (u32 i = 0; i < n; ++i) {
        memset(blocks[i], 0, sizes_new[i]);
        if (*slots[i] != NULL) {
            memcpy(blocks[i], *slots[i], sizes_old[i]);
            DE_FREE(t->alloc, *slots[i], sizes_old[i]);
        }
        *slots[i] = blocks[i];
    }
    return DE_OK;
}

static de_gpu_buffer make_buffer(de__gpu_table *t, u32 cap) {
    return de_gpu_buffer_create(
        t->dev,
        &(de_gpu_buffer_desc){.size = (u64)cap * t->stride,
                              .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC,
                              .memory = DE_MEM_GPU_ONLY,
                              .stride = t->stride,
                              .name = t->name});
}

de_result de__gpu_table_init(de__gpu_table *t, de_gpu_device *dev, const de_allocator *alloc,
                             const char *name, u32 stride, u32 capacity, bool shadow) {
    memset(t, 0, sizeof *t);
    t->name = name;
    t->dev = dev;
    t->alloc = alloc;
    t->stride = stride;
    if (capacity == 0)
        capacity = 1;
    t->want_shadow = shadow;
    de_result r = grow_arrays(t, capacity);
    if (r != DE_OK)
        return r;
    t->capacity = capacity;
    t->buffer = make_buffer(t, capacity);
    if (!DE_HANDLE_VALID(t->buffer)) {
        const de_error *e = de_error_last();
        return DE_FAIL_WRAP(e != NULL && e->result != DE_OK ? e->result : DE_ERR_INTERNAL,
                            "gpu scene: the %s table (%u x %u bytes)", name, capacity, stride);
    }
    t->bindless = de_gpu_buffer_bindless(dev, t->buffer);
    t->state = DE_ACCESS_NONE;
    return DE_OK;
}

void de__gpu_table_fini(de__gpu_table *t) {
    if (t->dev != NULL) {
        if (DE_HANDLE_VALID(t->buffer))
            de_gpu_buffer_destroy(t->dev, t->buffer);
        if (DE_HANDLE_VALID(t->old))
            de_gpu_buffer_destroy(t->dev, t->old);
    }
    if (t->alloc != NULL) {
        if (t->dirty_bits != NULL)
            DE_FREE(t->alloc, t->dirty_bits, bit_words(t->capacity) * sizeof(u64));
        if (t->dirty != NULL)
            DE_FREE(t->alloc, t->dirty, (size_t)t->capacity * sizeof(u32));
        if (t->free != NULL)
            DE_FREE(t->alloc, t->free, (size_t)t->capacity * sizeof(u32));
        if (t->shadow != NULL)
            DE_FREE(t->alloc, t->shadow, (size_t)t->capacity * t->stride);
    }
    memset(t, 0, sizeof *t);
}

de_result de__gpu_table_reserve(de__gpu_table *t, u32 n) {
    if (n <= t->capacity) {
        if (n > t->count)
            t->count = n;
        return DE_OK;
    }
    u32 cap = t->capacity;
    while (cap < n)
        cap = cap > 0x40000000u ? n : cap * 2u;
    de_gpu_buffer nb = make_buffer(t, cap);
    if (!DE_HANDLE_VALID(nb)) {
        const de_error *e = de_error_last();
        return DE_FAIL_WRAP(e != NULL && e->result != DE_OK ? e->result : DE_ERR_OUT_OF_MEMORY,
                            "gpu scene: growing the %s table to %u entries", t->name, cap);
    }
    const de_result r = grow_arrays(t, cap);
    if (r != DE_OK) {
        de_gpu_buffer_destroy(t->dev, nb);
        return r;
    }
    if (DE_HANDLE_VALID(t->old)) {
        /* Grown twice before an upload: the intermediate buffer was never written. */
        de_gpu_buffer_destroy(t->dev, t->buffer);
    } else {
        t->old = t->buffer;
        t->old_state = t->state;
        t->old_count = t->count;
    }
    t->buffer = nb;
    t->bindless = de_gpu_buffer_bindless(t->dev, nb);
    t->state = DE_ACCESS_NONE;
    t->capacity = cap;
    t->count = n;
    t->growths++;
    return DE_OK;
}

void de__gpu_table_mark(de__gpu_table *t, u32 s) {
    DE_ASSERT(s < t->capacity);
    u64 *w = &t->dirty_bits[s / 64u];
    const u64 bit = (u64)1 << (s % 64u);
    if (*w & bit)
        return;
    *w |= bit;
    t->dirty[t->dirty_count++] = s;
}

void de__gpu_table_mark_all(de__gpu_table *t) {
    t->all_dirty = true;
}

void de__gpu_table_clear_dirty(de__gpu_table *t) {
    if (t->dirty_count * 8u < t->capacity / 64u + 1u) {
        for (u32 i = 0; i < t->dirty_count; ++i)
            t->dirty_bits[t->dirty[i] / 64u] = 0;
    } else {
        memset(t->dirty_bits, 0, bit_words(t->capacity) * sizeof(u64));
    }
    t->dirty_count = 0;
    t->all_dirty = false;
}

static int cmp_u32(const void *a, const void *b) {
    const u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

void de__gpu_table_sort_dirty(de__gpu_table *t) {
    if (t->dirty_count > 1)
        qsort(t->dirty, t->dirty_count, sizeof(u32), cmp_u32);
}

u32 de__gpu_table_alloc(de__gpu_table *t) {
    if (t->free_count != 0)
        return t->free[--t->free_count];
    const u32 s = t->count;
    if (de__gpu_table_reserve(t, s + 1u) != DE_OK)
        return DE__GS_NONE;
    return s;
}

void de__gpu_table_free(de__gpu_table *t, u32 s) {
    DE_ASSERT(s < t->count && t->free_count < t->capacity);
    t->free[t->free_count++] = s;
    de__gpu_table_mark(t, s);
}

void de__gpu_table_retire_old(de__gpu_table *t) {
    if (!DE_HANDLE_VALID(t->old))
        return;
    de_gpu_buffer_destroy(t->dev, t->old);
    t->old = (de_gpu_buffer){0};
    t->old_count = 0;
    t->old_state = DE_ACCESS_NONE;
}

u64 de__gpu_table_bytes(const de__gpu_table *t) {
    return (u64)t->capacity * t->stride;
}
