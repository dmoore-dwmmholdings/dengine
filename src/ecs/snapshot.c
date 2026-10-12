/* snapshot.c: world snapshots: size, take, free, restore (DENG-352, SPEC §7.4).
 *
 * Snapshot block (one allocation from the caller's allocator, aligned to SNAP_ALIGN):
 *
 *   snap_header                                  40 bytes
 *   u32 generation[record_count]                 every index's generation at take
 *   u32 alive_bits[(record_count + 31) / 32]     bit i set: index i alive
 *   u32 free_list[free_count]                    the LIFO free list, bottom to top
 *   then, at SNAP_ALIGN, one section per non-empty archetype with >= 1 DE_COMP_SNAPSHOT
 *   component, in archetype creation order, each at SNAP_ALIGN:
 *     snap_section                               32 bytes
 *     u32 set[set_count]                         the archetype's full component set, sorted
 *     u32 snap[snap_count]                       its DE_COMP_SNAPSHOT components (tags too), sorted
 *     u32 col_comp[column_count]                 the snapshot components with data, sorted
 *     u32 col_size[column_count]
 *     (at 8) de_entity ids[rows]                 rows in table order
 *     per column, at SNAP_ALIGN: rows * size bytes, rows in table order
 *
 * Padding between the parts is zero. The layout is computed by the same functions for size,
 * take and validation (sec_layout), so it is never stored as offsets.
 *
 * Restore (de_world_snapshot_restore) runs in phases; nothing changes before validation and the
 * reservations pass, so a bad snapshot leaves the world unchanged:
 *   1. despawn every entity alive now that the snapshot does not have (with this generation),
 *      in ascending index order;
 *   2. set the records' generations and the free list to the snapshot's;
 *   3. place each snapshot entity: sections whose table already holds their entities in order
 *      are "fast" and skip this; other survivors move to (their components without
 *      DE_COMP_SNAPSHOT) + (the section's snapshot set); missing entities are re-created with the
 *      snapshot set only (the empty archetype for entities without a section);
 *   4. put each non-fast section's rows first in their table, in snapshot order, when its
 *      entities all share one table that no other section claimed (row swaps);
 *   5. copy the data: one memcpy per column and chunk for fast and ordered sections, one per
 *      column and row otherwise; written rows get `changed` = the current change tick
 *      (ecs_change_tick, DENG-347), like any change made outside a system.
 * Re-created entities go through the internal row functions (ecs_table_push_row,
 * ecs_row_construct), not commands: restore runs outside schedules. */
#include "change_internal.h" /* DENG-347: ecs_change_tick */
#include "ecs_internal.h"

#define SNAP_MAGIC   0x4E534544u /* "DESN" */
#define SNAP_VERSION 1u
#define SNAP_ALIGN   64u

typedef struct snap_header {
    u32 magic;
    u32 version;
    u64 size; /* bytes of the whole block */
    u64 tick;
    u32 record_count;
    u32 free_count;
    u32 alive_count;
    u32 section_count;
} snap_header;
_Static_assert(sizeof(snap_header) == 40, "snap_header has no padding");

typedef struct snap_section {
    u32 archetype;    /* archetype index at take (a hint: checked against `set`) */
    u32 set_count;    /* components of the archetype */
    u32 snap_count;   /* DE_COMP_SNAPSHOT components among them (tags included) */
    u32 column_count; /* snapshot components with data */
    u32 rows;
    u32 pad_;
    u64 size; /* bytes of the section, header included; a multiple of SNAP_ALIGN */
} snap_section;
_Static_assert(sizeof(snap_section) == 32, "snap_section has no padding");

static u64 align_up(u64 v, u64 a) {
    return (v + a - 1u) & ~(a - 1u);
}

/* ---- Layout --------------------------------------------------------------------------------- */

static u64 hdr_gens_offset(void) {
    return sizeof(snap_header);
}
static u64 hdr_alive_offset(u32 record_count) {
    return hdr_gens_offset() + 4u * (u64)record_count;
}
static u64 hdr_free_offset(u32 record_count) {
    return hdr_alive_offset(record_count) + 4u * (((u64)record_count + 31u) / 32u);
}
static u64 hdr_sections_offset(u32 record_count, u32 free_count) {
    return align_up(hdr_free_offset(record_count) + 4u * (u64)free_count, SNAP_ALIGN);
}

/* Offsets inside a section, relative to its start. */
static u64 sec_ids_offset(u32 set_count, u32 snap_count, u32 column_count) {
    return align_up(sizeof(snap_section) +
                        4u * ((u64)set_count + (u64)snap_count + 2u * (u64)column_count),
                    8u);
}
static u64 sec_first_column_offset(u64 ids_offset, u32 rows) {
    return align_up(ids_offset + 8u * (u64)rows, SNAP_ALIGN);
}
static u64 sec_column_end(u64 column_offset, u32 rows, u32 size) {
    return align_up(column_offset + (u64)rows * size, SNAP_ALIGN);
}

