/* aabb_tree2d.c: the 2D broadphase's dynamic AABB tree (DENG-253), the 2D port of
 * physics/broadphase/aabb_tree.c (DENG-133). See aabb_tree2d.h. */
#include "physics/2d/aabb_tree2d.h"

/* ---- Box helpers ------------------------------------------------------------------------ */

/* Half the perimeter: the SAH cost measure in 2D (a common factor of 2 changes no decision). */
static f32 box_area(const de_aabb2d *b) {
    return (b->max.x - b->min.x) + (b->max.y - b->min.y);
}

static de_vec2 box_center2(const de_aabb2d *b) {
    return de__v2_add(b->min, b->max); /* twice the center; only compared */
}

static bool box_equal(const de_aabb2d *a, const de_aabb2d *b) {
    return memcmp(a, b, sizeof *a) == 0;
}

static de_aabb2d box_union(const de_aabb2d *a, const de_aabb2d *b) {
    return de__aabb2d_union(a, b);
}

static u32 max_u32(u32 a, u32 b) {
    return a > b ? a : b;
}

/* ---- Pool -------------------------------------------------------------------------------- */

de_result de__tree2d_pool_init(de__tree2d_pool *pool, const de_allocator *a, u32 capacity) {
    memset(pool, 0, sizeof *pool);
    pool->free_head = DE__TREE2D_NULL;
    if (capacity == 0)
        return DE_OK;
    pool->nodes = (de__tree2d_node *)DE_ALLOC(a, sizeof(de__tree2d_node) * (size_t)capacity,
                                              DE_ALIGNOF(de__tree2d_node), DE_MEM_PHYSICS);
    if (pool->nodes == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "broadphase2d: tree pool of %u nodes", capacity);
    pool->capacity = capacity;
    return DE_OK;
}

void de__tree2d_pool_free(de__tree2d_pool *pool, const de_allocator *a) {
    if (pool->nodes != NULL)
        DE_FREE(a, pool->nodes, sizeof(de__tree2d_node) * (size_t)pool->capacity);
    memset(pool, 0, sizeof *pool);
    pool->free_head = DE__TREE2D_NULL;
}

static u32 node_alloc(de__tree2d_pool *pool) {
    u32 n;
    if (pool->free_head != DE__TREE2D_NULL) {
        n = pool->free_head;
        pool->free_head = pool->nodes[n].parent;
    } else {
        DE_ASSERT(pool->high_water < pool->capacity);
        n = pool->high_water++;
    }
    ++pool->used;
    de__tree2d_node *x = &pool->nodes[n];
    x->parent = DE__TREE2D_NULL;
    x->child1 = DE__TREE2D_NULL;
    x->child2 = DE__TREE2D_NULL;
    x->value = DE__TREE2D_NULL;
    x->height = 0;
    return n;
}

static void node_free(de__tree2d_pool *pool, u32 n) {
    de__tree2d_node *x = &pool->nodes[n];
    x->parent = pool->free_head;
    x->child1 = DE__TREE2D_NULL;
    x->child2 = DE__TREE2D_NULL;
    x->value = DE__TREE2D_NULL;
    x->height = DE__TREE2D_NULL;
    pool->free_head = n;
    --pool->used;
}

void de__tree2d_init(de__tree2d *t, de__tree2d_pool *pool) {
    t->pool = pool;
    t->root = DE__TREE2D_NULL;
    t->leaf_count = 0;
    t->node_count = 0;
}

/* ---- Insertion ---------------------------------------------------------------------------- */

/*
 * The sibling with the lowest SAH cost for a new leaf with box d (Box2D v3 b2FindBestSibling,
 * with the half perimeter). Cost of making node x the sibling: area(x ∪ d) (the new parent)
 * plus the growth of every ancestor of x ("inherited" cost). The descent follows one path and
 * stops when neither child can beat the best cost found so far (lower bound).
 */
