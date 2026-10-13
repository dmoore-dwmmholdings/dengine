/* broadphase.c: static + dynamic AABB trees and the sorted pair list (DENG-133). See
 * broadphase.h. */
#include "physics/broadphase/broadphase.h"

#include "core/de_sort.h"

#include <dengine/de_debug.h>

/* ---- State -------------------------------------------------------------------------------- */

struct de__broadphase {
    const de_allocator *alloc;
    u32 capacity;  /**< max_bodies */
    u32 max_pairs; /**< capacity of every pair buffer */
    f32 margin;
    f32 pair_distance; /**< DENG-135: output pairs whose world AABBs are this close (0..margin) */

    de__tree_pool pool;
    de__tree trees[DE__BP_TREE_COUNT];

    u32 *proxy; /**< [capacity] leaf of body i, or DE__TREE_NULL */
    u8 *moved;  /**< [capacity] 1 when the proxy was (re)inserted since the last pair pass */
    u32 moved_count;

    u64 *cand[2]; /**< [max_pairs] sorted unique candidate keys; cand[cur] is live */
    u32 cur;
    u32 cand_count;

    de_sort_key64 *fresh; /**< [max_pairs] keys found by this update's queries */
    u32 fresh_count;
    bool fresh_overflow;

    de__bp_pair *pairs; /**< [max_pairs] output */
    u32 pair_count;

    /* de_radix_sort_u64 scratch: a fixed buffer behind an allocator interface. */
    u8 *sort_buf;
    size_t sort_buf_size;
    bool sort_busy;
    de_allocator sort_alloc;

    bool requery_all;     /**< after an overflow: query every proxy on the next update */
    bool overflow_logged; /**< warned since the last update that fit */
    u32 queried;
    u32 overflows;

    /* Query context of the pair pass. */
    const de_physics_world *qp;
    u32 qbody;

    void *block; /**< the one allocation behind proxy .. sort_buf */
    size_t block_size;
};

static u64 pair_key(u32 a, u32 b) {
    return a < b ? ((u64)a << 32) | (u64)b : ((u64)b << 32) | (u64)a;
}
static u32 key_a(u64 k) {
    return (u32)(k >> 32);
}
static u32 key_b(u64 k) {
    return (u32)(k & 0xFFFFFFFFu);
}

static u32 tree_of(const de_physics_world *p, u32 i) {
    return p->bodies.type[i] == (u8)DE_BODY_STATIC ? (u32)DE__BP_TREE_STATIC
                                                   : (u32)DE__BP_TREE_DYNAMIC;
}

static bool box_valid(const de_aabb *b) {
    return b->min.x <= b->max.x && b->min.y <= b->max.y && b->min.z <= b->max.z;
}

/* The world box grown by the margin, rounded outward (computed in f64). */
static de_aabb fat_box(const de_aabb *b, f32 margin) {
    const f64 m = (f64)margin;
    de_aabb r;
    r.min.x = de__f32_round_down((f64)b->min.x - m);
    r.min.y = de__f32_round_down((f64)b->min.y - m);
    r.min.z = de__f32_round_down((f64)b->min.z - m);
    r.max.x = de__f32_round_up((f64)b->max.x + m);
    r.max.y = de__f32_round_up((f64)b->max.y + m);
    r.max.z = de__f32_round_up((f64)b->max.z + m);
    return r;
}

/* ---- Fixed scratch allocator for the radix sort ------------------------------------------- */

static void *sort_scratch_alloc(void *user, size_t size, size_t align, u32 tag) {
    de__broadphase *bp = (de__broadphase *)user;
    (void)tag;
    if (bp->sort_busy || size > bp->sort_buf_size || align > 16u)
        return NULL;
    bp->sort_busy = true;
    return bp->sort_buf;
}
static void *sort_scratch_realloc(void *user, void *ptr, size_t old_size, size_t new_size,
                                  size_t align, u32 tag) {
    (void)user;
    (void)ptr;
    (void)old_size;
    (void)new_size;
    (void)align;
    (void)tag;
    return NULL;
}
static void sort_scratch_free(void *user, void *ptr, size_t size) {
    de__broadphase *bp = (de__broadphase *)user;
    (void)ptr;
    (void)size;
    bp->sort_busy = false;
}