/* A section as pointers into the block. */
typedef struct sec_view {
    const snap_section *h;
    const u32 *set, *snap, *col_comp, *col_size;
    const de_entity *ids;
    const u8 *base; /* the section's first byte; column j starts at base + column offset */
} sec_view;

static void sec_view_at(const u8 *block, u64 offset, sec_view *v) {
    v->base = block + offset;
    v->h = (const snap_section *)(const void *)v->base;
    v->set = (const u32 *)(const void *)(v->base + sizeof(snap_section));
    v->snap = v->set + v->h->set_count;
    v->col_comp = v->snap + v->h->snap_count;
    v->col_size = v->col_comp + v->h->column_count;
    v->ids = (const de_entity *)(const void *)(v->base + sec_ids_offset(v->h->set_count,
                                                                        v->h->snap_count,
                                                                        v->h->column_count));
}

/* Offset of column `column`'s block in section `v`, from the section's start. */
static u64 sec_column_offset(const sec_view *v, u32 column) {
    u64 off = sec_first_column_offset(
        sec_ids_offset(v->h->set_count, v->h->snap_count, v->h->column_count), v->h->rows);
    for (u32 j = 0; j < column; ++j)
        off = sec_column_end(off, v->h->rows, v->col_size[j]);
    return off;
}

/* true if archetype `a` has at least one DE_COMP_SNAPSHOT component. Counts them and the ones
 * with data. */
static bool arch_snapshot_counts(const de_world *w, const ecs_archetype *a, u32 *snap_count,
                                 u32 *column_count) {
    u32 s = 0, c = 0;
    for (u32 i = 0; i < a->component_count; ++i) {
        if ((ecs_component_get(w, a->components[i])->flags & DE_COMP_SNAPSHOT) == 0)
            continue;
        ++s;
        if (a->column_of[i] != ECS_INVALID_INDEX)
            ++c;
    }
    *snap_count = s;
    *column_count = c;
    return s > 0;
}

/* The section of archetype `a` (with s snapshot components, c of them with data): its size. */
static u64 arch_section_size(const de_world *w, const ecs_archetype *a, u32 s, u32 c) {
    u64 off = sec_first_column_offset(sec_ids_offset(a->component_count, s, c), a->count);
    for (u32 i = 0; i < a->component_count; ++i) {
        const ecs_component_info *info = ecs_component_get(w, a->components[i]);
        if ((info->flags & DE_COMP_SNAPSHOT) != 0 && a->column_of[i] != ECS_INVALID_INDEX)
            off = sec_column_end(off, a->count, info->size);
    }
    return off;
}

/* Size of a snapshot of the world now, and its section count. */
static u64 snapshot_measure(const de_world *w, u32 *out_sections) {
    u64 total = hdr_sections_offset(w->records.count, w->free_indices.count);
    u32 sections = 0;
    for (u32 ai = 0; ai < ecs_archetype_count(w); ++ai) {
        const ecs_archetype *a = ecs_archetype_at(w, ai);
        u32 s = 0, c = 0;
        if (a->count == 0 || !arch_snapshot_counts(w, a, &s, &c))
            continue;
        total += arch_section_size(w, a, s, c);
        ++sections;
    }
    *out_sections = sections;
    return total;
}

u64 de_world_snapshot_size(const de_world *w) {
    if (w == NULL)
        return 0;
    u32 sections = 0;
    return snapshot_measure(w, &sections);
}

/* ---- Take and free -------------------------------------------------------------------------- */

