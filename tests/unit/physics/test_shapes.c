/* test_shapes.c: shape building, AABBs, support functions, mesh BVH and adjacency, heightfield
 * blocks, compound bounds (DENG-132, SPEC §13.1). */
#include "de_test.h"

#include <dengine/de_physics.h>

#include "physics/physics_internal.h"
#include "physics/shapes/shape_internal.h"

#include <math.h>
#include <string.h>

/* ---- helpers ---------------------------------------------------------------------------- */

typedef struct ps_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_physics_world *p;
} ps_fx;

static void ps_fx_setup(ps_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_PHYSICS);
    de_physics_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = &f->alloc;
    d.max_bodies = 16;
    ASSERT_EQ(de_physics_create(&d, &f->p), DE_OK);
}

static void ps_fx_teardown(ps_fx *f) {
    de_physics_destroy(f->p);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
}

static de_shape_desc desc_of(de_shape_kind k) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = k;
    return d;
}

static const de__shape *create(de_physics_world *p, const de_shape_desc *d) {
    const de_shape h = de_shape_create(p, d);
    ASSERT_TRUE(DE_HANDLE_VALID(h));
    const de__shape *s = de__shape_get(p, h);
    ASSERT_TRUE(s != NULL);
    return s;
}

static f32 rnd(de_rng *r, f32 lo, f32 hi) {
    return lo + (hi - lo) * de_rng_f32(r);
}

static de_quat rnd_quat(de_rng *r) {
    de_quat q = {rnd(r, -1, 1), rnd(r, -1, 1), rnd(r, -1, 1), rnd(r, -1, 1)};
    return de_dnormalize_quat(q);
}

static de_vec3 rnd_dir(de_rng *r) {
    return de_dnormalize_v3((de_vec3){rnd(r, -1, 1), rnd(r, -1, 1), rnd(r, -1, 1) + 1e-3f});
}

/* The convex test shapes: one of each kind, plus a hull. */
static void make_convex_set(de_physics_world *p, const de__shape *out[5]) {
    de_shape_desc d = desc_of(DE_SHAPE_SPHERE);
    d.radius = 0.7f;
    out[0] = create(p, &d);
    d = desc_of(DE_SHAPE_CAPSULE);
    d.radius = 0.3f;
    d.half_height = 0.9f;
    out[1] = create(p, &d);
    d = desc_of(DE_SHAPE_BOX);
    d.half_extents = (de_vec3){0.5f, 1.25f, 2.0f};
    out[2] = create(p, &d);
    d = desc_of(DE_SHAPE_CYLINDER);
    d.radius = 0.8f;
    d.half_height = 0.4f;
    out[3] = create(p, &d);
    de_rng r = de_rng_seed(7, 1);
    de_vec3 pts[40];
    for (u32 i = 0; i < 40; ++i)
        pts[i] = (de_vec3){rnd(&r, -1, 1), rnd(&r, -0.5f, 2), rnd(&r, -1, 0.5f)};
    d = desc_of(DE_SHAPE_CONVEX);
    d.points = pts;
    d.point_count = 40;
    out[4] = create(p, &d);
}

static bool aabb_contains_d(de_aabb b, de_dvec3 q, f64 eps) {
    return q.x >= (f64)b.min.x - eps && q.x <= (f64)b.max.x + eps && q.y >= (f64)b.min.y - eps &&
           q.y <= (f64)b.max.y + eps && q.z >= (f64)b.min.z - eps && q.z <= (f64)b.max.z + eps;
}

/* World support point of s at (pos, rot) in world direction dir. */
static de_dvec3 world_support(const de__shape *s, de_dvec3 pos, de_quat rot, de_vec3 dir) {
    const de__dmat3 m = de__dmat3_from_quat(rot);
    const de_dvec3 ld = de__dmat3_tmul(&m, de__d3_from(dir));
    const de_vec3 sp = de__shape_support(s, de__v3((f32)ld.x, (f32)ld.y, (f32)ld.z));
    return de__d3_add(pos, de__dmat3_mul(&m, de__d3_from(sp)));
}

/* ---- AABBs ------------------------------------------------------------------------------ */