/* ---- Proxies ------------------------------------------------------------------------------ */

static void proxy_insert(de_physics_world *p, de__broadphase *bp, u32 i) {
    const de_aabb *box = &p->bodies.world_aabb[i];
    if (!box_valid(box)) /* no shape: no proxy */
        return;
    const u32 leaf = de__tree_insert(&bp->trees[tree_of(p, i)], fat_box(box, bp->margin), i);
    /* The pool holds 2 * max_bodies nodes: one leaf and at most one internal node per body. */
    DE_ASSERT(leaf != DE__TREE_NULL);
    bp->proxy[i] = leaf;
    if (leaf != DE__TREE_NULL && !bp->moved[i]) {
        bp->moved[i] = 1;
        ++bp->moved_count;
    }
}

static void proxy_remove(de_physics_world *p, de__broadphase *bp, u32 i) {
    if (bp->proxy[i] == DE__TREE_NULL)
        return;
    de__tree_remove(&bp->trees[tree_of(p, i)], bp->proxy[i]);
    bp->proxy[i] = DE__TREE_NULL;
    if (bp->moved[i]) {
        bp->moved[i] = 0;
        --bp->moved_count;
    }
}

/* ---- Slot callbacks ----------------------------------------------------------------------- */

static void bp_free(de__broadphase *bp) {
    const de_allocator *a = bp->alloc;
    de__tree_pool_free(&bp->pool, a);
    if (bp->block != NULL)
        DE_FREE(a, bp->block, bp->block_size);
    DE_FREE(a, bp, sizeof *bp);
}

static void ext_destroy(de_physics_world *p, void *state) {
    (void)p;
    bp_free((de__broadphase *)state);
}

static void ext_body_added(de_physics_world *p, void *state, u32 index) {
    proxy_insert(p, (de__broadphase *)state, index);
}

static void ext_body_removed(de_physics_world *p, void *state, u32 index) {
    proxy_remove(p, (de__broadphase *)state, index);
}

static const de__phys_ext_vtbl k_bp_vtbl = {ext_destroy, ext_body_added, ext_body_removed};

/* ---- Create ------------------------------------------------------------------------------- */

/* Registers (or finds) the cvars; registration takes the registry lock, so only at create. */
static de_cvar margin_cvar(void) {
    de_cvar_desc d;
    memset(&d, 0, sizeof d);
    d.name = DE__BP_MARGIN_CVAR;
    d.help = "Physics broadphase fat-AABB margin in meters (read when the broadphase is created; "
             "performance only, never changes the pairs)";
    d.type = DE_CVAR_FLOAT;
    d.default_number = (f64)DE__BP_DEFAULT_MARGIN;
    d.min = 0.0;
    d.max = 10.0;
    return de_cvar_register(&d);
}

static de_cvar max_pairs_cvar(void) {
    de_cvar_desc d;
    memset(&d, 0, sizeof d);
    d.name = DE__BP_MAX_PAIRS_CVAR;
    d.help = "Physics broadphase pair capacity (read when the broadphase is created; 0 = "
             "max(1024, 4 * max_bodies))";
    d.type = DE_CVAR_INT;
    d.default_number = 0.0;
    d.min = 0.0;
    d.max = 67108864.0;
    return de_cvar_register(&d);
}

#define BP_ALIGN 16u

static size_t align_up(size_t x) {
    return (x + (BP_ALIGN - 1u)) & ~(size_t)(BP_ALIGN - 1u);
}