de_result de_world_snapshot_take(de_world *w, const de_allocator *a, de_world_snapshot *out) {
    if (out != NULL)
        memset(out, 0, sizeof *out);
    if (w == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_world_snapshot_take: world and out are "
                                                "required");
    if (a == NULL)
        a = de_heap_allocator();

    /* Snapshot data is copied as bytes: every snapshot column in use must be POD. */
    for (u32 ai = 0; ai < ecs_archetype_count(w); ++ai) {
        const ecs_archetype *arch = ecs_archetype_at(w, ai);
        if (arch->count == 0)
            continue;
        for (u32 c = 0; c < arch->column_count; ++c) {
            const ecs_component_info *info = ecs_component_get(w, arch->columns[c].component);
            if ((info->flags & DE_COMP_SNAPSHOT) != 0 && !info->is_pod)
                return DE_FAIL(DE_ERR_UNSUPPORTED,
                               "de_world_snapshot_take: snapshot component '%s' is not POD",
                               info->name);
        }
    }

    u32 section_count = 0;
    const u64 size = snapshot_measure(w, &section_count);
    if (size > (u64)SIZE_MAX)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_world_snapshot_take: snapshot too large");
    u8 *block = (u8 *)DE_ALLOC(a, (size_t)size, SNAP_ALIGN, DE_MEM_ECS);
    if (block == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_world_snapshot_take: out of memory (%llu bytes)",
                       (unsigned long long)size);

    /* Header, generations, alive bits, free list. */
    const u32 record_count = w->records.count;
    const u32 free_count = w->free_indices.count;
    const u64 sections_off = hdr_sections_offset(record_count, free_count);
    memset(block, 0, (size_t)sections_off); /* header padding and the alive bits start at 0 */
    snap_header *h = (snap_header *)(void *)block;
    h->magic = SNAP_MAGIC;
    h->version = SNAP_VERSION;
    h->size = size;
    h->tick = w->tick;
    h->record_count = record_count;
    h->free_count = free_count;
    h->section_count = section_count;
    u32 *gens = (u32 *)(void *)(block + hdr_gens_offset());
    u32 *alive_bits = (u32 *)(void *)(block + hdr_alive_offset(record_count));
    const ecs_record *recs = DE_ARRAY_DATA(&w->records, ecs_record);
    u32 alive = 0;
    for (u32 i = 0; i < record_count; ++i) {
        gens[i] = recs[i].generation;
        if (recs[i].archetype != ECS_INVALID_INDEX) {
            alive_bits[i / 32u] |= 1u << (i % 32u);
            ++alive;
        }
    }
    h->alive_count = alive;
    if (free_count > 0)
        memcpy(block + hdr_free_offset(record_count), w->free_indices.data,
               (size_t)free_count * sizeof(u32));

    /* Sections. */
    u64 off = sections_off;
    for (u32 ai = 0; ai < ecs_archetype_count(w); ++ai) {
        const ecs_archetype *arch = ecs_archetype_at(w, ai);
        u32 s = 0, c = 0;
        if (arch->count == 0 || !arch_snapshot_counts(w, arch, &s, &c))
            continue;
        const u64 sec_size = arch_section_size(w, arch, s, c);
        u8 *base = block + off;
        const u64 ids_off = sec_ids_offset(arch->component_count, s, c);
        const u64 col0 = sec_first_column_offset(ids_off, arch->count);
        memset(base, 0, (size_t)ids_off);
        snap_section *sh = (snap_section *)(void *)base;
        sh->archetype = arch->index;
        sh->set_count = arch->component_count;
        sh->snap_count = s;
        sh->column_count = c;
        sh->rows = arch->count;
        sh->size = sec_size;
        u32 *set = (u32 *)(void *)(base + sizeof(snap_section));
        u32 *snap = set + arch->component_count;
        u32 *col_comp = snap + s;
        u32 *col_size = col_comp + c;
        u32 si = 0, ci = 0;
        for (u32 i = 0; i < arch->component_count; ++i) {
            const u32 id = arch->components[i];
            set[i] = id;
            const ecs_component_info *info = ecs_component_get(w, id);
            if ((info->flags & DE_COMP_SNAPSHOT) == 0)
                continue;
            snap[si++] = id;
            if (arch->column_of[i] != ECS_INVALID_INDEX) {
                col_comp[ci] = id;
                col_size[ci] = info->size;
                ++ci;
            }
        }
        /* Entity ids, chunk by chunk (rows in table order). */
        de_entity *ids = (de_entity *)(void *)(base + ids_off);
        memset(base + ids_off + 8u * (u64)arch->count, 0,
               (size_t)(col0 - ids_off - 8u * (u64)arch->count));
        u64 row = 0;
        for (u32 ch = 0; ch < arch->chunks.count; ++ch) {
            const ecs_chunk *chunk = ecs_table_chunk(arch, ch);
            memcpy(ids + row, ecs_chunk_entities(chunk), (size_t)chunk->count * sizeof(de_entity));
            row += chunk->count;
        }
        /* One contiguous block per snapshot column. */
        u64 col_off = col0;
        for (u32 j = 0; j < c; ++j) {
            const u32 tcol = ecs_archetype_column(arch, col_comp[j]);
            const u32 sz = col_size[j];
            u8 *dst = base + col_off;
            for (u32 ch = 0; ch < arch->chunks.count; ++ch) {
                const ecs_chunk *chunk = ecs_table_chunk(arch, ch);
                const size_t bytes = (size_t)chunk->count * sz;
                memcpy(dst, ecs_chunk_column(arch, chunk, tcol), bytes);
                dst += bytes;
            }
            const u64 end = sec_column_end(col_off, arch->count, sz);
            memset(dst, 0, (size_t)(base + end - dst));
            col_off = end;
        }
        DE_ASSERT(col_off == sec_size);
        off += sec_size;
    }
    DE_ASSERT(off == size);

    out->data = block;
    out->size = size;
    out->tick = w->tick;
    return DE_OK;
}

void de_world_snapshot_free(const de_allocator *a, de_world_snapshot *snap) {
    if (snap == NULL)
        return;
    if (snap->data != NULL) {
        if (a == NULL)
            a = de_heap_allocator();
        DE_FREE(a, snap->data, (size_t)snap->size);
    }
    memset(snap, 0, sizeof *snap);
}

/* ---- Restore state -------------------------------------------------------------------------- */

/* How phase 3-5 treat a section. */
enum { SEC_FAST = 1, SEC_ORDERED = 2, SEC_LOOSE = 3 };

