/* test_dmath.c: deterministic math de_d* (DENG-243, SPEC §5.4, §5.7).
 *
 * dmath.golden_bitwise compares every function against tests/unit/math/vectors/dmath_golden.txt,
 * bit for bit. That file is the cross-platform contract: x86-64 and arm64 must reproduce it
 * without regenerating it. A mismatch is a determinism bug, never a reason to regenerate. Only a
 * deliberate change of the algorithms (with a reason in the commit message) rewrites it:
 *     DE_REGEN_GOLDEN=1 build/dev/tests/unit/math/test_dmath --filter=dmath.golden_bitwise
 * (The test runner takes no extra arguments, so the generator mode is an environment variable.)
 *
 * File format: a section per function, "@ <name> <inputs> <outputs> <bits> <count>", then
 * <count> lines of hex bit patterns, the inputs then the outputs. <bits> is 32 (f32) or 64 (f64).
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv, fopen, sscanf */
#endif
#include "de_test.h"
#include "fpenv_util.h"

#include <dengine/de_math.h>

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_DMATH_GOLDEN
#error "DE_DMATH_GOLDEN (path of dmath_golden.txt) must be defined by the build"
#endif

#define GOLDEN_COUNT        4096 /* per function (issue: >= 4096 inputs) */
#define GOLDEN_COUNT_HELPER 1024 /* vector and quaternion helpers */

static u32 b32(f32 x) {
    u32 u;
    memcpy(&u, &x, sizeof u);
    return u;
}
static f32 f32b(u32 u) {
    f32 x;
    memcpy(&x, &u, sizeof x);
    return x;
}
static u64 b64(f64 x) {
    u64 u;
    memcpy(&u, &x, sizeof u);
    return u;
}
static f64 f64b(u64 u) {
    f64 x;
    memcpy(&x, &u, sizeof x);
    return x;
}

/* ---- Input generation (only used to write the golden file and for the accuracy sweep) ---- */

typedef struct gen {
    u64 s;
} gen;