static u32 find_best_sibling(const de__tree2d *t, const de_aabb2d *d) {
    const de__tree2d_node *nodes = t->pool->nodes;
    const de_vec2 center_d = box_center2(d);
    const f32 area_d = box_area(d);

    u32 index = t->root;
    de_aabb2d root_box = nodes[index].box;
    f32 area_base = box_area(&root_box);
    de_aabb2d u = box_union(&root_box, d);
    f32 direct_cost = box_area(&u);
    f32 inherited = 0.0f;
    u32 best = index;
    f32 best_cost = direct_cost;

    while (nodes[index].height > 0) {
        const u32 c1 = nodes[index].child1;
        const u32 c2 = nodes[index].child2;

        const f32 cost = direct_cost + inherited;
        if (cost < best_cost) {
            best = index;
            best_cost = cost;
        }
        inherited += direct_cost - area_base;

        const bool leaf1 = nodes[c1].height == 0;
        const bool leaf2 = nodes[c2].height == 0;

        f32 lower1 = FLT_MAX, area1 = 0.0f;
        const de_aabb2d u1 = box_union(&nodes[c1].box, d);
        const f32 direct1 = box_area(&u1);
        if (leaf1) {
            const f32 cost1 = direct1 + inherited;
            if (cost1 < best_cost) {
                best = c1;
                best_cost = cost1;
            }
        } else {
            area1 = box_area(&nodes[c1].box);
            lower1 = inherited + direct1 + de__minf(area_d - area1, 0.0f);
        }

        f32 lower2 = FLT_MAX, area2 = 0.0f;
        const de_aabb2d u2 = box_union(&nodes[c2].box, d);
        const f32 direct2 = box_area(&u2);
        if (leaf2) {
            const f32 cost2 = direct2 + inherited;
            if (cost2 < best_cost) {
                best = c2;
                best_cost = cost2;
            }
        } else {
            area2 = box_area(&nodes[c2].box);
            lower2 = inherited + direct2 + de__minf(area_d - area2, 0.0f);
        }

        if (leaf1 && leaf2)
            break;
        if (best_cost <= lower1 && best_cost <= lower2)
            break;
        if (lower1 == lower2 && !leaf1) {
            /* No choice by area (both children contain d): take the closer center. */
            const de_vec2 e1 = de__v2_sub(box_center2(&nodes[c1].box), center_d);
            const de_vec2 e2 = de__v2_sub(box_center2(&nodes[c2].box), center_d);
            lower1 = de__v2_dot(e1, e1);
            lower2 = de__v2_dot(e2, e2);
        }
        if (lower1 < lower2 && !leaf1) {
            index = c1;
            area_base = area1;
            direct_cost = direct1;
        } else {
            index = c2;
            area_base = area2;
            direct_cost = direct2;
        }
    }
    return best;
}

/* Node a's height and box from its children. */
static void refit(de__tree2d_node *nodes, u32 a) {
    de__tree2d_node *n = &nodes[a];
    n->box = box_union(&nodes[n->child1].box, &nodes[n->child2].box);
    n->height = 1u + max_u32(nodes[n->child1].height, nodes[n->child2].height);
}

/*
 * Box2D v3 b2RotateNodes: at node A with children B and C, try swapping a child with a
 * grandchild on the other side, and keep the swap that lowers the summed area of A's children
 * the most (A's own box does not change). Ties keep the earlier candidate (none, BF, BG, CD, CE).
 */
