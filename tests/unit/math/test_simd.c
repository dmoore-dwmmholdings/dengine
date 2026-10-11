/* test_simd.c: the SIMD fast-math paths against the scalar reference (DENG-242).
 *
 * Every ISA table that is compiled in and supported by this CPU (de__math_table_for) is compared
 * with the scalar table on the same inputs, whatever the dispatcher picked. The ctest entry
 * simd.forced_scalar runs the whole binary again with DE_MATH_FORCE_SCALAR=1, and on x86-64
 * simd.isa_sse42 with DE_MATH_ISA=sse4.2.
 *
 * Accuracy measure. The SIMD kernels may fuse multiply-adds and sum in another order, so a result
 * that cancels (a small sum of large products) can differ from the scalar one by many ulps of
 * itself while both are as accurate as float allows. Errors are therefore measured in ulps of the
 * magnitude of the computation: the same formula evaluated on absolute values (for a dot product,
 * sum |a_i b_i|). When nothing cancels, that is the ulp of the result. The bound is 4 such ulps;
 * de_m4_inverse is bounded by 1e-5 relative to the largest element of the inverse. */
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#endif

#include "de_test.h"

#include "core/math/math_simd.h"
#include <dengine/de_math.h>

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_RANDOM 100000
#define MAX_ULPS 4.0

/* ---- Random inputs (xorshift32, fixed seed: every run sees the same inputs) ---- */

static u32 g_rng = 0x9E3779B9u;
static u32 rnd_u32(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static f32 rnd(f32 lo, f32 hi) {
    return lo + (hi - lo) * (f32)(rnd_u32() >> 8) * (1.0f / 16777216.0f);
}
static de_vec3 v3(f32 x, f32 y, f32 z) {
    de_vec3 r = {x, y, z};
    return r;
}
static de_vec3 rnd_v3(f32 lo, f32 hi) {
    return v3(rnd(lo, hi), rnd(lo, hi), rnd(lo, hi));
}
static de_quat rnd_unit_quat(void) {
    return de_quat_axis_angle(de_v3_normalize_safe(rnd_v3(-1, 1), DE_AXIS_UP), rnd(-DE_PI, DE_PI));
}
static de_quat quat(f32 x, f32 y, f32 z, f32 w) {
    de_quat q;
    q.x = x;
    q.y = y;
    q.z = z;
    q.w = w;
    return q;
}
static void rnd_m4(de_mat4 *m, f32 lo, f32 hi) {
    for (int i = 0; i < 16; ++i)
        m->m[i] = rnd(lo, hi);
}

/* ---- ulp measure ---- */

static f64 ulp_of(f64 scale) {
    f32 s = (f32)fabs(scale);
    if (!isfinite(s))
        return INFINITY;
    return (f64)(nextafterf(s, INFINITY) - s);
}
/* |a - b| in ulps of `scale`; 0 when equal (+0 == -0), infinite for a NaN. */
static f64 ulps_off(f32 a, f32 b, f64 scale) {
    if (a == b)
        return 0.0;
    if (isnan(a) || isnan(b))
        return INFINITY;
    return fabs((f64)a - (f64)b) / ulp_of(scale);
}

/* ---- The SIMD tables available here ---- */

static const de__math_table *g_isas[DE_MATH_ISA_COUNT];
static int simd_tables(void) {
    int n = 0;
    for (int i = 0; i < DE_MATH_ISA_COUNT; ++i) {
        if (i == DE_MATH_ISA_SCALAR)
            continue;
        const de__math_table *t = de__math_table_for((de_math_isa)i);
        if (t != NULL)
            g_isas[n++] = t;
    }
    return n;
}

static const de__math_table *scalar(void) {
    return &de__math_table_scalar;
}

static void report(const char *what, const de__math_table *t, f64 worst) {
    printf("  %-26s %-7s max %.2f ulp\n", what, de_math_isa_name(t->isa), worst);
}

/* ---- de_mat4 ---- */

TEST(simd, m4_mul_vs_scalar) {
    int nt = simd_tables();
    for (int k = 0; k < nt; ++k) {
        const de__math_table *t = g_isas[k];
        f64 worst = 0.0, worst_v = 0.0;
        g_rng = 0x1234567u;
        for (int it = 0; it < N_RANDOM; ++it) {
            de_mat4 a, b;
            rnd_m4(&a, -10.0f, 10.0f);
            rnd_m4(&b, -10.0f, 10.0f);
            de_mat4 rs = scalar()->m4_mul(&a, &b);
            de_mat4 rv = t->m4_mul(&a, &b);
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r) {
                    f64 mag = 0.0;
                    for (int i = 0; i < 4; ++i)
                        mag += fabs((f64)a.m[i * 4 + r] * b.m[c * 4 + i]);
                    f64 u = ulps_off(rv.m[c * 4 + r], rs.m[c * 4 + r], mag);
                    worst = u > worst ? u : worst;
                }
            }
            de_vec4 v;
            v.x = rnd(-100, 100);
            v.y = rnd(-100, 100);
            v.z = rnd(-100, 100);
            v.w = rnd(-2, 2);
            de_vec4 vs = scalar()->m4_mul_v4(&a, v), vv = t->m4_mul_v4(&a, v);
            const f32 vin[4] = {v.x, v.y, v.z, v.w};
            const f32 os[4] = {vs.x, vs.y, vs.z, vs.w}, ov[4] = {vv.x, vv.y, vv.z, vv.w};
            for (int r = 0; r < 4; ++r) {
                f64 mag = 0.0;
                for (int i = 0; i < 4; ++i)
                    mag += fabs((f64)a.m[i * 4 + r] * vin[i]);
                f64 u = ulps_off(ov[r], os[r], mag);
                worst_v = u > worst_v ? u : worst_v;
            }
        }
        report("m4_mul", t, worst);
        report("m4_mul_v4", t, worst_v);
        EXPECT_TRUE(worst <= MAX_ULPS);
        EXPECT_TRUE(worst_v <= MAX_ULPS);
    }
}