TEST_F(physics_shapes, aabb_rotated_contains_and_tight, ps_fx) {
    const de__shape *set[5];
    make_convex_set(f->p, set);
    de_rng r = de_rng_seed(132, 2);
    const f64 offsets[3] = {0.0, 1234.5, 100000.0};
    for (u32 k = 0; k < 5; ++k) {
        for (u32 trial = 0; trial < 60; ++trial) {
            const f64 off = offsets[trial % 3];
            const de_dvec3 pos = {off + rnd(&r, -5, 5), -off + rnd(&r, -5, 5), rnd(&r, -5, 5)};
            const de_quat q = rnd_quat(&r);
            const de_aabb b = de__shape_world_aabb(set[k], pos, q);
            /* Contains the shape: every sampled support point is inside. */
            for (u32 s = 0; s < 64; ++s) {
                const de_dvec3 w = world_support(set[k], pos, q, rnd_dir(&r));
                EXPECT_TRUE(aabb_contains_d(b, w, 1e-5));
            }
            /* Tight for sphere, capsule, box and cylinder: each face of the box is reached by
             * the shape within a small tolerance (plus the f32 spacing at the offset). */
            if (k <= 3) {
                const f64 slack = 1e-4 + off * 2e-7;
                const de_vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
                for (u32 a = 0; a < 3; ++a) {
                    const de_dvec3 hi = world_support(set[k], pos, q, axes[a]);
                    const de_dvec3 lo =
                        world_support(set[k], pos, q, de__v3(-axes[a].x, -axes[a].y, -axes[a].z));
                    const f64 hv = a == 0 ? hi.x : a == 1 ? hi.y : hi.z;
                    const f64 lv = a == 0 ? lo.x : a == 1 ? lo.y : lo.z;
                    const f32 bmax = a == 0 ? b.max.x : a == 1 ? b.max.y : b.max.z;
                    const f32 bmin = a == 0 ? b.min.x : a == 1 ? b.min.y : b.min.z;
                    EXPECT_TRUE((f64)bmax - hv <= slack);
                    EXPECT_TRUE(lv - (f64)bmin <= slack);
                    EXPECT_TRUE((f64)bmax >= hv - 1e-6);
                }
            }
        }
    }
}

TEST(physics_shapes, outward_rounding) {
    /* Bounds are rounded away from the value; exact floats stay. */
    EXPECT_TRUE(de__f32_round_down(1.0) == 1.0f);
    EXPECT_TRUE(de__f32_round_up(1.0) == 1.0f);
    const f64 x = 100000.123456789;
    EXPECT_TRUE((f64)de__f32_round_down(x) <= x);
    EXPECT_TRUE((f64)de__f32_round_up(x) >= x);
    EXPECT_TRUE(de__f32_round_up(x) > de__f32_round_down(x));
    EXPECT_TRUE((f64)de__f32_round_down(-x) <= -x);
    EXPECT_TRUE((f64)de__f32_round_up(-x) >= -x);
    EXPECT_TRUE(de__f32_round_up(1e-50) == FLT_MIN);
    EXPECT_TRUE(de__f32_round_down(1e-50) == 0.0f);
    EXPECT_TRUE(de__f32_round_down(-1e-50) == -FLT_MIN);
    EXPECT_TRUE(de__f32_next_up(0.0f) == FLT_MIN);
    EXPECT_TRUE(de__f32_next_down(0.0f) == -FLT_MIN);
}

/* ---- support functions ------------------------------------------------------------------ */

