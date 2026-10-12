/* handle_pool.c — generational slot pools behind the de_gpu handles (DENG-281).
 * See handle_pool.h. */
#include "gpu/handle_pool.h"

#include <string.h>

enum { SLOT_FREE = 0, SLOT_LIVE = 1, SLOT_RETIRED = 2 };
#define NO_SLOT 0xFFFFFFFFu

/* 16-byte slot header; the element follows it, so it is 16-byte aligned like the chunk. */
typedef struct slot_header {
    de_atomic_u32 gen; /* generation of the live object, or of the next one */
    u32 next_free;     /* FIFO link while free */
    u32 state;         /* SLOT_*; written under the lock */
    u32 pad_;
} slot_header;
DE_STATIC_ASSERT(sizeof(slot_header) == 16, "slot header must keep the element 16-byte aligned");

static u32 align16(u32 v) {
    return (v + 15u) & ~15u;
}

static slot_header *slot_in(u8 *chunk, const de__gpu_pool *p, u32 index) {
    return (slot_header *)(chunk + (size_t)(index % DE__GPU_POOL_CHUNK_SLOTS) * p->slot_stride);
}

/* Slot of an index below `used`; its chunk exists. */
static slot_header *slot_at(const de__gpu_pool *p, u32 index) {
    u8 *chunk =
        (u8 *)de_atomic_load_ptr(&p->chunks[index / DE__GPU_POOL_CHUNK_SLOTS], DE_ATOMIC_ACQUIRE);
    return slot_in(chunk, p, index);
}

de_result de__gpu_pool_init(de__gpu_pool *p, const de_allocator *alloc, const char *kind,
                            u32 elem_size, u32 max_slots) {
    memset(p, 0, sizeof *p);
    if (max_slots == 0)
        max_slots = DE__GPU_POOL_MAX_SLOTS;
    if (max_slots > DE__GPU_POOL_MAX_SLOTS)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "gpu %s pool: %u slots, more than the %u a handle "
                       "can index",
                       kind, max_slots, DE__GPU_POOL_MAX_SLOTS);
    p->alloc = alloc ? alloc : de_heap_allocator();
    p->kind = kind;
    p->elem_size = align16(elem_size ? elem_size : 16u);
    p->slot_stride = (u32)sizeof(slot_header) + p->elem_size;
    p->max_slots = max_slots;
    p->chunk_count = (max_slots + DE__GPU_POOL_CHUNK_SLOTS - 1u) / DE__GPU_POOL_CHUNK_SLOTS;
    size_t bytes = sizeof(de_atomic_ptr) * p->chunk_count;
    p->chunks =
        (de_atomic_ptr *)DE_ALLOC(p->alloc, bytes, DE_ALIGNOF(de_atomic_ptr), DE_MEM_RENDER);
    if (!p->chunks)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "gpu %s pool: chunk table", kind);
    for (u32 i = 0; i < p->chunk_count; ++i)
        de_atomic_init_ptr(&p->chunks[i], NULL);
    de_atomic_init_u32(&p->used, 0);
    de_mutex_init(&p->lock);
    p->free_head = p->free_tail = NO_SLOT;
    return DE_OK;
}

void de__gpu_pool_destroy(de__gpu_pool *p) {
    if (!p->chunks)
        return;
    size_t chunk_bytes = (size_t)p->slot_stride * DE__GPU_POOL_CHUNK_SLOTS;
    for (u32 i = 0; i < p->chunk_count; ++i) {
        void *c = de_atomic_load_ptr(&p->chunks[i], DE_ATOMIC_RELAXED);
        if (c)
            DE_FREE(p->alloc, c, chunk_bytes);
    }
    DE_FREE(p->alloc, p->chunks, sizeof(de_atomic_ptr) * p->chunk_count);
    de_mutex_destroy(&p->lock);
    memset(p, 0, sizeof *p);
}

u32 de__gpu_pool_alloc(de__gpu_pool *p, void **elem) {
    *elem = NULL;
    de_mutex_lock(&p->lock);
    u32 index;
    slot_header *s;
    if (p->free_count > 0) {
        index = p->free_head;
        s = slot_at(p, index);
        p->free_head = s->next_free;
        if (--p->free_count == 0)
            p->free_head = p->free_tail = NO_SLOT;
    } else {
        index = de_atomic_load_u32(&p->used, DE_ATOMIC_RELAXED);
        if (index >= p->max_slots) {
            de_mutex_unlock(&p->lock);
            DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                    "gpu %s pool is full (%u live, %u awaiting the GPU; "
                    "limit %u)",
                    p->kind, p->live, p->retired, p->max_slots);
            return 0;
        }
        u32 ci = index / DE__GPU_POOL_CHUNK_SLOTS;
        u8 *chunk = (u8 *)de_atomic_load_ptr(&p->chunks[ci], DE_ATOMIC_RELAXED);
        if (!chunk) {
            size_t bytes = (size_t)p->slot_stride * DE__GPU_POOL_CHUNK_SLOTS;
            chunk = (u8 *)DE_ALLOC(p->alloc, bytes, 16, DE_MEM_RENDER);
            if (!chunk) {
                de_mutex_unlock(&p->lock);
                DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "gpu %s pool: new chunk of %u slots", p->kind,
                             DE__GPU_POOL_CHUNK_SLOTS);
                return 0;
            }
            for (u32 i = 0; i < DE__GPU_POOL_CHUNK_SLOTS; ++i) {
                slot_header *h = slot_in(chunk, p, i);
                de_atomic_init_u32(&h->gen, 1);
                h->next_free = NO_SLOT;
                h->state = SLOT_FREE;
                h->pad_ = 0;
            }
            /* Release: a lock-free lookup that sees `used` past this chunk sees its contents. */
            de_atomic_store_ptr(&p->chunks[ci], chunk, DE_ATOMIC_RELEASE);
        }
        s = slot_in(chunk, p, index);
        de_atomic_store_u32(&p->used, index + 1u, DE_ATOMIC_RELEASE);
    }
    s->state = SLOT_LIVE;
    s->next_free = NO_SLOT;
    ++p->live;
    u32 gen = de_atomic_load_u32(&s->gen, DE_ATOMIC_RELAXED);
    void *e = (u8 *)s + sizeof(slot_header);
    memset(e, 0, p->elem_size);
    de_mutex_unlock(&p->lock);
    *elem = e;
    return de__gpu_handle_make(index, gen);
}