de_result de__broadphase_create(de_physics_world *p, const de__broadphase_desc *desc) {
    if (p == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__broadphase_create: world is NULL");
    if (de__broadphase_get(p) != NULL)
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "de__broadphase_create: broadphase exists");
    de__broadphase_desc d;
    if (desc != NULL)
        d = *desc;
    else
        memset(&d, 0, sizeof d);
    if (!de__f32_finite(d.margin) || d.margin < 0.0f)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__broadphase_create: bad margin");

    const de_cvar mcv = margin_cvar();
    const de_cvar pcv = max_pairs_cvar();
    f32 margin = d.margin;
    if (margin == 0.0f)
        margin = DE_HANDLE_VALID(mcv) ? (f32)de_cvar_float(mcv) : DE__BP_DEFAULT_MARGIN;
    if (!de__f32_finite(margin) || margin < 0.0f)
        margin = DE__BP_DEFAULT_MARGIN;
    const u32 capacity = p->bodies.capacity;
    u64 max_pairs = d.max_pairs;
    if (max_pairs == 0 && DE_HANDLE_VALID(pcv) && de_cvar_int(pcv) > 0)
        max_pairs = (u64)de_cvar_int(pcv);
    if (max_pairs == 0) {
        max_pairs = 4u * (u64)capacity;
        if (max_pairs < DE__BP_MIN_PAIRS)
            max_pairs = DE__BP_MIN_PAIRS;
    }
    if (max_pairs > 0x7FFFFFFFu)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__broadphase_create: max_pairs too large");

    const de_allocator *a = p->alloc;
    de__broadphase *bp =
        (de__broadphase *)DE_ALLOC(a, sizeof *bp, DE_ALIGNOF(de__broadphase), DE_MEM_PHYSICS);
    if (bp == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__broadphase_create: state");
    memset(bp, 0, sizeof *bp);
    bp->alloc = a;
    bp->capacity = capacity;
    bp->max_pairs = (u32)max_pairs;
    bp->margin = margin;
    bp->pool.free_head = DE__TREE_NULL;

    /* One block: proxy, moved, cand[0], cand[1], fresh, pairs, sort scratch. */
    const size_t mp = (size_t)max_pairs;
    size_t off[7], size = 0;
    const size_t bytes[7] = {sizeof(u32) * capacity,     sizeof(u8) * capacity,
                             sizeof(u64) * mp,           sizeof(u64) * mp,
                             sizeof(de_sort_key64) * mp, sizeof(de__bp_pair) * mp,
                             sizeof(de_sort_key64) * mp};
    for (u32 k = 0; k < 7; ++k) {
        off[k] = align_up(size);
        size = off[k] + bytes[k];
    }
    size = align_up(size);
    u8 *block = (u8 *)DE_ALLOC(a, size, BP_ALIGN, DE_MEM_PHYSICS);
    if (block == NULL) {
        DE_FREE(a, bp, sizeof *bp);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__broadphase_create: %zu bytes of pair buffers",
                       size);
    }
    memset(block, 0, size);
    bp->block = block;
    bp->block_size = size;
    bp->proxy = (u32 *)(block + off[0]);
    bp->moved = block + off[1];
    bp->cand[0] = (u64 *)(block + off[2]);
    bp->cand[1] = (u64 *)(block + off[3]);
    bp->fresh = (de_sort_key64 *)(block + off[4]);
    bp->pairs = (de__bp_pair *)(block + off[5]);
    bp->sort_buf = block + off[6];
    bp->sort_buf_size = bytes[6];
    bp->sort_alloc.alloc = sort_scratch_alloc;
    bp->sort_alloc.realloc = sort_scratch_realloc;
    bp->sort_alloc.free = sort_scratch_free;
    bp->sort_alloc.user = bp;
    for (u32 i = 0; i < capacity; ++i)
        bp->proxy[i] = DE__TREE_NULL;

    /* Each body has one leaf and adds at most one internal node to its tree. */
    const de_result r = de__tree_pool_init(&bp->pool, a, 2u * capacity);
    if (r != DE_OK) {
        bp_free(bp);
        return r;
    }
    for (u32 t = 0; t < DE__BP_TREE_COUNT; ++t)
        de__tree_init(&bp->trees[t], &bp->pool);

    for (u32 i = 0; i < p->bodies.high_water; ++i)
        if (p->bodies.alive[i])
            proxy_insert(p, bp, i);

    de__phys_ext_set(p, DE__PHYS_EXT_BROADPHASE, bp, &k_bp_vtbl);
    return DE_OK;
}

