/* bench_math.c: fast-math SIMD paths against the scalar reference (DENG-242).
 *
 * Times each dispatched function on every ISA available here (de__math_table_for) and prints
 * ns per operation and the speedup over scalar. Every ISA is called through its table, the same
 * indirect call the public functions make, so the comparison is fair.
 *
 *   bench_math           full run
 *   bench_math --quick   few iterations (the CTest entry `perf.math`)
 *
 * Exit code 1 when an ISA's batch cull differs from the scalar one; timings never fail. */
#include "core/math/math_simd.h"
#include <dengine/de_math.h>
#include <dengine/de_platform.h>

#include <stdio.h>
#include <string.h>

enum { N_MAT = 1024, N_BOX = 16384, N_PTS = 16384 };

static de_mat4 g_a[N_MAT], g_b[N_MAT], g_out[N_MAT];
static de_quat g_qa[N_MAT], g_qb[N_MAT];
static de_vec3 g_v[N_PTS];
static de_vec4 g_v4[N_PTS];
static de_affine g_aff[N_MAT], g_aff_out[N_MAT];
static de_aabb g_boxes[N_BOX], g_boxes_out[N_BOX];
static u8 g_vis[N_BOX], g_vis_ref[N_BOX];
static volatile f32 g_sink;

static u32 g_rng = 0x51ED270Bu;
static f32 rnd(f32 lo, f32 hi) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return lo + (hi - lo) * (f32)(g_rng >> 8) * (1.0f / 16777216.0f);
}
static de_vec3 rnd_v3(f32 lo, f32 hi) {
    de_vec3 v = {rnd(lo, hi), rnd(lo, hi), rnd(lo, hi)};
    return v;
}

typedef void (*bench_fn)(const de__math_table *t);

static void b_m4_mul(const de__math_table *t) {
    for (int i = 0; i < N_MAT; ++i)
        g_out[i] = t->m4_mul(&g_a[i], &g_b[i]);
}
static void b_m4_inverse(const de__math_table *t) {
    for (int i = 0; i < N_MAT; ++i)
        g_out[i] = t->m4_inverse(&g_a[i]);
}
static void b_m4_mul_v4(const de__math_table *t) {
    f32 s = 0.0f;
    for (int i = 0; i < N_MAT; ++i)
        s += t->m4_mul_v4(&g_a[i], g_v4[i]).x;
    g_sink = s;
}
static void b_quat_mul(const de__math_table *t) {
    f32 s = 0.0f;
    for (int i = 0; i < N_MAT; ++i)
        s += t->quat_mul(g_qa[i], g_qb[i]).w;
    g_sink = s;
}
static void b_quat_rotate(const de__math_table *t) {
    f32 s = 0.0f;
    for (int i = 0; i < N_MAT; ++i)
        s += t->quat_rotate(g_qa[i], g_v[i]).z;
    g_sink = s;
}
static void b_affine_mul(const de__math_table *t) {
    for (int i = 0; i < N_MAT - 1; ++i)
        g_aff_out[i] = t->affine_mul(&g_aff[i], &g_aff[i + 1]);
}
static de_frustum g_frustum;
static void b_frustum_test(const de__math_table *t) {
    u32 n = 0;
    for (int i = 0; i < N_BOX; ++i)
        n += t->frustum_test_aabb(&g_frustum, g_boxes[i]) ? 1u : 0u;
    g_sink = (f32)n;
}
static void b_transform_points(const de__math_table *t) {
    t->m4_transform_points(&g_a[0], g_v, g_v4, N_PTS);
}
static void b_transform_aabbs(const de__math_table *t) {
    t->affine_transform_aabbs(&g_aff[0], g_boxes, g_boxes_out, N_BOX);
}
static void b_cull(const de__math_table *t) {
    t->frustum_cull_aabbs(&g_frustum, g_boxes, N_BOX, g_vis);
}

typedef struct bench {
    const char *name;
    bench_fn fn;
    u32 ops; /* operations per call of fn */
} bench;

static const bench g_benches[] = {
    {"de_m4_mul", b_m4_mul, N_MAT},
    {"de_m4_inverse", b_m4_inverse, N_MAT},
    {"de_m4_mul_v4", b_m4_mul_v4, N_MAT},
    {"de_quat_mul", b_quat_mul, N_MAT},
    {"de_quat_rotate", b_quat_rotate, N_MAT},
    {"de_affine_mul", b_affine_mul, N_MAT - 1},
    {"de_frustum_test_aabb", b_frustum_test, N_BOX},
    {"de_m4_transform_points", b_transform_points, N_PTS},
    {"de_affine_transform_aabbs", b_transform_aabbs, N_BOX},
    {"de_frustum_cull_aabbs", b_cull, N_BOX},
};

