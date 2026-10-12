/*
 * midphase.c: composite shapes against convex shapes (DENG-134).
 *
 * Mesh: the BVH is queried with the convex shape's box in the mesh frame (grown by the
 * speculative distance); candidate triangles are visited in ascending index (a fixed buffer of
 * the next lowest indices, refilled by another traversal when it overflows). Heightfield: the
 * box selects a cell range, 8 x 8 blocks with no height in the box's z range are skipped, cells
 * and their two triangles are visited in sub-shape id order. Compound: the child BVH, children in
 * ascending index.
 *
 * Triangles are one-sided: a convex shape whose center is behind the triangle's plane gets no
 * contact from it. Triangle edges carry active flags (mesh: built by DENG-132; heightfield:
 * computed here from the neighbouring cells with the same rule): inactive (flat or concave)
 * edges never give edge or vertex normals, which removes ghost contacts on internal edges.
 */
#include "physics/narrow/narrowphase.h"

/* Candidates per traversal pass. */
#define NP_CHUNK 256u

static bool overlap(const de_aabb *a, const de_aabb *b) {
    return a->min.x <= b->max.x && a->max.x >= b->min.x && a->min.y <= b->max.y &&
           a->max.y >= b->min.y && a->min.z <= b->max.z && a->max.z >= b->min.z;
}

/* Inserts idx into the ascending list buf[0..*n) of at most NP_CHUNK entries; sets *more when an
 * index had to be left out. */
static void insert_sorted(u32 *buf, u32 *n, u32 idx, bool *more) {
    if (*n == NP_CHUNK) {
        *more = true;
        if (idx >= buf[NP_CHUNK - 1u])
            return;
        --*n; /* drop the largest */
    }
    u32 j = *n;
    while (j > 0 && buf[j - 1] > idx) {
        buf[j] = buf[j - 1];
        --j;
    }
    buf[j] = idx;
    ++*n;
}

typedef bool (*prim_filter)(const void *user, u32 prim, const de_aabb *box);

/* Calls fn for every primitive of `bvh` whose node boxes overlap `box` and that passes `keep`,
 * in ascending primitive index. */
static void bvh_query_sorted(const de__bvh *bvh, de_aabb box, prim_filter keep, const void *kuser,
                             void (*fn)(void *user, u32 prim), void *user) {
    if (bvh->node_count == 0)
        return;
    u32 lo = 0;
    for (;;) {
        u32 buf[NP_CHUNK];
        u32 n = 0;
        bool more = false;
        u32 stack[2u * DE__BVH_MAX_DEPTH + 2u];
        u32 sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const de__bvh_node *node = &bvh->nodes[stack[--sp]];
            if (!overlap(&node->bounds, &box))
                continue;
            if (node->count > 0) {
                for (u32 k = 0; k < node->count; ++k) {
                    const u32 prim = bvh->prims[node->first + k];
                    if (prim >= lo && (keep == NULL || keep(kuser, prim, &box)))
                        insert_sorted(buf, &n, prim, &more);
                }
            } else if (sp + 2u <= 2u * DE__BVH_MAX_DEPTH + 2u) {
                stack[sp++] = node->first + 1u; /* right, popped after the left */
                stack[sp++] = node->first;
            }
        }
        for (u32 i = 0; i < n; ++i)
            fn(user, buf[i]);
        if (!more || n == 0)
            break;
        lo = buf[n - 1] + 1u;
    }
}

/* ---- Compound -------------------------------------------------------------------------- */

static bool child_keep(const void *user, u32 prim, const de_aabb *box) {
    const de__compound *c = (const de__compound *)user;
    return overlap(&c->children[prim].bounds, box);
}

void de__np_compound_children(const de__shape *c, de_aabb box, void (*fn)(void *user, u32 child),
                              void *user) {
    bvh_query_sorted(&c->u.compound.bvh, box, child_keep, &c->u.compound, fn, user);
}

/* ---- Triangles ------------------------------------------------------------------------- */

typedef struct tri_ctx {
    de__np_ctx *ctx;
    const de__np_side *a, *b;
    const de__np_rel *rel;
    bool swapped;
    de_aabb box;    /* B in A's frame, grown */
    de_vec3 center; /* B's reference point in A's frame */
} tri_ctx;