typedef struct snap_sec_info {
    u64 offset; /* of the section in the block */
    u32 mode;   /* SEC_* (0 until phase 3) */
    u32 table;  /* table archetype index for SEC_FAST / SEC_ORDERED */
} snap_sec_info;

struct ecs_snap_state {
    ecs_snapshot_stats stats;
    DE_ARRAY(u32) bits;          /* one bit per snapshot index: placed by a section / free */
    DE_ARRAY(u32) ids;           /* component-set scratch */
    DE_ARRAY(snap_sec_info) sec; /* per section */
};

static struct ecs_snap_state *snap_state(de_world *w) {
    if (w->snap != NULL)
        return w->snap;
    struct ecs_snap_state *st = DE_NEW(w->a, struct ecs_snap_state, DE_MEM_ECS);
    if (st == NULL)
        return NULL;
    memset(st, 0, sizeof *st);
    DE_ARRAY_INIT_TAG(&st->bits, u32, w->a, DE_MEM_ECS);
    DE_ARRAY_INIT_TAG(&st->ids, u32, w->a, DE_MEM_ECS);
    DE_ARRAY_INIT_TAG(&st->sec, snap_sec_info, w->a, DE_MEM_ECS);
    w->snap = st;
    return st;
}

void ecs_snapshot_destroy(de_world *w) {
    if (w->snap == NULL)
        return;
    de_array_free(&w->snap->bits);
    de_array_free(&w->snap->ids);
    de_array_free(&w->snap->sec);
    DE_FREE(w->a, w->snap, sizeof *w->snap);
    w->snap = NULL;
}

void ecs_snapshot_stats_get(const de_world *w, ecs_snapshot_stats *out) {
    if (w->snap != NULL)
        *out = w->snap->stats;
    else
        memset(out, 0, sizeof *out);
}

static bool bit_get(const u32 *bits, u32 i) {
    return (bits[i / 32u] >> (i % 32u)) & 1u;
}
static void bit_set(u32 *bits, u32 i) {
    bits[i / 32u] |= 1u << (i % 32u);
}

/* ---- Validation ----------------------------------------------------------------------------- */

#define SNAP_BAD(...) DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_world_snapshot_restore: " __VA_ARGS__)

/* true if `ids[0..n)` is strictly ascending and every id is a registered component. */
static bool ids_valid(const de_world *w, const u32 *ids, u32 n) {
    for (u32 i = 0; i < n; ++i) {
        if (ecs_component_get(w, ids[i]) == NULL || (i > 0 && ids[i] <= ids[i - 1u]))
            return false;
    }
    return true;
}

/* Checks the whole snapshot against the world and fills st->sec (offsets) and st->bits (the
 * entities of sections). Changes nothing in the world. */