de_result de__broadphase_ensure(de_physics_world *p, de__broadphase **out) {
    if (p == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__broadphase_ensure: world is NULL");
    de__broadphase *bp = de__broadphase_get(p);
    if (bp == NULL) {
        const de_result r = de__broadphase_create(p, NULL);
        if (r != DE_OK)
            return r;
        bp = de__broadphase_get(p);
    }
    if (out != NULL)
        *out = bp;
    return DE_OK;
}

/* ---- Update ------------------------------------------------------------------------------- */

static void sync(de_physics_world *p, de__broadphase *bp) {
    const de__body_pool *b = &p->bodies;
    for (u32 i = 0; i < b->high_water; ++i) {
        const u32 leaf = bp->proxy[i];
        if (!b->alive[i] || leaf == DE__TREE_NULL)
            continue;
        de__tree *t = &bp->trees[tree_of(p, i)];
        const de_aabb fat = de__tree_box(t, leaf);
        /* With a pair distance d (DENG-135) the fat box must hold the world box grown by d, so
         * two world boxes within d of each other always have overlapping fat boxes. */
        const de_aabb need = bp->pair_distance > 0.0f
                                 ? fat_box(&b->world_aabb[i], bp->pair_distance)
                                 : b->world_aabb[i];
        if (de__aabb_contains(&fat, &need))
            continue;
        de__tree_move(t, leaf, fat_box(&b->world_aabb[i], bp->margin));
        if (!bp->moved[i]) {
            bp->moved[i] = 1;
            ++bp->moved_count;
        }
    }
}

void de__broadphase_sync(de_physics_world *p) {
    de__broadphase *bp = p != NULL ? de__broadphase_get(p) : NULL;
    if (bp != NULL)
        sync(p, bp);
}

/* World boxes a and b overlap, or (DENG-135) are within distance d on every axis. Such pairs are
 * always candidates: sync keeps every fat box around its world box grown by d (d <= margin), and
 * boxes within d have overlapping grown boxes (the f32 sums here round no further out than the
 * outward-rounded grown boxes). */
static bool boxes_within(const de_aabb *a, const de_aabb *b, f32 d) {
    if (d == 0.0f)
        return de__aabb_overlap(a, b);
    return a->min.x <= b->max.x + d && b->min.x <= a->max.x + d && a->min.y <= b->max.y + d &&
           b->min.y <= a->max.y + d && a->min.z <= b->max.z + d && b->min.z <= a->max.z + d;
}

/* Fat boxes of live bodies a and b overlap (both have proxies). */
static bool fat_overlap(const de_physics_world *p, const de__broadphase *bp, u32 a, u32 b) {
    const de_aabb ba = de__tree_box(&bp->trees[tree_of(p, a)], bp->proxy[a]);
    const de_aabb bb = de__tree_box(&bp->trees[tree_of(p, b)], bp->proxy[b]);
    return de__aabb_overlap(&ba, &bb);
}

/* Pair pass query callback: body `qbody` (moved) found body `j`. */
static bool pair_query_cb(void *user, u32 leaf, u32 j) {
    de__broadphase *bp = (de__broadphase *)user;
    const u32 i = bp->qbody;
    (void)leaf;
    if (j == i)
        return true;
    /* Both moved: j's own query records the pair when j < i (each query that can find the
     * other does: a dynamic body searches both trees, the others the dynamic tree). */
    if (bp->moved[j] && j < i)
        return true;
    if (!de__bp_should_collide(bp->qp, i, j))
        return true;
    if (bp->fresh_count >= bp->max_pairs) {
        bp->fresh_overflow = true;
        return false;
    }
    de_sort_key64 *k = &bp->fresh[bp->fresh_count++];
    k->key = pair_key(i, j);
    k->index = 0;
    return true;
}

static void log_overflow(de__broadphase *bp, u32 needed) {
    ++bp->overflows;
    if (bp->overflow_logged)
        return;
    bp->overflow_logged = true;
    DE_WARN(de__physics_log(),
            "broadphase: more than %u candidate pairs (at least %u); pairs are missing until the "
            "count drops (raise cvar " DE__BP_MAX_PAIRS_CVAR ")",
            bp->max_pairs, needed);
}

de_result de__broadphase_update(de_physics_world *p) {
    de__broadphase *bp = NULL;
    const de_result er = de__broadphase_ensure(p, &bp);
    if (er != DE_OK)
        return er;
    const de__body_pool *b = &p->bodies;

    /* 1. Proxies follow their bodies. */
    sync(p, bp);

    /* 2. Drop candidates that no longer apply (a body died, its index was reused by a body that
     *    may not collide, or the fat boxes separated). Order is kept. */
    u64 *cand = bp->cand[bp->cur];
    u32 n = 0;
    for (u32 k = 0; k < bp->cand_count; ++k) {
        const u64 key = cand[k];
        const u32 a = key_a(key), c = key_b(key);
        if (!b->alive[a] || !b->alive[c] || bp->proxy[a] == DE__TREE_NULL ||
            bp->proxy[c] == DE__TREE_NULL || !de__bp_should_collide(p, a, c) ||
            !fat_overlap(p, bp, a, c))
            continue;
        cand[n++] = key;
    }
    bp->cand_count = n;

    /* 3. Query the moved proxies for new candidates, in ascending body index. */
    bool overflow = false;
    if (bp->requery_all) {
        bp->requery_all = false;
        for (u32 i = 0; i < b->high_water; ++i)
            if (b->alive[i] && bp->proxy[i] != DE__TREE_NULL && !bp->moved[i]) {
                bp->moved[i] = 1;
                ++bp->moved_count;
            }
    }
    bp->fresh_count = 0;
    bp->fresh_overflow = false;
    bp->queried = 0;
    bp->qp = p;
    if (bp->moved_count > 0) {
        for (u32 i = 0; i < b->high_water && !bp->fresh_overflow; ++i) {
            if (!bp->moved[i])
                continue;
            ++bp->queried;
            const u32 leaf = bp->proxy[i];
            const u32 ti = tree_of(p, i);
            const de_aabb fat = de__tree_box(&bp->trees[ti], leaf);
            bp->qbody = i;
            if (b->type[i] == (u8)DE_BODY_DYNAMIC &&
                !de__tree_query(&bp->trees[DE__BP_TREE_STATIC], fat, pair_query_cb, bp))
                break;
            if (!de__tree_query(&bp->trees[DE__BP_TREE_DYNAMIC], fat, pair_query_cb, bp))
                break;
        }
        memset(bp->moved, 0, b->high_water);
        bp->moved_count = 0;
    }
    if (bp->fresh_overflow)
        overflow = true;

    /* 4. Sort the new keys, then merge them into the candidates without duplicates. */
    if (bp->fresh_count > 0) {
        const de_result sr = de_radix_sort_u64(bp->fresh, bp->fresh_count, &bp->sort_alloc);
        DE_ASSERT(sr == DE_OK); /* the scratch buffer holds max_pairs keys */
        (void)sr;
        u64 *out = bp->cand[bp->cur ^ 1u];
        const de_sort_key64 *fr = bp->fresh;
        u32 i = 0, j = 0, m = 0;
        const u32 nc = bp->cand_count, nf = bp->fresh_count;
        while (i < nc || j < nf) {
            u64 key;
            if (j >= nf || (i < nc && cand[i] < fr[j].key)) {
                key = cand[i++];
            } else if (i >= nc || fr[j].key < cand[i]) {
                key = fr[j++].key;
            } else {
                key = cand[i++];
                ++j;
            }
            while (j < nf && fr[j].key == key) /* duplicates in the new keys */
                ++j;
            if (m >= bp->max_pairs) {
                overflow = true;
                break;
            }
            out[m++] = key;
        }
        bp->cur ^= 1u;
        bp->cand_count = m;
        cand = out;
    }

    /* 5. Output: the candidates whose world AABBs overlap (or are within pair_distance), already
     *    sorted. */
    u32 np = 0;
    for (u32 k = 0; k < bp->cand_count; ++k) {
        const u32 a = key_a(cand[k]), c = key_b(cand[k]);
        if (!boxes_within(&b->world_aabb[a], &b->world_aabb[c], bp->pair_distance))
            continue;
        bp->pairs[np].a = a;
        bp->pairs[np].b = c;
        ++np;
    }
    bp->pair_count = np;

#if defined(DE_DEBUG)
    DE_ASSERT(de__broadphase_validate(p));
#endif

    if (overflow) {
        bp->requery_all = true;
        log_overflow(bp, bp->cand_count + bp->fresh_count);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__broadphase_update: more than %u pairs",
                       bp->max_pairs);
    }
    bp->overflow_logged = false;
    return DE_OK;
}

