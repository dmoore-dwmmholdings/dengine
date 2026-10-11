/* simd_kernels.h: the 4-wide fast-math kernels, written once for every ISA (DENG-242).
 *
 * Included only by simd_sse.c, simd_avx2.c and simd_neon.c, each compiled for its own ISA. The
 * includer defines the vector type and operations first:
 *
 *   v4                          4 x f32 register type
 *   v4_load(p) v4_store(p, v)   unaligned 4-float load and store
 *   v4_set1(x)                  broadcast
 *   v4_add v4_sub v4_mul v4_div lane-wise, IEEE (v4_div is a true divide, never a reciprocal)
 *   v4_madd(a, b, c)            a * b + c: one FMA where the ISA has it, else mul then add
 *   v4_shuf(a, b, i, j, k, l)   (a[i], a[j], b[k], b[l]), indices are constants (SHUFPS order)
 *   v4_cmpge v4_cmplt v4_cmpgt  lane masks (all ones or zero), false for NaN
 *   v4_or(a, b)                 bitwise or of masks
 *   v4_select(m, a, b)          m ? a : b per lane
 *   v4_movemask(m)              bit i = lane i of the mask
 *   v4_abs(v)                   |v|
 *   DE__SIMD_FN(name)           the ISA-suffixed function name
 *
 * The frustum kernels never use v4_madd: they multiply and add separately in the order of the
 * scalar reference, ((nx * px + ny * py) + nz * pz) + d, so every ISA culls exactly the boxes the
 * scalar test culls. The ISA files are also compiled with -ffp-contract=off (GCC/Clang), so no
 * compiler fuses those separate operations.
 *
 * Other kernels may use FMA and sum in another order than the scalar code; tests/unit/math/
 * test_simd.c bounds the difference (4 ulp of the magnitude, 1e-5 relative for the inverse). */
#ifndef DE_CORE_MATH_SIMD_KERNELS_H
#define DE_CORE_MATH_SIMD_KERNELS_H

#include "core/math/math_simd.h"

#include <string.h>

/* The kernels read boxes, planes and frustums as packed float arrays. */
_Static_assert(sizeof(de_aabb) == 6 * sizeof(f32), "de_aabb must be 6 packed floats");
_Static_assert(sizeof(de_plane) == 4 * sizeof(f32), "de_plane must be 4 packed floats");
_Static_assert(sizeof(de_frustum) == 24 * sizeof(f32), "de_frustum must be 6 packed planes");

#define v4_splat(v, i)        v4_shuf((v), (v), i, i, i, i)
#define v4_swz(v, i, j, k, l) v4_shuf((v), (v), i, j, k, l)

/* ---- Load/store helpers. memcpy keeps the struct accesses well-defined; it compiles away. ---- */

static inline v4 de__k_load_quat(de_quat q) {
    f32 t[4];
    memcpy(t, &q, sizeof t);
    return v4_load(t);
}

static inline v4 de__k_load_v3(de_vec3 v) {
    f32 t[4] = {v.x, v.y, v.z, 0.0f};
    return v4_load(t);
}

static inline de_vec3 de__k_store_v3(v4 v) {
    f32 t[4];
    v4_store(t, v);
    de_vec3 r = {t[0], t[1], t[2]};
    return r;
}

/* cross(a, b) in lanes 0..2: a.yzx * b.zxy - a.zxy * b.yzx (de_v3_cross). */
static inline v4 de__k_cross(v4 a, v4 b) {
    v4 l = v4_mul(v4_swz(a, 1, 2, 0, 3), v4_swz(b, 2, 0, 1, 3));
    v4 r = v4_mul(v4_swz(a, 2, 0, 1, 3), v4_swz(b, 1, 2, 0, 3));
    return v4_sub(l, r);
}

/* ---- de_mat4 ---- */

static de_mat4 DE__SIMD_FN(m4_mul)(const de_mat4 *a, const de_mat4 *b) {
    /* Column j of a * b is the combination of a's columns weighted by column j of b. */
    v4 a0 = v4_load(a->m + 0), a1 = v4_load(a->m + 4);
    v4 a2 = v4_load(a->m + 8), a3 = v4_load(a->m + 12);
    de_mat4 r;
    for (int j = 0; j < 4; ++j) {
        v4 bj = v4_load(b->m + 4 * j);
        v4 c = v4_mul(a0, v4_splat(bj, 0));
        c = v4_madd(a1, v4_splat(bj, 1), c);
        c = v4_madd(a2, v4_splat(bj, 2), c);
        c = v4_madd(a3, v4_splat(bj, 3), c);
        v4_store(r.m + 4 * j, c);
    }
    return r;
}