/* Well-conditioned random matrices: diagonally dominant, or T*R*S with moderate scales. */
static void rnd_invertible(de_mat4 *m, int kind) {
    if (kind == 0) {
        rnd_m4(m, -1.0f, 1.0f);
        for (int i = 0; i < 4; ++i)
            m->m[i * 5] += (rnd_u32() & 1) ? 5.0f : -5.0f;
    } else {
        *m = de_m4_trs(rnd_v3(-100, 100), rnd_unit_quat(), rnd_v3(0.5f, 2.0f));
    }
}

static f64 inverse_rel_err(const de_mat4 *a, const de_mat4 *b) {
    f64 big = 0.0, diff = 0.0;
    for (int i = 0; i < 16; ++i) {
        big = fmax(big, fabs((f64)b->m[i]));
        diff = fmax(diff, fabs((f64)a->m[i] - (f64)b->m[i]));
    }
    return big > 0.0 ? diff / big : diff;
}

TEST(simd, m4_inverse_vs_scalar) {
    int nt = simd_tables();
    for (int k = 0; k < nt; ++k) {
        const de__math_table *t = g_isas[k];
        f64 worst = 0.0;
        g_rng = 0xBADC0DEu;
        for (int it = 0; it < N_RANDOM; ++it) {
            de_mat4 m;
            rnd_invertible(&m, it & 1);
            de_mat4 rs = scalar()->m4_inverse(&m);
            de_mat4 rv = t->m4_inverse(&m);
            f64 e = inverse_rel_err(&rv, &rs);
            worst = e > worst ? e : worst;
        }
        /* A camera: reverse-Z infinite perspective times a view (row 3 is not (0 0 0 1)). */
        de_mat4 view = de_m4_look_at(v3(3, -7, 2), v3(0, 0, 1), DE_AXIS_UP);
        de_mat4 proj = de_m4_perspective_rz(1.0f, 16.0f / 9.0f, 0.1f);
        de_mat4 vp = scalar()->m4_mul(&proj, &view);
        de_mat4 vps = scalar()->m4_inverse(&vp), vpv = t->m4_inverse(&vp);
        f64 e = inverse_rel_err(&vpv, &vps);
        worst = e > worst ? e : worst;
        printf("  %-26s %-7s max %.3g relative\n", "m4_inverse", de_math_isa_name(t->isa), worst);
        EXPECT_TRUE(worst <= 1e-5);

        /* Singular: the zero matrix, like the scalar reference. */
        de_mat4 sing = {{1, 2, 3, 4, 2, 4, 6, 8, 0, 1, 0, 1, 5, 1, 2, 3}};
        de_mat4 zs = scalar()->m4_inverse(&sing), zv = t->m4_inverse(&sing);
        const de_mat4 zero = {{0}};
        EXPECT_MEMEQ(zs.m, zero.m, sizeof zero.m);
        EXPECT_MEMEQ(zv.m, zero.m, sizeof zero.m);
        de_mat4 id = de_m4_identity();
        de_mat4 idv = t->m4_inverse(&id);
        for (int i = 0; i < 16; ++i) /* exact; the adjugate signs may give -0 off the diagonal */
            EXPECT_TRUE(idv.m[i] == id.m[i]);
    }
}