static de_result snap_validate(de_world *w, struct ecs_snap_state *st,
                               const de_world_snapshot *snap, const snap_header **out_h) {
    if (snap->data == NULL || snap->size < sizeof(snap_header))
        return SNAP_BAD("empty snapshot");
    const u8 *block = (const u8 *)snap->data;
    const snap_header *h = (const snap_header *)snap->data;
    if (h->magic != SNAP_MAGIC || h->version != SNAP_VERSION)
        return SNAP_BAD("not a version %u world snapshot", SNAP_VERSION);
    if (h->size != snap->size)
        return SNAP_BAD("size %llu does not match the header (%llu)",
                        (unsigned long long)snap->size, (unsigned long long)h->size);
    const u32 rc = h->record_count;
    if (rc == ECS_MAX_ENTITIES || h->free_count > rc || h->alive_count > rc)
        return SNAP_BAD("bad entity counts");
    const u64 sections_off = hdr_sections_offset(rc, h->free_count);
    if (sections_off > h->size)
        return SNAP_BAD("truncated entity table");
    const u32 *gens = (const u32 *)(const void *)(block + hdr_gens_offset());
    const u32 *alive_bits = (const u32 *)(const void *)(block + hdr_alive_offset(rc));
    const u32 *free_list = (const u32 *)(const void *)(block + hdr_free_offset(rc));

    const u32 words = (rc + 31u) / 32u;
    if (h->section_count > (h->size - sections_off) / sizeof(snap_section))
        return SNAP_BAD("bad section count");
    if (de_array_resize(&st->bits, words) != DE_OK ||
        de_array_resize(&st->sec, h->section_count) != DE_OK)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_world_snapshot_restore: out of memory");
    u32 *bits = DE_ARRAY_DATA(&st->bits, u32);

    /* Entity allocator: every index is alive (generation != 0), free (on the list once,
     * generation != 0) or retired (generation 0). */
    if (words > 0) /* bits is NULL for an empty entity table */
        memset(bits, 0, (size_t)words * sizeof(u32));
    for (u32 k = 0; k < h->free_count; ++k) {
        const u32 i = free_list[k];
        if (i >= rc || bit_get(bits, i) || bit_get(alive_bits, i) || gens[i] == 0)
            return SNAP_BAD("bad free list entry %u", i);
        bit_set(bits, i);
    }
    u32 alive = 0;
    for (u32 i = 0; i < rc; ++i) {
        const bool is_alive = bit_get(alive_bits, i);
        alive += is_alive ? 1u : 0u;
        if ((is_alive || bit_get(bits, i)) != (gens[i] != 0))
            return SNAP_BAD("bad generation of index %u", i);
    }
    if (rc % 32u != 0 && (alive_bits[words - 1u] >> (rc % 32u)) != 0)
        return SNAP_BAD("alive bits past the last index");
    if (alive != h->alive_count)
        return SNAP_BAD("alive count does not match the alive bits");

    /* Sections. `bits` now marks the entities placed by a section. */
    if (words > 0) /* bits is NULL for an empty entity table */
        memset(bits, 0, (size_t)words * sizeof(u32));
    u64 off = sections_off;
    for (u32 s = 0; s < h->section_count; ++s) {
        if (h->size - off < sizeof(snap_section))
            return SNAP_BAD("truncated section %u", s);
        const snap_section *sh = (const snap_section *)(const void *)(block + off);
        if (sh->set_count > w->components.count || sh->snap_count == 0 ||
            sh->snap_count > sh->set_count || sh->column_count > sh->snap_count || sh->rows == 0 ||
            sh->rows > h->alive_count || sh->size > h->size - off)
            return SNAP_BAD("bad section %u header", s);
        const u64 ids_off = sec_ids_offset(sh->set_count, sh->snap_count, sh->column_count);
        if (ids_off > sh->size)
            return SNAP_BAD("truncated section %u", s);
        sec_view v;
        sec_view_at(block, off, &v);
        if (!ids_valid(w, v.set, sh->set_count) || !ids_valid(w, v.snap, sh->snap_count) ||
            !ids_valid(w, v.col_comp, sh->column_count))
            return SNAP_BAD("section %u names a component the world does not have", s);
        /* Same registration as at take: the snapshot set is exactly the DE_COMP_SNAPSHOT part of
         * the set, and the columns are its components with data, with the same sizes. */
        u32 si = 0, ci = 0;
        for (u32 i = 0; i < sh->set_count; ++i) {
            const ecs_component_info *info = ecs_component_get(w, v.set[i]);
            if ((info->flags & DE_COMP_SNAPSHOT) == 0)
                continue;
            if (si >= sh->snap_count || v.snap[si] != v.set[i])
                return SNAP_BAD("section %u: snapshot flags differ from the world's", s);
            ++si;
            if (info->size == 0)
                continue;
            if (ci >= sh->column_count || v.col_comp[ci] != v.set[i] ||
                v.col_size[ci] != info->size || !info->is_pod)
                return SNAP_BAD("section %u: component %u differs from the world's", s, v.set[i]);
            ++ci;
        }
        if (si != sh->snap_count || ci != sh->column_count)
            return SNAP_BAD("section %u: snapshot flags differ from the world's", s);
        u64 end = sec_first_column_offset(ids_off, sh->rows);
        for (u32 j = 0; j < sh->column_count; ++j)
            end = sec_column_end(end, sh->rows, v.col_size[j]);
        if (end != sh->size)
            return SNAP_BAD("section %u: size does not match its layout", s);
        for (u32 r = 0; r < sh->rows; ++r) {
            const u32 i = de_entity_index(v.ids[r]);
            if (i >= rc || !bit_get(alive_bits, i) || gens[i] != de_entity_generation(v.ids[r]) ||
                bit_get(bits, i))
                return SNAP_BAD("section %u: bad entity 0x%016llx", s,
                                (unsigned long long)v.ids[r].bits);
            bit_set(bits, i);
        }
        snap_sec_info *info = &DE_ARRAY_AT(&st->sec, snap_sec_info, s);
        info->offset = off;
        info->mode = 0;
        info->table = ECS_INVALID_INDEX;
        off += sh->size;
    }
    if (off != h->size)
        return SNAP_BAD("%llu bytes after the last section", (unsigned long long)(h->size - off));
    *out_h = h;
    return DE_OK;
}

/* ---- Restore helpers ------------------------------------------------------------------------ */

/* The archetype of (components of `cur` without DE_COMP_SNAPSHOT) + snap[0..n). `cur` NULL: the
 * snapshot set alone. */
static de_result target_archetype(de_world *w, struct ecs_snap_state *st, const ecs_archetype *cur,
                                  const u32 *snap, u32 n, ecs_archetype **out) {
    const u32 cur_count = cur != NULL ? cur->component_count : 0u;
    DE_TRY(de_array_resize(&st->ids, cur_count + n));
    u32 *ids = DE_ARRAY_DATA(&st->ids, u32);
    u32 k = 0;
    for (u32 i = 0; i < cur_count; ++i) {
        if ((ecs_component_get(w, cur->components[i])->flags & DE_COMP_SNAPSHOT) == 0)
            ids[k++] = cur->components[i];
    }
    if (n > 0)
        memcpy(ids + k, snap, (size_t)n * sizeof(u32));
    return ecs_archetype_get_or_create(w, ids, k + n, out);
}