TEST_F(physics_shapes, support_vs_bruteforce, ps_fx) {
    const de__shape *set[5];
    make_convex_set(f->p, set);
    de_rng r = de_rng_seed(5, 5);
    for (u32 n = 0; n < 500; ++n) {
        const de_vec3 d = de__v3_scale(rnd_dir(&r), rnd(&r, 0.1f, 10.0f));
        const f64 dl = de_dsqrt64(de__d3_dot(de__d3_from(d), de__d3_from(d)));
        /* Sphere: radius along d. */
        const de_vec3 ss = de__shape_support(set[0], d);
        EXPECT_FLOAT_EQ(de__d3_dot(de__d3_from(ss), de__d3_from(d)) / dl, 0.7, 1e-6);
        /* Capsule: the farther end cap center plus r. */
        const de_vec3 cs = de__shape_support(set[1], d);
        EXPECT_FLOAT_EQ(de__d3_dot(de__d3_from(cs), de__d3_from(d)) / dl,
                        0.9 * de__absd(d.z) / dl + 0.3, 1e-6);
        /* Box: the best of the 8 corners. */
        const de_vec3 e = set[2]->u.box.half_extents;
        f32 best = -FLT_MAX;
        for (u32 c = 0; c < 8; ++c) {
            const de_vec3 corner = {(c & 1) ? e.x : -e.x, (c & 2) ? e.y : -e.y,
                                    (c & 4) ? e.z : -e.z};
            best = de__maxf(best, de__v3_dot(corner, d));
        }
        EXPECT_TRUE(de__v3_dot(de__shape_support(set[2], d), d) == best);
        /* Cylinder: on the cylinder, and at least as far as 720 rim samples. */
        const de_vec3 ys = de__shape_support(set[3], d);
        EXPECT_FLOAT_EQ(de__absf(ys.z), 0.4f, 0.0f);
        EXPECT_FLOAT_EQ(de_dsqrt64((f64)ys.x * ys.x + (f64)ys.y * ys.y), 0.8, 1e-6);
        f64 rim = -1e30;
        for (u32 k = 0; k < 720; ++k) {
            const f32 ang = (f32)k * (DE_TAU / 720.0f);
            for (u32 cap = 0; cap < 2; ++cap) {
                const de_vec3 q = {0.8f * de_dcos(ang), 0.8f * de_dsin(ang), cap ? 0.4f : -0.4f};
                rim = de__maxd(rim, de__d3_dot(de__d3_from(q), de__d3_from(d)));
            }
        }
        EXPECT_TRUE(de__d3_dot(de__d3_from(ys), de__d3_from(d)) >= rim - 1e-5);
        /* Hull: the best vertex. */
        const de__hull *h = &set[4]->u.hull;
        best = -FLT_MAX;
        for (u32 v = 0; v < h->vertex_count; ++v)
            best = de__maxf(best, de__v3_dot(h->vertices[v], d));
        EXPECT_TRUE(de__v3_dot(de__shape_support(set[4], d), d) == best);
    }
    /* Ties and zero directions are deterministic. */
    const de_vec3 zero = {0, 0, 0};
    EXPECT_TRUE(de__shape_support(set[0], zero).z == 0.7f);
    EXPECT_TRUE(de__shape_support_core(set[1], zero).z == 0.9f);
    const de_vec3 bz = de__shape_support(set[2], zero);
    EXPECT_TRUE(bz.x == 0.5f && bz.y == 1.25f && bz.z == 2.0f);
    const de_vec3 cz = de__shape_support(set[3], (de_vec3){0, 0, -1});
    EXPECT_TRUE(cz.x == 0.0f && cz.y == 0.0f && cz.z == -0.4f);
    EXPECT_TRUE(de__shape_margin(set[0]) == 0.7f && de__shape_margin(set[1]) == 0.3f);
    EXPECT_TRUE(de__shape_margin(set[2]) == 0.0f && de__shape_margin(set[4]) == 0.0f);
    EXPECT_TRUE(de__shape_kind_is_convex(DE_SHAPE_CONVEX));
    EXPECT_FALSE(de__shape_kind_is_convex(DE_SHAPE_MESH));
}

/* ---- mesh ------------------------------------------------------------------------------- */

/* Checks a BVH: every primitive once, boxes nested, leaf sizes, depth. */
static void check_bvh(const de__bvh *bvh, const de_aabb *prim_bounds, u32 prim_universe,
                      u32 expected_prims, u32 max_leaf) {
    ASSERT_EQ(bvh->prim_count, expected_prims);
    /* Internal nodes have two children: an odd count, at most one leaf per primitive. */
    ASSERT_EQ(bvh->node_count % 2u, 1u);
    ASSERT_LE(bvh->node_count, 2u * expected_prims - 1u);
    u32 leaves = 0;
    ASSERT_LE(bvh->depth, DE__BVH_MAX_DEPTH);
    u32 seen_count = 0;
    u8 seen[4096];
    ASSERT_LE(prim_universe, 4096u);
    memset(seen, 0, sizeof seen);
    u32 stack[DE__BVH_MAX_DEPTH * 2], sp = 0, depth_stack[DE__BVH_MAX_DEPTH * 2];
    stack[sp] = 0;
    depth_stack[sp++] = 1;
    while (sp > 0) {
        --sp;
        const de__bvh_node *n = &bvh->nodes[stack[sp]];
        const u32 depth = depth_stack[sp];
        ASSERT_LE(depth, bvh->depth);
        if (n->count > 0) {
            ++leaves;
            ASSERT_LE(n->count, max_leaf);
            for (u32 i = 0; i < n->count; ++i) {
                const u32 prim = bvh->prims[n->first + i];
                ASSERT_LT(prim, prim_universe);
                EXPECT_EQ(seen[prim], 0u);
                seen[prim] = 1;
                ++seen_count;
                const de_aabb pb = prim_bounds[prim];
                EXPECT_TRUE(pb.min.x >= n->bounds.min.x && pb.max.x <= n->bounds.max.x &&
                            pb.min.y >= n->bounds.min.y && pb.max.y <= n->bounds.max.y &&
                            pb.min.z >= n->bounds.min.z && pb.max.z <= n->bounds.max.z);
            }
        } else {
            ASSERT_LT(n->first + 1u, bvh->node_count);
            for (u32 c = 0; c < 2; ++c) {
                const de_aabb cb = bvh->nodes[n->first + c].bounds;
                EXPECT_TRUE(cb.min.x >= n->bounds.min.x && cb.max.x <= n->bounds.max.x &&
                            cb.min.y >= n->bounds.min.y && cb.max.y <= n->bounds.max.y &&
                            cb.min.z >= n->bounds.min.z && cb.max.z <= n->bounds.max.z);
                ASSERT_LT(sp, DE__BVH_MAX_DEPTH * 2u);
                stack[sp] = n->first + c;
                depth_stack[sp++] = depth + 1u;
            }
        }
    }
    EXPECT_EQ(seen_count, expected_prims);
    EXPECT_EQ(leaves, (bvh->node_count + 1u) / 2u);
}

