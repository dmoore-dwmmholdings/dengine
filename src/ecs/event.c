/* event.c: double-buffered typed events and readers (DENG-348, SPEC §7.1).
 *
 * Each event type has two payload buffers: last frame's (`older`) and this frame's. Every event of
 * a type has a sequence number, given in send order; the buffers hold consecutive runs of them, so
 * a reader is just the sequence number of the next event it reads. de_world_update_events() clears
 * the older buffer (keeping its capacity) and makes it the new frame's buffer, so an event is
 * readable during the frame it was sent and the next one, and a warmed-up queue never allocates.
 *
 * Event types live in a dense array in registration order; the type -> id hash map serves lookups
 * only and is never iterated (SPEC §5.7 rule 5). */
#include "ecs_internal.h"
#include "system_internal.h" /* DENG-350: access checks inside systems */

#include <stdint.h>

static u64 type_key_hash(u32 type_id) {
    return de_hash64(&type_id, sizeof type_id, 0);
}

static ecs_event_queue *event_queue(const de_world *w, de_event_type t) {
    if (t.id == 0 || t.id > w->events.count)
        return NULL;
    return (ecs_event_queue *)de_array_at(&w->events, t.id - 1u);
}

/* Runs the destruct hook on every payload of `b`. */
static void destruct_buffer(const ecs_event_queue *q, const de_array *b) {
    if (q->hooks.destruct == NULL)
        return;
    for (u32 i = 0; i < b->count; ++i)
        q->hooks.destruct(de_array_at(b, i));
}

de_result ecs_events_init(de_world *w) {
    DE_ARRAY_INIT_TAG(&w->events, ecs_event_queue, w->a, DE_MEM_ECS);
    const de_hashmap_desc by_type = {
        .allocator = w->a, .key_size = sizeof(u32), .value_size = sizeof(u32), .tag = DE_MEM_ECS};
    return de_hashmap_init(&w->event_by_type, &by_type);
}

void ecs_events_destroy(de_world *w) {
    for (u32 i = 0; i < w->events.count; ++i) {
        ecs_event_queue *q = (ecs_event_queue *)de_array_at(&w->events, i);
        /* Send order: last frame's events, then this frame's. */
        destruct_buffer(q, &q->buf[q->older]);
        destruct_buffer(q, &q->buf[q->older ^ 1u]);
        de_array_free(&q->buf[0]);
        de_array_free(&q->buf[1]);
    }
    de_array_free(&w->events);
    de_hashmap_free(&w->event_by_type);
}

de_event_type de_event_register(de_world *w, de_type_id payload) {
    const de_event_type invalid = {0};
    if (w == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_event_register: world is NULL");
        return invalid;
    }
    const de_type_desc *desc = de_type_get(w->types, payload);
    if (desc == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                "de_event_register: type id 0x%x is not a struct type of the world's registry",
                payload.id);
        return invalid;
    }
    const u64 hash = type_key_hash(payload.id);
    const u32 *existing = DE_HASHMAP_GET(&w->event_by_type, u32, hash, &payload.id);
    if (existing != NULL)
        return (de_event_type){*existing};

    /* Buffers move payloads with memcpy when they grow and copy them in on send. */
    if (!desc->is_pod && (desc->hooks.copy == NULL || desc->hooks.destruct == NULL)) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                "de_event_register: type '%s' is neither POD nor has copy and destruct hooks",
                desc->name);
        return invalid;
    }
    if (desc->size == 0) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_event_register: type '%s' has size 0", desc->name);
        return invalid;
    }
    const u32 align = desc->align ? desc->align : 1u;
    if (align > DE_ECS_MAX_COMPONENT_ALIGN) {
        DE_FAIL(DE_ERR_UNSUPPORTED, "de_event_register: type '%s' has alignment %u; the most is %u",
                desc->name, align, DE_ECS_MAX_COMPONENT_ALIGN);
        return invalid;
    }
    if (w->events.count >= 0x7FFFFFFFu) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_event_register: too many event types");
        return invalid;
    }

    ecs_event_queue q;
    memset(&q, 0, sizeof q);
    q.payload = payload;
    q.size = desc->size;
    q.align = align;
    q.is_pod = desc->is_pod;
    q.hooks = desc->hooks;
    q.older = 0;
    de_array_init(&q.buf[0], w->a, q.size, q.align, DE_MEM_ECS);
    de_array_init(&q.buf[1], w->a, q.size, q.align, DE_MEM_ECS);

    const u32 id = w->events.count + 1u;
    if (de_array_push(&w->events, &q) != DE_OK) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_event_register: out of memory");
        return invalid;
    }
    if (de_hashmap_put(&w->event_by_type, hash, &payload.id, &id) != DE_OK) {
        de_array_pop(&w->events, NULL);
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_event_register: out of memory");
        return invalid;
    }
    return (de_event_type){id};
}