static void rotate(de__tree2d_node *nodes, u32 ia) {
    de__tree2d_node *A = &nodes[ia];
    if (A->height < 2)
        return;
    const u32 ib = A->child1, ic = A->child2;
    de__tree2d_node *B = &nodes[ib];
    de__tree2d_node *C = &nodes[ic];

    if (B->height == 0) {
        /* B leaf, C internal: swap B with a child of C. */
        const u32 i_f = C->child1, ig = C->child2;
        de__tree2d_node *F = &nodes[i_f];
        de__tree2d_node *G = &nodes[ig];
        const f32 cost_base = box_area(&C->box);
        const de_aabb2d bg = box_union(&B->box, &G->box);
        const f32 cost_bf = box_area(&bg);
        const de_aabb2d bf = box_union(&B->box, &F->box);
        const f32 cost_bg = box_area(&bf);
        if (cost_base < cost_bf && cost_base < cost_bg)
            return;
        if (cost_bf < cost_bg) {
            A->child1 = i_f;
            C->child1 = ib;
            B->parent = ic;
            F->parent = ia;
            C->box = bg;
            C->height = 1u + max_u32(B->height, G->height);
            A->height = 1u + max_u32(C->height, F->height);
        } else {
            A->child1 = ig;
            C->child2 = ib;
            B->parent = ic;
            G->parent = ia;
            C->box = bf;
            C->height = 1u + max_u32(B->height, F->height);
            A->height = 1u + max_u32(C->height, G->height);
        }
    } else if (C->height == 0) {
        /* C leaf, B internal: swap C with a child of B. */
        const u32 id = B->child1, ie = B->child2;
        de__tree2d_node *D = &nodes[id];
        de__tree2d_node *E = &nodes[ie];
        const f32 cost_base = box_area(&B->box);
        const de_aabb2d ce = box_union(&C->box, &E->box);
        const f32 cost_cd = box_area(&ce);
        const de_aabb2d cd = box_union(&C->box, &D->box);
        const f32 cost_ce = box_area(&cd);
        if (cost_base < cost_cd && cost_base < cost_ce)
            return;
        if (cost_cd < cost_ce) {
            A->child2 = id;
            B->child1 = ic;
            C->parent = ib;
            D->parent = ia;
            B->box = ce;
            B->height = 1u + max_u32(C->height, E->height);
            A->height = 1u + max_u32(B->height, D->height);
        } else {
            A->child2 = ie;
            B->child2 = ic;
            C->parent = ib;
            E->parent = ia;
            B->box = cd;
            B->height = 1u + max_u32(C->height, D->height);
            A->height = 1u + max_u32(B->height, E->height);
        }
    } else {
        const u32 id = B->child1, ie = B->child2, i_f = C->child1, ig = C->child2;
        de__tree2d_node *D = &nodes[id];
        de__tree2d_node *E = &nodes[ie];
        de__tree2d_node *F = &nodes[i_f];
        de__tree2d_node *G = &nodes[ig];
        const f32 area_b = box_area(&B->box);
        const f32 area_c = box_area(&C->box);
        enum { ROT_NONE, ROT_BF, ROT_BG, ROT_CD, ROT_CE } best = ROT_NONE;
        f32 best_cost = area_b + area_c;

        const de_aabb2d bg = box_union(&B->box, &G->box);
        const f32 cost_bf = area_b + box_area(&bg);
        if (cost_bf < best_cost) {
            best = ROT_BF;
            best_cost = cost_bf;
        }
        const de_aabb2d bf = box_union(&B->box, &F->box);
        const f32 cost_bg = area_b + box_area(&bf);
        if (cost_bg < best_cost) {
            best = ROT_BG;
            best_cost = cost_bg;
        }
        const de_aabb2d ce = box_union(&C->box, &E->box);
        const f32 cost_cd = area_c + box_area(&ce);
        if (cost_cd < best_cost) {
            best = ROT_CD;
            best_cost = cost_cd;
        }
        const de_aabb2d cd = box_union(&C->box, &D->box);
        const f32 cost_ce = area_c + box_area(&cd);
        if (cost_ce < best_cost) {
            best = ROT_CE;
            best_cost = cost_ce;
        }
        switch (best) {
        case ROT_NONE:
            break;
        case ROT_BF:
            A->child1 = i_f;
            C->child1 = ib;
            B->parent = ic;
            F->parent = ia;
            C->box = bg;
            C->height = 1u + max_u32(B->height, G->height);
            A->height = 1u + max_u32(C->height, F->height);
            break;
        case ROT_BG:
            A->child1 = ig;
            C->child2 = ib;
            B->parent = ic;
            G->parent = ia;
            C->box = bf;
            C->height = 1u + max_u32(B->height, F->height);
            A->height = 1u + max_u32(C->height, G->height);
            break;
        case ROT_CD:
            A->child2 = id;
            B->child1 = ic;
            C->parent = ib;
            D->parent = ia;
            B->box = ce;
            B->height = 1u + max_u32(C->height, E->height);
            A->height = 1u + max_u32(B->height, D->height);
            break;
        case ROT_CE:
            A->child2 = ie;
            B->child2 = ic;
            C->parent = ib;
            E->parent = ia;
            B->box = cd;
            B->height = 1u + max_u32(C->height, D->height);
            A->height = 1u + max_u32(B->height, E->height);
            break;
        }
    }
}