/* Re-creates entity `e` (its record holds the snapshot generation, archetype invalid) in `a`:
 * a new row, every column constructed (hook or zero, ticks = now). */
static de_result recreate(de_world *w, ecs_archetype *a, de_entity e) {
    u32 chunk = 0, row = 0;
    DE_TRY(ecs_table_push_row(w, a, e, &chunk, &row));
    ecs_record *rec = (ecs_record *)de_array_at(&w->records, de_entity_index(e));
    rec->archetype = a->index;
    rec->chunk = chunk;
    rec->row = row;
    for (u32 col = 0; col < a->column_count; ++col)
        ecs_row_construct(w, a, chunk, row, col);
    return DE_OK;
}

static void swap_bytes(u8 *x, u8 *y, u32 n) {
    u8 tmp[64];
    while (n > 0) {
        const u32 k = n < (u32)sizeof tmp ? n : (u32)sizeof tmp;
        memcpy(tmp, x, k);
        memcpy(x, y, k);
        memcpy(y, tmp, k);
        x += k;
        y += k;
        n -= k;
    }
}

/* Swaps table rows `i` and `j` (positions in table order) of `a`: entities, data and ticks, and
 * both entities' records. */
static void swap_rows(de_world *w, ecs_archetype *a, u32 i, u32 j) {
    const u32 ci = i / a->rows_per_chunk, ri = i % a->rows_per_chunk;
    const u32 cj = j / a->rows_per_chunk, rj = j % a->rows_per_chunk;
    const ecs_chunk *x = ecs_table_chunk(a, ci);
    const ecs_chunk *y = ecs_table_chunk(a, cj);
    de_entity *ex = &ecs_chunk_entities(x)[ri];
    de_entity *ey = &ecs_chunk_entities(y)[rj];
    const de_entity t = *ex;
    *ex = *ey;
    *ey = t;
    for (u32 c = 0; c < a->column_count; ++c) {
        const u32 size = a->columns[c].size;
        swap_bytes((u8 *)ecs_chunk_column(a, x, c) + (size_t)ri * size,
                   (u8 *)ecs_chunk_column(a, y, c) + (size_t)rj * size, size);
        u32 *ax = &ecs_chunk_added(a, x, c)[ri], *ay = &ecs_chunk_added(a, y, c)[rj];
        u32 tt = *ax;
        *ax = *ay;
        *ay = tt;
        u32 *gx = &ecs_chunk_changed(a, x, c)[ri], *gy = &ecs_chunk_changed(a, y, c)[rj];
        tt = *gx;
        *gx = *gy;
        *gy = tt;
    }
    ecs_record *rx = (ecs_record *)de_array_at(&w->records, de_entity_index(*ex));
    ecs_record *ry = (ecs_record *)de_array_at(&w->records, de_entity_index(*ey));
    rx->chunk = ci;
    rx->row = ri;
    ry->chunk = cj;
    ry->row = rj;
}

/* The archetype at the section's recorded index if it has the section's set, else the one found
 * by set, else NULL. */
static ecs_archetype *section_archetype(const de_world *w, const sec_view *v) {
    if (v->h->archetype < ecs_archetype_count(w)) {
        ecs_archetype *a = ecs_archetype_at(w, v->h->archetype);
        if (a->component_count == v->h->set_count &&
            memcmp(a->components, v->set, (size_t)v->h->set_count * sizeof(u32)) == 0)
            return a;
    }
    return ecs_archetype_find(w, v->set, v->h->set_count);
}

/* true if table `a` holds exactly ids[0..rows) in this order. */
static bool table_matches(const ecs_archetype *a, const de_entity *ids, u32 rows) {
    if (a->count != rows)
        return false;
    u32 base = 0;
    for (u32 c = 0; c < a->chunks.count; ++c) {
        const ecs_chunk *ch = ecs_table_chunk(a, c);
        if (memcmp(ecs_chunk_entities(ch), ids + base, (size_t)ch->count * sizeof(de_entity)) != 0)
            return false;
        base += ch->count;
    }
    return true;
}

/* Phase 3 for one non-fast section: puts every entity in its target archetype. */
static de_result place_section(de_world *w, struct ecs_snap_state *st, const sec_view *v) {
    const ecs_archetype *last_cur = NULL;
    ecs_archetype *last_target = NULL;
    ecs_archetype *fresh = NULL; /* target of re-created entities: the snapshot set alone */
    for (u32 r = 0; r < v->h->rows; ++r) {
        const de_entity e = v->ids[r];
        const ecs_record *rec = (const ecs_record *)de_array_at(&w->records, de_entity_index(e));
        if (rec->archetype == ECS_INVALID_INDEX) {
            if (fresh == NULL)
                DE_TRY(target_archetype(w, st, NULL, v->snap, v->h->snap_count, &fresh));
            DE_TRY(recreate(w, fresh, e));
            ++st->stats.recreated;
            continue;
        }
        const ecs_archetype *cur = ecs_archetype_at(w, rec->archetype);
        if (cur != last_cur) {
            DE_TRY(target_archetype(w, st, cur, v->snap, v->h->snap_count, &last_target));
            last_cur = cur;
        }
        if (last_target != cur) {
            DE_TRY(ecs_entity_move(w, e, last_target));
            ++st->stats.moved;
        }
    }
    return DE_OK;
}

