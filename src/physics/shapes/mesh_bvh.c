/*
 * mesh_bvh.c: the deterministic binned-SAH BVH builder and the static triangle mesh shape
 * (DENG-132).
 *
 * BVH: top-down, one node at a time from an explicit stack (left child first). Each split tries
 * 12 bins on each axis of the centroid bounds and takes the lowest SAH cost; ties keep the lower
 * axis, then the lower bin. Partitions are stable (input order is kept on each side). Below depth
 * 32 every split is a median split along the longest centroid axis (stable sort), which bounds
 * the depth by DE__BVH_MAX_DEPTH. The result depends only on the input boxes and their order.
 */
#include "physics/shapes/shape_internal.h"

#include "core/de_sort.h"

#define BVH_BINS         12u
#define BVH_MEDIAN_DEPTH 32u

typedef struct bvh_work {
    u32 node, depth;
} bvh_work;

static f32 centroid_axis(const de_aabb *b, u32 axis) {
    const f32 lo = axis == 0 ? b->min.x : axis == 1 ? b->min.y : b->min.z;
    const f32 hi = axis == 0 ? b->max.x : axis == 1 ? b->max.y : b->max.z;
    return (lo + hi) * 0.5f;
}

static f64 half_area(de_aabb b) {
    if (b.min.x > b.max.x)
        return 0.0;
    const f64 dx = (f64)b.max.x - b.min.x, dy = (f64)b.max.y - b.min.y, dz = (f64)b.max.z - b.min.z;
    return dx * dy + dy * dz + dz * dx;
}

static de_aabb box_empty(void) {
    de_aabb b;
    b.min = de__v3(FLT_MAX, FLT_MAX, FLT_MAX);
    b.max = de__v3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    return b;
}

static de_aabb box_merge(de_aabb a, de_aabb b) {
    de_aabb r;
    r.min = de__v3_min(a.min, b.min);
    r.max = de__v3_max(a.max, b.max);
    return r;
}

typedef struct axis_ctx {
    const de_aabb *bounds;
    u32 axis;
} axis_ctx;

static int cmp_centroid(const void *x, const void *y, void *user) {
    const axis_ctx *c = (const axis_ctx *)user;
    const f32 a = centroid_axis(&c->bounds[*(const u32 *)x], c->axis);
    const f32 b = centroid_axis(&c->bounds[*(const u32 *)y], c->axis);
    return a < b ? -1 : (a > b ? 1 : 0);
}

void de__bvh_free(const de_allocator *a, de__bvh *bvh) {
    if (bvh->nodes != NULL)
        DE_FREE(a, bvh->nodes, sizeof(de__bvh_node) * (2u * (size_t)bvh->prim_count - 1u));
    if (bvh->prims != NULL)
        DE_FREE(a, bvh->prims, sizeof(u32) * (size_t)bvh->prim_count);
    memset(bvh, 0, sizeof *bvh);
}