/* ---- de_quat ---- */

TEST(simd, quat_vs_scalar) {
    int nt = simd_tables();
    for (int k = 0; k < nt; ++k) {
        const de__math_table *t = g_isas[k];
        f64 worst_mul = 0.0, worst_rot = 0.0;
        g_rng = 0xC0FFEEu;
        for (int it = 0; it < N_RANDOM; ++it) {
            /* mul: unit and non-unit quaternions. */
            de_quat a =
                (it & 1) ? rnd_unit_quat() : quat(rnd(-3, 3), rnd(-3, 3), rnd(-3, 3), rnd(-3, 3));
            de_quat b =
                (it & 2) ? rnd_unit_quat() : quat(rnd(-3, 3), rnd(-3, 3), rnd(-3, 3), rnd(-3, 3));
            de_quat ms = scalar()->quat_mul(a, b), mv = t->quat_mul(a, b);
            const f64 ax = fabs(a.x), ay = fabs(a.y), az = fabs(a.z), aw = fabs(a.w);
            const f64 bx = fabs(b.x), by = fabs(b.y), bz = fabs(b.z), bw = fabs(b.w);
            const f64 mag[4] = {
                aw * bx + ax * bw + ay * bz + az * by, aw * by + ax * bz + ay * bw + az * bx,
                aw * bz + ax * by + ay * bx + az * bw, aw * bw + ax * bx + ay * by + az * bz};
            const f32 os[4] = {ms.x, ms.y, ms.z, ms.w}, ov[4] = {mv.x, mv.y, mv.z, mv.w};
            for (int i = 0; i < 4; ++i) {
                f64 u = ulps_off(ov[i], os[i], mag[i]);
                worst_mul = u > worst_mul ? u : worst_mul;
            }

            /* rotate: unit quaternions. Magnitude of v + w t + u x t with t = 2 (u x v). */
            de_quat q = rnd_unit_quat();
            de_vec3 v = rnd_v3(-100, 100);
            de_vec3 rs = scalar()->quat_rotate(q, v), rv = t->quat_rotate(q, v);
            const f64 ux = fabs(q.x), uy = fabs(q.y), uz = fabs(q.z), w = fabs(q.w);
            const f64 vx = fabs(v.x), vy = fabs(v.y), vz = fabs(v.z);
            const f64 tx = 2 * (uy * vz + uz * vy), ty = 2 * (uz * vx + ux * vz),
                      tz = 2 * (ux * vy + uy * vx);
            const f64 rmag[3] = {vx + w * tx + uy * tz + uz * ty, vy + w * ty + uz * tx + ux * tz,
                                 vz + w * tz + ux * ty + uy * tx};
            const f32 rso[3] = {rs.x, rs.y, rs.z}, rvo[3] = {rv.x, rv.y, rv.z};
            for (int i = 0; i < 3; ++i) {
                f64 u = ulps_off(rvo[i], rso[i], rmag[i]);
                worst_rot = u > worst_rot ? u : worst_rot;
            }
        }
        report("quat_mul", t, worst_mul);
        report("quat_rotate", t, worst_rot);
        EXPECT_TRUE(worst_mul <= MAX_ULPS);
        EXPECT_TRUE(worst_rot <= MAX_ULPS);
    }
}