/* Phase 3 for snapshot entities without a section (no snapshot components at take). */
static de_result place_unsectioned(de_world *w, struct ecs_snap_state *st, const u32 *alive_bits,
                                   u32 record_count) {
    const u32 *placed = DE_ARRAY_DATA(&st->bits, u32);
    for (u32 i = 0; i < record_count; ++i) {
        if (!bit_get(alive_bits, i) || bit_get(placed, i))
            continue;
        const ecs_record *rec = (const ecs_record *)de_array_at(&w->records, i);
        const de_entity e = ecs_entity_make(i, rec->generation);
        if (rec->archetype == ECS_INVALID_INDEX) {
            DE_TRY(recreate(w, ecs_archetype_at(w, ECS_EMPTY_ARCHETYPE), e));
            ++st->stats.recreated;
            continue;
        }
        const ecs_archetype *cur = ecs_archetype_at(w, rec->archetype);
        u32 s = 0, c = 0;
        if (!arch_snapshot_counts(w, cur, &s, &c))
            continue;
        ecs_archetype *target = NULL;
        DE_TRY(target_archetype(w, st, cur, NULL, 0, &target));
        DE_TRY(ecs_entity_move(w, e, target));
        ++st->stats.moved;
    }
    return DE_OK;
}

/* Phase 4 for section `s`: SEC_ORDERED (its rows put first in its table, in snapshot order) when
 * every entity of the section is in one table that no earlier section claimed, else SEC_LOOSE.
 * Rows after the section's (entities placed there from other sections) keep their order. */
static void order_section(de_world *w, struct ecs_snap_state *st, const sec_view *v,
                          snap_sec_info *secs, u32 section_count, u32 s) {
    snap_sec_info *info = &secs[s];
    const u32 first =
        ((const ecs_record *)de_array_at(&w->records, de_entity_index(v->ids[0])))->archetype;
    ecs_archetype *a = ecs_archetype_at(w, first);
    bool one_table = true;
    /* A table is claimed by at most one section (FAST ones included): two sections can share a
     * table only when components without DE_COMP_SNAPSHOT changed since take. */
    for (u32 k = 0; k < section_count && one_table; ++k)
        one_table = !(secs[k].table == first &&
                      (secs[k].mode == SEC_FAST || (k < s && secs[k].mode == SEC_ORDERED)));
    for (u32 r = 1; r < v->h->rows && one_table; ++r) {
        one_table =
            ((const ecs_record *)de_array_at(&w->records, de_entity_index(v->ids[r])))->archetype ==
            first;
    }
    if (!one_table) {
        info->mode = SEC_LOOSE;
        ++st->stats.loose_sections;
        return;
    }
    /* Rows before r already hold their snapshot entities, so the entity for row r is at r or
     * later: one swap per misplaced row. */
    for (u32 r = 0; r < v->h->rows; ++r) {
        const ecs_record *rec =
            (const ecs_record *)de_array_at(&w->records, de_entity_index(v->ids[r]));
        const u32 at = rec->chunk * a->rows_per_chunk + rec->row;
        DE_ASSERT(at >= r);
        if (at != r) {
            swap_rows(w, a, r, at);
            ++st->stats.row_swaps;
        }
    }
    info->mode = SEC_ORDERED;
    info->table = first;
    ++st->stats.ordered_sections;
}

/* Phase 5 for one section. */
static void copy_section(de_world *w, struct ecs_snap_state *st, const sec_view *v,
                         const snap_sec_info *info) {
    const u32 now = ecs_change_tick(w); /* restore runs outside systems */
    if (info->mode == SEC_FAST || info->mode == SEC_ORDERED) {
        const ecs_archetype *a = ecs_archetype_at(w, info->table);
        for (u32 j = 0; j < v->h->column_count; ++j) {
            const u32 col = ecs_archetype_column(a, v->col_comp[j]);
            const u32 size = v->col_size[j];
            const u8 *src = v->base + sec_column_offset(v, j);
            /* The section's rows are the table's first rows (more may follow). */
            u32 left = v->h->rows;
            for (u32 c = 0; left > 0; ++c) {
                const ecs_chunk *ch = ecs_table_chunk(a, c);
                const u32 n = ch->count < left ? ch->count : left;
                memcpy(ecs_chunk_column(a, ch, col), src, (size_t)n * size);
                src += (size_t)n * size;
                u32 *changed = ecs_chunk_changed(a, ch, col);
                for (u32 r = 0; r < n; ++r)
                    changed[r] = now;
                left -= n;
                ++st->stats.block_copies;
            }
        }
        return;
    }
    for (u32 j = 0; j < v->h->column_count; ++j) {
        const u32 size = v->col_size[j];
        const u8 *src = v->base + sec_column_offset(v, j);
        for (u32 r = 0; r < v->h->rows; ++r) {
            const ecs_record *rec =
                (const ecs_record *)de_array_at(&w->records, de_entity_index(v->ids[r]));
            const ecs_archetype *a = ecs_archetype_at(w, rec->archetype);
            const u32 col = ecs_archetype_column(a, v->col_comp[j]);
            memcpy(ecs_row_column(a, rec->chunk, rec->row, col), src + (size_t)r * size, size);
            ecs_chunk_changed(a, ecs_table_chunk(a, rec->chunk), col)[rec->row] = now;
            ++st->stats.row_copies;
        }
    }
}

