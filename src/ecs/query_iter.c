/* query_iter.c: query iteration: slices (de_query_chunk), de_query_each, de_query_count and the
 * deterministic de_query_par_each (DENG-346, SPEC §7.2, §5.7 rule 5, §8).
 *
 * Order: matched archetypes in creation order (the query's entries, then any archetypes the
 * cache could not store yet), each table's chunks in order, rows in order. The row space of
 * de_query_par_each numbers the rows of that order from 0; range k is rows
 * [k * grain, (k + 1) * grain), so which rows a range holds depends only on the grain and the data.
 *
 * Every slice goes through emit_rows -> emit_slice, the one place that applies the change
 * filters and builds a de_query_chunk. DENG-347's tick logic is there (the predicate and the
 * write marking in query_slice_done); DENG-350 adds per-range command buffers to emit_slice /
 * run_range.
 *
 * Nothing here allocates. Iteration only reads the world and the query, except
 * de_query_par_each, which first writes the row prefix of each entry (entry[1]), and the write
 * marking of DE_WRITE / DE_OPTIONAL_WRITE terms, which writes the changed ticks of the yielded
 * rows (DENG-347). */
#include "query_internal.h"

#include "change_internal.h" /* DENG-347: tick test and write marking */

/* State shared by every slice of one de_query_each / de_query_par_each call. */
typedef struct query_run {
    de_world *w;
    const ecs_query *q;
    de_chunk_fn fn;
    void *user;
    u32 grain;        /* rows per range (par_each) */
    u32 matched_rows; /* rows of the query's cached entries (the prefix total) */
} query_run;

bool ecs_change_filter_default(const ecs_query *q, const ecs_archetype *a, const ecs_chunk *ch,
                               const u32 *filter_cols, u32 row) {
    /* DENG-347: the added / changed tick test against the query's window
     * (last_run_tick, this_run_tick] (change.c). */
    return ecs_change_filter_ticks(q, a, ch, filter_cols, row);
}

/* Called after the callback has seen rows [row, row + count) of chunk `ch`. DENG-347: marks the
 * changed ticks of the yielded rows in every DE_WRITE / DE_OPTIONAL_WRITE data column
 * (conservative: written or not). Only this slice's rows, on this thread: no atomics needed. */
static void query_slice_done(const query_run *r, const ecs_archetype *a, const ecs_chunk *ch,
                             const u32 *cols, u32 row, u32 count) {
    ecs_change_mark_slice(r->w, r->q, a, ch, cols, row, count);
}

/* Hands rows [row, row + count) of chunk `ch` of `a` to the callback as one slice. */
static void emit_slice(const query_run *r, ecs_archetype *a, const ecs_chunk *ch, const u32 *cols,
                       u32 row, u32 count, u32 first_row, u32 range) {
    const ecs_query *q = r->q;
    void *columns[DE_QUERY_MAX_TERMS] = {0};
    for (u32 i = 0; i < q->term_count; ++i) {
        const u32 c = cols[i];
        if (c == ECS_QUERY_COL_ABSENT)
            columns[i] = NULL;
        else if (c == ECS_QUERY_COL_TAG)
            columns[i] = (void *)r->w->tag_placeholder;
        else
            columns[i] = (u8 *)ecs_chunk_column(a, ch, c) + (size_t)row * a->columns[c].size;
    }
    de_query_chunk qc;
    memset(&qc, 0, sizeof qc);
    qc.count = count;
    qc.entities = ecs_chunk_entities(ch) + row;
    qc.columns = columns;
    qc.range = range;
    qc.first_row = first_row;
    r->fn(&qc, r->user);
    query_slice_done(r, a, ch, cols, row, count);
}

/* Rows [row, row + count) of chunk `chunk` of `a`: one slice, or with change filters the maximal
 * runs of rows that pass them. `first_row` is the row-space number of `row`. */
static void emit_rows(const query_run *r, ecs_archetype *a, u32 chunk, const u32 *cols, u32 row,
                      u32 count, u32 first_row, u32 range) {
    const ecs_query *q = r->q;
    const ecs_chunk *ch = ecs_table_chunk(a, chunk);
    if (q->change_count == 0) {
        emit_slice(r, a, ch, cols, row, count, first_row, range);
        return;
    }
    const u32 *filter_cols = cols + q->term_count;
    const u32 end = row + count;
    u32 i = row;
    while (i < end) {
        while (i < end && !q->change_filter(q, a, ch, filter_cols, i))
            ++i;
        const u32 start = i;
        while (i < end && q->change_filter(q, a, ch, filter_cols, i))
            ++i;
        if (i > start)
            emit_slice(r, a, ch, cols, start, i - start, first_row + (start - row), range);
    }
}

/* Table rows [off, off + count) of `a` (table order: chunk by chunk; every chunk but the last is
 * full), cut at chunk boundaries. */
static void emit_table_rows(const query_run *r, ecs_archetype *a, const u32 *cols, u32 off,
                            u32 count, u32 first_row, u32 range) {
    const u32 rpc = a->rows_per_chunk;
    u32 chunk = off / rpc;
    u32 row = off % rpc;
    while (count > 0) {
        const ecs_chunk *ch = ecs_table_chunk(a, chunk);
        DE_ASSERT(row < ch->count);
        const u32 avail = ch->count - row;
        const u32 take = avail < count ? avail : count;
        emit_rows(r, a, chunk, cols, row, take, first_row, range);
        first_row += take;
        count -= take;
        ++chunk;
        row = 0;
    }
}