/* ---- de_affine ---- */

static de_affine rnd_affine(void) {
    de_affine a;
    for (int i = 0; i < 9; ++i)
        a.m[i] = rnd(-4, 4);
    for (int i = 9; i < 12; ++i)
        a.m[i] = rnd(-1000, 1000);
    return a;
}

static de_aabb rnd_box(f32 lo, f32 hi, f32 max_size) {
    de_vec3 c = rnd_v3(lo, hi);
    de_vec3 e = rnd_v3(0.0f, max_size);
    de_aabb b = {de_v3_sub(c, e), de_v3_add(c, e)};
    return b;
}

/* Worst ulp error of a transformed box against the scalar one. Each output coordinate is
 * c_i +- e_i, with c_i = sum L_ij c_j + t_i and e_i = sum |L_ij| e_j. */
static f64 aabb_ulps(const de_affine *a, de_aabb in, de_aabb rs, de_aabb rv) {
    const f32 cin[3] = {(in.min.x + in.max.x) * 0.5f, (in.min.y + in.max.y) * 0.5f,
                        (in.min.z + in.max.z) * 0.5f};
    const f32 ein[3] = {(in.max.x - in.min.x) * 0.5f, (in.max.y - in.min.y) * 0.5f,
                        (in.max.z - in.min.z) * 0.5f};
    const f32 s[6] = {rs.min.x, rs.min.y, rs.min.z, rs.max.x, rs.max.y, rs.max.z};
    const f32 v[6] = {rv.min.x, rv.min.y, rv.min.z, rv.max.x, rv.max.y, rv.max.z};
    f64 worst = 0.0;
    for (int i = 0; i < 3; ++i) {
        f64 mag = fabs((f64)a->m[9 + i]);
        for (int j = 0; j < 3; ++j)
            mag += fabs((f64)a->m[j * 3 + i]) * (fabs((f64)cin[j]) + fabs((f64)ein[j]));
        for (int side = 0; side < 2; ++side) {
            f64 u = ulps_off(v[side * 3 + i], s[side * 3 + i], mag);
            worst = u > worst ? u : worst;
        }
    }
    return worst;
}