/* A grid of (n x n) quads, two triangles each, with heights from `h`. */
static void grid_mesh(u32 n, f32 (*h)(u32 x, u32 y), de_vec3 *verts, u32 *idx) {
    for (u32 y = 0; y <= n; ++y)
        for (u32 x = 0; x <= n; ++x)
            verts[y * (n + 1) + x] = (de_vec3){(f32)x, (f32)y, h(x, y)};
    u32 k = 0;
    for (u32 y = 0; y < n; ++y)
        for (u32 x = 0; x < n; ++x) {
            const u32 a = y * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
            idx[k++] = a;
            idx[k++] = b;
            idx[k++] = d;
            idx[k++] = a;
            idx[k++] = d;
            idx[k++] = c;
        }
}

static f32 h_flat(u32 x, u32 y) {
    (void)x;
    (void)y;
    return 0.0f;
}
static f32 h_bumpy(u32 x, u32 y) {
    return 0.25f * de_dsin(0.7f * (f32)x) * de_dcos(0.45f * (f32)y);
}

static de_aabb tri_box(const de__mesh *m, u32 t) {
    de_aabb b;
    b.min = b.max = m->vertices[m->indices[3 * t]];
    for (u32 c = 1; c < 3; ++c) {
        b.min = de__v3_min(b.min, m->vertices[m->indices[3 * t + c]]);
        b.max = de__v3_max(b.max, m->vertices[m->indices[3 * t + c]]);
    }
    return b;
}

TEST_F(physics_shapes, mesh_bvh_valid_and_deterministic, ps_fx) {
    enum { N = 32 };
    static de_vec3 verts[(N + 1) * (N + 1)];
    static u32 idx[N * N * 6];
    grid_mesh(N, h_bumpy, verts, idx);
    /* Two degenerate triangles at the end: excluded from the BVH. */
    static u32 idx2[N * N * 6 + 6];
    memcpy(idx2, idx, sizeof idx);
    idx2[N * N * 6 + 0] = 0;
    idx2[N * N * 6 + 1] = N + 1;
    idx2[N * N * 6 + 2] = 2 * (N + 1); /* colinear: the x = 0 column, where z = 0 */
    idx2[N * N * 6 + 3] = 5;
    idx2[N * N * 6 + 4] = 5;
    idx2[N * N * 6 + 5] = 6; /* repeated index */
    de_shape_desc d = desc_of(DE_SHAPE_MESH);
    d.vertices = verts;
    d.vertex_count = (N + 1) * (N + 1);
    d.indices = idx2;
    d.index_count = N * N * 6 + 6;
    const de__shape *s = create(f->p, &d);
    const de__mesh *m = &s->u.mesh;
    EXPECT_EQ(m->tri_count, (u32)(N * N * 2 + 2));
    EXPECT_TRUE((m->edge_flags[N * N * 2] & DE__MESH_TRI_DEGENERATE) != 0);
    EXPECT_TRUE((m->edge_flags[N * N * 2 + 1] & DE__MESH_TRI_DEGENERATE) != 0);
    EXPECT_FALSE(s->has_volume);
    static de_aabb boxes[N * N * 2 + 2];
    for (u32 t = 0; t < m->tri_count; ++t)
        boxes[t] = tri_box(m, t);
    check_bvh(&m->bvh, boxes, m->tri_count, N * N * 2, 4);
    /* The local box is the BVH root. */
    EXPECT_TRUE(s->local_aabb.min.x == 0.0f && s->local_aabb.max.x == (f32)N);

    /* Same input, same bytes. */
    const de__shape *s2 = create(f->p, &d);
    ASSERT_EQ(s2->u.mesh.bvh.node_count, m->bvh.node_count);
    EXPECT_MEMEQ(s2->u.mesh.bvh.nodes, m->bvh.nodes, sizeof(de__bvh_node) * m->bvh.node_count);
    EXPECT_MEMEQ(s2->u.mesh.bvh.prims, m->bvh.prims, sizeof(u32) * m->bvh.prim_count);
    EXPECT_MEMEQ(s2->data, s->data, s->data_size);
    EXPECT_EQ(s2->u.mesh.bvh.depth, m->bvh.depth);

    /* A larger degenerate-free mesh with all centroids on one line keeps the depth bounded. */
    static de_vec3 lv[3 * 600];
    static u32 li[3 * 600];
    for (u32 t = 0; t < 600; ++t) {
        lv[3 * t] = (de_vec3){0, 0, 0};
        lv[3 * t + 1] = (de_vec3){1, 0, 0};
        lv[3 * t + 2] = (de_vec3){0, 1, 0};
        li[3 * t] = 3 * t;
        li[3 * t + 1] = 3 * t + 1;
        li[3 * t + 2] = 3 * t + 2;
    }
    d.vertices = lv;
    d.vertex_count = 3 * 600;
    d.indices = li;
    d.index_count = 3 * 600;
    const de__shape *s3 = create(f->p, &d);
    static de_aabb lboxes[600];
    for (u32 t = 0; t < 600; ++t)
        lboxes[t] = tri_box(&s3->u.mesh, t);
    check_bvh(&s3->u.mesh.bvh, lboxes, 600, 600, 4);
}