/* The next matched archetype with rows. The caller knows that one exists. */
static void next_nonempty(const query_run *r, ecs_query_iter *it, ecs_archetype **a,
                          const u32 **cols) {
    for (;;) {
        const bool ok = ecs_query_iter_next(r->w, r->q, it, a, cols);
        DE_ASSERT(ok);
        if (!ok || (*a)->count > 0)
            return;
    }
}

void de_query_each(de_world *w, de_query qh, de_chunk_fn fn, void *user) {
    const ecs_query *q = ecs_query_get(w, qh);
    if (q == NULL || fn == NULL)
        return;
    query_run r;
    memset(&r, 0, sizeof r);
    r.w = w;
    r.q = q;
    r.fn = fn;
    r.user = user;
    ecs_query_iter it;
    ecs_query_iter_init(q, &it, 0);
    ecs_archetype *a = NULL;
    const u32 *cols = NULL;
    u32 first_row = 0;
    while (ecs_query_iter_next(w, q, &it, &a, &cols)) {
        if (a->count == 0)
            continue;
        emit_table_rows(&r, a, cols, 0, a->count, first_row, 0);
        first_row += a->count;
    }
}

u32 de_query_count(de_world *w, de_query qh) {
    const ecs_query *q = ecs_query_get(w, qh);
    if (q == NULL)
        return 0;
    ecs_query_iter it;
    ecs_query_iter_init(q, &it, 0);
    ecs_archetype *a = NULL;
    const u32 *cols = NULL;
    u32 n = 0;
    while (ecs_query_iter_next(w, q, &it, &a, &cols))
        n += a->count;
    return n;
}

/* Runs range [begin, end) of the row space: finds the archetype that holds row `begin`, then
 * emits rows up to `end`, crossing chunk and archetype boundaries. */
static void run_range(const query_run *r, u32 begin, u32 end) {
    const ecs_query *q = r->q;
    const u32 range = begin / r->grain;
    ecs_query_iter it;
    ecs_archetype *a = NULL;
    const u32 *cols = NULL;
    u32 off;
    if (begin < r->matched_rows) {
        /* Binary search of the row prefixes: the last entry whose prefix is <= begin. It has
         * rows (the next entry's prefix is > begin), so empty archetypes are skipped. */
        u32 lo = 0, hi = ecs_query_entry_count(q);
        while (hi - lo > 1u) {
            const u32 mid = lo + (hi - lo) / 2u;
            if (ecs_query_entry(q, mid)[1] <= begin)
                lo = mid;
            else
                hi = mid;
        }
        ecs_query_iter_init(q, &it, lo);
        next_nonempty(r, &it, &a, &cols);
        off = begin - ecs_query_entry(q, lo)[1];
    } else {
        /* Rows of archetypes the cache could not store yet (after an out of memory). */
        ecs_query_iter_init(q, &it, ecs_query_entry_count(q));
        u32 base = r->matched_rows;
        for (;;) {
            next_nonempty(r, &it, &a, &cols);
            if (begin - base < a->count)
                break;
            base += a->count;
        }
        off = begin - base;
    }
    u32 row = begin;
    for (;;) {
        const u32 avail = a->count - off;
        const u32 take = avail < end - row ? avail : end - row;
        emit_table_rows(r, a, cols, off, take, row, range);
        row += take;
        if (row == end)
            return;
        next_nonempty(r, &it, &a, &cols);
        off = 0;
    }
}

static void par_range_fn(u32 begin, u32 end, void *user) {
    run_range((const query_run *)user, begin, end);
}

void de_query_par_each(de_world *w, de_query qh, u32 grain_rows, de_chunk_fn fn, void *user) {
    ecs_query *q = ecs_query_get(w, qh);
    if (q == NULL || fn == NULL)
        return;
    query_run r;
    memset(&r, 0, sizeof r);
    r.w = w;
    r.q = q;
    r.fn = fn;
    r.user = user;
    r.grain = grain_rows != 0 ? grain_rows : DE_QUERY_DEFAULT_GRAIN_ROWS;

    /* Row prefix of each cached entry, for the ranges' binary search. */
    u32 total = 0;
    for (u32 k = 0; k < ecs_query_entry_count(q); ++k) {
        u32 *e = ecs_query_entry(q, k);
        e[1] = total;
        total += ecs_archetype_at(w, e[0])->count;
    }
    r.matched_rows = total;
    ecs_query_iter it;
    ecs_query_iter_init(q, &it, ecs_query_entry_count(q));
    ecs_archetype *a = NULL;
    const u32 *cols = NULL;
    while (ecs_query_iter_next(w, q, &it, &a, &cols))
        total += a->count;
    if (total == 0)
        return;

    if (w->tasks != NULL) {
        /* An explicit grain, never 0: grain 0 depends on the machine's worker count. */
        de_task_parallel_for(w->tasks, total, r.grain, par_range_fn, &r);
        return;
    }
    for (u64 begin = 0; begin < total; begin += r.grain) {
        const u64 end = begin + r.grain < total ? begin + r.grain : total;
        run_range(&r, (u32)begin, (u32)end);
    }
}