TEST(simd, affine_vs_scalar) {
    int nt = simd_tables();
    for (int k = 0; k < nt; ++k) {
        const de__math_table *t = g_isas[k];
        f64 worst_mul = 0.0, worst_box = 0.0, worst_pts = 0.0;
        g_rng = 0xA11CEu;
        for (int it = 0; it < N_RANDOM; ++it) {
            de_affine a = rnd_affine(), b = rnd_affine();
            de_affine rs = scalar()->affine_mul(&a, &b), rv = t->affine_mul(&a, &b);
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 3; ++r) {
                    f64 mag = c == 3 ? fabs((f64)a.m[9 + r]) : 0.0;
                    for (int i = 0; i < 3; ++i)
                        mag += fabs((f64)a.m[i * 3 + r] * b.m[c * 3 + i]);
                    f64 u = ulps_off(rv.m[c * 3 + r], rs.m[c * 3 + r], mag);
                    worst_mul = u > worst_mul ? u : worst_mul;
                }
            }

            de_aabb box = rnd_box(-500, 500, 50);
            de_aabb bs, bv;
            scalar()->affine_transform_aabbs(&a, &box, &bs, 1);
            t->affine_transform_aabbs(&a, &box, &bv, 1);
            f64 u = aabb_ulps(&a, box, bs, bv);
            worst_box = u > worst_box ? u : worst_box;

            de_mat4 m;
            rnd_m4(&m, -10, 10);
            de_vec3 p = rnd_v3(-100, 100);
            de_vec4 ps, pv;
            scalar()->m4_transform_points(&m, &p, &ps, 1);
            t->m4_transform_points(&m, &p, &pv, 1);
            const f32 os[4] = {ps.x, ps.y, ps.z, ps.w}, ov[4] = {pv.x, pv.y, pv.z, pv.w};
            for (int r = 0; r < 4; ++r) {
                f64 mag = fabs((f64)m.m[r] * p.x) + fabs((f64)m.m[4 + r] * p.y) +
                          fabs((f64)m.m[8 + r] * p.z) + fabs((f64)m.m[12 + r]);
                f64 e = ulps_off(ov[r], os[r], mag);
                worst_pts = e > worst_pts ? e : worst_pts;
            }
        }
        report("affine_mul", t, worst_mul);
        report("affine_transform_aabbs", t, worst_box);
        report("m4_transform_points", t, worst_pts);
        EXPECT_TRUE(worst_mul <= MAX_ULPS);
        EXPECT_TRUE(worst_box <= MAX_ULPS);
        EXPECT_TRUE(worst_pts <= MAX_ULPS);

        /* An empty box stays exactly the same. */
        de_aabb empty = de_aabb_empty(), out;
        de_affine a = rnd_affine();
        t->affine_transform_aabbs(&a, &empty, &out, 1);
        EXPECT_MEMEQ(&out, &empty, sizeof empty);
    }
}

/* ---- de_frustum ---- */

static de_frustum rnd_frustum(int kind) {
    de_vec3 eye = rnd_v3(-50, 50);
    de_vec3 target = de_v3_add(eye, rnd_v3(-10, 10));
    de_mat4 view = de_m4_look_at(eye, target, DE_AXIS_UP);
    de_mat4 proj = kind == 0 ? de_m4_perspective_rz(rnd(0.5f, 2.0f), rnd(0.5f, 2.5f), 0.1f)
                             : de_m4_ortho_rz(-20, 20, -10, 10, 0.5f, 200.0f);
    de_mat4 vp = de__m4_mul_scalar(&proj, &view);
    return de_frustum_from_m4(&vp);
}

/* Boxes of every kind: random, empty, huge, non-finite, and boxes whose p-vertex lies on a plane
 * (distance within rounding of 0), where a different rounding would flip the result. */
static de_aabb rnd_cull_box(const de_frustum *f) {
    u32 kind = rnd_u32() % 16;
    if (kind == 0) {
        de_aabb b = rnd_box(-60, 60, 5);
        f32 t = b.min.x; /* empty on x */
        b.min.x = b.max.x;
        b.max.x = t - 1.0f;
        return b;
    }
    if (kind == 1) {
        de_aabb b = rnd_box(-1e6f, 1e6f, 1e5f);
        return b;
    }
    if (kind == 2) {
        de_aabb b = rnd_box(-60, 60, 5);
        u32 which = rnd_u32() % 3;
        if (which == 0)
            b.max.y = INFINITY;
        else if (which == 1)
            b.min.z = -INFINITY;
        else
            b.max.x = NAN;
        return b;
    }
    if (kind < 9) {
        /* Put the p-vertex of plane p on the plane. */
        const de_plane *p = &f->planes[rnd_u32() % 6];
        de_vec3 q = rnd_v3(-60, 60);
        f32 d = de_plane_distance(*p, q);
        q = de_v3_sub(q, de_v3_scale(p->n, d));
        de_vec3 e = rnd_v3(0.0f, 4.0f);
        de_aabb b;
        b.max = q;
        b.min = q;
        /* q is the corner farthest along n: max where n >= 0, min where n < 0. */
        if (p->n.x >= 0.0f)
            b.min.x = q.x - e.x;
        else
            b.max.x = q.x + e.x;
        if (p->n.y >= 0.0f)
            b.min.y = q.y - e.y;
        else
            b.max.y = q.y + e.y;
        if (p->n.z >= 0.0f)
            b.min.z = q.z - e.z;
        else
            b.max.z = q.z + e.z;
        return b;
    }
    return rnd_box(-60, 60, 8);
}