void de_event_send(de_world *w, de_event_type t, const void *payload) {
    ECS_CHECK_EVENT(w, t.id, true); /* DENG-350: declared in writes_events */
    ecs_event_queue *q = w != NULL ? event_queue(w, t) : NULL;
    if (q == NULL || payload == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_event_send: invalid world, event type %u or payload",
                t.id);
        return;
    }
    de_array *b = &q->buf[q->older ^ 1u];
    /* `payload` may point into this buffer (an event read back and sent again); growing it moves
     * the payload, so find it again by offset. */
    const uintptr_t p = (uintptr_t)payload;
    const uintptr_t base = (uintptr_t)b->data;
    const bool inside = b->data != NULL && p >= base && p < base + (size_t)b->count * q->size;
    if (de_array_push_n(b, NULL, 1) != DE_OK) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_event_send: event of type %u dropped (out of memory)",
                t.id);
        return;
    }
    const void *src = inside ? (const void *)((const u8 *)b->data + (p - base)) : payload;
    void *dst = de_array_at(b, b->count - 1u);
    if (q->hooks.copy != NULL)
        q->hooks.copy(dst, src);
    else
        memcpy(dst, src, q->size);
}

de_event_reader de_event_reader_create(de_world *w, de_event_type t) {
    ECS_CHECK_EVENT(w, t.id, false); /* DENG-350: declared in reads_events (or writes_events) */
    de_event_reader r;
    memset(&r, 0, sizeof r);
    const ecs_event_queue *q = w != NULL ? event_queue(w, t) : NULL;
    if (q == NULL)
        return r;
    r.type = t;
    r.cursor = q->start[q->older];
    return r;
}

const void *de_event_next(de_world *w, de_event_reader *r) {
    if (w == NULL || r == NULL)
        return NULL;
    ECS_CHECK_EVENT(w, r->type.id, false); /* DENG-350 */
    const ecs_event_queue *q = event_queue(w, r->type);
    if (q == NULL)
        return NULL;
    const u32 o = q->older;
    const u64 oldest = q->start[o];
    if (r->cursor < oldest) {
        /* Fell behind: the events in between were dropped. Count them and continue. */
        const u64 lost = oldest - r->cursor;
        const u64 total = (u64)r->missed + lost;
        r->missed = total > 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)total;
        r->cursor = oldest;
    }
    for (u32 k = 0; k < 2; ++k) {
        const u32 bi = o ^ k; /* older buffer first, then this frame's */
        const u64 start = q->start[bi];
        if (r->cursor >= start && r->cursor - start < q->buf[bi].count) {
            const void *e = de_array_at(&q->buf[bi], (u32)(r->cursor - start));
            ++r->cursor;
            return e;
        }
    }
    return NULL;
}

u32 de_event_reader_missed(const de_event_reader *r) {
    return r != NULL ? r->missed : 0u;
}

void de_world_update_events(de_world *w) {
    if (w == NULL)
        return;
    for (u32 i = 0; i < w->events.count; ++i) {
        ecs_event_queue *q = (ecs_event_queue *)de_array_at(&w->events, i);
        const u32 o = q->older;
        const u32 n = o ^ 1u;
        destruct_buffer(q, &q->buf[o]);
        de_array_clear(&q->buf[o]); /* keeps the capacity: no allocation next frame */
        q->start[o] = q->start[n] + q->buf[n].count;
        q->older = n;
    }
}