static de_vec4 DE__SIMD_FN(m4_mul_v4)(const de_mat4 *m, de_vec4 v) {
    f32 t[4];
    memcpy(t, &v, sizeof t);
    v4 vv = v4_load(t);
    v4 c = v4_mul(v4_load(m->m + 0), v4_splat(vv, 0));
    c = v4_madd(v4_load(m->m + 4), v4_splat(vv, 1), c);
    c = v4_madd(v4_load(m->m + 8), v4_splat(vv, 2), c);
    c = v4_madd(v4_load(m->m + 12), v4_splat(vv, 3), c);
    v4_store(t, c);
    de_vec4 r;
    memcpy(&r, t, sizeof t);
    return r;
}

/* 2x2 matrices packed in one register as (m00, m01, m10, m11). The block-inverse helpers below
 * follow Eric Zhang's "Fast 4x4 matrix inverse with SSE SIMD, explained" (2017). The method is
 * the same for row- and column-major storage, because inverse(M^T) = inverse(M)^T. */
static inline v4 de__k_mat2_mul(v4 a, v4 b) { /* a * b */
    return v4_add(v4_mul(a, v4_swz(b, 0, 3, 0, 3)),
                  v4_mul(v4_swz(a, 1, 0, 3, 2), v4_swz(b, 2, 1, 2, 1)));
}
static inline v4 de__k_mat2_adj_mul(v4 a, v4 b) { /* adj(a) * b */
    return v4_sub(v4_mul(v4_swz(a, 3, 3, 0, 0), b),
                  v4_mul(v4_swz(a, 1, 1, 2, 2), v4_swz(b, 2, 3, 0, 1)));
}
static inline v4 de__k_mat2_mul_adj(v4 a, v4 b) { /* a * adj(b) */
    return v4_sub(v4_mul(a, v4_swz(b, 3, 0, 3, 0)),
                  v4_mul(v4_swz(a, 1, 0, 3, 2), v4_swz(b, 2, 1, 2, 1)));
}

static de_mat4 DE__SIMD_FN(m4_inverse)(const de_mat4 *mat) {
    /* Split M into 2x2 blocks [A B; C D] and use the adjugate form of the block inverse. Valid
     * for any non-singular M (the formulas are polynomial identities; no block must be
     * invertible). */
    v4 m0 = v4_load(mat->m + 0), m1 = v4_load(mat->m + 4);
    v4 m2 = v4_load(mat->m + 8), m3 = v4_load(mat->m + 12);
    v4 A = v4_shuf(m0, m1, 0, 1, 0, 1);
    v4 B = v4_shuf(m0, m1, 2, 3, 2, 3);
    v4 C = v4_shuf(m2, m3, 0, 1, 0, 1);
    v4 D = v4_shuf(m2, m3, 2, 3, 2, 3);

    /* (|A|, |B|, |C|, |D|) */
    v4 det_sub = v4_sub(v4_mul(v4_shuf(m0, m2, 0, 2, 0, 2), v4_shuf(m1, m3, 1, 3, 1, 3)),
                        v4_mul(v4_shuf(m0, m2, 1, 3, 1, 3), v4_shuf(m1, m3, 0, 2, 0, 2)));
    v4 det_a = v4_splat(det_sub, 0), det_b = v4_splat(det_sub, 1);
    v4 det_c = v4_splat(det_sub, 2), det_d = v4_splat(det_sub, 3);

    v4 d_c = de__k_mat2_adj_mul(D, C); /* adj(D) C */
    v4 a_b = de__k_mat2_adj_mul(A, B); /* adj(A) B */
    v4 x_ = v4_sub(v4_mul(det_d, A), de__k_mat2_mul(B, d_c));
    v4 w_ = v4_sub(v4_mul(det_a, D), de__k_mat2_mul(C, a_b));
    v4 y_ = v4_sub(v4_mul(det_b, C), de__k_mat2_mul_adj(D, a_b));
    v4 z_ = v4_sub(v4_mul(det_c, B), de__k_mat2_mul_adj(A, d_c));

    /* |M| = |A| |D| + |B| |C| - tr(adj(A) B adj(D) C) */
    v4 det_m = v4_add(v4_mul(det_a, det_d), v4_mul(det_b, det_c));
    v4 tr = v4_mul(a_b, v4_swz(d_c, 0, 2, 1, 3));
    tr = v4_add(tr, v4_swz(tr, 1, 0, 3, 2));
    tr = v4_add(tr, v4_swz(tr, 2, 3, 0, 1));
    det_m = v4_sub(det_m, tr);

    de_mat4 r;
    f32 det[4];
    v4_store(det, det_m);
    if (det[0] == 0.0f) { /* singular: the zero matrix, like the scalar reference */
        memset(&r, 0, sizeof r);
        return r;
    }
    const f32 sign[4] = {1.0f, -1.0f, -1.0f, 1.0f};
    v4 rdet = v4_div(v4_load(sign), det_m); /* a true divide, no reciprocal estimate */
    x_ = v4_mul(x_, rdet);
    y_ = v4_mul(y_, rdet);
    z_ = v4_mul(z_, rdet);
    w_ = v4_mul(w_, rdet);
    /* The adjugate shuffle and the block reassembly in one step. */
    v4_store(r.m + 0, v4_shuf(x_, y_, 3, 1, 3, 1));
    v4_store(r.m + 4, v4_shuf(x_, y_, 2, 0, 2, 0));
    v4_store(r.m + 8, v4_shuf(z_, w_, 3, 1, 3, 1));
    v4_store(r.m + 12, v4_shuf(z_, w_, 2, 0, 2, 0));
    return r;
}