TEST(simd, frustum_cull_bitwise) {
    enum { N = 4096 };
    static de_aabb boxes[N];
    static u8 vis_s[N], vis_v[N];
    int nt = simd_tables();
    g_rng = 0xF00Du;
    u32 total = 0, visible = 0, mismatches = 0;
    for (int round = 0; round < N_RANDOM / N + 1; ++round) {
        de_frustum f = rnd_frustum(round & 1);
        for (int i = 0; i < N; ++i)
            boxes[i] = rnd_cull_box(&f);
        de__frustum_cull_aabbs_scalar(&f, boxes, N, vis_s);
        for (int i = 0; i < N; ++i)
            visible += vis_s[i];
        total += N;
        for (int k = 0; k < nt; ++k) {
            const de__math_table *t = g_isas[k];
            memset(vis_v, 0xAA, sizeof vis_v);
            t->frustum_cull_aabbs(&f, boxes, N, vis_v);
            for (int i = 0; i < N; ++i) {
                u8 single = t->frustum_test_aabb(&f, boxes[i]) ? 1 : 0;
                if (vis_v[i] != vis_s[i] || single != vis_s[i])
                    ++mismatches;
            }
        }
        /* The public entry points agree too, whatever the dispatcher picked. */
        de_frustum_cull_aabbs(&f, boxes, N, vis_v);
        EXPECT_MEMEQ(vis_v, vis_s, sizeof vis_s);
        for (int i = 0; i < 64; ++i)
            EXPECT_EQ(de_frustum_test_aabb(&f, boxes[i]) ? 1 : 0, vis_s[i]);
    }
    printf("  frustum_cull: %u boxes, %u visible, %u mismatches over %d ISA(s)\n", total, visible,
           mismatches, nt);
    EXPECT_EQ(mismatches, 0u);
    /* The set is not trivial: some boxes kept, some culled. */
    EXPECT_GT(visible, 0u);
    EXPECT_LT(visible, total);
}

/* ---- Batch tails ---- */

