/* dmath_reduce.h: internals shared by the deterministic math (dmath.c, dmath64.c). DENG-243.
 *
 * Only dmath*.c include this header. It must never include <math.h> or math_internal.h (the
 * fast math): every result of the deterministic math comes from the IEEE basic operations
 * (+ - * / sqrt, correctly rounded on every target), evaluated in a fixed order. The files that
 * include it are compiled with -ffp-contract=off (/fp:precise), so no product is fused into an
 * FMA, and nothing here uses an approximate reciprocal.
 *
 * Numeric model (SPEC §5.7):
 *   - Every f32 function converts its inputs to f64 (exact), computes in f64 with the kernels
 *     below and rounds once at the end (de__dm_f32_result). The f64 functions use the same
 *     kernels on a double-double argument.
 *   - Denormal inputs are treated as zero of the same sign, results below the smallest normal
 *     flush to zero of the same sign, and every NaN result is the canonical quiet NaN
 *     (0x7FC00000 / 0x7FF8000000000000). This is the FTZ/DAZ behaviour of de_fp_env_init_thread,
 *     done in code, so the results do not depend on the thread's FP mode or on how a CPU
 *     propagates NaN payloads. Rounding must be round-to-nearest-even (the default).
 */
#ifndef DE_CORE_MATH_DMATH_REDUCE_H
#define DE_CORE_MATH_DMATH_REDUCE_H

#include <dengine/de_math.h>

#include <string.h>

/* Belt and braces with the -ffp-contract=off / /fp:precise build flags: clang-cl's /fp:precise
 * alone still allows contraction. GCC does not know the STDC pragma (it warns), it gets the
 * command-line flag. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

/* ---- Bit access ---- */

static inline u32 de__dm_bits32(f32 x) {
    u32 u;
    memcpy(&u, &x, sizeof u);
    return u;
}
static inline f32 de__dm_from32(u32 u) {
    f32 x;
    memcpy(&x, &u, sizeof x);
    return x;
}
static inline u64 de__dm_bits64(f64 x) {
    u64 u;
    memcpy(&u, &x, sizeof u);
    return u;
}
static inline f64 de__dm_from64(u64 u) {
    f64 x;
    memcpy(&x, &u, sizeof x);
    return x;
}

#define DE__DM_NAN32 0x7FC00000u
#define DE__DM_NAN64 0x7FF8000000000000ull

static inline f32 de__dm_nan32(void) {
    return de__dm_from32(DE__DM_NAN32);
}
static inline f64 de__dm_nan64(void) {
    return de__dm_from64(DE__DM_NAN64);
}

/* Denormal f32 input -> zero of the same sign. Everything else unchanged. */
static inline f32 de__dm_daz32(f32 x) {
    u32 u = de__dm_bits32(x);
    if ((u & 0x7F800000u) == 0) {
        return de__dm_from32(u & 0x80000000u);
    }
    return x;
}
static inline f64 de__dm_daz64(f64 x) {
    u64 u = de__dm_bits64(x);
    if ((u & 0x7FF0000000000000ull) == 0) {
        return de__dm_from64(u & 0x8000000000000000ull);
    }
    return x;
}
static inline bool de__dm_isnan32(f32 x) {
    return (de__dm_bits32(x) & 0x7FFFFFFFu) > 0x7F800000u;
}
static inline bool de__dm_isnan64(f64 x) {
    return (de__dm_bits64(x) & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull;
}

static inline bool de__dm_isinf32(f32 x) {
    return (de__dm_bits32(x) & 0x7FFFFFFFu) == 0x7F800000u;
}
static inline bool de__dm_isinf64(f64 x) {
    return (de__dm_bits64(x) & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull;
}
static inline bool de__dm_signbit32(f32 x) {
    return (de__dm_bits32(x) >> 31) != 0;
}

/* |x| as bits, sign dropped. */
static inline f64 de__dm_fabs64(f64 x) {
    return de__dm_from64(de__dm_bits64(x) & 0x7FFFFFFFFFFFFFFFull);
}

/**
 * The one rounding of an f32 function: f64 -> f32, round to nearest even. A NaN becomes the
 * canonical NaN; a result whose magnitude is below FLT_MIN *before* rounding becomes a zero of
 * its sign. (x86 FTZ tests tininess after rounding, arm64 FZ before: doing it here makes both the
 * same.) Overflow rounds to infinity as IEEE says.
 */
static inline f32 de__dm_f32_result(f64 r) {
    u64 u = de__dm_bits64(r);
    u64 a = u & 0x7FFFFFFFFFFFFFFFull;
    if (a > 0x7FF0000000000000ull) {
        return de__dm_nan32();
    }
    if (a < 0x3810000000000000ull) { /* 2^-126 = FLT_MIN */
        return de__dm_from32((u32)(u >> 32) & 0x80000000u);
    }
    return (f32)r;
}

/* ---- Range reduction (dmath64.c) ---- */

/**
 * x = n * pi/2 + (*y0 + *y1) with |*y0 + *y1| <= pi/4 (a hair more after the rounding of n).
 * Returns n mod 4 (0..3). x must be finite and normal (or zero); *y1 is tiny next to *y0.
 *
 *   |x| <= pi/4:             y0 = x, y1 = 0, n = 0.
 *   |x| <  2^20 * pi/2:      Cody-Waite: n = round(x * 2/pi), x - n * (P1 + P2 + P3 + P3T) with
 *                            P1..P3 of 33 bits (n * Pi exact) and the error terms kept as a
 *                            double-double: about 2^-130 absolute.
 *   larger:                  Payne-Hanek in integer arithmetic: the 53-bit significand times a
 *                            192-bit window of 2/pi, exact up to 2^-137 of a quarter turn,
 *                            for every finite double up to DBL_MAX.
 */
int de__dm_rem_pio2(f64 x, f64 *y0, f64 *y1);

/* ---- Kernels (dmath64.c). All in f64, results rounded once. ---- */

/** sin(y0 + y1) for |y0 + y1| <= pi/4 + a hair, y1 tiny next to y0. */
f64 de__dm_ksin(f64 y0, f64 y1);
/** cos(y0 + y1), same domain. */
f64 de__dm_kcos(f64 y0, f64 y1);
/** sin and cos of a finite x with the same reduction (sincos matches sin and cos bitwise).
 *  Handles x = 0 and tiny |x| exactly; x must not be NaN or infinite. */
void de__dm_sincos(f64 x, f64 *s, f64 *c);
/** tan of a finite x (not NaN or infinite): sin / cos of one reduction, in f64. */
f64 de__dm_tan(f64 x);
/** atan(t) for any t, including +-inf (-> +-pi/2). t = +-0 gives +-0. Not NaN. */
f64 de__dm_atan(f64 t);
/** exp(t) for finite t with |t| <= 708 (the caller clamps: the result is finite and normal). */
f64 de__dm_exp(f64 t);
/** log(x) for finite, normal x > 0. */
f64 de__dm_log(f64 x);

#endif