TEST_F(physics_shapes, mesh_adjacency_and_internal_edges, ps_fx) {
    enum { N = 4 };
    de_vec3 verts[(N + 1) * (N + 1)];
    u32 idx[N * N * 6];
    grid_mesh(N, h_flat, verts, idx);
    de_shape_desc d = desc_of(DE_SHAPE_MESH);
    d.vertices = verts;
    d.vertex_count = (N + 1) * (N + 1);
    d.indices = idx;
    d.index_count = N * N * 6;
    const de__mesh *m = &create(f->p, &d)->u.mesh;
    /* Flat grid: internal edges inactive, boundary edges active. */
    u32 boundary = 0;
    for (u32 t = 0; t < m->tri_count; ++t) {
        for (u32 e = 0; e < 3; ++e) {
            const u32 nb = m->adjacent[3 * t + e];
            const bool active = (m->edge_flags[t] >> e) & 1u;
            if (nb == DE__MESH_NO_ADJ) {
                ++boundary;
                EXPECT_TRUE(active);
            } else {
                EXPECT_FALSE(active);
                /* Symmetric: the neighbour points back. */
                bool back = false;
                for (u32 k = 0; k < 3; ++k)
                    back = back || m->adjacent[3 * nb + k] == t;
                EXPECT_TRUE(back);
            }
        }
    }
    EXPECT_EQ(boundary, 4u * N);

    /* Two triangles sharing edge (0,1) folded down by 30 deg (a roof ridge): convex, active.
     * Folded up (a valley): concave, inactive. */
    for (u32 valley = 0; valley < 2; ++valley) {
        const f32 z = valley ? 0.5f : -0.5f;
        const de_vec3 rv[4] = {{0, 0, 0}, {1, 0, 0}, {0.5f, 1, z}, {0.5f, -1, z}};
        const u32 ri[6] = {0, 1, 2, 1, 0, 3};
        de_shape_desc rd = desc_of(DE_SHAPE_MESH);
        rd.vertices = rv;
        rd.vertex_count = 4;
        rd.indices = ri;
        rd.index_count = 6;
        const de__mesh *rm = &create(f->p, &rd)->u.mesh;
        EXPECT_EQ(rm->adjacent[0], 1u);
        EXPECT_EQ(rm->adjacent[3], 0u);
        EXPECT_EQ((u32)(rm->edge_flags[0] & 1u), valley ? 0u : 1u);
        EXPECT_EQ((u32)(rm->edge_flags[1] & 1u), valley ? 0u : 1u);
        EXPECT_EQ((u32)(rm->edge_flags[0] & 6u), 6u); /* open edges */
    }
}

/* ---- heightfield ------------------------------------------------------------------------ */

