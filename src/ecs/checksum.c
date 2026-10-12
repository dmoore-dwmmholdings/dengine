/* checksum.c: de_world_checksum, the ECS world checksum (DENG-352, SPEC §5.7 rule 7, §19.4).
 *
 * The checksum is XXH3-64 (de_hash64, the S02 wrapper) over a canonical byte stream:
 *
 *   u32 'ECK1' tag, u32 mask
 *   per alive entity, in ascending index order:
 *     u32 index, u32 generation, u32 n = components whose flags intersect the mask
 *     per such component, in ascending component id:
 *       u32 component id
 *       data components: the bytes of each reflected field in declaration order (nested structs
 *       and struct or string arrays field by field; a DE_FK_STRING up to and with its NUL)
 *   u32 alive entity count
 *
 * u32 values are written little-endian; field bytes are copied as they are in memory (every
 * supported target is little-endian). The stream is hashed in fixed CK_BLOCK-byte blocks, each
 * with the previous block's result as the seed (the first with seed 0, the last, possibly short or
 * empty, block too), so the value depends only on the stream's bytes.
 *
 * Entities are walked through the records array by index, never by archetype, chunk or hash map,
 * so the value does not depend on archetype creation order or chunk placement (SPEC §5.7 rule 5).
 * Only bytes that a reflected field covers enter the stream, so struct padding never does.
 *
 * Each component's field walk is flattened once into byte spans (adjacent fields merged) and
 * cached in w->ck, so a checksum allocates only when it meets a component for the first time. If
 * that allocation fails, the walk runs directly per row; it emits the same bytes. */
#include "ecs_internal.h"

/* Bytes per hashed block of the stream. Part of the checksum's definition: do not change. */
#define CK_BLOCK 4096u
/* Deepest nesting of struct fields the walk follows; deeper structs are hashed as raw bytes
 * (the type registry has no cycles, so real types never get there). */
#define CK_MAX_DEPTH  16u
#define CK_STREAM_TAG 0x314B4345u /* "ECK1" */

/* ---- Field spans ---------------------------------------------------------------------------- */

/* A run of bytes of a component to hash: plain bytes, or a NUL-terminated string buffer. */
typedef struct ck_span {
    u32 offset;
    u32 size;
    u32 is_string;
} ck_span;

/* The cached spans of one component: spans[first, first + count). */
typedef struct ck_plan {
    u32 first;
    u32 count;
    u32 built; /* 0: not built yet */
} ck_plan;

struct ecs_ck_cache {
    DE_ARRAY(ck_plan) plans; /* component id - 1 */
    DE_ARRAY(ck_span) spans;
};

typedef void (*ck_emit_fn)(void *sink, u32 offset, u32 size, bool is_string);

/* Emits the spans of struct type `type` at byte `base` of the component, field by field. */
static void ck_walk(const de_type_registry *r, de_type_id type, u32 base, u32 depth,
                    ck_emit_fn emit, void *sink) {
    u32 count = 0;
    const de_field_info *fields = de_type_fields(r, type, &count);
    for (u32 i = 0; i < count; ++i) {
        const de_field_info *f = &fields[i];
        const u32 off = base + f->offset;
        if (f->kind == DE_FK_STRUCT && depth < CK_MAX_DEPTH) {
            const de_type_id nested = de_type_find(r, f->attrs.nested_type);
            if (de_type_get(r, nested) != NULL) {
                ck_walk(r, nested, off, depth + 1u, emit, sink);
                continue;
            }
        } else if (f->kind == DE_FK_STRING) {
            emit(sink, off, f->size, true);
            continue;
        } else if (f->kind == DE_FK_FIXED_ARRAY) {
            const de_field_info *el = de_type_array_element(r, type, i);
            if (el != NULL && el->size > 0 && el->kind == DE_FK_STRING) {
                for (u32 k = 0; k < f->size / el->size; ++k)
                    emit(sink, off + k * el->size, el->size, true);
                continue;
            }
            if (el != NULL && el->size > 0 && el->kind == DE_FK_STRUCT && depth < CK_MAX_DEPTH) {
                const de_type_id nested = de_type_find(r, el->attrs.nested_type);
                if (de_type_get(r, nested) != NULL) {
                    for (u32 k = 0; k < f->size / el->size; ++k)
                        ck_walk(r, nested, off + k * el->size, depth + 1u, emit, sink);
                    continue;
                }
            }
        }
        emit(sink, off, f->size, false); /* scalars, vectors, arrays of scalars */
    }
}

/* Sink that appends spans to the cache, merging a plain span into the previous plain span of
 * the same plan when it starts where that one ends. */
typedef struct ck_build {
    struct ecs_ck_cache *c;
    u32 first;
    bool failed;
} ck_build;

static void ck_build_emit(void *sink, u32 offset, u32 size, bool is_string) {
    ck_build *b = (ck_build *)sink;
    if (b->failed || size == 0)
        return;
    if (!is_string && b->c->spans.count > b->first) {
        ck_span *last = &DE_ARRAY_LAST(&b->c->spans, ck_span);
        if (!last->is_string && last->offset + last->size == offset) {
            last->size += size;
            return;
        }
    }
    const ck_span s = {offset, size, is_string ? 1u : 0u};
    if (de_array_push(&b->c->spans, &s) != DE_OK)
        b->failed = true;
}