static u64 gnext(gen *g) { /* splitmix64: independent of de_rng on purpose */
    u64 z = (g->s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static f64 gunit(gen *g) {
    return (f64)(gnext(g) >> 11) * 0x1p-53;
}
static f64 guniform(gen *g, f64 lo, f64 hi) {
    return lo + (hi - lo) * gunit(g);
}
/* f32 with a biased exponent uniform in [emin + 127, emax + 127], random mantissa. */
static u32 gexp32(gen *g, int emin, int emax, bool any_sign) {
    u64 r = gnext(g);
    u32 e = (u32)(emin + 127 + (int)(r % (u64)(emax - emin + 1)));
    u32 m = (u32)(r >> 20) & 0x007FFFFFu;
    u32 s = (any_sign && ((r >> 63) != 0)) ? 0x80000000u : 0u;
    return s | (e << 23) | m;
}
static u64 gexp64(gen *g, int emin, int emax, bool any_sign) {
    u64 r = gnext(g);
    u64 e = (u64)(emin + 1023 + (int)(r % (u64)(emax - emin + 1)));
    u64 m = gnext(g) & 0x000FFFFFFFFFFFFFull;
    u64 s = (any_sign && ((r >> 63) != 0)) ? 0x8000000000000000ull : 0u;
    return s | (e << 52) | m;
}
static u32 gu32f(gen *g, f64 lo, f64 hi) {
    return b32((f32)guniform(g, lo, hi));
}

/* Special f32 inputs every function sees. */
static const u32 SPEC32[] = {
    0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x00400000u, 0x00800000u, 0x80800000u,
    0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0x7FFFFFFFu, 0x7F7FFFFFu,
    0xFF7FFFFFu, 0x3F800000u, 0xBF800000u, 0x3F000000u, 0xBF000000u, 0x40000000u, 0x40490FDBu,
};
#define NSPEC32 ((int)(sizeof SPEC32 / sizeof SPEC32[0]))

static const u64 SPEC64[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull, 0x800FFFFFFFFFFFFFull,
    0x0010000000000000ull, 0x8010000000000000ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
    0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x7FF0000000000001ull, 0x7FEFFFFFFFFFFFFFull,
    0xFFEFFFFFFFFFFFFFull, 0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x400921FB54442D18ull,
};
#define NSPEC64 ((int)(sizeof SPEC64 / sizeof SPEC64[0]))

/* Edge list helper: inputs [0, n) come from `list`; returns true when i was an edge. */
static bool from_list32(int *i, const u32 *list, int n, u64 *in) {
    if (*i < n) {
        in[0] = list[*i];
        return true;
    }
    *i -= n;
    return false;
}

static void gen_trig(gen *g, int i, u64 *in) {
    static const u32 extra[] = {0x3F490FDBu, 0x3F490FDCu, 0x3F490FDAu, 0xBF490FDBu, 0x32000000u,
                                0x31FFFFFFu, 0x39800000u, 0x47C35000u, 0x49C90FDBu, 0x49C90FDAu,
                                0x49C90FDCu, 0x4B800000u, 0x501502F9u, 0x60AD78ECu, 0x7149F2CAu,
                                0x7E967699u, 0x5A8C1000u, 0x4E6BB4A7u};
    if (from_list32(&i, SPEC32, NSPEC32, in) || from_list32(&i, extra, 18, in)) {
        return;
    }
    if (i < 81 * 3) { /* the f32 values nearest to k * pi/2, k in [-40, 40], and +-1 ulp */
        int k = i / 3 - 40, d = i % 3 - 1;
        u32 u = b32((f32)(k * 1.57079632679489661923));
        in[0] = (k == 0) ? (u32)(d == 0 ? 0u : (d > 0 ? 1u : 0x80000001u)) : (u32)((i32)u + d);
        return;
    }
    u64 r = gnext(g) % 4;
    if (r < 2) {
        in[0] = gu32f(g, -8 * 3.14159265358979, 8 * 3.14159265358979);
    } else if (r == 2) {
        in[0] = gexp32(g, -30, 30, true);
    } else {
        in[0] = gexp32(g, -126, 127, true);
    }
}

static void gen_asin(gen *g, int i, u64 *in) {
    static const u32 extra[] = {0x3F7FFFFFu, 0xBF7FFFFFu, 0x3F800001u, 0xBF800001u, 0x3F7FFFFEu,
                                0x3EFFFFFFu, 0x3F000001u, 0xBEFFFFFFu, 0x32000000u, 0x3F3504F3u};
    if (from_list32(&i, SPEC32, NSPEC32, in) || from_list32(&i, extra, 10, in)) {
        return;
    }
    u64 r = gnext(g) % 10;
    if (r < 6) {
        in[0] = gu32f(g, -1.0, 1.0);
    } else if (r < 9) {
        in[0] = gexp32(g, -126, -1, true);
    } else {
        in[0] = gu32f(g, -2.0, 2.0);
    }
}

static void gen_atan(gen *g, int i, u64 *in) {
    static const u32 extra[] = {0x3ED413CDu, 0x3ED413CCu, 0x3ED413CEu, 0xBED413CDu, 0x3F7FFFFFu,
                                0x3F800001u, 0x32000000u, 0x4B800000u, 0x5A800000u};
    if (from_list32(&i, SPEC32, NSPEC32, in) || from_list32(&i, extra, 9, in)) {
        return;
    }
    if (gnext(g) % 2) {
        in[0] = gexp32(g, -126, 127, true);
    } else {
        in[0] = gexp32(g, -20, 20, true);
    }
}

static const u32 GRID_ATAN2[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x3F800000u,
                                 0xBF800000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F7FFFFFu,
                                 0xFF7FFFFFu, 0x00800000u, 0x80800000u, 0x3F000000u, 0xC0400000u};
#define NGRID_ATAN2 15

static void gen_atan2(gen *g, int i, u64 *in) {
    if (i < NGRID_ATAN2 * NGRID_ATAN2) {
        in[0] = GRID_ATAN2[i / NGRID_ATAN2];
        in[1] = GRID_ATAN2[i % NGRID_ATAN2];
        return;
    }
    u64 r = gnext(g) % 10;
    if (r < 5) {
        in[0] = gexp32(g, -40, 40, true);
        in[1] = gexp32(g, -40, 40, true);
    } else if (r < 8) {
        in[0] = gu32f(g, -10, 10);
        in[1] = gu32f(g, -10, 10);
    } else {
        in[0] = gexp32(g, -126, 127, true);
        in[1] = gexp32(g, -126, 127, true);
    }
}

static void gen_exp(gen *g, int i, u64 *in) {
    static const u32 extra[] = {0x42B17217u, 0x42B17218u, 0x42B17216u, 0xC2AEAC4Fu, 0xC2AEAC50u,
                                0xC2AEAC4Eu, 0xC2CFF1B4u, 0xC2CFF1B5u, 0x42B20000u, 0xC2D00000u,
                                0x42B1FFFFu, 0xC2CFFFFFu, 0x33000000u, 0x3F317218u, 0xBF317218u,
                                0x40B17218u, 0xC2B00000u, 0xC2B17218u};
    if (from_list32(&i, SPEC32, NSPEC32, in) || from_list32(&i, extra, 18, in)) {
        return;
    }
    if (gnext(g) % 10 < 7) {
        in[0] = gu32f(g, -104.0, 89.0);
    } else {
        in[0] = gexp32(g, -40, 6, true);
    }
}

static void gen_log(gen *g, int i, u64 *in) {
    static const u32 extra[] = {0x3F7FFFFFu, 0x3F800001u, 0x3FB504F3u, 0x3FB504F4u, 0x3FB504F2u,
                                0x3F3504F3u, 0x3F3504F2u, 0x402DF854u, 0x40800000u, 0x3F7FFFFEu};
    if (from_list32(&i, SPEC32, NSPEC32, in) || from_list32(&i, extra, 10, in)) {
        return;
    }
    u64 r = gnext(g) % 10;
    if (r < 6) {
        in[0] = gexp32(g, -126, 127, false);
    } else if (r < 9) {
        in[0] = gu32f(g, 0.5, 2.0);
    } else {
        in[0] = gexp32(g, -126, 127, false) | 0x80000000u;
    }
}

static const u32 GRID_POW_X[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u,
                                 0x3F800000u, 0xBF800000u, 0x3F000000u, 0xBF000000u,
                                 0x40000000u, 0xC0000000u, 0xC0400000u, 0x7F800000u,
                                 0xFF800000u, 0x7FC00000u, 0x3F800001u, 0x3F7FFFFFu};
static const u32 GRID_POW_Y[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x3F800000u, 0xBF800000u,
                                 0x40000000u, 0xC0000000u, 0x40400000u, 0xC0400000u, 0x3F000000u,
                                 0xBF000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x501502F9u,
                                 0xD01502F9u, 0x42FE0000u, 0xC2FC0000u, 0x43000000u, 0xC3160000u,
                                 0x4B800001u, 0x4B7FFFFFu};
#define NGRID_POW_X 16
#define NGRID_POW_Y 22

static void gen_pow(gen *g, int i, u64 *in) {
    if (i < NGRID_POW_X * NGRID_POW_Y) {
        in[0] = GRID_POW_X[i / NGRID_POW_Y];
        in[1] = GRID_POW_Y[i % NGRID_POW_Y];
        return;
    }
    u64 r = gnext(g) % 20;
    if (r < 10) {
        in[0] = gexp32(g, -20, 20, false);
        in[1] = gu32f(g, -30, 30);
    } else if (r < 14) {
        in[0] = gu32f(g, 0.5, 2.0);
        in[1] = gu32f(g, -200, 200);
    } else if (r < 17) {
        in[0] = gu32f(g, -10, -0.01);
        in[1] = b32((f32)(i32)(gnext(g) % 41) - 20.0f);
    } else if (r < 18) {
        in[0] = gu32f(g, -10, -0.01);
        in[1] = gu32f(g, -5, 5);
    } else {
        in[0] = gexp32(g, -126, 127, true);
        in[1] = gexp32(g, -126, 127, true);
    }
}

static void gen_sqrt(gen *g, int i, u64 *in) {
    static const u32 extra[] = {0x40800000u, 0x41100000u, 0x3E800000u, 0x3F7FFFFFu, 0x3F800001u};
    if (from_list32(&i, SPEC32, NSPEC32, in) || from_list32(&i, extra, 5, in)) {
        return;
    }
    in[0] = gexp32(g, -126, 127, gnext(g) % 5 == 0);
}

static void gen_round(gen *g, int i, u64 *in) {
    static const u32 extra[] = {0x3F000000u, 0xBF000000u, 0x3FC00000u, 0xBFC00000u, 0x40200000u,
                                0xC0200000u, 0x3EFFFFFFu, 0xBEFFFFFFu, 0x3F000001u, 0xBF000001u,
                                0x4AFFFFFFu, 0xCAFFFFFFu, 0x4B000000u, 0xCB000000u, 0x4A800001u,
                                0xCA800001u, 0x0DA24260u, 0x8DA24260u, 0x3F7FFFFFu, 0xBF7FFFFFu,
                                0x3ECCCCCDu, 0xBECCCCCDu, 0x4B7FFFFFu, 0x4AFFFFFEu};
    if (from_list32(&i, SPEC32, NSPEC32, in) || from_list32(&i, extra, 24, in)) {
        return;
    }
    u64 r = gnext(g) % 10;
    if (r < 4) {
        in[0] = gu32f(g, -1000, 1000);
    } else if (r < 6) {
        in[0] = b32((f32)((f64)(i32)(gnext(g) % 2001) - 1000.0 + 0.5));
    } else {
        in[0] = gexp32(g, -30, 30, true);
    }
}

static const u32 GRID_FMOD[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x3F800000u, 0xBF800000u,
                                0x3FC00000u, 0xC0200000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u,
                                0x7F7FFFFFu, 0x00800000u, 0x80800000u, 0x40400000u};
#define NGRID_FMOD 14

static void gen_fmod(gen *g, int i, u64 *in) {
    static const u32 extra[][2] = {{0x7F7FFFFFu, 0x3A83126Fu}, {0x7F7FFFFFu, 0x00C00000u},
                                   {0x00C00000u, 0x00800000u}, {0x40B00000u, 0x3F8CCCCDu},
                                   {0xC0E00000u, 0x40000000u}, {0x4B800001u, 0x40400000u}};
    if (i < NGRID_FMOD * NGRID_FMOD) {
        in[0] = GRID_FMOD[i / NGRID_FMOD];
        in[1] = GRID_FMOD[i % NGRID_FMOD];
        return;
    }
    i -= NGRID_FMOD * NGRID_FMOD;
    if (i < 6) {
        in[0] = extra[i][0];
        in[1] = extra[i][1];
        return;
    }
    u64 r = gnext(g) % 10;
    if (r < 6) {
        in[0] = gexp32(g, -20, 40, true);
        in[1] = gexp32(g, -20, 20, true);
    } else if (r < 8) {
        in[0] = gu32f(g, -100, 100);
        in[1] = gu32f(g, -10, 10);
    } else {
        in[0] = gexp32(g, -126, 127, true);
        in[1] = gexp32(g, -126, 127, true);
    }
}

static void gen_trig64(gen *g, int i, u64 *in) {
    static const u64 extra[] = {
        0x3E40000000000000ull, 0x3E3FFFFFFFFFFFFFull, 0x413921FB54442D18ull, 0x413921FB54442D17ull,
        0x413921FB54442D19ull, 0x7496AC5B262CA1FFull, /* x = 6381956970095103 * 2^797 */
        0x3FE921FB54442D18ull, 0x3FE921FB54442D19ull, 0x3FE921FB54442D17ull, 0x4C9C5BCF4F5DD7E0ull,
        0x47EFFFFFFFFFFFFFull, 0x43F0000000000000ull,
    };
    if (i < NSPEC64) {
        in[0] = SPEC64[i];
        return;
    }
    i -= NSPEC64;
    if (i < 12) {
        in[0] = extra[i];
        return;
    }
    i -= 12;
    if (i < 81 * 3) {
        int k = i / 3 - 40, d = i % 3 - 1;
        u64 u = b64(k * 1.57079632679489661923);
        in[0] = (k == 0) ? (d == 0 ? 0u : (d > 0 ? 1u : 0x8000000000000001ull)) : (u64)((i64)u + d);
        return;
    }
    u64 r = gnext(g) % 10;
    if (r < 4) {
        in[0] = b64(guniform(g, -10, 10));
    } else if (r < 7) {
        in[0] = gexp64(g, -40, 30, true);
    } else {
        in[0] = gexp64(g, -1022, 1023, true);
    }
}

static void gen_sqrt64(gen *g, int i, u64 *in) {
    if (i < NSPEC64) {
        in[0] = SPEC64[i];
        return;
    }
    in[0] = gexp64(g, -1022, 1023, gnext(g) % 5 == 0);
}

static void gen_vec(gen *g, int i, u64 *in, int n) {
    static const u32 edge[][4] = {
        {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
        {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u},
        {0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u},
        {0x3F800000u, 0x00000000u, 0x00000000u, 0x00000000u},
        {0x7F800000u, 0x00000000u, 0x00000000u, 0x00000000u},
        {0x7FC00000u, 0x3F800000u, 0x3F800000u, 0x3F800000u},
        {0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F7FFFFFu},
        {0x00800000u, 0x00000000u, 0x00000000u, 0x00000000u},
        {0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u},
        {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u},
    };
    if (i < 10) {
        for (int k = 0; k < n; k++) {
            in[k] = edge[i][k];
        }
        return;
    }
    bool wide = gnext(g) % 2;
    for (int k = 0; k < n; k++) {
        in[k] = wide ? gexp32(g, -60, 60, true) : gu32f(g, -10, 10);
    }
}
static void gen_v3n(gen *g, int i, u64 *in) {
    gen_vec(g, i, in, 3);
}
static void gen_qn(gen *g, int i, u64 *in) {
    gen_vec(g, i, in, 4);
}

static void gen_qi(gen *g, int i, u64 *in) {
    /* q (4), angular velocity (3), dt. */
    static const u32 edge[][8] = {
        {0, 0, 0, 0x3F800000u, 0, 0, 0, 0x3C888889u},
        {0, 0, 0, 0x3F800000u, 0x3F800000u, 0, 0, 0},
        {0, 0, 0, 0, 0x3F800000u, 0, 0, 0x3C888889u},
        {0, 0, 0, 0x3F800000u, 0x7F800000u, 0, 0, 0x3C888889u},
        {0, 0, 0, 0x3F800000u, 0x7FC00000u, 0, 0, 0x3C888889u},
        {0, 0, 0, 0x3F800000u, 0, 0, 0x40C90FDBu, 0x3F800000u},
        {0, 0, 0, 0x3F800000u, 0, 0, 0x40C90FDBu, 0x49742400u},
        {0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F800000u, 0x40000000u, 0x40400000u,
         0xBC888889u},
        {0, 0, 0, 0x3F800000u, 0x00000001u, 0, 0, 0x3C888889u},
        {0, 0, 0, 0x3F800000u, 0x2F800000u, 0, 0, 0x3C888889u},
    };
    if (i < 10) {
        for (int k = 0; k < 8; k++) {
            in[k] = edge[i][k];
        }
        return;
    }
    for (int k = 0; k < 4; k++) {
        in[k] = gu32f(g, -1, 1);
    }
    for (int k = 4; k < 7; k++) {
        in[k] = gu32f(g, -20, 20);
    }
    static const f32 dts[4] = {1.0f / 60.0f, 1.0f / 120.0f, 1.0f / 30.0f, 0.1f};
    u64 r = gnext(g) % 5;
    in[7] = r < 4 ? b32(dts[r]) : gu32f(g, 0, 1);
}

/* ---- Evaluation ---- */

#define U1(name, fn)                                                                               \
    static void name(const u64 *in, u64 *out) {                                                    \
        out[0] = b32(fn(f32b((u32)in[0])));                                                        \
    }
#define B1(name, fn)                                                                               \
    static void name(const u64 *in, u64 *out) {                                                    \
        out[0] = b32(fn(f32b((u32)in[0]), f32b((u32)in[1])));                                      \
    }
U1(ev_sin, de_dsin)
U1(ev_cos, de_dcos)
U1(ev_tan, de_dtan)
U1(ev_asin, de_dasin)
U1(ev_acos, de_dacos)
U1(ev_atan, de_datan)
U1(ev_exp, de_dexp)
U1(ev_log, de_dlog)
U1(ev_sqrt, de_dsqrt)
U1(ev_floor, de_dfloor)
U1(ev_ceil, de_dceil)
U1(ev_round, de_dround)
B1(ev_atan2, de_datan2)
B1(ev_pow, de_dpow)
B1(ev_fmod, de_dfmod)

static void ev_sincos(const u64 *in, u64 *out) {
    f32 s, c;
    de_dsincos(f32b((u32)in[0]), &s, &c);
    out[0] = b32(s);
    out[1] = b32(c);
}
static void ev_sin64(const u64 *in, u64 *out) {
    out[0] = b64(de_dsin64(f64b(in[0])));
}
static void ev_cos64(const u64 *in, u64 *out) {
    out[0] = b64(de_dcos64(f64b(in[0])));
}
static void ev_sqrt64(const u64 *in, u64 *out) {
    out[0] = b64(de_dsqrt64(f64b(in[0])));
}
static void ev_v3n(const u64 *in, u64 *out) {
    de_vec3 v = {f32b((u32)in[0]), f32b((u32)in[1]), f32b((u32)in[2])};
    de_vec3 r = de_dv3_normalize(v);
    out[0] = b32(r.x);
    out[1] = b32(r.y);
    out[2] = b32(r.z);
}
static de_quat quat_in(const u64 *in) {
    de_quat q;
    q.x = f32b((u32)in[0]);
    q.y = f32b((u32)in[1]);
    q.z = f32b((u32)in[2]);
    q.w = f32b((u32)in[3]);
    return q;
}
static void quat_out(de_quat q, u64 *out) {
    out[0] = b32(q.x);
    out[1] = b32(q.y);
    out[2] = b32(q.z);
    out[3] = b32(q.w);
}
static void ev_qn(const u64 *in, u64 *out) {
    quat_out(de_dquat_normalize(quat_in(in)), out);
}
static void ev_qi(const u64 *in, u64 *out) {
    de_vec3 w = {f32b((u32)in[4]), f32b((u32)in[5]), f32b((u32)in[6])};
    quat_out(de_dquat_integrate(quat_in(in), w, f32b((u32)in[7])), out);
}

typedef void (*eval_fn)(const u64 *in, u64 *out);
typedef void (*gen_fn)(gen *g, int i, u64 *in);

typedef struct golden_fn {
    const char *name;
    int nin, nout, bits, count;
    eval_fn eval;
    gen_fn gen;
} golden_fn;

static const golden_fn FNS[] = {
    {"sin", 1, 1, 32, GOLDEN_COUNT, ev_sin, gen_trig},
    {"cos", 1, 1, 32, GOLDEN_COUNT, ev_cos, gen_trig},
    {"sincos", 1, 2, 32, GOLDEN_COUNT, ev_sincos, gen_trig},
    {"tan", 1, 1, 32, GOLDEN_COUNT, ev_tan, gen_trig},
    {"asin", 1, 1, 32, GOLDEN_COUNT, ev_asin, gen_asin},
    {"acos", 1, 1, 32, GOLDEN_COUNT, ev_acos, gen_asin},
    {"atan", 1, 1, 32, GOLDEN_COUNT, ev_atan, gen_atan},
    {"atan2", 2, 1, 32, GOLDEN_COUNT, ev_atan2, gen_atan2},
    {"exp", 1, 1, 32, GOLDEN_COUNT, ev_exp, gen_exp},
    {"log", 1, 1, 32, GOLDEN_COUNT, ev_log, gen_log},
    {"pow", 2, 1, 32, GOLDEN_COUNT, ev_pow, gen_pow},
    {"sqrt", 1, 1, 32, GOLDEN_COUNT, ev_sqrt, gen_sqrt},
    {"floor", 1, 1, 32, GOLDEN_COUNT, ev_floor, gen_round},
    {"ceil", 1, 1, 32, GOLDEN_COUNT, ev_ceil, gen_round},
    {"round", 1, 1, 32, GOLDEN_COUNT, ev_round, gen_round},
    {"fmod", 2, 1, 32, GOLDEN_COUNT, ev_fmod, gen_fmod},
    {"sin64", 1, 1, 64, GOLDEN_COUNT, ev_sin64, gen_trig64},
    {"cos64", 1, 1, 64, GOLDEN_COUNT, ev_cos64, gen_trig64},
    {"sqrt64", 1, 1, 64, GOLDEN_COUNT, ev_sqrt64, gen_sqrt64},
    {"v3_normalize", 3, 3, 32, GOLDEN_COUNT_HELPER, ev_v3n, gen_v3n},
    {"quat_normalize", 4, 4, 32, GOLDEN_COUNT_HELPER, ev_qn, gen_qn},
    {"quat_integrate", 8, 4, 32, GOLDEN_COUNT_HELPER, ev_qi, gen_qi},
};
#define NFNS ((int)(sizeof FNS / sizeof FNS[0]))

static const golden_fn *find_fn(const char *name) {
    for (int i = 0; i < NFNS; i++) {
        if (strcmp(FNS[i].name, name) == 0) {
            return &FNS[i];
        }
    }
    return NULL;
}

/* The generator seeds per function, so adding a function does not change the others. */
static gen gen_for(const golden_fn *f) {
    gen g = {0x243DE243ull};
    for (const char *p = f->name; *p; p++) {
        g.s = g.s * 131u + (u8)*p;
    }
    return g;
}

static void regen_golden(void) {
    FILE *out = fopen(DE_DMATH_GOLDEN, "wb");
    ASSERT_TRUE(out != NULL);
    fprintf(out, "# dmath_golden.txt: de_d* reference results (DENG-243). Generated by test_dmath\n"
                 "# with DE_REGEN_GOLDEN=1; never edit by hand. x86-64 and arm64 must reproduce\n"
                 "# every line bit for bit. Section: @ <name> <inputs> <outputs> <bits> <count>,\n"
                 "# then one line per case: the inputs, then the outputs, as hex bit patterns.\n");
    for (int k = 0; k < NFNS; k++) {
        const golden_fn *f = &FNS[k];
        gen g = gen_for(f);
        fprintf(out, "@ %s %d %d %d %d\n", f->name, f->nin, f->nout, f->bits, f->count);
        for (int i = 0; i < f->count; i++) {
            u64 in[8] = {0}, res[8] = {0};
            f->gen(&g, i, in);
            f->eval(in, res);
            for (int j = 0; j < f->nin + f->nout; j++) {
                u64 v = j < f->nin ? in[j] : res[j - f->nin];
                if (f->bits == 32) {
                    fprintf(out, j ? " %08x" : "%08x", (unsigned)v);
                } else {
                    fprintf(out, j ? " %016llx" : "%016llx", (unsigned long long)v);
                }
            }
            fputc('\n', out);
        }
    }
    fclose(out);
}

/* Compares every line of the golden file; returns the number of mismatching results. */
static int check_golden(const char *mode) {
    FILE *in = fopen(DE_DMATH_GOLDEN, "rb");
    ASSERT_TRUE(in != NULL);
    char line[512];
    const golden_fn *f = NULL;
    int left = 0, bad = 0, sections = 0, lineno = 0;
    while (fgets(line, sizeof line, in)) {
        lineno++;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        if (line[0] == '@') {
            EXPECT_EQ(left, 0);
            char name[64];
            int nin, nout, bits, count;
            ASSERT_EQ(sscanf(line, "@ %63s %d %d %d %d", name, &nin, &nout, &bits, &count), 5);
            f = find_fn(name);
            if (!f) {
                printf("dmath_golden.txt:%d: unknown function '%s'\n", lineno, name);
                FAIL("unknown function in the golden file");
            }
            ASSERT_EQ(nin, f->nin);
            ASSERT_EQ(nout, f->nout);
            ASSERT_EQ(bits, f->bits);
            EXPECT_GE(count, f->count);
            left = count;
            sections++;
            continue;
        }
        ASSERT_TRUE(f != NULL);
        ASSERT_GT(left, 0);
        left--;
        u64 v[12] = {0};
        char *p = line;
        for (int j = 0; j < f->nin + f->nout; j++) {
            char *end;
            v[j] = strtoull(p, &end, 16);
            ASSERT_TRUE(end != p);
            p = end;
        }
        u64 got[8] = {0};
        f->eval(v, got);
        for (int j = 0; j < f->nout; j++) {
            if (got[j] != v[f->nin + j]) {
                if (bad < 20) {
                    printf("  [%s] %s line %d: in", mode, f->name, lineno);
                    for (int k = 0; k < f->nin; k++) {
                        printf(" %llx", (unsigned long long)v[k]);
                    }
                    printf(" out[%d] expected %llx got %llx\n", j,
                           (unsigned long long)v[f->nin + j], (unsigned long long)got[j]);
                }
                bad++;
            }
        }
    }
    fclose(in);
    EXPECT_EQ(left, 0);
    EXPECT_EQ(sections, NFNS);
    return bad;
}

TEST(dmath, golden_bitwise) {
    const char *regen = getenv("DE_REGEN_GOLDEN");
    if (regen && strcmp(regen, "1") == 0) {
        regen_golden();
        SKIP("DE_REGEN_GOLDEN=1: rewrote " DE_DMATH_GOLDEN);
    }
    /* Once in the thread's default FP mode, once in the deterministic mode: the results must
     * not depend on it. */
    EXPECT_EQ(check_golden("default FP mode"), 0);
    u64 saved = de_test_fpenv_get();
    de_fp_env_init_thread();
    EXPECT_EQ(check_golden("de_fp_env_init_thread"), 0);
    de_test_fpenv_set(saved);
}

/* ---- Special values ---- */

#define EXPECT_BITS(expr, bits) EXPECT_EQ(b32(expr), (u32)(bits))
#define NAN32                   0x7FC00000u
#define PINF                    0x7F800000u
#define NINF                    0xFF800000u
#define PZERO                   0x00000000u
#define NZERO                   0x80000000u

TEST(dmath, special_values) {
    const f32 inf = f32b(PINF), ninf = f32b(NINF), nz = f32b(NZERO);
    const f32 qnan = f32b(0x7FC00000u), nnan = f32b(0xFFC00123u), snan = f32b(0x7F800001u);
    const f32 den = f32b(0x00000001u), nden = f32b(0x80400000u);
    const f32 nans[3] = {qnan, nnan, snan};

    /* Every NaN result is the canonical quiet NaN, whatever NaN came in. */
    for (int i = 0; i < 3; i++) {
        f32 n = nans[i];
        EXPECT_BITS(de_dsin(n), NAN32);
        EXPECT_BITS(de_dcos(n), NAN32);
        EXPECT_BITS(de_dtan(n), NAN32);
        EXPECT_BITS(de_dasin(n), NAN32);
        EXPECT_BITS(de_dacos(n), NAN32);
        EXPECT_BITS(de_datan(n), NAN32);
        EXPECT_BITS(de_datan2(n, 1.0f), NAN32);
        EXPECT_BITS(de_datan2(1.0f, n), NAN32);
        EXPECT_BITS(de_dexp(n), NAN32);
        EXPECT_BITS(de_dlog(n), NAN32);
        EXPECT_BITS(de_dpow(n, 2.0f), NAN32);
        EXPECT_BITS(de_dpow(2.0f, n), NAN32);
        EXPECT_BITS(de_dsqrt(n), NAN32);
        EXPECT_BITS(de_dfloor(n), NAN32);
        EXPECT_BITS(de_dceil(n), NAN32);
        EXPECT_BITS(de_dround(n), NAN32);
        EXPECT_BITS(de_dfmod(n, 1.0f), NAN32);
        EXPECT_BITS(de_dfmod(1.0f, n), NAN32);
        f32 s = 0, c = 0;
        de_dsincos(n, &s, &c);
        EXPECT_BITS(s, NAN32);
        EXPECT_BITS(c, NAN32);
    }
    EXPECT_EQ(b64(de_dsin64(f64b(0xFFF8000000000123ull))), 0x7FF8000000000000ull);
    EXPECT_EQ(b64(de_dcos64(f64b(0x7FF0000000000001ull))), 0x7FF8000000000000ull);
    EXPECT_EQ(b64(de_dsqrt64(-1.0)), 0x7FF8000000000000ull);

    /* Trigonometry. */
    EXPECT_BITS(de_dsin(0.0f), PZERO);
    EXPECT_BITS(de_dsin(nz), NZERO);
    EXPECT_BITS(de_dsin(nden), NZERO); /* denormal reads as -0 */
    EXPECT_BITS(de_dcos(0.0f), 0x3F800000u);
    EXPECT_BITS(de_dcos(den), 0x3F800000u);
    EXPECT_BITS(de_dtan(nz), NZERO);
    EXPECT_BITS(de_dsin(inf), NAN32);
    EXPECT_BITS(de_dcos(ninf), NAN32);
    EXPECT_BITS(de_dtan(inf), NAN32);
    EXPECT_EQ(b64(de_dsin64(-0.0)), 0x8000000000000000ull);
    EXPECT_EQ(b64(de_dcos64(f64b(0x8000000000000001ull))), b64(1.0));
    EXPECT_EQ(b64(de_dsin64(f64b(0x7FF0000000000000ull))), 0x7FF8000000000000ull);
    /* sin(pi_f32) is the tiny difference pi - (f32)pi = -8.742278e-8. */
    EXPECT_BITS(de_dsin(f32b(0x40490FDBu)), 0xB3BBBD2Eu);
    EXPECT_BITS(de_dcos(f32b(0x40490FDBu)), 0xBF800000u);

    /* Inverse trigonometry. */
    EXPECT_BITS(de_dasin(1.0f), 0x3FC90FDBu); /* pi/2 */
    EXPECT_BITS(de_dasin(-1.0f), 0xBFC90FDBu);
    EXPECT_BITS(de_dasin(nz), NZERO);
    EXPECT_BITS(de_dasin(f32b(0x3F800001u)), NAN32);
    EXPECT_BITS(de_dasin(inf), NAN32);
    EXPECT_BITS(de_dacos(1.0f), PZERO);
    EXPECT_BITS(de_dacos(-1.0f), 0x40490FDBu); /* pi */
    EXPECT_BITS(de_dacos(0.0f), 0x3FC90FDBu);
    EXPECT_BITS(de_dacos(f32b(0xBF800001u)), NAN32);
    EXPECT_BITS(de_datan(inf), 0x3FC90FDBu);
    EXPECT_BITS(de_datan(ninf), 0xBFC90FDBu);
    EXPECT_BITS(de_datan(nz), NZERO);
    EXPECT_BITS(de_datan(1.0f), 0x3F490FDBu); /* pi/4 */

    /* atan2, C11 Annex F. */
    EXPECT_BITS(de_datan2(0.0f, 0.0f), PZERO);
    EXPECT_BITS(de_datan2(nz, 0.0f), NZERO);
    EXPECT_BITS(de_datan2(0.0f, nz), 0x40490FDBu);
    EXPECT_BITS(de_datan2(nz, nz), 0xC0490FDBu);
    EXPECT_BITS(de_datan2(0.0f, -1.0f), 0x40490FDBu);
    EXPECT_BITS(de_datan2(nz, -1.0f), 0xC0490FDBu);
    EXPECT_BITS(de_datan2(0.0f, 1.0f), PZERO);
    EXPECT_BITS(de_datan2(1.0f, 0.0f), 0x3FC90FDBu);
    EXPECT_BITS(de_datan2(-1.0f, nz), 0xBFC90FDBu);
    EXPECT_BITS(de_datan2(1.0f, ninf), 0x40490FDBu);
    EXPECT_BITS(de_datan2(-1.0f, ninf), 0xC0490FDBu);
    EXPECT_BITS(de_datan2(1.0f, inf), PZERO);
    EXPECT_BITS(de_datan2(-1.0f, inf), NZERO);
    EXPECT_BITS(de_datan2(inf, 1.0f), 0x3FC90FDBu);
    EXPECT_BITS(de_datan2(ninf, -1.0f), 0xBFC90FDBu);
    EXPECT_BITS(de_datan2(inf, ninf), 0x4016CBE4u); /* 3pi/4 */
    EXPECT_BITS(de_datan2(ninf, inf), 0xBF490FDBu);
    EXPECT_BITS(de_datan2(1.0f, 1.0f), 0x3F490FDBu);
    EXPECT_BITS(de_datan2(den, 1.0f), PZERO); /* denormal y reads as +0 */
    EXPECT_BITS(de_datan2(1.0f, nden), 0x3FC90FDBu);
    EXPECT_BITS(de_datan2(FLT_MIN, FLT_MAX), PZERO); /* 2^-254 flushes */

    /* exp and log. */
    EXPECT_BITS(de_dexp(0.0f), 0x3F800000u);
    EXPECT_BITS(de_dexp(nz), 0x3F800000u);
    EXPECT_BITS(de_dexp(inf), PINF);
    EXPECT_BITS(de_dexp(ninf), PZERO);
    EXPECT_BITS(de_dexp(89.0f), PINF);
    EXPECT_BITS(de_dexp(f32b(0x42B17217u)), 0x7F7FFF84u); /* 88.72283: just below FLT_MAX */
    EXPECT_BITS(de_dexp(f32b(0x42B17218u)), PINF);
    EXPECT_BITS(de_dexp(-88.0f), PZERO); /* 6.05e-39 < FLT_MIN: flushed */
    EXPECT_TRUE(b32(de_dexp(-87.0f)) >= 0x00800000u);
    EXPECT_BITS(de_dexp(1.0f), 0x402DF854u); /* e */
    EXPECT_BITS(de_dlog(1.0f), PZERO);
    EXPECT_BITS(de_dlog(0.0f), NINF);
    EXPECT_BITS(de_dlog(nz), NINF);
    EXPECT_BITS(de_dlog(den), NINF);
    EXPECT_BITS(de_dlog(-1.0f), NAN32);
    EXPECT_BITS(de_dlog(ninf), NAN32);
    EXPECT_BITS(de_dlog(inf), PINF);
    EXPECT_BITS(de_dlog(f32b(0x402DF854u)), 0x3F7FFFFFu); /* (f32)e < e: log is 1 - 3.0e-8 */
    EXPECT_BITS(de_dlog(2.0f), 0x3F317218u);              /* ln 2 */

    /* pow, C11 Annex F. */
    EXPECT_BITS(de_dpow(qnan, 0.0f), 0x3F800000u);
    EXPECT_BITS(de_dpow(qnan, nz), 0x3F800000u);
    EXPECT_BITS(de_dpow(1.0f, qnan), 0x3F800000u);
    EXPECT_BITS(de_dpow(1.0f, inf), 0x3F800000u);
    EXPECT_BITS(de_dpow(-1.0f, inf), 0x3F800000u);
    EXPECT_BITS(de_dpow(-1.0f, ninf), 0x3F800000u);
    EXPECT_BITS(de_dpow(0.5f, inf), PZERO);
    EXPECT_BITS(de_dpow(0.5f, ninf), PINF);
    EXPECT_BITS(de_dpow(2.0f, inf), PINF);
    EXPECT_BITS(de_dpow(2.0f, ninf), PZERO);
    EXPECT_BITS(de_dpow(0.0f, -3.0f), PINF);
    EXPECT_BITS(de_dpow(nz, -3.0f), NINF);
    EXPECT_BITS(de_dpow(nz, -2.0f), PINF);
    EXPECT_BITS(de_dpow(nz, -0.5f), PINF);
    EXPECT_BITS(de_dpow(nz, 3.0f), NZERO);
    EXPECT_BITS(de_dpow(nz, 2.0f), PZERO);
    EXPECT_BITS(de_dpow(nz, 0.5f), PZERO);
    EXPECT_BITS(de_dpow(nz, ninf), PINF);
    EXPECT_BITS(de_dpow(ninf, -3.0f), NZERO);
    EXPECT_BITS(de_dpow(ninf, -2.0f), PZERO);
    EXPECT_BITS(de_dpow(ninf, 3.0f), NINF);
    EXPECT_BITS(de_dpow(ninf, 2.0f), PINF);
    EXPECT_BITS(de_dpow(inf, -1.0f), PZERO);
    EXPECT_BITS(de_dpow(inf, 0.5f), PINF);
    EXPECT_BITS(de_dpow(-2.0f, 0.5f), NAN32);
    EXPECT_BITS(de_dpow(-2.0f, 3.0f), 0xC1000000u); /* -8 */
    EXPECT_BITS(de_dpow(-2.0f, 2.0f), 0x40800000u); /* 4 */
    EXPECT_BITS(de_dpow(-2.0f, -1.0f), 0xBF000000u);
    EXPECT_BITS(de_dpow(2.0f, 10.0f), 0x44800000u);
    EXPECT_BITS(de_dpow(2.0f, 128.0f), PINF);
    EXPECT_BITS(de_dpow(-2.0f, 129.0f), NINF);
    EXPECT_BITS(de_dpow(2.0f, -127.0f), PZERO); /* 2^-127 < FLT_MIN: flushed */
    EXPECT_BITS(de_dpow(2.0f, -126.0f), 0x00800000u);
    EXPECT_BITS(de_dpow(-2.0f, -149.0f), NZERO);
    EXPECT_BITS(de_dpow(4.0f, 0.5f), 0x40000000u);
    EXPECT_BITS(de_dpow(2.0f, den), 0x3F800000u);         /* denormal y reads as 0 */
    EXPECT_BITS(de_dpow(nden, 3.0f), NZERO);              /* denormal x reads as -0 */
    EXPECT_BITS(de_dpow(-8.0f, f32b(0x4B7FFFFFu)), NINF); /* odd integer 16777215 */

    /* sqrt. */
    EXPECT_BITS(de_dsqrt(4.0f), 0x40000000u);
    EXPECT_BITS(de_dsqrt(nz), NZERO);
    EXPECT_BITS(de_dsqrt(den), PZERO);
    EXPECT_BITS(de_dsqrt(nden), NZERO);
    EXPECT_BITS(de_dsqrt(-1.0f), NAN32);
    EXPECT_BITS(de_dsqrt(ninf), NAN32);
    EXPECT_BITS(de_dsqrt(inf), PINF);
    EXPECT_BITS(de_dsqrt(2.0f), 0x3FB504F3u);
    EXPECT_EQ(b64(de_dsqrt64(2.0)), 0x3FF6A09E667F3BCDull);
    EXPECT_EQ(b64(de_dsqrt64(f64b(0x0000000000000001ull))), 0u);

    /* floor, ceil, round. */
    EXPECT_BITS(de_dfloor(-0.5f), 0xBF800000u);
    EXPECT_BITS(de_dfloor(0.5f), PZERO);
    EXPECT_BITS(de_dfloor(nz), NZERO);
    EXPECT_BITS(de_dfloor(nden), NZERO);
    EXPECT_BITS(de_dfloor(-1.5f), 0xC0000000u);
    EXPECT_BITS(de_dfloor(2.5f), 0x40000000u);
    EXPECT_BITS(de_dfloor(-8388607.5f), 0xCB000000u); /* -8388608 */
    EXPECT_BITS(de_dfloor(inf), PINF);
    EXPECT_BITS(de_dceil(-0.5f), NZERO);
    EXPECT_BITS(de_dceil(0.5f), 0x3F800000u);
    EXPECT_BITS(de_dceil(1.5f), 0x40000000u);
    EXPECT_BITS(de_dceil(-1.5f), 0xBF800000u);
    EXPECT_BITS(de_dceil(8388607.5f), 0x4B000000u);
    EXPECT_BITS(de_dceil(den), PZERO);
    EXPECT_BITS(de_dround(0.5f), 0x3F800000u);
    EXPECT_BITS(de_dround(-0.5f), 0xBF800000u);
    EXPECT_BITS(de_dround(f32b(0x3EFFFFFFu)), PZERO); /* 0.49999997 */
    EXPECT_BITS(de_dround(f32b(0xBEFFFFFFu)), NZERO);
    EXPECT_BITS(de_dround(1.5f), 0x40000000u);
    EXPECT_BITS(de_dround(2.5f), 0x40400000u); /* away from zero, not to even */
    EXPECT_BITS(de_dround(-2.5f), 0xC0400000u);
    EXPECT_BITS(de_dround(8388607.5f), 0x4B000000u);
    EXPECT_BITS(de_dround(f32b(0x4B000001u)), 0x4B000001u);

    /* fmod. */
    EXPECT_BITS(de_dfmod(5.5f, 2.0f), 0x3FC00000u); /* 1.5 */
    EXPECT_BITS(de_dfmod(-5.5f, 2.0f), 0xBFC00000u);
    EXPECT_BITS(de_dfmod(5.5f, -2.0f), 0x3FC00000u);
    EXPECT_BITS(de_dfmod(-4.0f, 2.0f), NZERO);
    EXPECT_BITS(de_dfmod(1.0f, 0.0f), NAN32);
    EXPECT_BITS(de_dfmod(1.0f, den), NAN32); /* denormal y reads as 0 */
    EXPECT_BITS(de_dfmod(inf, 1.0f), NAN32);
    EXPECT_BITS(de_dfmod(1.0f, inf), 0x3F800000u);
    EXPECT_BITS(de_dfmod(nz, 3.0f), NZERO);
    EXPECT_BITS(de_dfmod(1.0f, 3.0f), 0x3F800000u);
    EXPECT_BITS(de_dfmod(FLT_MAX, 11.0f), 0x41100000u);    /* (2^24 - 1) 2^104 mod 11 = 9 */
    EXPECT_BITS(de_dfmod(1.5f * FLT_MIN, FLT_MIN), PZERO); /* 0.5 FLT_MIN: flushed */
    EXPECT_BITS(de_dfmod(-3.0f * FLT_MIN, 2.0f * FLT_MIN), 0x80800000u);
}

/* ---- sincos ---- */

TEST(dmath, sincos_matches_sin_cos) {
    gen g = {77u};
    for (int i = 0; i < 300000; i++) {
        u64 in[1];
        gen_trig(&g, i, in);
        f32 x = f32b((u32)in[0]), s, c;
        de_dsincos(x, &s, &c);
        if (b32(s) != b32(de_dsin(x)) || b32(c) != b32(de_dcos(x))) {
            printf("  x = %08x\n", (unsigned)in[0]);
            FAIL("de_dsincos differs from de_dsin / de_dcos");
        }
    }
    f32 s = 0.0f;
    de_dsincos(1.0f, &s, NULL); /* NULL outputs are allowed */
    EXPECT_BITS(s, b32(de_dsin(1.0f)));
}

/* ---- Accuracy ---- */

/* |res - ref| in f32 ulps of ref (ref in the normal f32 range). */
static f64 ulp_err32(f32 res, f64 ref) {
    int e;
    frexp(ref, &e);
    f64 ulp = ldexp(1.0, e - 24);
    if (isinf(res) || isnan(res)) {
        return 1e30;
    }
    return fabs((f64)res - ref) / ulp;
}
static f64 ulp_err64(f64 res, f64 ref) {
    int e;
    frexp(ref, &e);
    f64 ulp = ldexp(1.0, e - 53);
    if (isinf(res) || isnan(res)) {
        return 1e30;
    }
    return fabs(res - ref) / ulp;
}

static bool usable32(u32 u) { /* finite and not denormal */
    u32 e = (u >> 23) & 0xFFu;
    return e != 0xFFu && (e != 0 || (u & 0x7FFFFFFFu) == 0);
}
static bool ref_in_range32(f64 ref) {
    return isfinite(ref) && (ref == 0.0 || (fabs(ref) >= FLT_MIN && fabs(ref) <= FLT_MAX));
}

typedef struct acc_case {
    const char *name;
    int nin;
    gen_fn gen;
    f64 limit; /* ulps (the issue's bound) */
} acc_case;

static f64 ref_eval(int k, f64 a, f64 b) {
    switch (k) {
    case 0:
        return sin(a);
    case 1:
        return cos(a);
    case 2:
        return tan(a);
    case 3:
        return asin(a);
    case 4:
        return acos(a);
    case 5:
        return atan(a);
    case 6:
        return atan2(a, b);
    case 7:
        return exp(a);
    case 8:
        return log(a);
    case 9:
        return pow(a, b);
    default:
        return sqrt(a);
    }
}
static f32 de_eval(int k, f32 a, f32 b) {
    switch (k) {
    case 0:
        return de_dsin(a);
    case 1:
        return de_dcos(a);
    case 2:
        return de_dtan(a);
    case 3:
        return de_dasin(a);
    case 4:
        return de_dacos(a);
    case 5:
        return de_datan(a);
    case 6:
        return de_datan2(a, b);
    case 7:
        return de_dexp(a);
    case 8:
        return de_dlog(a);
    case 9:
        return de_dpow(a, b);
    default:
        return de_dsqrt(a);
    }
}

TEST(dmath, accuracy_report) {
    /* The issue's bounds: sin/cos/tan/exp/log <= 2 ulp, asin/acos/atan/atan2 <= 3, pow <= 4. The
     * reference is the platform's double-precision libm, whose own error (~2^-53 relative) is
     * 2^-29 of an f32 ulp. Inputs: the golden inputs plus 200000 more from the same generators. */
    static const acc_case cases[] = {
        {"sin", 1, gen_trig, 2},    {"cos", 1, gen_trig, 2},    {"tan", 1, gen_trig, 2},
        {"asin", 1, gen_asin, 3},   {"acos", 1, gen_asin, 3},   {"atan", 1, gen_atan, 3},
        {"atan2", 2, gen_atan2, 3}, {"exp", 1, gen_exp, 2},     {"log", 1, gen_log, 2},
        {"pow", 2, gen_pow, 4},     {"sqrt", 1, gen_sqrt, 0.5},
    };
    printf("  %-6s %9s %9s  worst input\n", "func", "samples", "max ulp");
    for (int k = 0; k < (int)(sizeof cases / sizeof cases[0]); k++) {
        const acc_case *c = &cases[k];
        gen g = {1000u + (u64)k};
        f64 worst = 0.0;
        u64 worst_in[2] = {0, 0};
        int n = 0;
        for (int i = 0; i < GOLDEN_COUNT + 200000; i++) {
            u64 in[2] = {0, 0};
            c->gen(&g, i, in);
            if (!usable32((u32)in[0]) || (c->nin == 2 && !usable32((u32)in[1]))) {
                continue;
            }
            f32 a = f32b((u32)in[0]), b = f32b((u32)in[1]);
            f64 ref = ref_eval(k, a, b);
            if (!ref_in_range32(ref)) {
                continue;
            }
            f32 res = de_eval(k, a, b);
            f64 e = ref == 0.0 ? (res == 0.0f ? 0.0 : 1e30) : ulp_err32(res, ref);
            n++;
            if (e > worst) {
                worst = e;
                worst_in[0] = in[0];
                worst_in[1] = in[1];
            }
        }
        printf("  %-6s %9d %9.4f  %08x %08x\n", c->name, n, worst, (unsigned)worst_in[0],
               (unsigned)worst_in[1]);
        EXPECT_GT(n, 100000);
        EXPECT_TRUE(worst <= c->limit);
    }

    /* f64 against libm sin/cos. libm is not exact: on x86-64 macOS (Rosetta 2) its sin is 3 ulp
     * off at 0x41BD064CC50606D7, where de_dsin64 is correctly rounded. So libm only bounds the
     * error loosely here; the table below holds correctly rounded values (mpmath, 450 digits)
     * and is checked to 1 ulp. */
    for (int k = 0; k < 2; k++) {
        gen g = {2000u + (u64)k};
        f64 worst = 0.0;
        u64 worst_in = 0;
        for (int i = 0; i < GOLDEN_COUNT + 200000; i++) {
            u64 in[1];
            gen_trig64(&g, i, in);
            f64 x = f64b(in[0]);
            if (!isfinite(x) || (x != 0.0 && fabs(x) < DBL_MIN)) {
                continue;
            }
            f64 ref = k ? cos(x) : sin(x), res = k ? de_dcos64(x) : de_dsin64(x);
            f64 e = ref == 0.0 ? (res == 0.0 ? 0.0 : 1e30) : ulp_err64(res, ref);
            if (e > worst) {
                worst = e;
                worst_in = in[0];
            }
        }
        printf("  %-6s %9d %9.4f  %016llx (f64 ulps vs libm)\n", k ? "cos64" : "sin64",
               GOLDEN_COUNT + 200000, worst, (unsigned long long)worst_in);
        EXPECT_TRUE(worst <= 4.0);
    }
    static const u64 exact64[][3] = {
        /* x, sin x, cos x (correctly rounded). The first is the hardest double for the
         * reduction (6381956970095103 * 2^797, within 2^-61 of a multiple of pi/2). */
        {0x7496AC5B262CA1FFULL, 0xBFE9777EF4C7D742ULL, 0x3FE36058B10659F3ULL},
        {0x41BD064CC50606D7ULL, 0x3FBFD4007045688DULL, 0x3FEFC07065467DBDULL},
        {0xE16E82CCFAB5427DULL, 0xBFEBBC5BDBD09D11ULL, 0x3FDFEB9A090617DBULL},
        {0x400921FB54442D18ULL, 0x3CA1A62633145C07ULL, 0xBFF0000000000000ULL},
        {0x3FF921FB54442D18ULL, 0x3FF0000000000000ULL, 0x3C91A62633145C07ULL},
        {0x3FE921FB54442D18ULL, 0x3FE6A09E667F3BCCULL, 0x3FE6A09E667F3BCDULL},
        {0x4012D97C7F3321D2ULL, 0xBFF0000000000000ULL, 0xBCAA79394C9E8A0AULL},
        {0x413921FB54442D18ULL, 0xBDD1A62633145C07ULL, 0x3FF0000000000000ULL},
        {0x413921FB54442D19ULL, 0x3DE72CECE675D1FDULL, 0x3FF0000000000000ULL},
        {0x4E6BB4A7DEADBEEFULL, 0xBFE0014C9492A6B4ULL, 0x3FEBB5BADA0466EAULL},
        {0x3E40000000000001ULL, 0x3E40000000000001ULL, 0x3FF0000000000000ULL},
        {0x3FF0000000000000ULL, 0x3FEAED548F090CEEULL, 0x3FE14A280FB5068CULL},
        {0xC00921FB54442D18ULL, 0xBCA1A62633145C07ULL, 0xBFF0000000000000ULL},
        {0x7FEFFFFFFFFFFFFFULL, 0x3F7452FC98B34E97ULL, 0xBFEFFFE62ECFAB75ULL},
        {0x3FE0000000000000ULL, 0x3FDEAEE8744B05F0ULL, 0x3FEC1528065B7D50ULL},
        {0x4415B35BD4E9B2A8ULL, 0xBFA9B86C60046115ULL, 0xBFEFF5A82BFA5403ULL},
    };
    for (size_t i = 0; i < sizeof exact64 / sizeof exact64[0]; i++) {
        f64 x = f64b(exact64[i][0]);
        f64 es = ulp_err64(de_dsin64(x), f64b(exact64[i][1]));
        f64 ec = ulp_err64(de_dcos64(x), f64b(exact64[i][2]));
        if (!(es <= 1.0 && ec <= 1.0)) {
            printf("  x = %016llx: sin %.2f ulp, cos %.2f ulp\n", (unsigned long long)exact64[i][0],
                   es, ec);
        }
        EXPECT_TRUE(es <= 1.0);
        EXPECT_TRUE(ec <= 1.0);
    }
}

/* ---- Quaternion integration ---- */

TEST(dmath, quat_integrate_stable) {
    /* A constant world-space angular velocity for 10000 steps of 1/60 s (about 167 s, 4300
     * radians): the orientation must stay unit length and match the closed form. */
    const f64 wx = 0.3, wy = -1.2, wz = 2.0, dt = 1.0 / 60.0;
    const int steps = 10000;
    de_quat q0;
    q0.x = 0.0f;
    q0.y = 0.0f;
    q0.z = 0.38268343f;
    q0.w = 0.92387953f;
    de_vec3 w = {(f32)wx, (f32)wy, (f32)wz};
    de_quat q = q0;
    f64 max_norm_err = 0.0;
    for (int i = 0; i < steps; i++) {
        q = de_dquat_integrate(q, w, (f32)dt);
        f64 n = sqrt((f64)q.x * q.x + (f64)q.y * q.y + (f64)q.z * q.z + (f64)q.w * q.w);
        max_norm_err = fmax(max_norm_err, fabs(n - 1.0));
    }
    /* Closed form: rotation by |w| T about w / |w|, applied after q0 (world frame). */
    f64 wl = sqrt((f64)w.x * w.x + (f64)w.y * w.y + (f64)w.z * w.z);
    f64 T = steps * (f64)(f32)dt, half = 0.5 * wl * T;
    f64 s = sin(half) / wl, c = cos(half);
    f64 dx = w.x * s, dy = w.y * s, dz = w.z * s, dw = c;
    f64 ex = dw * q0.x + dx * q0.w + dy * q0.z - dz * q0.y;
    f64 ey = dw * q0.y - dx * q0.z + dy * q0.w + dz * q0.x;
    f64 ez = dw * q0.z + dx * q0.y - dy * q0.x + dz * q0.w;
    f64 ew = dw * q0.w - dx * q0.x - dy * q0.y - dz * q0.z;
    f64 dot = fabs(ex * q.x + ey * q.y + ez * q.z + ew * q.w);
    f64 angle = 2.0 * acos(fmin(1.0, dot));
    printf("  max | |q| - 1 | = %.3g, angle to closed form after %d steps = %.3g rad\n",
           max_norm_err, steps, angle);
    EXPECT_TRUE(max_norm_err < 2e-7);
    EXPECT_TRUE(angle < 2e-3);

    /* Deterministic: the same steps give the same bits. */
    de_quat q2 = q0;
    for (int i = 0; i < steps; i++) {
        q2 = de_dquat_integrate(q2, w, (f32)dt);
    }
    EXPECT_EQ(b32(q2.x), b32(q.x));
    EXPECT_EQ(b32(q2.y), b32(q.y));
    EXPECT_EQ(b32(q2.z), b32(q.z));
    EXPECT_EQ(b32(q2.w), b32(q.w));

    /* No rotation: the input normalized. */
    de_vec3 zero = {0.0f, 0.0f, 0.0f};
    de_quat a = de_dquat_integrate(q0, zero, (f32)dt), b = de_dquat_normalize(q0);
    EXPECT_EQ(b32(a.z), b32(b.z));
    EXPECT_EQ(b32(a.w), b32(b.w));
    /* A half turn about +Z: identity -> (0, 0, 1, 0) up to sign and rounding. */
    de_quat id;
    id.x = id.y = id.z = 0.0f;
    id.w = 1.0f;
    de_vec3 wz_axis = {0.0f, 0.0f, 3.14159265f};
    de_quat h = de_dquat_integrate(id, wz_axis, 1.0f);
    EXPECT_FLOAT_EQ(h.z, 1.0, 1e-7);
    EXPECT_FLOAT_EQ(h.w, 0.0, 1e-7);
}

TEST(dmath, helpers) {
    de_vec3 v = {3.0f, 0.0f, 4.0f};
    de_vec3 n = de_dv3_normalize(v);
    EXPECT_BITS(n.x, 0x3F19999Au); /* 0.6 */
    EXPECT_BITS(n.y, PZERO);
    EXPECT_BITS(n.z, 0x3F4CCCCDu); /* 0.8 */
    de_vec3 zero = {0.0f, 0.0f, 0.0f};
    n = de_dv3_normalize(zero);
    EXPECT_BITS(n.x, PZERO);
    de_vec3 big = {FLT_MAX, FLT_MAX, 0.0f}; /* no overflow: |v|^2 is formed in f64 */
    n = de_dv3_normalize(big);
    EXPECT_BITS(n.x, 0x3F3504F3u);
    de_quat z;
    z.x = z.y = z.z = z.w = 0.0f;
    de_quat q = de_dquat_normalize(z);
    EXPECT_BITS(q.w, 0x3F800000u);
    EXPECT_BITS(q.x, PZERO);
}