TEST_F(physics_shapes, heightfield_block_bounds, ps_fx) {
    enum { R = 21, C = 19 };
    static f32 h[R * C];
    de_rng r = de_rng_seed(11, 3);
    for (u32 i = 0; i < R * C; ++i)
        h[i] = rnd(&r, -3, 5);
    /* Holes: one sample, and a whole block region. */
    h[5 * C + 4] = DE_HEIGHTFIELD_HOLE;
    for (u32 row = 8; row <= 16; ++row)
        for (u32 col = 8; col <= 16; ++col)
            h[row * C + col] = DE_HEIGHTFIELD_HOLE;
    de_shape_desc d = desc_of(DE_SHAPE_HEIGHTFIELD);
    d.heights = h;
    d.rows = R;
    d.cols = C;
    d.scale = (de_vec3){0.5f, 2.0f, 1.5f};
    const de__shape *s = create(f->p, &d);
    const de__heightfield *hf = &s->u.hf;
    EXPECT_EQ(hf->blocks_x, 3u); /* 18 cells / 8 */
    EXPECT_EQ(hf->blocks_y, 3u); /* 20 cells / 8 */
    EXPECT_TRUE(de__hf_cell_is_hole(hf, 4, 5) && de__hf_cell_is_hole(hf, 3, 4));
    EXPECT_FALSE(de__hf_cell_is_hole(hf, 6, 5));
    f32 zmin = FLT_MAX, zmax = -FLT_MAX;
    for (u32 by = 0; by < hf->blocks_y; ++by) {
        for (u32 bx = 0; bx < hf->blocks_x; ++bx) {
            f32 lo = FLT_MAX, hi = -FLT_MAX;
            for (u32 row = by * 8; row < by * 8 + 8 && row < R - 1; ++row)
                for (u32 col = bx * 8; col < bx * 8 + 8 && col < C - 1; ++col) {
                    if (de__hf_cell_is_hole(hf, col, row))
                        continue;
                    de_vec3 tri[2][3];
                    de__hf_cell_triangles(hf, col, row, tri);
                    for (u32 t = 0; t < 2; ++t)
                        for (u32 v = 0; v < 3; ++v) {
                            lo = de__minf(lo, tri[t][v].z);
                            hi = de__maxf(hi, tri[t][v].z);
                        }
                }
            EXPECT_TRUE(hf->block_min[by * hf->blocks_x + bx] == lo);
            EXPECT_TRUE(hf->block_max[by * hf->blocks_x + bx] == hi);
            if (lo <= hi) {
                zmin = de__minf(zmin, lo);
                zmax = de__maxf(zmax, hi);
            }
        }
    }
    /* Block (1, 1) covers cells 8..15: all holes. */
    EXPECT_TRUE(hf->block_min[1 * 3 + 1] > hf->block_max[1 * 3 + 1]);
    EXPECT_TRUE(s->local_aabb.min.z == zmin && s->local_aabb.max.z == zmax);
    EXPECT_TRUE(s->local_aabb.min.x == 0.0f && s->local_aabb.max.x == 9.0f);
    EXPECT_TRUE(s->local_aabb.max.y == 40.0f);
    /* Triangulation: counter-clockwise from +Z. */
    de_vec3 tri[2][3];
    de__hf_cell_triangles(hf, 0, 0, tri);
    for (u32 t = 0; t < 2; ++t) {
        const de_vec3 n =
            de__v3_cross(de__v3_sub(tri[t][1], tri[t][0]), de__v3_sub(tri[t][2], tri[t][0]));
        EXPECT_TRUE(n.z > 0.0f);
    }
}

/* ---- compound --------------------------------------------------------------------------- */