static void collide_tri(const tri_ctx *t, de_vec3 v0, de_vec3 v1, de_vec3 v2, u8 active, u32 sub) {
    de_aabb tb;
    tb.min = de__v3_min(v0, de__v3_min(v1, v2));
    tb.max = de__v3_max(v0, de__v3_max(v1, v2));
    if (!overlap(&tb, &t->box))
        return;
    de__np_tri tri;
    if (!de__np_tri_init(&tri, v0, v1, v2, active))
        return;
    if (de__v3_dot(tri.n, t->center) + tri.p[0].d < 0.0f)
        return; /* behind the triangle: one-sided */
    de__np_manifold m;
    m.count = 0;
    if (de__np_triangle_convex(&tri, t->b->shape, t->rel, &m))
        de__np_emit(t->ctx, &m, t->a, t->b, t->rel, sub, t->swapped);
}

static bool mesh_keep(const void *user, u32 prim, const de_aabb *box) {
    const de__mesh *m = (const de__mesh *)user;
    const u32 *ix = &m->indices[3u * prim];
    const de_vec3 v0 = m->vertices[ix[0]], v1 = m->vertices[ix[1]], v2 = m->vertices[ix[2]];
    de_aabb tb;
    tb.min = de__v3_min(v0, de__v3_min(v1, v2));
    tb.max = de__v3_max(v0, de__v3_max(v1, v2));
    return overlap(&tb, box);
}

static void mesh_tri_cb(void *user, u32 t) {
    const tri_ctx *c = (const tri_ctx *)user;
    const de__mesh *m = &c->a->shape->u.mesh;
    const u8 flags = m->edge_flags[t];
    if (flags & DE__MESH_TRI_DEGENERATE)
        return;
    const u32 *ix = &m->indices[3u * t];
    collide_tri(c, m->vertices[ix[0]], m->vertices[ix[1]], m->vertices[ix[2]], (u8)(flags & 7u), t);
}

/* ---- Heightfield ----------------------------------------------------------------------- */

#define NP_ACTIVE_EDGE_COS 0.99619469809174553 /* cos(5 deg): same rule as the mesh */

static de_dvec3 dvert(const de__heightfield *hf, u32 c, u32 r) {
    return de__d3_from(de__hf_vertex(hf, c, r));
}

static de_dvec3 unit_normal(de_dvec3 a, de_dvec3 b, de_dvec3 c) {
    const de_dvec3 n = de__d3_cross(de__d3_sub(b, a), de__d3_sub(c, a));
    const f64 l2 = de__d3_dot(n, n);
    return l2 > 0.0 ? de__d3_scale(n, 1.0 / de_dsqrt64(l2)) : n;
}

/* Is the edge e0 -> e1 of a triangle with unit normal n active against the neighbour triangle
 * (e1, e0, o)? Active when it bends convexly by more than 5 degrees. */
static bool edge_active(de_dvec3 n, de_dvec3 e0, de_dvec3 e1, de_dvec3 o) {
    const de_dvec3 n2 = unit_normal(e1, e0, o);
    const f64 side = de__d3_dot(n, de__d3_sub(o, e0));
    return side < 0.0 && de__d3_dot(n, n2) < NP_ACTIVE_EDGE_COS;
}