void de__broadphase_set_pair_distance(de_physics_world *p, f32 distance) {
    de__broadphase *bp = p != NULL ? de__broadphase_get(p) : NULL;
    if (bp == NULL)
        return;
    if (!de__f32_finite(distance) || !(distance > 0.0f))
        distance = 0.0f;
    bp->pair_distance = de__minf(distance, bp->margin);
}

void de__broadphase_pairs(const de_physics_world *p, const de__bp_pair **pairs, u32 *count) {
    const de__broadphase *bp = p != NULL ? de__broadphase_get(p) : NULL;
    if (pairs != NULL)
        *pairs = bp != NULL ? bp->pairs : NULL;
    if (count != NULL)
        *count = bp != NULL ? bp->pair_count : 0u;
}

void de__broadphase_stats(const de_physics_world *p, de__bp_stats *out) {
    memset(out, 0, sizeof *out);
    const de__broadphase *bp = p != NULL ? de__broadphase_get(p) : NULL;
    if (bp == NULL)
        return;
    for (u32 t = 0; t < DE__BP_TREE_COUNT; ++t) {
        out->proxies[t] = bp->trees[t].leaf_count;
        out->nodes[t] = bp->trees[t].node_count;
        out->height[t] = de__tree_height(&bp->trees[t]);
    }
    out->moved = bp->moved_count;
    out->queried = bp->queried;
    out->candidates = bp->cand_count;
    out->pairs = bp->pair_count;
    out->max_pairs = bp->max_pairs;
    out->overflows = bp->overflows;
    out->margin = bp->margin;
}