TEST_F(physics_shapes, compound_bounds, ps_fx) {
    de_physics_world *p = f->p;
    de_shape_desc d = desc_of(DE_SHAPE_BOX);
    d.half_extents = (de_vec3){1, 0.5f, 0.25f};
    const de_shape box = de_shape_create(p, &d);
    d = desc_of(DE_SHAPE_SPHERE);
    d.radius = 0.5f;
    const de_shape sph = de_shape_create(p, &d);
    d = desc_of(DE_SHAPE_CAPSULE);
    d.radius = 0.2f;
    d.half_height = 1.0f;
    const de_shape cap = de_shape_create(p, &d);
    de_rng r = de_rng_seed(3, 9);
    enum { K = 9 };
    de_shape kids[K];
    de_transform xf[K];
    for (u32 i = 0; i < K; ++i) {
        kids[i] = i % 3 == 0 ? box : i % 3 == 1 ? sph : cap;
        xf[i].position = (de_dvec3){rnd(&r, -4, 4), rnd(&r, -4, 4), rnd(&r, -4, 4)};
        xf[i].rotation = rnd_quat(&r);
        xf[i].scale = (de_vec3){1, 1, 1};
    }
    d = desc_of(DE_SHAPE_COMPOUND);
    d.children = kids;
    d.child_xforms = xf;
    d.child_count = K;
    const de__shape *s = create(p, &d);
    const de__compound *c = &s->u.compound;
    ASSERT_EQ(c->child_count, (u32)K);
    de_aabb boxes[K];
    for (u32 i = 0; i < K; ++i) {
        boxes[i] = c->children[i].bounds;
        const de__shape *child = de__shape_get(p, kids[i]);
        const de_aabb want = de__shape_world_aabb(child, xf[i].position, xf[i].rotation);
        EXPECT_MEMEQ(&boxes[i], &want, sizeof want);
        EXPECT_TRUE(boxes[i].min.x >= s->local_aabb.min.x && boxes[i].max.z <= s->local_aabb.max.z);
    }
    check_bvh(&c->bvh, boxes, K, K, 2);
    EXPECT_TRUE(s->has_volume);
    /* The compound's world box at a transform contains each child's world box. */
    const de_dvec3 pos = {50000.0, -20.0, 3.0};
    const de_quat q = rnd_quat(&r);
    const de_aabb wb = de__shape_world_aabb(s, pos, q);
    for (u32 i = 0; i < K; ++i) {
        for (u32 n = 0; n < 32; ++n) {
            /* Child support point, through child then compound transforms. */
            const de__shape *child = c->children[i].shape;
            const de__dmat3 cm = de__dmat3_from_quat(c->children[i].rotation);
            const de__dmat3 pm = de__dmat3_from_quat(q);
            const de_vec3 dir = rnd_dir(&r);
            const de_vec3 sp = de__shape_support(child, dir);
            const de_dvec3 inc = de__d3_add(de__d3_from(c->children[i].position),
                                            de__dmat3_mul(&cm, de__d3_from(sp)));
            const de_dvec3 w = de__d3_add(pos, de__dmat3_mul(&pm, inc));
            EXPECT_TRUE(aabb_contains_d(wb, w, 1e-5));
        }
    }
    /* No transforms: children at the origin. */
    d.child_xforms = NULL;
    d.child_count = 2;
    const de__shape *s0 = create(p, &d);
    /* Child boxes are rounded outward by at most an ulp or so. */
    EXPECT_FLOAT_EQ(s0->local_aabb.max.x, 1.0f, 1e-6f);
    EXPECT_FLOAT_EQ(s0->local_aabb.max.y, 0.5f, 1e-6f);
    EXPECT_TRUE(s0->local_aabb.max.x >= 1.0f);
}

/* ---- refusals --------------------------------------------------------------------------- */

static void expect_refused(de_physics_world *p, const de_shape_desc *d) {
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape_create(p, d)));
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
}

TEST_F(physics_shapes, invalid_descs, ps_fx) {
    de_physics_world *p = f->p;
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape_create(p, NULL)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape_create(NULL, NULL)));
    de_shape_desc d = desc_of(DE_SHAPE_SPHERE);
    expect_refused(p, &d); /* radius 0 */
    d.radius = -1.0f;
    expect_refused(p, &d);
    d.radius = NAN;
    expect_refused(p, &d);
    d = desc_of(DE_SHAPE_CAPSULE);
    d.radius = 1.0f;
    d.half_height = -0.5f;
    expect_refused(p, &d);
    d.half_height = 0.0f; /* a capsule of height 0 is a sphere: fine */
    EXPECT_TRUE(DE_HANDLE_VALID(de_shape_create(p, &d)));
    d = desc_of(DE_SHAPE_BOX);
    d.half_extents = (de_vec3){1, 0, 1};
    expect_refused(p, &d);
    d = desc_of(DE_SHAPE_CYLINDER);
    d.radius = 1.0f;
    expect_refused(p, &d); /* half_height 0 */
    d = desc_of(DE_SHAPE_CONVEX);
    expect_refused(p, &d); /* no points */
    const de_vec3 three[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    d.points = three;
    d.point_count = 3;
    expect_refused(p, &d);
    d = desc_of(DE_SHAPE_MESH);
    expect_refused(p, &d);
    const u32 bad_idx[3] = {0, 1, 7};
    d.vertices = three;
    d.vertex_count = 3;
    d.indices = bad_idx;
    d.index_count = 3;
    expect_refused(p, &d); /* index out of range */
    d.index_count = 2;
    expect_refused(p, &d); /* not a multiple of 3 */
    const u32 degen[3] = {0, 0, 1};
    d.indices = degen;
    d.index_count = 3;
    expect_refused(p, &d); /* every triangle degenerate */
    d = desc_of(DE_SHAPE_HEIGHTFIELD);
    const f32 hts[4] = {0, 1, 2, 3};
    d.heights = hts;
    d.rows = 2;
    d.cols = 2;
    expect_refused(p, &d); /* scale 0 */
    d.scale = (de_vec3){1, 1, 1};
    d.rows = 1;
    expect_refused(p, &d);
    d.rows = 2;
    EXPECT_TRUE(DE_HANDLE_VALID(de_shape_create(p, &d)));
    d = desc_of(DE_SHAPE_COMPOUND);
    expect_refused(p, &d); /* no children */
    const de_shape stale = {de__handle_make(9, 3)};
    d.children = &stale;
    d.child_count = 1;
    expect_refused(p, &d);
    de_shape_desc sd = desc_of(DE_SHAPE_SPHERE);
    sd.radius = 1.0f;
    const de_shape sph = de_shape_create(p, &sd);
    d.children = &sph;
    const de_shape comp = de_shape_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(comp));
    d.children = &comp;
    expect_refused(p, &d); /* nested compound */
    d = desc_of((de_shape_kind)42);
    expect_refused(p, &d);
    /* Getters on a live shape. */
    de_shape_kind k;
    ASSERT_TRUE(de_shape_get_kind(p, sph, &k));
    EXPECT_EQ((u32)k, (u32)DE_SHAPE_SPHERE);
    de_aabb b;
    ASSERT_TRUE(de_shape_get_local_aabb(p, sph, &b));
    EXPECT_TRUE(b.max.x == 1.0f);
}

