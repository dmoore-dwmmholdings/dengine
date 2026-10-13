/*
 * graph_color.c: deterministic greedy graph coloring of the solver's constraint records
 * (DENG-218, SPEC §13.2, §5.7; see graph_color.h).
 *
 * One pass over the records in record order. Color k has a bitset over the solver slots; a record
 * takes the first color whose bitset has none of its writable sides, and sets them. A side that
 * the record may not write (the static slot, a kinematic or sleeping body, the world) is only
 * read during a stage and so never conflicts. Records that fit none of the DE__GRAPH_MAX_COLORS
 * colors form the overflow set. Then a counting sort by color gives `order`, the records of each
 * color in record order.
 */
#include "physics/solver/graph_color.h"

#include "physics/solver/solver_internal.h"

#include <dengine/de_debug.h>

/* ================================ Layout =============================================== */

#define GC_ALIGN 16u

static size_t gc_align(size_t x) {
    return (x + (GC_ALIGN - 1u)) & ~(size_t)(GC_ALIGN - 1u);
}

static u32 gc_words(u32 slots) {
    return (slots + 63u) / 64u;
}

size_t de__coloring_bytes(u32 slot_capacity) {
    size_t off = gc_align(sizeof(de__coloring));
    off = gc_align(off + sizeof(u64) * (size_t)DE__GRAPH_MAX_COLORS * gc_words(slot_capacity));
    off = gc_align(off + sizeof(u32) * (size_t)slot_capacity);
    return off;
}

static u32 read_grain(void) {
    de_cvar_desc d;
    memset(&d, 0, sizeof d);
    d.name = DE__SOLVER_GRAIN_CVAR;
    d.help = "Physics: constraints per parallel chunk of a solver color (read when the solver is "
             "created; never changes results)";
    d.type = DE_CVAR_INT;
    d.default_number = (f64)DE__SOLVER_DEFAULT_GRAIN;
    d.min = 1.0;
    d.max = 1048576.0;
    const de_cvar cv = de_cvar_register(&d);
    if (!DE_HANDLE_VALID(cv))
        return DE__SOLVER_DEFAULT_GRAIN;
    const i64 v = de_cvar_int(cv);
    return v >= 1 && v <= 1048576 ? (u32)v : DE__SOLVER_DEFAULT_GRAIN;
}

de__coloring *de__coloring_init(void *mem, u32 slot_capacity) {
    u8 *m = (u8 *)mem;
    de__coloring *c = (de__coloring *)mem;
    memset(c, 0, sizeof *c);
    c->slot_capacity = slot_capacity;
    c->words = gc_words(slot_capacity);
    size_t off = gc_align(sizeof(de__coloring));
    c->bits = (u64 *)(void *)(m + off);
    off = gc_align(off + sizeof(u64) * (size_t)DE__GRAPH_MAX_COLORS * c->words);
    c->stamp = (u32 *)(void *)(m + off);
    c->grain = read_grain();
    c->body_grain = DE__SOLVER_BODY_GRAIN;
    return c;
}

/* ================================ Coloring ============================================= */

static bool bit_test(const u64 *bits, u32 slot) {
    return ((bits[slot >> 6] >> (slot & 63u)) & 1u) != 0u;
}

static void bit_set(u64 *bits, u32 slot) {
    bits[slot >> 6] |= (u64)1u << (slot & 63u);
}

void de__graph_color(de__solver *s) {
    de__coloring *g = s->coloring;
    const u32 n = s->constraint_count;
    DE_ASSERT(n <= g->order_capacity);
    DE_ASSERT(s->body_count <= g->slot_capacity);
    const size_t used = gc_words(s->body_count);
    for (u32 k = 0; k < DE__GRAPH_MAX_COLORS; ++k)
        memset(g->bits + (size_t)k * g->words, 0, sizeof(u64) * used);

    u32 count[DE__GRAPH_MAX_COLORS + 1u];
    memset(count, 0, sizeof count);
    u32 colors = 0;
    for (u32 i = 0; i < n; ++i) {
        de__constraint *c = &s->constraints[i];
        const bool wa = (c->flags & DE__CONSTRAINT_WRITE_A) != 0u;
        const bool wb = (c->flags & DE__CONSTRAINT_WRITE_B) != 0u;
        u32 color = DE__GRAPH_OVERFLOW;
        for (u32 k = 0; k < DE__GRAPH_MAX_COLORS; ++k) {
            u64 *bk = g->bits + (size_t)k * g->words;
            if ((wa && bit_test(bk, c->solver_a)) || (wb && bit_test(bk, c->solver_b)))
                continue;
            if (wa)
                bit_set(bk, c->solver_a);
            if (wb)
                bit_set(bk, c->solver_b);
            color = k;
            colors = k + 1u > colors ? k + 1u : colors;
            break;
        }
        c->color = color;
        ++count[color];
    }

    g->start[0] = 0;
    u32 largest = 0;
    for (u32 k = 0; k <= DE__GRAPH_OVERFLOW; ++k) {
        g->start[k + 1u] = g->start[k] + count[k];
        if (k < DE__GRAPH_MAX_COLORS)
            largest = count[k] > largest ? count[k] : largest;
    }
    u32 next[DE__GRAPH_MAX_COLORS + 1u];
    memcpy(next, g->start, sizeof next);
    for (u32 i = 0; i < n; ++i)
        g->order[next[s->constraints[i].color]++] = i;

    g->color_count = colors;
    g->stats.colors = colors;
    g->stats.overflow = count[DE__GRAPH_OVERFLOW];
    g->stats.largest = largest;
    const u32 grain = g->grain > 0u ? g->grain : 1u;
    u32 chunks = 0;
    for (u32 k = 0; k < colors; ++k)
        chunks += (count[k] + grain - 1u) / grain;
    g->stats.chunks = chunks;
#if defined(DE_DEBUG)
    DE_ASSERT(de__graph_color_validate(s));
#endif
}

bool de__graph_color_validate(const de__solver *s) {
    const de__coloring *g = s->coloring;
    const u32 n = s->constraint_count;
    if (g->start[0] != 0u || g->start[DE__GRAPH_OVERFLOW + 1u] != n)
        return false;
    memset(g->stamp, 0, sizeof(u32) * (size_t)s->body_count);
    for (u32 k = 0; k <= DE__GRAPH_OVERFLOW; ++k) {
        if (g->start[k + 1u] < g->start[k])
            return false;
        u32 prev = 0;
        for (u32 j = g->start[k]; j < g->start[k + 1u]; ++j) {
            const u32 i = g->order[j];
            /* Ascending within a color: no record twice, and with the color check and the total,
             * every record exactly once. */
            if (i >= n || (j > g->start[k] && i <= prev))
                return false;
            prev = i;
            const de__constraint *c = &s->constraints[i];
            if (c->color != k)
                return false;
            if (k == DE__GRAPH_OVERFLOW)
                continue; /* solved serially */
            const u32 sides[2] = {(c->flags & DE__CONSTRAINT_WRITE_A) ? c->solver_a : 0u,
                                  (c->flags & DE__CONSTRAINT_WRITE_B) ? c->solver_b : 0u};
            for (u32 e = 0; e < 2; ++e) {
                const u32 slot = sides[e];
                if (slot == DE__SOLVER_STATIC_SLOT)
                    continue;
                if (slot >= s->body_count || g->stamp[slot] == k + 1u)
                    return false;
                g->stamp[slot] = k + 1u;
            }
        }
    }
    return true;
}