/* Best (lowest) ns per operation over `reps` timed runs of `iters` calls each. */
static f64 time_ns_per_op(const bench *b, const de__math_table *t, int iters, int reps) {
    b->fn(t); /* warm up */
    f64 best = 1e30;
    for (int r = 0; r < reps; ++r) {
        u64 t0 = de_time_now_ns();
        for (int i = 0; i < iters; ++i)
            b->fn(t);
        u64 t1 = de_time_now_ns();
        f64 ns = (f64)(t1 - t0) / ((f64)iters * b->ops);
        best = ns < best ? ns : best;
    }
    return best;
}

int main(int argc, char **argv) {
    bool quick = argc > 1 && strcmp(argv[1], "--quick") == 0;
    int iters = quick ? 2 : 200, reps = quick ? 1 : 7;

    for (int i = 0; i < N_MAT; ++i) {
        for (int k = 0; k < 16; ++k) {
            g_a[i].m[k] = rnd(-2, 2);
            g_b[i].m[k] = rnd(-2, 2);
        }
        for (int k = 0; k < 4; ++k)
            g_a[i].m[k * 5] += 4.0f; /* well conditioned for the inverse */
        g_qa[i] = de_quat_axis_angle(de_v3_normalize_safe(rnd_v3(-1, 1), DE_AXIS_UP), rnd(-3, 3));
        g_qb[i] = de_quat_axis_angle(de_v3_normalize_safe(rnd_v3(-1, 1), DE_AXIS_UP), rnd(-3, 3));
        g_aff[i] = de_affine_from_trs(rnd_v3(-100, 100), g_qa[i], rnd_v3(0.5f, 2.0f));
    }
    for (int i = 0; i < N_PTS; ++i) {
        g_v[i] = rnd_v3(-100, 100);
        g_v4[i].x = g_v[i].x;
        g_v4[i].y = g_v[i].y;
        g_v4[i].z = g_v[i].z;
        g_v4[i].w = 1.0f;
    }
    /* A camera in a field of boxes: roughly a fifth are visible, the usual culling load. */
    de_mat4 view = de_m4_look_at(de_v3_add(DE_AXIS_UP, DE_AXIS_UP), DE_AXIS_FORWARD, DE_AXIS_UP);
    de_mat4 proj = de_m4_perspective_rz(1.2f, 16.0f / 9.0f, 0.1f);
    de_mat4 vp = de__m4_mul_scalar(&proj, &view);
    g_frustum = de_frustum_from_m4(&vp);
    for (int i = 0; i < N_BOX; ++i) {
        de_vec3 c = rnd_v3(-200, 200), e = rnd_v3(0.1f, 3.0f);
        g_boxes[i].min = de_v3_sub(c, e);
        g_boxes[i].max = de_v3_add(c, e);
    }

    const de__math_table *tables[DE_MATH_ISA_COUNT];
    int nt = 0;
    for (int i = 0; i < DE_MATH_ISA_COUNT; ++i) {
        const de__math_table *t = de__math_table_for((de_math_isa)i);
        if (t != NULL)
            tables[nt++] = t;
    }

    int rc = 0;
    de__frustum_cull_aabbs_scalar(&g_frustum, g_boxes, N_BOX, g_vis_ref);
    u32 visible = 0;
    for (int i = 0; i < N_BOX; ++i)
        visible += g_vis_ref[i];
    for (int k = 1; k < nt; ++k) {
        memset(g_vis, 0xAA, sizeof g_vis);
        tables[k]->frustum_cull_aabbs(&g_frustum, g_boxes, N_BOX, g_vis);
        if (memcmp(g_vis, g_vis_ref, sizeof g_vis) != 0) {
            fprintf(stderr, "bench_math: %s cull differs from scalar\n",
                    de_math_isa_name(tables[k]->isa));
            rc = 1;
        }
    }

    printf("bench_math%s: dispatcher picks %s; %u of %u boxes visible\n", quick ? " --quick" : "",
           de_math_isa_name(de_math_init()), visible, (u32)N_BOX);
    printf("%-26s", "ns/op (speedup)");
    for (int k = 0; k < nt; ++k)
        printf(" %18s", de_math_isa_name(tables[k]->isa));
    printf("\n");
    for (size_t b = 0; b < sizeof g_benches / sizeof g_benches[0]; ++b) {
        printf("%-26s", g_benches[b].name);
        f64 base = 0.0;
        for (int k = 0; k < nt; ++k) {
            f64 ns = time_ns_per_op(&g_benches[b], tables[k], iters, reps);
            if (k == 0) {
                base = ns;
                printf(" %10.2f        ", ns);
            } else {
                printf(" %10.2f (%4.2fx)", ns, base / ns);
            }
        }
        printf("\n");
    }
    return rc;
}