/* ---- de_quat ---- */

static de_quat DE__SIMD_FN(quat_mul)(de_quat qa, de_quat qb) {
    /* a * b = aw b + ax (bw, -bz, by, -bx) + ay (bz, bw, -bx, -by) + az (-by, bx, bw, -bz). The
     * sign flips are exact; the four terms are summed as two independent pairs, which halves the
     * dependency chain (the scalar code sums them left to right). */
    static const f32 s_x[4] = {1.0f, -1.0f, 1.0f, -1.0f};
    static const f32 s_y[4] = {1.0f, 1.0f, -1.0f, -1.0f};
    static const f32 s_z[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    v4 a = de__k_load_quat(qa), b = de__k_load_quat(qb);
    v4 l = v4_madd(v4_mul(v4_splat(a, 0), v4_load(s_x)), v4_swz(b, 3, 2, 1, 0),
                   v4_mul(v4_splat(a, 3), b));
    v4 h = v4_madd(v4_mul(v4_splat(a, 1), v4_load(s_y)), v4_swz(b, 2, 3, 0, 1),
                   v4_mul(v4_mul(v4_splat(a, 2), v4_load(s_z)), v4_swz(b, 1, 0, 3, 2)));
    f32 t[4];
    v4_store(t, v4_add(l, h));
    de_quat q;
    memcpy(&q, t, sizeof t);
    return q;
}

static de_vec3 DE__SIMD_FN(quat_rotate)(de_quat q, de_vec3 v) {
    /* v' = v + w t + u x t, t = 2 (u x v), u = (x, y, z): the scalar formula. */
    v4 u = de__k_load_quat(q);
    v4 vv = de__k_load_v3(v);
    v4 t = de__k_cross(u, vv);
    t = v4_add(t, t);
    v4 r = v4_madd(t, v4_splat(u, 3), vv);
    r = v4_add(r, de__k_cross(u, t));
    return de__k_store_v3(r);
}

/* ---- de_affine ---- */

/* The four 3-float columns of a de_affine, one per register (lane 3 is junk). Every load stays
 * inside the 12 floats. */
static inline void de__k_affine_cols(const de_affine *a, v4 c[4]) {
    c[0] = v4_load(a->m + 0);
    c[1] = v4_load(a->m + 3);
    c[2] = v4_load(a->m + 6);
    c[3] = v4_swz(v4_load(a->m + 8), 1, 2, 3, 3);
}

static de_affine DE__SIMD_FN(affine_mul)(const de_affine *a, const de_affine *b) {
    /* [La ta] [Lb tb] = [La Lb, La tb + ta] */
    v4 c[4];
    de__k_affine_cols(a, c);
    v4 r[4];
    for (int j = 0; j < 4; ++j) {
        const f32 *bj = b->m + 3 * j;
        v4 s = v4_mul(c[0], v4_set1(bj[0]));
        s = v4_madd(c[1], v4_set1(bj[1]), s);
        s = v4_madd(c[2], v4_set1(bj[2]), s);
        r[j] = s;
    }
    r[3] = v4_add(r[3], c[3]);
    /* Pack the four 3-float columns into 12 floats: (r0 r1 r2 r3) xyz. */
    v4 t0 = v4_shuf(r[0], r[1], 2, 2, 0, 0); /* r0z r0z r1x r1x */
    v4 t2 = v4_shuf(r[2], r[3], 2, 2, 0, 0); /* r2z r2z r3x r3x */
    de_affine out;
    v4_store(out.m + 0, v4_shuf(r[0], t0, 0, 1, 0, 2));   /* r0x r0y r0z r1x */
    v4_store(out.m + 4, v4_shuf(r[1], r[2], 1, 2, 0, 1)); /* r1y r1z r2x r2y */
    v4_store(out.m + 8, v4_shuf(t2, r[3], 0, 2, 1, 2));   /* r2z r3x r3y r3z */
    return out;
}

/* ---- de_aabb batch ---- */

static void DE__SIMD_FN(affine_transform_aabbs)(const de_affine *a, const de_aabb *in, de_aabb *out,
                                                u32 n) {
    /* Arvo in center/extents form, like de_aabb_transform. */
    v4 c[4];
    de__k_affine_cols(a, c);
    v4 abs0 = v4_abs(c[0]), abs1 = v4_abs(c[1]), abs2 = v4_abs(c[2]);
    const v4 half = v4_set1(0.5f);
    for (u32 i = 0; i < n; ++i) {
        /* A de_aabb is 6 floats: lo = (minx miny minz maxx), hi = (minz maxx maxy maxz). */
        const f32 *src = &in[i].min.x;
        v4 lo = v4_load(src);
        v4 hi = v4_load(src + 2);
        v4 mn = lo;
        v4 mx = v4_swz(hi, 1, 2, 3, 3);
        if ((v4_movemask(v4_cmpgt(mn, mx)) & 7) != 0) {
            out[i] = in[i]; /* empty stays empty (a no-op when out == in) */
            continue;
        }
        v4 ctr = v4_mul(v4_add(mn, mx), half);
        v4 ext = v4_mul(v4_sub(mx, mn), half);
        v4 nc = v4_mul(c[0], v4_splat(ctr, 0));
        nc = v4_madd(c[1], v4_splat(ctr, 1), nc);
        nc = v4_madd(c[2], v4_splat(ctr, 2), nc);
        nc = v4_add(nc, c[3]);
        v4 ne = v4_mul(abs0, v4_splat(ext, 0));
        ne = v4_madd(abs1, v4_splat(ext, 1), ne);
        ne = v4_madd(abs2, v4_splat(ext, 2), ne);
        v4 nmin = v4_sub(nc, ne), nmax = v4_add(nc, ne);
        v4 t = v4_shuf(nmin, nmax, 2, 2, 0, 0); /* nminz nminz nmaxx nmaxx */
        f32 *dst = &out[i].min.x;
        /* Two overlapping stores cover the 6 floats; both write the same value to floats 2-3. */
        v4_store(dst, v4_shuf(nmin, t, 0, 1, 0, 2));
        v4_store(dst + 2, v4_shuf(t, nmax, 0, 2, 1, 2));
    }
}

/* ---- de_mat4 batch ---- */

static void DE__SIMD_FN(m4_transform_points)(const de_mat4 *m, const de_vec3 *in, de_vec4 *out,
                                             u32 n) {
    v4 c0 = v4_load(m->m + 0), c1 = v4_load(m->m + 4);
    v4 c2 = v4_load(m->m + 8), c3 = v4_load(m->m + 12);
    for (u32 i = 0; i < n; ++i) {
        v4 r = v4_mul(c0, v4_set1(in[i].x));
        r = v4_madd(c1, v4_set1(in[i].y), r);
        r = v4_madd(c2, v4_set1(in[i].z), r);
        r = v4_add(r, c3);
        v4_store(&out[i].x, r);
    }
}

/* ---- de_frustum ---- */

/* Boxes i..i+3 as six registers of one coordinate each (AoS -> SoA). A de_aabb is 6 floats, so
 * four boxes are exactly six 4-float loads. */
static inline void de__k_load_boxes4(const de_aabb *b, v4 out[6]) {
    const f32 *p = &b->min.x;
    v4 v0 = v4_load(p + 0), v1 = v4_load(p + 4), v2 = v4_load(p + 8);
    v4 w0 = v4_load(p + 12), w1 = v4_load(p + 16), w2 = v4_load(p + 20);
    /* v0 = mnx0 mny0 mnz0 mxx0 | v1 = mxy0 mxz0 mnx1 mny1 | v2 = mnz1 mxx1 mxy1 mxz1 */
    v4 a01 = v4_shuf(v0, v1, 0, 1, 2, 3); /* mnx0 mny0 mnx1 mny1 */
    v4 b01 = v4_shuf(v0, v2, 2, 3, 0, 1); /* mnz0 mxx0 mnz1 mxx1 */
    v4 c01 = v4_shuf(v1, v2, 0, 1, 2, 3); /* mxy0 mxz0 mxy1 mxz1 */
    v4 a23 = v4_shuf(w0, w1, 0, 1, 2, 3); /* boxes 2 and 3, from w0..w2 the same way */
    v4 b23 = v4_shuf(w0, w2, 2, 3, 0, 1);
    v4 c23 = v4_shuf(w1, w2, 0, 1, 2, 3);
    out[0] = v4_shuf(a01, a23, 0, 2, 0, 2); /* min.x */
    out[1] = v4_shuf(a01, a23, 1, 3, 1, 3); /* min.y */
    out[2] = v4_shuf(b01, b23, 0, 2, 0, 2); /* min.z */
    out[3] = v4_shuf(b01, b23, 1, 3, 1, 3); /* max.x */
    out[4] = v4_shuf(c01, c23, 0, 2, 0, 2); /* max.y */
    out[5] = v4_shuf(c01, c23, 1, 3, 1, 3); /* max.z */
}

static bool DE__SIMD_FN(frustum_test_aabb)(const de_frustum *f, de_aabb box) {
    if (box.min.x > box.max.x || box.min.y > box.max.y || box.min.z > box.max.z)
        return false;
    /* Planes 0-3 in one group and 4, 5, 4, 5 in the other (repeating a plane changes nothing),
     * transposed so each register holds one plane component. */
    const f32 *p = &f->planes[0].n.x;
    v4 p0 = v4_load(p + 0), p1 = v4_load(p + 4), p2 = v4_load(p + 8);
    v4 p3 = v4_load(p + 12), p4 = v4_load(p + 16), p5 = v4_load(p + 20);
    v4 lo01 = v4_shuf(p0, p1, 0, 1, 0, 1), hi01 = v4_shuf(p0, p1, 2, 3, 2, 3);
    v4 lo23 = v4_shuf(p2, p3, 0, 1, 0, 1), hi23 = v4_shuf(p2, p3, 2, 3, 2, 3);
    v4 lo45 = v4_shuf(p4, p5, 0, 1, 0, 1), hi45 = v4_shuf(p4, p5, 2, 3, 2, 3);
    v4 g[2][4] = {
        {v4_shuf(lo01, lo23, 0, 2, 0, 2), v4_shuf(lo01, lo23, 1, 3, 1, 3),
         v4_shuf(hi01, hi23, 0, 2, 0, 2), v4_shuf(hi01, hi23, 1, 3, 1, 3)},
        {v4_shuf(lo45, lo45, 0, 2, 0, 2), v4_shuf(lo45, lo45, 1, 3, 1, 3),
         v4_shuf(hi45, hi45, 0, 2, 0, 2), v4_shuf(hi45, hi45, 1, 3, 1, 3)},
    };
    const v4 zero = v4_set1(0.0f);
    v4 mnx = v4_set1(box.min.x), mny = v4_set1(box.min.y), mnz = v4_set1(box.min.z);
    v4 mxx = v4_set1(box.max.x), mxy = v4_set1(box.max.y), mxz = v4_set1(box.max.z);
    int out = 0;
    for (int k = 0; k < 2; ++k) {
        v4 nx = g[k][0], ny = g[k][1], nz = g[k][2], d = g[k][3];
        /* The p-vertex: the corner farthest along the normal (n >= 0 picks max, like scalar). */
        v4 px = v4_select(v4_cmpge(nx, zero), mxx, mnx);
        v4 py = v4_select(v4_cmpge(ny, zero), mxy, mny);
        v4 pz = v4_select(v4_cmpge(nz, zero), mxz, mnz);
        v4 dist = v4_mul(nx, px);
        dist = v4_add(dist, v4_mul(ny, py));
        dist = v4_add(dist, v4_mul(nz, pz));
        dist = v4_add(dist, d);
        out |= v4_movemask(v4_cmplt(dist, zero));
    }
    return out == 0;
}

/* Per-plane constants of a batch cull, read once before the box loop: the stores to the u8
 * output may alias the frustum, so the compiler would otherwise reload and re-broadcast the planes
 * for every group of boxes. sel[] picks the p-vertex coordinate of each axis: the max register
 * (3, 4, 5) when the normal component is >= 0, else the min register (0, 1, 2), as in scalar. */
typedef struct de__k_cull_plane {
    f32 n[3], d;
    int sel[3];
} de__k_cull_plane;

static inline void de__k_cull_planes(const de_frustum *f, de__k_cull_plane out[6]) {
    for (int k = 0; k < 6; ++k) {
        const de_plane *p = &f->planes[k];
        out[k].n[0] = p->n.x;
        out[k].n[1] = p->n.y;
        out[k].n[2] = p->n.z;
        out[k].d = p->d;
        out[k].sel[0] = p->n.x >= 0.0f ? 3 : 0;
        out[k].sel[1] = p->n.y >= 0.0f ? 4 : 1;
        out[k].sel[2] = p->n.z >= 0.0f ? 5 : 2;
    }
}

#if !defined(DE__SIMD_CUSTOM_CULL)
static void DE__SIMD_FN(frustum_cull_aabbs)(const de_frustum *f, const de_aabb *boxes, u32 n,
                                            u8 *visible) {
    de__k_cull_plane pl[6];
    de__k_cull_planes(f, pl);
    v4 pn[6][4];
    for (int k = 0; k < 6; ++k) {
        pn[k][0] = v4_set1(pl[k].n[0]);
        pn[k][1] = v4_set1(pl[k].n[1]);
        pn[k][2] = v4_set1(pl[k].n[2]);
        pn[k][3] = v4_set1(pl[k].d);
    }
    const v4 zero = v4_set1(0.0f);
    u32 i = 0;
    for (; i + 4 <= n; i += 4) {
        v4 b[6];
        de__k_load_boxes4(boxes + i, b);
        v4 culled = v4_or(v4_or(v4_cmpgt(b[0], b[3]), v4_cmpgt(b[1], b[4])), v4_cmpgt(b[2], b[5]));
        for (int k = 0; k < 6; ++k) {
            v4 dist = v4_mul(pn[k][0], b[pl[k].sel[0]]);
            dist = v4_add(dist, v4_mul(pn[k][1], b[pl[k].sel[1]]));
            dist = v4_add(dist, v4_mul(pn[k][2], b[pl[k].sel[2]]));
            dist = v4_add(dist, pn[k][3]);
            culled = v4_or(culled, v4_cmplt(dist, zero));
        }
        int mask = v4_movemask(culled);
        visible[i + 0] = (u8)(((mask >> 0) & 1) ^ 1);
        visible[i + 1] = (u8)(((mask >> 1) & 1) ^ 1);
        visible[i + 2] = (u8)(((mask >> 2) & 1) ^ 1);
        visible[i + 3] = (u8)(((mask >> 3) & 1) ^ 1);
    }
    for (; i < n; ++i)
        visible[i] = de__frustum_test_aabb_scalar(f, boxes[i]) ? 1 : 0;
}
#endif

/* The ISA's table. Kernels an ISA file overrides are named by DE__SIMD_CULL_FN. */
#if !defined(DE__SIMD_CULL_FN)
#define DE__SIMD_CULL_FN DE__SIMD_FN(frustum_cull_aabbs)
#endif
#define DE__SIMD_TABLE_INIT(isa_id)                                                                \
    {                                                                                              \
        (isa_id),                                                                                  \
        DE__SIMD_FN(m4_mul),                                                                       \
        DE__SIMD_FN(m4_inverse),                                                                   \
        DE__SIMD_FN(m4_mul_v4),                                                                    \
        DE__SIMD_FN(quat_mul),                                                                     \
        DE__SIMD_FN(quat_rotate),                                                                  \
        DE__SIMD_FN(affine_mul),                                                                   \
        DE__SIMD_FN(frustum_test_aabb),                                                            \
        DE__SIMD_FN(m4_transform_points),                                                          \
        DE__SIMD_FN(affine_transform_aabbs),                                                       \
        DE__SIMD_CULL_FN,                                                                          \
    }

#endif