/* Links an allocated leaf node into the tree. */
static void insert_leaf(de__tree2d *t, u32 leaf) {
    de__tree2d_node *nodes = t->pool->nodes;
    if (t->root == DE__TREE2D_NULL) {
        t->root = leaf;
        nodes[leaf].parent = DE__TREE2D_NULL;
        return;
    }
    const de_aabb2d leaf_box = nodes[leaf].box;
    const u32 sibling = find_best_sibling(t, &leaf_box);

    const u32 old_parent = nodes[sibling].parent;
    const u32 new_parent = node_alloc(t->pool);
    de__tree2d_node *np = &nodes[new_parent];
    np->parent = old_parent;
    np->value = DE__TREE2D_NULL;
    np->box = box_union(&leaf_box, &nodes[sibling].box);
    np->height = nodes[sibling].height + 1u;
    np->child1 = sibling;
    np->child2 = leaf;
    nodes[sibling].parent = new_parent;
    nodes[leaf].parent = new_parent;
    if (old_parent != DE__TREE2D_NULL) {
        if (nodes[old_parent].child1 == sibling)
            nodes[old_parent].child1 = new_parent;
        else
            nodes[old_parent].child2 = new_parent;
    } else {
        t->root = new_parent;
    }
    ++t->node_count;

    /* Refit and rotate up to the root. */
    for (u32 i = nodes[leaf].parent; i != DE__TREE2D_NULL; i = nodes[i].parent) {
        refit(nodes, i);
        rotate(nodes, i);
    }
}

/* Unlinks a leaf (its node stays allocated) and frees its parent. */
static void remove_leaf(de__tree2d *t, u32 leaf) {
    de__tree2d_node *nodes = t->pool->nodes;
    if (leaf == t->root) {
        t->root = DE__TREE2D_NULL;
        return;
    }
    const u32 parent = nodes[leaf].parent;
    const u32 grand = nodes[parent].parent;
    const u32 sibling = nodes[parent].child1 == leaf ? nodes[parent].child2 : nodes[parent].child1;
    if (grand != DE__TREE2D_NULL) {
        if (nodes[grand].child1 == parent)
            nodes[grand].child1 = sibling;
        else
            nodes[grand].child2 = sibling;
        nodes[sibling].parent = grand;
        node_free(t->pool, parent);
        for (u32 i = grand; i != DE__TREE2D_NULL; i = nodes[i].parent)
            refit(nodes, i);
    } else {
        t->root = sibling;
        nodes[sibling].parent = DE__TREE2D_NULL;
        node_free(t->pool, parent);
    }
    --t->node_count;
}

u32 de__tree2d_insert(de__tree2d *t, de_aabb2d box, u32 value) {
    de__tree2d_pool *pool = t->pool;
    /* A leaf and (unless the tree is empty) a new parent. */
    if (pool->capacity - pool->used < 2u)
        return DE__TREE2D_NULL;
    const u32 leaf = node_alloc(pool);
    de__tree2d_node *n = &pool->nodes[leaf];
    n->box = box;
    n->value = value;
    n->height = 0;
    ++t->leaf_count;
    ++t->node_count;
    insert_leaf(t, leaf);
    return leaf;
}

