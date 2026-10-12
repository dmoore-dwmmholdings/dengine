/* table.c: archetype tables: chunked SoA rows, swap-remove, row moves between tables
 * (DENG-344, SPEC §7.1, §5.3 pools).
 *
 * A table is dense: every chunk but the last is full, so the rows of an archetype are exactly
 * chunk 0 rows 0..n, chunk 1 rows 0..n, ... in that order (the SPEC §5.7 rule 5 order). Chunks
 * come from the world's de_pool; an emptied last chunk goes back to it at once. */
#include "ecs_internal.h"

de_result ecs_table_push_row(de_world *w, ecs_archetype *a, de_entity e, u32 *out_chunk,
                             u32 *out_row) {
    ecs_chunk *last =
        a->chunks.count > 0 ? ecs_table_chunk(a, a->chunks.count - 1u) : (ecs_chunk *)NULL;
    if (last == NULL || last->count == a->rows_per_chunk) {
        if (a->count == ECS_INVALID_INDEX)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: archetype table is full");
        u8 *mem = (u8 *)de_pool_alloc(w->chunk_pool);
        if (mem == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: out of memory for a table chunk");
        ecs_chunk ch;
        memset(&ch, 0, sizeof ch);
        ch.data = mem;
        if (de_array_push(&a->chunks, &ch) != DE_OK) {
            de_pool_free(w->chunk_pool, mem);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs: out of memory for a table chunk");
        }
        last = ecs_table_chunk(a, a->chunks.count - 1u);
    }
    const u32 row = last->count++;
    ++a->count;
    ecs_chunk_entities(last)[row] = e;
    *out_chunk = a->chunks.count - 1u;
    *out_row = row;
    return DE_OK;
}

void ecs_table_swap_remove(de_world *w, ecs_archetype *a, u32 chunk, u32 row) {
    DE_ASSERT(a->chunks.count > 0 && chunk < a->chunks.count);
    const u32 last_chunk = a->chunks.count - 1u;
    ecs_chunk *last = ecs_table_chunk(a, last_chunk);
    const u32 last_row = last->count - 1u;
    if (chunk != last_chunk || row != last_row) {
        ecs_chunk *dst = ecs_table_chunk(a, chunk);
        DE_ASSERT(row < dst->count);
        const de_entity moved = ecs_chunk_entities(last)[last_row];
        ecs_chunk_entities(dst)[row] = moved;
        for (u32 c = 0; c < a->column_count; ++c) {
            const u32 size = a->columns[c].size;
            memcpy((u8 *)ecs_chunk_column(a, dst, c) + (size_t)row * size,
                   (const u8 *)ecs_chunk_column(a, last, c) + (size_t)last_row * size, size);
            ecs_chunk_added(a, dst, c)[row] = ecs_chunk_added(a, last, c)[last_row];
            ecs_chunk_changed(a, dst, c)[row] = ecs_chunk_changed(a, last, c)[last_row];
        }
        ecs_record *rec = (ecs_record *)de_array_at(&w->records, de_entity_index(moved));
        DE_ASSERT(rec->archetype == a->index && rec->chunk == last_chunk && rec->row == last_row);
        rec->chunk = chunk;
        rec->row = row;
    }
    --last->count;
    --a->count;
    if (last->count == 0) {
        de_pool_free(w->chunk_pool, last->data);
        de_array_pop(&a->chunks, NULL);
    }
}

void ecs_row_construct(de_world *w, ecs_archetype *a, u32 chunk, u32 row, u32 column) {
    const ecs_component_info *info = ecs_component_get(w, a->columns[column].component);
    void *data = ecs_row_column(a, chunk, row, column);
    if (info->hooks.construct != NULL)
        info->hooks.construct(data);
    else
        memset(data, 0, info->size);
    ecs_row_init_ticks(w, a, chunk, row, column);
}

void ecs_row_destruct(de_world *w, ecs_archetype *a, u32 chunk, u32 row, u32 column) {
    const ecs_component_info *info = ecs_component_get(w, a->columns[column].component);
    if (info->hooks.destruct != NULL)
        info->hooks.destruct(ecs_row_column(a, chunk, row, column));
}

de_result ecs_entity_move(de_world *w, de_entity e, ecs_archetype *dst) {
    ecs_record *rec = ecs_entity_record(w, e);
    if (rec == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "ecs: entity 0x%016llx is not alive",
                       (unsigned long long)e.bits);
    ecs_archetype *src = ecs_archetype_at(w, rec->archetype);
    if (src == dst)
        return DE_OK;

    u32 dc = 0, dr = 0;
    DE_TRY(ecs_table_push_row(w, dst, e, &dc, &dr));
    const u32 sc = rec->chunk, sr = rec->row;
    const ecs_chunk *src_chunk = ecs_table_chunk(src, sc);
    const ecs_chunk *dst_chunk = ecs_table_chunk(dst, dc);

    /* Columns of the destination: move the shared ones (bytes and ticks), construct new ones. */
    for (u32 c = 0; c < dst->column_count; ++c) {
        const u32 s = ecs_archetype_column(src, dst->columns[c].component);
        if (s == ECS_INVALID_INDEX) {
            ecs_row_construct(w, dst, dc, dr, c);
            continue;
        }
        const u32 size = dst->columns[c].size;
        memcpy((u8 *)ecs_chunk_column(dst, dst_chunk, c) + (size_t)dr * size,
               (const u8 *)ecs_chunk_column(src, src_chunk, s) + (size_t)sr * size, size);
        ecs_chunk_added(dst, dst_chunk, c)[dr] = ecs_chunk_added(src, src_chunk, s)[sr];
        ecs_chunk_changed(dst, dst_chunk, c)[dr] = ecs_chunk_changed(src, src_chunk, s)[sr];
    }
    /* Columns only the source has are dropped: destruct them. */
    for (u32 c = 0; c < src->column_count; ++c) {
        if (!ecs_archetype_has(dst, src->columns[c].component))
            ecs_row_destruct(w, src, sc, sr, c);
    }

    ecs_table_swap_remove(w, src, sc, sr);
    rec->archetype = dst->index;
    rec->chunk = dc;
    rec->row = dr;
    return DE_OK;
}

/* ---- Change ticks (default; DENG-347 owns the semantics) ------------------------------------ */

void ecs_row_init_ticks(de_world *w, ecs_archetype *a, u32 chunk, u32 row, u32 column) {
    const ecs_chunk *ch = ecs_table_chunk(a, chunk);
    ecs_chunk_added(a, ch, column)[row] = w->change_tick;
    ecs_chunk_changed(a, ch, column)[row] = w->change_tick;
}