u8 de__np_hf_edge_flags(const de__heightfield *hf, u32 col, u32 row, u32 k) {
    const u32 cells_x = hf->cols - 1u, cells_y = hf->rows - 1u;
    const de_dvec3 c0 = dvert(hf, col, row), c1 = dvert(hf, col + 1u, row);
    const de_dvec3 c2 = dvert(hf, col, row + 1u), c3 = dvert(hf, col + 1u, row + 1u);
    u8 flags = 0;
    if (k == 0) {
        /* (c0, c1, c3): edge 0 bottom, edge 1 right, edge 2 diagonal. */
        const de_dvec3 n = unit_normal(c0, c1, c3);
        if (row == 0 || de__hf_cell_is_hole(hf, col, row - 1u) ||
            edge_active(n, c0, c1, dvert(hf, col, row - 1u)))
            flags |= DE__MESH_EDGE0_ACTIVE;
        if (col + 1u >= cells_x || de__hf_cell_is_hole(hf, col + 1u, row) ||
            edge_active(n, c1, c3, dvert(hf, col + 2u, row + 1u)))
            flags |= DE__MESH_EDGE1_ACTIVE;
        if (edge_active(n, c3, c0, c2))
            flags |= DE__MESH_EDGE2_ACTIVE;
    } else {
        /* (c0, c3, c2): edge 0 diagonal, edge 1 top, edge 2 left. */
        const de_dvec3 n = unit_normal(c0, c3, c2);
        if (edge_active(n, c0, c3, c1))
            flags |= DE__MESH_EDGE0_ACTIVE;
        if (row + 1u >= cells_y || de__hf_cell_is_hole(hf, col, row + 1u) ||
            edge_active(n, c3, c2, dvert(hf, col + 1u, row + 2u)))
            flags |= DE__MESH_EDGE1_ACTIVE;
        if (col == 0 || de__hf_cell_is_hole(hf, col - 1u, row) ||
            edge_active(n, c2, c0, dvert(hf, col - 1u, row)))
            flags |= DE__MESH_EDGE2_ACTIVE;
    }
    return flags;
}

/* Cell range [*c0, *c1] of coordinates [lo, hi] / step over `cells` cells; false when empty. */
static bool cell_range(f32 lo, f32 hi, f32 step, u32 cells, u32 *c0, u32 *c1) {
    const f32 a = lo / step, b = hi / step;
    if (!(b >= 0.0f) || !(a <= (f32)cells))
        return false;
    const f32 last = (f32)(cells - 1u);
    *c0 = a <= 0.0f ? 0u : (a >= last ? cells - 1u : (u32)a);
    *c1 = b >= last ? cells - 1u : (u32)b;
    return *c0 <= *c1;
}

static void heightfield(const tri_ctx *t) {
    const de__heightfield *hf = &t->a->shape->u.hf;
    const u32 cells_x = hf->cols - 1u, cells_y = hf->rows - 1u;
    u32 cx0, cx1, cy0, cy1;
    if (!cell_range(t->box.min.x, t->box.max.x, hf->scale.x, cells_x, &cx0, &cx1) ||
        !cell_range(t->box.min.y, t->box.max.y, hf->scale.y, cells_y, &cy0, &cy1))
        return;
    const u32 B = DE_HEIGHTFIELD_BLOCK;
    for (u32 r = cy0; r <= cy1; ++r) {
        for (u32 c = cx0; c <= cx1; ++c) {
            const u32 blk = (r / B) * hf->blocks_x + c / B;
            if (hf->block_min[blk] > t->box.max.z || hf->block_max[blk] < t->box.min.z)
                continue;
            if (de__hf_cell_is_hole(hf, c, r))
                continue;
            de_vec3 tri[2][3];
            de__hf_cell_triangles(hf, c, r, tri);
            for (u32 k = 0; k < 2; ++k)
                collide_tri(t, tri[k][0], tri[k][1], tri[k][2], de__np_hf_edge_flags(hf, c, r, k),
                            2u * (r * cells_x + c) + k);
        }
    }
}

void de__np_midphase(de__np_ctx *ctx, const de__np_side *a, const de__np_side *b,
                     const de__np_rel *rel, bool swapped) {
    tri_ctx t;
    t.ctx = ctx;
    t.a = a;
    t.b = b;
    t.rel = rel;
    t.swapped = swapped;
    t.box = de__np_box_in_a(rel, b->shape->local_aabb, ctx->spec);
    const de_aabb lb = b->shape->local_aabb;
    de_vec3 c = de__v3((lb.min.x + lb.max.x) * 0.5f, (lb.min.y + lb.max.y) * 0.5f,
                       (lb.min.z + lb.max.z) * 0.5f);
    if (b->shape->kind == DE_SHAPE_CONVEX)
        c = b->shape->u.hull.centroid;
    t.center = de__np_to_a(rel, c);
    if (a->shape->kind == DE_SHAPE_MESH)
        bvh_query_sorted(&a->shape->u.mesh.bvh, t.box, mesh_keep, &a->shape->u.mesh, mesh_tri_cb,
                         &t);
    else if (a->shape->kind == DE_SHAPE_HEIGHTFIELD)
        heightfield(&t);
}