/* The plan of component `id` (built on first use), or NULL if it cannot be stored. */
static const ck_plan *ck_plan_get(de_world *w, u32 id) {
    if (w->ck == NULL) {
        struct ecs_ck_cache *c = DE_NEW(w->a, struct ecs_ck_cache, DE_MEM_ECS);
        if (c == NULL)
            return NULL;
        DE_ARRAY_INIT_TAG(&c->plans, ck_plan, w->a, DE_MEM_ECS);
        DE_ARRAY_INIT_TAG(&c->spans, ck_span, w->a, DE_MEM_ECS);
        w->ck = c;
    }
    struct ecs_ck_cache *c = w->ck;
    if (c->plans.count < w->components.count &&
        de_array_resize(&c->plans, w->components.count) != DE_OK)
        return NULL;
    ck_plan *p = &DE_ARRAY_AT(&c->plans, ck_plan, id - 1u);
    if (p->built)
        return p;
    ck_build b = {c, c->spans.count, false};
    ck_walk(w->types, ecs_component_get(w, id)->type, 0, 0, ck_build_emit, &b);
    if (b.failed) {
        c->spans.count = b.first; /* drop the partial plan */
        return NULL;
    }
    p->first = b.first;
    p->count = c->spans.count - b.first;
    p->built = 1;
    return p;
}

void ecs_checksum_destroy(de_world *w) {
    if (w->ck == NULL)
        return;
    de_array_free(&w->ck->plans);
    de_array_free(&w->ck->spans);
    DE_FREE(w->a, w->ck, sizeof *w->ck);
    w->ck = NULL;
}

/* ---- Byte stream ---------------------------------------------------------------------------- */

typedef struct ck_stream {
    u64 h;
    u32 n;
    u8 buf[CK_BLOCK];
} ck_stream;

static void ck_put(ck_stream *s, const void *data, size_t len) {
    const u8 *p = (const u8 *)data;
    while (len > 0) {
        size_t take = CK_BLOCK - s->n;
        if (take > len)
            take = len;
        memcpy(s->buf + s->n, p, take);
        s->n += (u32)take;
        p += take;
        len -= take;
        if (s->n == CK_BLOCK) {
            s->h = de_hash64(s->buf, CK_BLOCK, s->h);
            s->n = 0;
        }
    }
}

static void ck_put_u32(ck_stream *s, u32 v) {
    const u8 b[4] = {(u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24)};
    ck_put(s, b, sizeof b);
}

/* A string buffer up to and with its NUL (the whole buffer if it has none): bytes after the
 * terminator are not part of the value. */
static void ck_put_string(ck_stream *s, const u8 *p, u32 size) {
    const u8 *nul = (const u8 *)memchr(p, 0, size);
    ck_put(s, p, nul != NULL ? (size_t)(nul - p) + 1u : size);
}

/* Sink of the uncached walk: hashes each span of one component value directly. */
typedef struct ck_direct {
    ck_stream *s;
    const u8 *data;
} ck_direct;

static void ck_direct_emit(void *sink, u32 offset, u32 size, bool is_string) {
    const ck_direct *d = (const ck_direct *)sink;
    if (is_string)
        ck_put_string(d->s, d->data + offset, size);
    else
        ck_put(d->s, d->data + offset, size);
}

/* ---- Checksum ------------------------------------------------------------------------------- */

u64 de_world_checksum(de_world *w, u32 component_flags_mask) {
    if (w == NULL)
        return 0;
    ck_stream s;
    s.h = 0;
    s.n = 0;
    ck_put_u32(&s, CK_STREAM_TAG);
    ck_put_u32(&s, component_flags_mask);

    u32 alive = 0;
    const ecs_record *recs = DE_ARRAY_DATA(&w->records, ecs_record);
    for (u32 index = 0; index < w->records.count; ++index) {
        const ecs_record *rec = &recs[index];
        if (rec->archetype == ECS_INVALID_INDEX)
            continue;
        ++alive;
        const ecs_archetype *a = ecs_archetype_at(w, rec->archetype);
        u32 n = 0;
        for (u32 i = 0; i < a->component_count; ++i) {
            if ((ecs_component_get(w, a->components[i])->flags & component_flags_mask) != 0)
                ++n;
        }
        ck_put_u32(&s, index);
        ck_put_u32(&s, rec->generation);
        ck_put_u32(&s, n);
        if (n == 0)
            continue;
        /* a->components is sorted ascending: the stream's component order. */
        for (u32 i = 0; i < a->component_count; ++i) {
            const u32 id = a->components[i];
            if ((ecs_component_get(w, id)->flags & component_flags_mask) == 0)
                continue;
            ck_put_u32(&s, id);
            const u32 col = a->column_of[i];
            if (col == ECS_INVALID_INDEX)
                continue; /* a tag: its id only */
            const u8 *data = (const u8 *)ecs_row_column(a, rec->chunk, rec->row, col);
            const ck_plan *p = ck_plan_get(w, id);
            if (p == NULL) {
                ck_direct d = {&s, data};
                ck_walk(w->types, ecs_component_get(w, id)->type, 0, 0, ck_direct_emit, &d);
                continue;
            }
            const ck_span *spans = DE_ARRAY_DATA(&w->ck->spans, ck_span) + p->first;
            for (u32 k = 0; k < p->count; ++k) {
                if (spans[k].is_string)
                    ck_put_string(&s, data + spans[k].offset, spans[k].size);
                else
                    ck_put(&s, data + spans[k].offset, spans[k].size);
            }
        }
    }
    ck_put_u32(&s, alive);
    return de_hash64(s.buf, s.n, s.h);
}