TEST(simd, batch_tails) {
    enum { MAXN = 1023, GUARD = 8 };
    static de_vec3 pts[MAXN];
    static de_vec4 out_s[MAXN + GUARD], out_v[MAXN + GUARD];
    static de_aabb boxes[MAXN], abox_s[MAXN + GUARD], abox_v[MAXN + GUARD], inplace[MAXN];
    static u8 vis_s[MAXN + GUARD], vis_v[MAXN + GUARD];
    g_rng = 0x7A115u;
    de_mat4 m;
    rnd_m4(&m, -3, 3);
    de_affine a = rnd_affine();
    de_frustum f = rnd_frustum(0);
    for (int i = 0; i < MAXN; ++i) {
        pts[i] = rnd_v3(-100, 100);
        boxes[i] = rnd_cull_box(&f);
    }
    /* Tables to check: every SIMD ISA, then the public (dispatched) API as table NULL. */
    const de__math_table *tabs[DE_MATH_ISA_COUNT + 1];
    int nt = simd_tables();
    for (int k = 0; k < nt; ++k)
        tabs[k] = g_isas[k];
    tabs[nt] = NULL;
    static const u32 sizes[] = {0, 1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 1023};
    for (int k = 0; k <= nt; ++k) {
        const de__math_table *t = tabs[k];
        for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; ++si) {
            u32 n = sizes[si];
            memset(out_s, 0x5A, sizeof out_s);
            memset(out_v, 0x5A, sizeof out_v);
            memset(abox_s, 0x5A, sizeof abox_s);
            memset(abox_v, 0x5A, sizeof abox_v);
            memset(vis_s, 0x5A, sizeof vis_s);
            memset(vis_v, 0x5A, sizeof vis_v);
            de__m4_transform_points_scalar(&m, pts, out_s, n);
            de__affine_transform_aabbs_scalar(&a, boxes, abox_s, n);
            de__frustum_cull_aabbs_scalar(&f, boxes, n, vis_s);
            if (t != NULL) {
                if (n > 0) {
                    t->m4_transform_points(&m, pts, out_v, n);
                    t->affine_transform_aabbs(&a, boxes, abox_v, n);
                    t->frustum_cull_aabbs(&f, boxes, n, vis_v);
                }
            } else {
                de_m4_transform_points(&m, pts, out_v, n);
                de_affine_transform_aabbs(&a, boxes, abox_v, n);
                de_frustum_cull_aabbs(&f, boxes, n, vis_v);
            }
            /* Culling is exact, including the bytes past n (untouched). */
            EXPECT_MEMEQ(vis_v, vis_s, sizeof vis_s);
            /* Nothing written past n. */
            EXPECT_MEMEQ(&out_v[n], &out_s[n], GUARD * sizeof out_s[0]);
            EXPECT_MEMEQ(&abox_v[n], &abox_s[n], GUARD * sizeof abox_s[0]);
            for (u32 i = 0; i < n; ++i) {
                const f32 os[4] = {out_s[i].x, out_s[i].y, out_s[i].z, out_s[i].w};
                const f32 ov[4] = {out_v[i].x, out_v[i].y, out_v[i].z, out_v[i].w};
                for (int r = 0; r < 4; ++r) {
                    f64 mag = fabs((f64)m.m[r] * pts[i].x) + fabs((f64)m.m[4 + r] * pts[i].y) +
                              fabs((f64)m.m[8 + r] * pts[i].z) + fabs((f64)m.m[12 + r]);
                    EXPECT_TRUE(ulps_off(ov[r], os[r], mag) <= MAX_ULPS);
                }
                if (de_aabb_is_empty(boxes[i]) ||
                    !isfinite(boxes[i].min.x + boxes[i].min.y + boxes[i].min.z + boxes[i].max.x +
                              boxes[i].max.y + boxes[i].max.z)) {
                    /* Empty boxes are copied; non-finite ones only need to agree on NaN-ness. */
                    if (de_aabb_is_empty(boxes[i]))
                        EXPECT_MEMEQ(&abox_v[i], &abox_s[i], sizeof abox_s[i]);
                    continue;
                }
                EXPECT_TRUE(aabb_ulps(&a, boxes[i], abox_s[i], abox_v[i]) <= MAX_ULPS);
            }
            /* In place (out == in) gives the same boxes. */
            if (n > 0) {
                memcpy(inplace, boxes, n * sizeof boxes[0]);
                if (t != NULL)
                    t->affine_transform_aabbs(&a, inplace, inplace, n);
                else
                    de_affine_transform_aabbs(&a, inplace, inplace, n);
                EXPECT_MEMEQ(inplace, abox_v, n * sizeof inplace[0]);
            }
        }
    }
    /* n = 0 touches nothing, not even the pointers. */
    de_m4_transform_points(&m, NULL, NULL, 0);
    de_affine_transform_aabbs(&a, NULL, NULL, 0);
    de_frustum_cull_aabbs(&f, NULL, 0, NULL);
}