/* ---- Restore -------------------------------------------------------------------------------- */

de_result de_world_snapshot_restore(de_world *w, const de_world_snapshot *snap) {
    if (w == NULL || snap == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_world_snapshot_restore: world and snapshot "
                                                "are required");
    ECS_ASSERT_STRUCTURAL(w);
    struct ecs_snap_state *st = snap_state(w);
    if (st == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_world_snapshot_restore: out of memory");
    const snap_header *h = NULL;
    DE_TRY(snap_validate(w, st, snap, &h));
    const u8 *block = (const u8 *)snap->data;
    const u32 rc = h->record_count;
    const u32 *gens = (const u32 *)(const void *)(block + hdr_gens_offset());
    const u32 *alive_bits = (const u32 *)(const void *)(block + hdr_alive_offset(rc));
    const u32 *free_list = (const u32 *)(const void *)(block + hdr_free_offset(rc));

    /* Reservations: phases 1 and 2 must not allocate. The free list's capacity follows the
     * records' (ecs_entity_free relies on it). */
    if (de_array_reserve(&w->records, rc) != DE_OK ||
        de_array_reserve(&w->free_indices, w->records.capacity) != DE_OK)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_world_snapshot_restore: out of memory");

    /* Phase 1: despawn what the snapshot does not have. */
    for (u32 i = 0; i < w->records.count; ++i) {
        const ecs_record *rec = (const ecs_record *)de_array_at(&w->records, i);
        if (rec->archetype == ECS_INVALID_INDEX)
            continue;
        if (i < rc && bit_get(alive_bits, i) && gens[i] == rec->generation)
            continue;
        const de_result r = ecs_entity_despawn(w, ecs_entity_make(i, rec->generation));
        DE_VERIFY(r == DE_OK);
        ++st->stats.despawned;
    }

    /* Phase 2: the entity allocator. Every index past rc is free now; entities to re-create keep
     * an invalid archetype (not alive) until phase 3 gives them a row. */
    const u32 old_count = w->records.count;
    {
        const de_result r = de_array_resize(&w->records, rc); /* capacity reserved */
        DE_VERIFY(r == DE_OK);
    }
    ecs_record *recs = DE_ARRAY_DATA(&w->records, ecs_record);
    for (u32 i = 0; i < rc; ++i) {
        if (i < old_count && recs[i].archetype != ECS_INVALID_INDEX)
            continue; /* a survivor: same generation */
        recs[i].archetype = ECS_INVALID_INDEX;
        recs[i].chunk = 0;
        recs[i].row = 0;
        recs[i].generation = gens[i];
    }
    {
        const de_result r = de_array_resize(&w->free_indices, h->free_count);
        DE_VERIFY(r == DE_OK);
        if (h->free_count > 0)
            memcpy(w->free_indices.data, free_list, (size_t)h->free_count * sizeof(u32));
    }

    /* Phase 3: placement. A section whose table already holds its entities in order is fast. */
    snap_sec_info *secs = DE_ARRAY_DATA(&st->sec, snap_sec_info);
    for (u32 s = 0; s < h->section_count; ++s) {
        sec_view v;
        sec_view_at(block, secs[s].offset, &v);
        const ecs_archetype *a = section_archetype(w, &v);
        if (a != NULL && table_matches(a, v.ids, v.h->rows)) {
            secs[s].mode = SEC_FAST;
            secs[s].table = a->index;
            ++st->stats.fast_sections;
            continue;
        }
        DE_TRY(place_section(w, st, &v));
    }
    DE_TRY(place_unsectioned(w, st, alive_bits, rc));

    /* Phase 4: row order. After every placement, so later sections cannot disturb it. */
    for (u32 s = 0; s < h->section_count; ++s) {
        if (secs[s].mode == SEC_FAST)
            continue;
        sec_view v;
        sec_view_at(block, secs[s].offset, &v);
        order_section(w, st, &v, secs, h->section_count, s);
    }

    /* Phase 5: data. */
    for (u32 s = 0; s < h->section_count; ++s) {
        sec_view v;
        sec_view_at(block, secs[s].offset, &v);
        copy_section(w, st, &v, &secs[s]);
    }

    w->tick = h->tick;
    ++st->stats.restores;
    return DE_OK;
}