de_result de__bvh_build(const de_allocator *a, const de_aabb *bounds, const u32 *prims_in,
                        u32 count, u32 max_leaf, de__bvh *out) {
    memset(out, 0, sizeof *out);
    if (count == 0)
        return DE_OK;
    if (max_leaf == 0)
        max_leaf = 1;
    const size_t cap = 2u * (size_t)count - 1u;
    de__bvh_node *nodes = (de__bvh_node *)DE_ALLOC(a, sizeof(de__bvh_node) * cap,
                                                   DE_ALIGNOF(de__bvh_node), DE_MEM_PHYSICS);
    u32 *prims = (u32 *)DE_ALLOC(a, sizeof(u32) * (size_t)count, DE_ALIGNOF(u32), DE_MEM_PHYSICS);
    u32 *tmp = (u32 *)DE_ALLOC(a, sizeof(u32) * (size_t)count, DE_ALIGNOF(u32), DE_MEM_PHYSICS);
    bvh_work *stack =
        (bvh_work *)DE_ALLOC(a, sizeof(bvh_work) * cap, DE_ALIGNOF(bvh_work), DE_MEM_PHYSICS);
    u8 *side = (u8 *)DE_ALLOC(a, (size_t)count, 1, DE_MEM_PHYSICS);
    de_result r = DE_OK;
    if (nodes == NULL || prims == NULL || tmp == NULL || stack == NULL || side == NULL) {
        r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "bvh: %u primitives", count);
        goto fail;
    }
    memcpy(prims, prims_in, sizeof(u32) * (size_t)count);
    out->nodes = nodes;
    out->prims = prims;
    out->prim_count = count;
    out->node_count = 1;
    nodes[0].first = 0;
    nodes[0].count = count;
    u32 sp = 0;
    stack[sp].node = 0;
    stack[sp].depth = 1;
    ++sp;

    while (sp > 0) {
        const bvh_work w = stack[--sp];
        de__bvh_node *node = &nodes[w.node];
        const u32 first = node->first, n = node->count;
        de_aabb nb = box_empty(), cb = box_empty();
        for (u32 i = 0; i < n; ++i) {
            const de_aabb *b = &bounds[prims[first + i]];
            nb = box_merge(nb, *b);
            const de_vec3 c = de__v3(centroid_axis(b, 0), centroid_axis(b, 1), centroid_axis(b, 2));
            cb.min = de__v3_min(cb.min, c);
            cb.max = de__v3_max(cb.max, c);
        }
        node->bounds = nb;
        if (w.depth > out->depth)
            out->depth = w.depth;
        if (n <= max_leaf)
            continue;

        const f32 cmin[3] = {cb.min.x, cb.min.y, cb.min.z};
        const f32 cext[3] = {cb.max.x - cb.min.x, cb.max.y - cb.min.y, cb.max.z - cb.min.z};
        u32 best_axis = 3, best_bin = 0;
        f64 best_cost = 0.0;
        if (w.depth < BVH_MEDIAN_DEPTH) {
            for (u32 ax = 0; ax < 3; ++ax) {
                if (!(cext[ax] > 0.0f))
                    continue;
                const f32 k = (f32)BVH_BINS / cext[ax];
                u32 bin_n[BVH_BINS];
                de_aabb bin_b[BVH_BINS];
                for (u32 bi = 0; bi < BVH_BINS; ++bi) {
                    bin_n[bi] = 0;
                    bin_b[bi] = box_empty();
                }
                for (u32 i = 0; i < n; ++i) {
                    const de_aabb *b = &bounds[prims[first + i]];
                    u32 bi = (u32)((centroid_axis(b, ax) - cmin[ax]) * k);
                    if (bi >= BVH_BINS)
                        bi = BVH_BINS - 1u;
                    ++bin_n[bi];
                    bin_b[bi] = box_merge(bin_b[bi], *b);
                }
                f64 right_cost[BVH_BINS];
                de_aabb acc = box_empty();
                u32 acc_n = 0;
                for (u32 bi = BVH_BINS; bi-- > 1;) {
                    acc = box_merge(acc, bin_b[bi]);
                    acc_n += bin_n[bi];
                    right_cost[bi] = acc_n > 0 ? (f64)acc_n * half_area(acc) : -1.0;
                }
                acc = box_empty();
                acc_n = 0;
                for (u32 bi = 0; bi + 1 < BVH_BINS; ++bi) {
                    acc = box_merge(acc, bin_b[bi]);
                    acc_n += bin_n[bi];
                    if (acc_n == 0 || acc_n == n)
                        continue;
                    const f64 cost = (f64)acc_n * half_area(acc) + right_cost[bi + 1];
                    if (best_axis == 3 || cost < best_cost) {
                        best_axis = ax;
                        best_bin = bi;
                        best_cost = cost;
                    }
                }
            }
        }

        u32 nleft;
        if (best_axis < 3) {
            const f32 k = (f32)BVH_BINS / cext[best_axis];
            u32 l = 0, rr = 0;
            for (u32 i = 0; i < n; ++i) {
                const de_aabb *b = &bounds[prims[first + i]];
                u32 bi = (u32)((centroid_axis(b, best_axis) - cmin[best_axis]) * k);
                if (bi >= BVH_BINS)
                    bi = BVH_BINS - 1u;
                side[i] = (u8)(bi <= best_bin ? 0u : 1u);
                if (side[i] == 0)
                    ++l;
            }
            /* Stable partition through tmp. */
            u32 li = 0;
            rr = l;
            for (u32 i = 0; i < n; ++i) {
                if (side[i] == 0)
                    tmp[li++] = prims[first + i];
                else
                    tmp[rr++] = prims[first + i];
            }
            memcpy(prims + first, tmp, sizeof(u32) * n);
            nleft = l;
        } else {
            /* Median split along the longest centroid axis (ties: lower axis). */
            u32 ax = 0;
            if (cext[1] > cext[ax])
                ax = 1;
            if (cext[2] > cext[ax])
                ax = 2;
            if (cext[ax] > 0.0f) {
                axis_ctx ctx = {bounds, ax};
                r = de_sort_stable(prims + first, n, sizeof(u32), cmp_centroid, &ctx, a);
                if (r != DE_OK)
                    goto fail;
            }
            nleft = n / 2u;
        }
        const u32 left = out->node_count;
        out->node_count += 2;
        node = &nodes[w.node];
        node->first = left;
        node->count = 0;
        nodes[left].first = first;
        nodes[left].count = nleft;
        nodes[left + 1].first = first + nleft;
        nodes[left + 1].count = n - nleft;
        /* Right pushed first, so the left subtree is built first. */
        stack[sp].node = left + 1;
        stack[sp].depth = w.depth + 1;
        ++sp;
        stack[sp].node = left;
        stack[sp].depth = w.depth + 1;
        ++sp;
    }
    DE_ASSERT(out->depth <= DE__BVH_MAX_DEPTH);
    DE_FREE(a, tmp, sizeof(u32) * (size_t)count);
    DE_FREE(a, stack, sizeof(bvh_work) * cap);
    DE_FREE(a, side, (size_t)count);
    return DE_OK;