void de__tree2d_remove(de__tree2d *t, u32 leaf) {
    DE_ASSERT(leaf < t->pool->high_water && t->pool->nodes[leaf].height == 0);
    remove_leaf(t, leaf);
    node_free(t->pool, leaf);
    --t->leaf_count;
    --t->node_count;
}

void de__tree2d_move(de__tree2d *t, u32 leaf, de_aabb2d box) {
    DE_ASSERT(leaf < t->pool->high_water && t->pool->nodes[leaf].height == 0);
    remove_leaf(t, leaf);
    t->pool->nodes[leaf].box = box;
    insert_leaf(t, leaf);
}

u32 de__tree2d_height(const de__tree2d *t) {
    return t->root == DE__TREE2D_NULL ? 0u : t->pool->nodes[t->root].height;
}

/* ---- Traversal ---------------------------------------------------------------------------- */

/*
 * The node after `n` in a depth-first, child1-first walk that skips n's subtree: climb while n
 * is a child2 (or the root), then step to the sibling. DE__TREE2D_NULL when the walk is over.
 */
static u32 next_skip(const de__tree2d *t, u32 n) {
    const de__tree2d_node *nodes = t->pool->nodes;
    while (n != t->root) {
        const u32 par = nodes[n].parent;
        if (nodes[par].child1 == n)
            return nodes[par].child2;
        n = par;
    }
    return DE__TREE2D_NULL;
}

f64 de__tree2d_area_cost(const de__tree2d *t) {
    f64 sum = 0.0;
    const de__tree2d_node *nodes = t->pool->nodes;
    u32 n = t->root;
    while (n != DE__TREE2D_NULL) {
        if (nodes[n].height > 0) {
            sum += (f64)box_area(&nodes[n].box);
            n = nodes[n].child1;
        } else {
            n = next_skip(t, n);
        }
    }
    return sum;
}

bool de__tree2d_query(const de__tree2d *t, de_aabb2d box, de__tree2d_query_fn fn, void *user) {
    const de__tree2d_node *nodes = t->pool->nodes;
    u32 n = t->root;
    while (n != DE__TREE2D_NULL) {
        const de__tree2d_node *x = &nodes[n];
        if (de__aabb2d_overlap(&x->box, &box)) {
            if (x->height == 0) {
                if (!fn(user, n, x->value))
                    return false;
            } else {
                n = x->child1;
                continue;
            }
        }
        n = next_skip(t, n);
    }
    return true;
}

bool de__ray2d_aabb(const de__tree2d_ray *ray, f32 max_fraction, const de_aabb2d *box,
                    f64 *t_enter) {
    const f64 o[2] = {ray->origin.x, ray->origin.y};
    const f64 d[2] = {(f64)ray->delta.x, (f64)ray->delta.y};
    const f64 lo[2] = {(f64)box->min.x, (f64)box->min.y};
    const f64 hi[2] = {(f64)box->max.x, (f64)box->max.y};
    f64 tmin = 0.0, tmax = (f64)max_fraction;
    if (!(tmax >= 0.0))
        return false;
    for (u32 k = 0; k < 2; ++k) {
        if (d[k] == 0.0) {
            if (o[k] < lo[k] || o[k] > hi[k])
                return false;
            continue;
        }
        f64 t1 = (lo[k] - o[k]) / d[k];
        f64 t2 = (hi[k] - o[k]) / d[k];
        if (t1 > t2) {
            const f64 sw = t1;
            t1 = t2;
            t2 = sw;
        }
        tmin = de__maxd(tmin, t1);
        tmax = de__mind(tmax, t2);
        if (tmin > tmax)
            return false;
    }
    *t_enter = tmin;
    return true;
}