void *de__gpu_pool_get(const de__gpu_pool *p, u32 id) {
    u32 index = de__gpu_handle_index(id);
    u32 gen = de__gpu_handle_gen(id);
    if (gen == 0 || index >= de_atomic_load_u32(&p->used, DE_ATOMIC_ACQUIRE))
        return NULL;
    slot_header *s = slot_at(p, index);
    if (de_atomic_load_u32(&s->gen, DE_ATOMIC_ACQUIRE) != gen)
        return NULL;
    return (u8 *)s + sizeof(slot_header);
}

bool de__gpu_pool_is_stale(const de__gpu_pool *p, u32 id) {
    u32 index = de__gpu_handle_index(id);
    if (id == 0 || index >= de_atomic_load_u32(&p->used, DE_ATOMIC_ACQUIRE))
        return false;
    return de__gpu_pool_get(p, id) == NULL;
}

u32 de__gpu_pool_retire(de__gpu_pool *p, u32 id) {
    u32 index = de__gpu_handle_index(id);
    u32 gen = de__gpu_handle_gen(id);
    de_mutex_lock(&p->lock);
    if (gen == 0 || index >= de_atomic_load_u32(&p->used, DE_ATOMIC_RELAXED)) {
        de_mutex_unlock(&p->lock);
        return DE__GPU_POOL_MAX_SLOTS;
    }
    slot_header *s = slot_at(p, index);
    if (s->state != SLOT_LIVE || de_atomic_load_u32(&s->gen, DE_ATOMIC_RELAXED) != gen) {
        de_mutex_unlock(&p->lock);
        return DE__GPU_POOL_MAX_SLOTS;
    }
    u32 next = gen == DE__GPU_HANDLE_GEN_MAX ? 1u : gen + 1u; /* never 0: ids are never 0 */
    de_atomic_store_u32(&s->gen, next, DE_ATOMIC_RELEASE);
    s->state = SLOT_RETIRED;
    --p->live;
    ++p->retired;
    de_mutex_unlock(&p->lock);
    return index;
}

void *de__gpu_pool_at(const de__gpu_pool *p, u32 index) {
    return (u8 *)slot_at(p, index) + sizeof(slot_header);
}

void de__gpu_pool_release(de__gpu_pool *p, u32 index) {
    de_mutex_lock(&p->lock);
    slot_header *s = slot_at(p, index);
    DE_ASSERT(s->state == SLOT_RETIRED);
    s->state = SLOT_FREE;
    s->next_free = NO_SLOT;
    if (p->free_count == 0) {
        p->free_head = p->free_tail = index;
    } else {
        slot_at(p, p->free_tail)->next_free = index;
        p->free_tail = index;
    }
    ++p->free_count;
    --p->retired;
    de_mutex_unlock(&p->lock);
}

u32 de__gpu_pool_id_at(const de__gpu_pool *p, u32 index) {
    return de__gpu_handle_make(index,
                               de_atomic_load_u32(&slot_at(p, index)->gen, DE_ATOMIC_ACQUIRE));
}

bool de__gpu_pool_is_live(const de__gpu_pool *p, u32 index) {
    if (index >= de_atomic_load_u32(&p->used, DE_ATOMIC_ACQUIRE))
        return false;
    return slot_at(p, index)->state == SLOT_LIVE;
}

de__gpu_pool_stats de__gpu_pool_get_stats(de__gpu_pool *p) {
    de__gpu_pool_stats st;
    de_mutex_lock(&p->lock);
    st.live = p->live;
    st.retired = p->retired;
    st.free = p->free_count;
    st.used = de_atomic_load_u32(&p->used, DE_ATOMIC_RELAXED);
    st.chunks = (st.used + DE__GPU_POOL_CHUNK_SLOTS - 1u) / DE__GPU_POOL_CHUNK_SLOTS;
    de_mutex_unlock(&p->lock);
    return st;
}