fail:
    if (nodes != NULL)
        DE_FREE(a, nodes, sizeof(de__bvh_node) * cap);
    if (prims != NULL)
        DE_FREE(a, prims, sizeof(u32) * (size_t)count);
    if (tmp != NULL)
        DE_FREE(a, tmp, sizeof(u32) * (size_t)count);
    if (stack != NULL)
        DE_FREE(a, stack, sizeof(bvh_work) * cap);
    if (side != NULL)
        DE_FREE(a, side, (size_t)count);
    memset(out, 0, sizeof *out);
    return r;
}

/* ---- Mesh ------------------------------------------------------------------------------ */

/* An edge is active (may give edge contacts) when it bends convexly by more than 5 degrees. */
#define DE__MESH_ACTIVE_EDGE_COS 0.99619469809174553 /* cos(5 deg) */

static de_dvec3 tri_normal(const de_vec3 *v, const u32 *idx) {
    const de_dvec3 a = de__d3_from(v[idx[0]]);
    return de__d3_cross(de__d3_sub(de__d3_from(v[idx[1]]), a),
                        de__d3_sub(de__d3_from(v[idx[2]]), a));
}

de_result de__mesh_build(de_physics_world *p, const de_shape_desc *d, de__shape *s) {
    if (d->vertices == NULL || d->vertex_count < 3 || d->indices == NULL || d->index_count < 3 ||
        d->index_count % 3 != 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_shape_create: mesh needs >= 3 vertices and a multiple of 3 indices");
    if (d->vertex_count > 0x7FFFFFFFu)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape_create: mesh too large");
    for (u32 i = 0; i < d->vertex_count; ++i)
        if (!de__v3_finite(d->vertices[i]))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape_create: vertex %u not finite", i);
    for (u32 i = 0; i < d->index_count; ++i)
        if (d->indices[i] >= d->vertex_count)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape_create: index %u out of range", i);

    const de_allocator *a = p->alloc;
    const u32 nv = d->vertex_count, nt = d->index_count / 3u;
    const size_t off_idx = ((sizeof(de_vec3) * (size_t)nv) + 15u) & ~(size_t)15u;
    const size_t off_adj = off_idx + ((sizeof(u32) * 3u * (size_t)nt + 15u) & ~(size_t)15u);
    const size_t off_flags = off_adj + ((sizeof(u32) * 3u * (size_t)nt + 15u) & ~(size_t)15u);
    const size_t size = off_flags + (((size_t)nt + 15u) & ~(size_t)15u);
    u8 *block = (u8 *)DE_ALLOC(a, size, 16, DE_MEM_PHYSICS);
    if (block == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_shape_create: mesh of %u triangles", nt);
    memset(block, 0, size);
    s->data = block;
    s->data_size = size;
    de__mesh *m = &s->u.mesh;
    m->vertices = (de_vec3 *)block;
    m->indices = (u32 *)(block + off_idx);
    m->adjacent = (u32 *)(block + off_adj);
    m->edge_flags = block + off_flags;
    m->vertex_count = nv;
    m->tri_count = nt;
    memcpy(m->vertices, d->vertices, sizeof(de_vec3) * nv);
    memcpy(m->indices, d->indices, sizeof(u32) * 3u * nt);

    /* Degenerate triangles: |cross| tiny next to the longest edge squared. */
    u32 good = 0;
    for (u32 t = 0; t < nt; ++t) {
        const u32 *ix = &m->indices[3u * t];
        const de_dvec3 n = tri_normal(m->vertices, ix);
        f64 l2 = 0.0;
        for (u32 e = 0; e < 3; ++e) {
            const de_dvec3 ed = de__d3_sub(de__d3_from(m->vertices[ix[(e + 1) % 3]]),
                                           de__d3_from(m->vertices[ix[e]]));
            l2 = de__maxd(l2, de__d3_dot(ed, ed));
        }
        const bool degenerate = ix[0] == ix[1] || ix[1] == ix[2] || ix[0] == ix[2] ||
                                !(de__d3_dot(n, n) > 1e-14 * l2 * l2);
        m->edge_flags[t] = (u8)(degenerate ? DE__MESH_TRI_DEGENERATE : 0u);
        if (!degenerate)
            ++good;
        m->adjacent[3u * t] = m->adjacent[3u * t + 1] = m->adjacent[3u * t + 2] = DE__MESH_NO_ADJ;
    }
    if (good == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_shape_create: every mesh triangle is degenerate");

    /* Adjacency: sort the undirected edge keys of the good triangles; a key shared by exactly two
     * opposite half-edges joins two triangles. */
    de_sort_key64 *keys = (de_sort_key64 *)DE_ALLOC(a, sizeof(de_sort_key64) * 3u * (size_t)good,
                                                    DE_ALIGNOF(de_sort_key64), DE_MEM_PHYSICS);
    de_aabb *tb = keys != NULL ? (de_aabb *)DE_ALLOC(a, sizeof(de_aabb) * (size_t)nt,
                                                     DE_ALIGNOF(de_aabb), DE_MEM_PHYSICS)
                               : NULL;
    u32 *prims =
        tb != NULL ? (u32 *)DE_ALLOC(a, sizeof(u32) * (size_t)good, DE_ALIGNOF(u32), DE_MEM_PHYSICS)
                   : NULL;
    de_result r = DE_OK;
    if (prims == NULL) {
        r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_shape_create: mesh scratch");
        goto done;
    }
    u32 nk = 0, np = 0;
    for (u32 t = 0; t < nt; ++t) {
        const u32 *ix = &m->indices[3u * t];
        de_aabb b;
        b.min = b.max = m->vertices[ix[0]];
        for (u32 c = 1; c < 3; ++c) {
            b.min = de__v3_min(b.min, m->vertices[ix[c]]);
            b.max = de__v3_max(b.max, m->vertices[ix[c]]);
        }
        tb[t] = b;
        if (m->edge_flags[t] & DE__MESH_TRI_DEGENERATE)
            continue;
        prims[np++] = t;
        for (u32 e = 0; e < 3; ++e) {
            const u32 va = ix[e], vb = ix[(e + 1) % 3];
            const u64 lo = va < vb ? va : vb, hi = va < vb ? vb : va;
            keys[nk].key = (lo << 32) | hi;
            keys[nk].index = 3u * (u64)t + e;
            ++nk;
        }
    }
    r = de_radix_sort_u64(keys, nk, a);
    if (r != DE_OK)
        goto done;
    for (u32 k = 0; k < nk;) {
        u32 run = 1;
        while (k + run < nk && keys[k + run].key == keys[k].key)
            ++run;
        if (run == 2) {
            const u32 h0 = (u32)keys[k].index, h1 = (u32)keys[k + 1].index;
            const u32 t0 = h0 / 3u, e0 = h0 % 3u, t1 = h1 / 3u, e1 = h1 % 3u;
            /* Opposite directions only (consistent winding). */
            if (m->indices[3u * t0 + e0] == m->indices[3u * t1 + (e1 + 1) % 3]) {
                m->adjacent[h0] = t1;
                m->adjacent[h1] = t0;
            }
        }
        k += run;
    }
    /* Edge flags. */
    for (u32 t = 0; t < nt; ++t) {
        if (m->edge_flags[t] & DE__MESH_TRI_DEGENERATE)
            continue;
        const u32 *ix = &m->indices[3u * t];
        de_dvec3 n = tri_normal(m->vertices, ix);
        n = de__d3_scale(n, 1.0 / de_dsqrt64(de__d3_dot(n, n)));
        u8 flags = 0;
        for (u32 e = 0; e < 3; ++e) {
            const u32 nb = m->adjacent[3u * t + e];
            bool active = true;
            if (nb != DE__MESH_NO_ADJ) {
                const u32 *jx = &m->indices[3u * nb];
                u32 w = jx[0];
                for (u32 c = 0; c < 3; ++c)
                    if (jx[c] != ix[e] && jx[c] != ix[(e + 1) % 3])
                        w = jx[c];
                de_dvec3 n2 = tri_normal(m->vertices, jx);
                n2 = de__d3_scale(n2, 1.0 / de_dsqrt64(de__d3_dot(n2, n2)));
                const f64 side = de__d3_dot(
                    n, de__d3_sub(de__d3_from(m->vertices[w]), de__d3_from(m->vertices[ix[e]])));
                active = side < 0.0 && de__d3_dot(n, n2) < DE__MESH_ACTIVE_EDGE_COS;
            }
            if (active)
                flags = (u8)(flags | (1u << e));
        }
        m->edge_flags[t] = flags;
    }

    r = de__bvh_build(a, tb, prims, np, 4, &m->bvh);
    if (r != DE_OK)
        goto done;
    s->local_aabb = m->bvh.nodes[0].bounds;
    s->has_volume = false;
    {
        f64 r2 = 0.0;
        for (u32 c = 0; c < 8; ++c) {
            const f64 x = (c & 1) ? s->local_aabb.max.x : s->local_aabb.min.x;
            const f64 y = (c & 2) ? s->local_aabb.max.y : s->local_aabb.min.y;
            const f64 z = (c & 4) ? s->local_aabb.max.z : s->local_aabb.min.z;
            r2 = de__maxd(r2, x * x + y * y + z * z);
        }
        s->bounding_radius = de__f32_round_up(de_dsqrt64(r2));
    }

done:
    if (keys != NULL)
        DE_FREE(a, keys, sizeof(de_sort_key64) * 3u * (size_t)good);
    if (tb != NULL)
        DE_FREE(a, tb, sizeof(de_aabb) * (size_t)nt);
    if (prims != NULL)
        DE_FREE(a, prims, sizeof(u32) * (size_t)good);
    return r;
}