/* ---- Validation --------------------------------------------------------------------------- */

#define BP_CHECK(cond, ...)                                                                        \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            DE_WARN(de__physics_log(), "de__broadphase_validate: " __VA_ARGS__);                   \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

bool de__broadphase_validate(const de_physics_world *p) {
    const de__broadphase *bp = p != NULL ? de__broadphase_get(p) : NULL;
    if (bp == NULL)
        return true;
    for (u32 t = 0; t < DE__BP_TREE_COUNT; ++t)
        if (!de__tree_validate(&bp->trees[t]))
            return false;
    const de__body_pool *b = &p->bodies;
    u32 proxies[DE__BP_TREE_COUNT] = {0, 0};
    for (u32 i = 0; i < bp->capacity; ++i) {
        const u32 leaf = bp->proxy[i];
        const bool want = i < b->high_water && b->alive[i] && box_valid(&b->world_aabb[i]);
        if (!want) {
            BP_CHECK(leaf == DE__TREE_NULL, "body %u has a proxy but no live shape", i);
            continue;
        }
        BP_CHECK(leaf != DE__TREE_NULL, "body %u has no proxy", i);
        const u32 ti = tree_of(p, i);
        const de__tree *t = &bp->trees[ti];
        BP_CHECK(leaf < bp->pool.high_water, "body %u leaf %u out of range", i, leaf);
        const de__tree_node *x = &bp->pool.nodes[leaf];
        BP_CHECK(x->height == 0 && x->value == i, "body %u leaf %u is not its leaf", i, leaf);
        u32 r = leaf;
        while (bp->pool.nodes[r].parent != DE__TREE_NULL)
            r = bp->pool.nodes[r].parent;
        BP_CHECK(r == t->root, "body %u is in the wrong tree", i);
        BP_CHECK(de__aabb_contains(&x->box, &b->world_aabb[i]),
                 "body %u fat box does not contain its world box (sync missing)", i);
        ++proxies[ti];
    }
    for (u32 t = 0; t < DE__BP_TREE_COUNT; ++t)
        BP_CHECK(proxies[t] == bp->trees[t].leaf_count, "tree %u has %u leaves, %u proxies", t,
                 bp->trees[t].leaf_count, proxies[t]);
    BP_CHECK(bp->pool.used == bp->trees[0].node_count + bp->trees[1].node_count,
             "pool uses %u nodes, trees hold %u", bp->pool.used,
             bp->trees[0].node_count + bp->trees[1].node_count);
    return true;
}