TEST(physics_shapes, create_oom_no_leak) {
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
    de_physics_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.allocator = &a;
    pd.max_bodies = 4;
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(&pd, &p), DE_OK);
    de_rng r = de_rng_seed(1, 1);
    de_vec3 pts[64];
    for (u32 i = 0; i < 64; ++i)
        pts[i] = (de_vec3){rnd(&r, -1, 1), rnd(&r, -1, 1), rnd(&r, -1, 1)};
    enum { N = 6 };
    de_vec3 verts[(N + 1) * (N + 1)];
    u32 idx[N * N * 6];
    grid_mesh(N, h_bumpy, verts, idx);
    f32 hts[25];
    for (u32 i = 0; i < 25; ++i)
        hts[i] = rnd(&r, 0, 1);
    de_shape_desc sd = desc_of(DE_SHAPE_SPHERE);
    sd.radius = 1.0f;
    const de_shape sph = de_shape_create(p, &sd);
    ASSERT_TRUE(DE_HANDLE_VALID(sph));
    de_shape_desc descs[4];
    descs[0] = desc_of(DE_SHAPE_CONVEX);
    descs[0].points = pts;
    descs[0].point_count = 64;
    descs[1] = desc_of(DE_SHAPE_MESH);
    descs[1].vertices = verts;
    descs[1].vertex_count = (N + 1) * (N + 1);
    descs[1].indices = idx;
    descs[1].index_count = N * N * 6;
    descs[2] = desc_of(DE_SHAPE_HEIGHTFIELD);
    descs[2].heights = hts;
    descs[2].rows = descs[2].cols = 5;
    descs[2].scale = (de_vec3){1, 1, 1};
    const de_shape kids[3] = {sph, sph, sph};
    descs[3] = desc_of(DE_SHAPE_COMPOUND);
    descs[3].children = kids;
    descs[3].child_count = 3;
    for (u32 k = 0; k < 4; ++k) {
        const u64 live = de_tracking_allocator_stats(&t).bytes_live;
        const u32 shapes = p->shape_live_count;
        bool done = false;
        for (i64 n = 0; n < 400 && !done; ++n) {
            de_tracking_allocator_fail_after(&t, n);
            const de_shape s = de_shape_create(p, &descs[k]);
            de_tracking_allocator_fail_after(&t, -1);
            if (DE_HANDLE_VALID(s)) {
                done = true;
                ASSERT_EQ(de_shape_destroy(p, s), DE_OK);
            } else {
                EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
            }
            EXPECT_EQ(p->shape_live_count, shapes);
            EXPECT_EQ(de__shape_get(p, sph)->refcount, 0u);
        }
        EXPECT_TRUE(done);
        /* The slot arrays may have grown once; nothing else stays. */
        EXPECT_LE(de_tracking_allocator_stats(&t).bytes_live, live + 1024u);
    }
    de_physics_destroy(p);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
}