f32 de__tree2d_raycast(const de__tree2d *t, const de__tree2d_ray *ray, de__tree2d_ray_fn fn,
                       void *user) {
    const de__tree2d_node *nodes = t->pool->nodes;
    f32 max_fraction = ray->max_fraction;
    u32 n = t->root;
    while (n != DE__TREE2D_NULL) {
        const de__tree2d_node *x = &nodes[n];
        f64 t_enter;
        if (de__ray2d_aabb(ray, max_fraction, &x->box, &t_enter)) {
            if (x->height == 0) {
                const f32 r = fn(user, ray, n, x->value, max_fraction);
                if (!(r > 0.0f)) /* <= 0 or NaN: stop */
                    return 0.0f;
                if (r < max_fraction)
                    max_fraction = r;
            } else {
                n = x->child1;
                continue;
            }
        }
        n = next_skip(t, n);
    }
    return max_fraction;
}

/* ---- Validation --------------------------------------------------------------------------- */

#define TREE_CHECK(cond, ...)                                                                      \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            DE_WARN(de__physics_log(), "de__tree2d_validate: " __VA_ARGS__);                       \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

bool de__tree2d_validate(const de__tree2d *t) {
    const de__tree2d_pool *pool = t->pool;
    const de__tree2d_node *nodes = pool->nodes;
    if (t->root == DE__TREE2D_NULL) {
        TREE_CHECK(t->leaf_count == 0 && t->node_count == 0, "empty tree with %u leaves %u nodes",
                   t->leaf_count, t->node_count);
        return true;
    }
    TREE_CHECK(t->root < pool->high_water, "root %u out of range", t->root);
    TREE_CHECK(nodes[t->root].parent == DE__TREE2D_NULL, "root %u has a parent", t->root);
    u32 leaves = 0, count = 0;
    u32 n = t->root;
    while (n != DE__TREE2D_NULL) {
        TREE_CHECK(n < pool->high_water, "node %u out of range", n);
        const de__tree2d_node *x = &nodes[n];
        TREE_CHECK(x->height != DE__TREE2D_NULL, "node %u is on the free list", n);
        ++count;
        TREE_CHECK(count <= pool->used, "more nodes than allocated (cycle?)");
        if (x->height == 0) {
            TREE_CHECK(x->child1 == DE__TREE2D_NULL && x->child2 == DE__TREE2D_NULL,
                       "leaf %u has children", n);
            TREE_CHECK(x->box.min.x <= x->box.max.x && x->box.min.y <= x->box.max.y,
                       "leaf %u has an inverted box", n);
            ++leaves;
            n = next_skip(t, n);
            continue;
        }
        const u32 c1 = x->child1, c2 = x->child2;
        TREE_CHECK(c1 < pool->high_water && c2 < pool->high_water && c1 != c2,
                   "node %u has bad children %u %u", n, c1, c2);
        TREE_CHECK(nodes[c1].parent == n && nodes[c2].parent == n,
                   "children of %u do not point back", n);
        TREE_CHECK(x->value == DE__TREE2D_NULL, "internal node %u has a value", n);
        TREE_CHECK(x->height == 1u + max_u32(nodes[c1].height, nodes[c2].height),
                   "node %u height %u is not 1 + max(%u, %u)", n, x->height, nodes[c1].height,
                   nodes[c2].height);
        const de_aabb2d u = box_union(&nodes[c1].box, &nodes[c2].box);
        TREE_CHECK(box_equal(&u, &x->box), "node %u box is not the union of its children", n);
        n = c1;
    }
    TREE_CHECK(leaves == t->leaf_count, "%u leaves reachable, %u recorded", leaves, t->leaf_count);
    TREE_CHECK(count == t->node_count, "%u nodes reachable, %u recorded", count, t->node_count);
    TREE_CHECK(t->node_count == 2u * t->leaf_count - 1u, "%u nodes for %u leaves", t->node_count,
               t->leaf_count);
    return true;
}