/* ---- Queries ------------------------------------------------------------------------------ */

const de__tree *de__broadphase_tree(const de_physics_world *p, u32 tree) {
    const de__broadphase *bp = p != NULL ? de__broadphase_get(p) : NULL;
    return bp != NULL && tree < DE__BP_TREE_COUNT ? &bp->trees[tree] : NULL;
}

u32 de__broadphase_proxy(const de_physics_world *p, u32 index) {
    const de__broadphase *bp = p != NULL ? de__broadphase_get(p) : NULL;
    return bp != NULL && index < bp->capacity ? bp->proxy[index] : DE__TREE_NULL;
}

typedef struct query_ctx {
    const de_physics_world *p;
    const de_collision_filter *filter;
    de__bp_query_fn fn;
    de__bp_ray_fn ray_fn;
    void *user;
    bool stopped; /**< the ray callback returned <= 0 */
} query_ctx;

static bool query_cb(void *user, u32 leaf, u32 body) {
    const query_ctx *c = (const query_ctx *)user;
    (void)leaf;
    if (c->filter != NULL && !de__collision_filter_test(*c->filter, c->p->bodies.filter[body]))
        return true;
    return c->fn(c->user, body);
}

bool de__broadphase_query_aabb(const de_physics_world *p, de_aabb box,
                               const de_collision_filter *filter, de__bp_query_fn fn, void *user) {
    const de__broadphase *bp = p != NULL ? de__broadphase_get(p) : NULL;
    if (bp == NULL || fn == NULL)
        return true;
    query_ctx c = {p, filter, fn, NULL, user, false};
    for (u32 t = 0; t < DE__BP_TREE_COUNT; ++t)
        if (!de__tree_query(&bp->trees[t], box, query_cb, &c))
            return false;
    return true;
}

static f32 ray_cb(void *user, const de__tree_ray *ray, u32 leaf, u32 body, f32 max_fraction) {
    query_ctx *c = (query_ctx *)user;
    (void)leaf;
    if (c->filter != NULL && !de__collision_filter_test(*c->filter, c->p->bodies.filter[body]))
        return max_fraction;
    const f32 r = c->ray_fn(c->user, ray, body, max_fraction);
    if (!(r > 0.0f))
        c->stopped = true;
    return r;
}

f32 de__broadphase_raycast(const de_physics_world *p, const de__tree_ray *ray,
                           const de_collision_filter *filter, de__bp_ray_fn fn, void *user) {
    const de__broadphase *bp = p != NULL ? de__broadphase_get(p) : NULL;
    if (bp == NULL || fn == NULL || ray == NULL)
        return ray != NULL ? ray->max_fraction : 0.0f;
    query_ctx c = {p, filter, NULL, fn, user, false};
    de__tree_ray r = *ray;
    for (u32 t = 0; t < DE__BP_TREE_COUNT; ++t) {
        r.max_fraction = de__tree_raycast(&bp->trees[t], &r, ray_cb, &c);
        if (c.stopped)
            return 0.0f;
    }
    return r.max_fraction;
}