/* ---- Dispatch ---- */

TEST(simd, dispatch_reports_isa) {
    de_math_isa isa = de_math_init();
    EXPECT_EQ(isa, de_math_active_isa());
    EXPECT_EQ(de_math_init(), isa); /* stable */
    const de__math_table *active = de__math_active_table();
    ASSERT_TRUE(active != NULL);
    EXPECT_EQ(active->isa, isa);
    EXPECT_TRUE(de__math_table_for(isa) == active);

    EXPECT_STREQ(de_math_isa_name(DE_MATH_ISA_SCALAR), "scalar");
    EXPECT_STREQ(de_math_isa_name(DE_MATH_ISA_SSE42), "sse4.2");
    EXPECT_STREQ(de_math_isa_name(DE_MATH_ISA_AVX2), "avx2");
    EXPECT_STREQ(de_math_isa_name(DE_MATH_ISA_NEON), "neon");
    EXPECT_STREQ(de_math_isa_name(DE_MATH_ISA_COUNT), "unknown");
    EXPECT_TRUE(de__math_table_for(DE_MATH_ISA_SCALAR) == &de__math_table_scalar);
    EXPECT_TRUE(de__math_table_for(DE_MATH_ISA_COUNT) == NULL);
    for (int i = 0; i < DE_MATH_ISA_COUNT; ++i) {
        const de__math_table *t = de__math_table_for((de_math_isa)i);
        if (t != NULL)
            EXPECT_EQ(t->isa, (de_math_isa)i);
    }

    /* What the dispatcher should have picked. */
    const char *force = getenv("DE_MATH_FORCE_SCALAR");
    const char *want = getenv("DE_MATH_ISA");
    printf("  active ISA: %s (DE_MATH_FORCE_SCALAR=%s, DE_MATH_ISA=%s)\n", de_math_isa_name(isa),
           force ? force : "", want ? want : "");
    if (force != NULL && force[0] != '\0' && strcmp(force, "0") != 0) {
        EXPECT_EQ(isa, DE_MATH_ISA_SCALAR);
    } else if (want != NULL && strcmp(want, "sse4.2") == 0 &&
               de__math_table_for(DE_MATH_ISA_SSE42) != NULL) {
        EXPECT_EQ(isa, DE_MATH_ISA_SSE42);
    } else if (want == NULL) {
#if defined(__aarch64__) || defined(_M_ARM64)
        EXPECT_EQ(isa, DE_MATH_ISA_NEON);
#elif defined(__x86_64__) || defined(_M_X64)
        de_math_isa best = de__math_table_for(DE_MATH_ISA_AVX2)    ? DE_MATH_ISA_AVX2
                           : de__math_table_for(DE_MATH_ISA_SSE42) ? DE_MATH_ISA_SSE42
                                                                   : DE_MATH_ISA_SCALAR;
        EXPECT_EQ(isa, best);
#endif
    }

    /* The public functions are the active table's, bit for bit. */
    g_rng = 0xD15Cu;
    de_mat4 a, b;
    rnd_m4(&a, -5, 5);
    rnd_m4(&b, -5, 5);
    de_mat4 p1 = de_m4_mul(&a, &b), p2 = active->m4_mul(&a, &b);
    EXPECT_MEMEQ(p1.m, p2.m, sizeof p1.m);
    de_quat qa = rnd_unit_quat(), qb = rnd_unit_quat();
    de_quat q1 = de_quat_mul(qa, qb), q2 = active->quat_mul(qa, qb);
    EXPECT_MEMEQ(&q1, &q2, sizeof q1);
    de_vec3 v = rnd_v3(-10, 10);
    de_vec3 r1 = de_quat_rotate(qa, v), r2 = active->quat_rotate(qa, v);
    EXPECT_MEMEQ(&r1, &r2, sizeof r1);
}
