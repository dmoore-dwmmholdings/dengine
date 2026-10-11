/* dmath64.c: deterministic math, f64 part (DENG-243, SPEC §5.4, §5.7).
 *
 * The range reduction, the polynomial kernels every de_d* function is built on, and the f64
 * public functions. See dmath_reduce.h for the numeric model. Compiled with -ffp-contract=off
 * (de_mark_deterministic_sources); no <math.h> function except sqrt.
 *
 * Polynomials are minimax (weighted Remez, relative error) and are evaluated in Horner order,
 * exactly as written: the order of operations is part of the result. The constants come from
 * tools/dmath/gen_dmath_constants.py; changing one changes results, which needs a regenerated
 * golden file (tests/unit/math/vectors/dmath_golden.txt) and a reason in the commit message.
 */
#include "core/math/dmath_reduce.h"

#include <math.h> /* sqrt only (correctly rounded IEEE operation) */

/* ---- Constants (tools/dmath/gen_dmath_constants.py) ---- */

/* pi/2 = PIO2_1 + PIO2_2 + PIO2_3 + PIO2_3T; the first three have 33 bits, so n * PIO2_k is
 * exact for |n| <= 2^20. */
#define DE__PIO2_1   0x1.921fb54400000p+0
#define DE__PIO2_2   0x1.0b4611a600000p-34
#define DE__PIO2_3   0x1.3198a2e000000p-69
#define DE__PIO2_3T  0x1.b839a252049c1p-104
#define DE__PIO2_HI  0x1.921fb54442d18p+0
#define DE__PIO2_LO  0x1.1a62633145c07p-54
#define DE__INV_PIO2 0x1.45f306dc9c883p-1
#define DE__PIO4_HI  0x1.921fb54442d18p-1
#define DE__PIO4_LO  0x1.1a62633145c07p-55
/* Cody-Waite limit: 2^20 * pi/2. */
#define DE__CW_LIMIT 0x1.921fb54442d18p+20
/* tan(pi/8) = sqrt(2) - 1: above it atan reduces with pi/4. */
#define DE__TAN_PIO8 0x1.a827999fcef34p-2
#define DE__SQRT2    0x1.6a09e667f3bcdp+0
/* Adding and subtracting 1.5 * 2^52 rounds |t| < 2^51 to the nearest integer (ties to even). */
#define DE__ROUND_MAGIC 0x1.8p52

/* 2/pi = 0.w[0] w[1] ... in base 2^32 (1280 bits): enough for every double exponent. */
static const u32 de__two_over_pi[40] = {
    0xa2f9836eu, 0x4e441529u, 0xfc2757d1u, 0xf534ddc0u, 0xdb629599u, 0x3c439041u, 0xfe5163abu,
    0xdebbc561u, 0xb7246e3au, 0x424dd2e0u, 0x06492eeau, 0x09d1921cu, 0xfe1deb1cu, 0xb129a73eu,
    0xe88235f5u, 0x2ebb4484u, 0xe99c7026u, 0xb45f7e41u, 0x3991d639u, 0x835339f4u, 0x9c845f8bu,
    0xbdf9283bu, 0x1ff897ffu, 0xde05980fu, 0xef2f118bu, 0x5a0a6d1fu, 0x6d367ecfu, 0x27cb09b7u,
    0x4f463f66u, 0x9e5fea2du, 0x7527bac7u, 0xebe5f17bu, 0x3d0739f7u, 0x8a5292eau, 0x6bfb5fb1u,
    0x1f8d5d08u, 0x56033046u, 0xfc7b6babu, 0xf0cfbc20u, 0x9af4361du,
};

/* ln 2 = LN2_HI + LN2_LO; LN2_HI has 42 bits, so k * LN2_HI is exact for |k| < 2^11. */
#define DE__LN2_HI  0x1.62e42fefa3800p-1
#define DE__LN2_LO  0x1.ef35793c76730p-45
#define DE__INV_LN2 0x1.71547652b82fep+0

/* sin(x) = x + x^3 * S(x^2) on |x| <= pi/4: relative error 2^-56.4 with these doubles. */
#define DE__S0 -0x1.5555555555548p-3
#define DE__S1 0x1.111111110f7d0p-7
#define DE__S2 -0x1.a01a019bfdee0p-13
#define DE__S3 0x1.71de3567d29eap-19
#define DE__S4 -0x1.ae5e5a91070b2p-26
#define DE__S5 0x1.5d8fd187b1044p-33

/* cos(x) = 1 - x^2/2 + x^4 * C(x^2) on |x| <= pi/4: 2^-59.7. */
#define DE__C0 0x1.555555555554bp-5
#define DE__C1 -0x1.6c16c16c14f91p-10
#define DE__C2 0x1.a01a019c844d8p-16
#define DE__C3 -0x1.27e4f7eac364fp-22
#define DE__C4 0x1.1ee9d7b3f4727p-29
#define DE__C5 -0x1.8fa49989519f4p-37

/* atan(u) = u + u^3 * A(u^2) on |u| <= tan(pi/8): 2^-57.4. */
#define DE__A0  -0x1.555555555553dp-2
#define DE__A1  0x1.99999999957cdp-3
#define DE__A2  -0x1.24924922aa4c6p-3
#define DE__A3  0x1.c71c70e50d5dbp-4
#define DE__A4  -0x1.745cf8c355ceep-4
#define DE__A5  0x1.3b111792d6b19p-4
#define DE__A6  -0x1.10eba8acc1a3ap-4
#define DE__A7  0x1.df0e7e8c4fedcp-5
#define DE__A8  -0x1.9ccf778710a41p-5
#define DE__A9  0x1.37ce0d056c05fp-5
#define DE__A10 -0x1.255b15738c7f9p-6

/* exp(r) = 1 + r + r^2 * E(r) on |r| <= ln2/2: 2^-60.3. */
#define DE__E0  0x1.0000000000000p-1
#define DE__E1  0x1.555555555555bp-3
#define DE__E2  0x1.5555555555503p-5
#define DE__E3  0x1.111111110ec65p-7
#define DE__E4  0x1.6c16c16c3043bp-10
#define DE__E5  0x1.a01a01b37b4adp-13
#define DE__E6  0x1.a01a01369f545p-16
#define DE__E7  0x1.71ddf02846685p-19
#define DE__E8  0x1.27e5b433d964bp-22
#define DE__E9  0x1.af6bda9d1c772p-26
#define DE__E10 0x1.1e3d35a891f1ap-29

/* log(m) = 2s + s^3 * L(s^2), s = (m - 1) / (m + 1), m in [sqrt(2)/2, sqrt(2)]: 2^-61.6. */
#define DE__L0 0x1.5555555555555p-1
#define DE__L1 0x1.9999999999dfbp-2
#define DE__L2 0x1.249249242b90bp-2
#define DE__L3 0x1.c71c7258a5d85p-3
#define DE__L4 0x1.745cde4f90accp-3
#define DE__L5 0x1.3b20a759b5b5ep-3
#define DE__L6 0x1.0f5d23a588986p-3
#define DE__L7 0x1.0f5ffc0239010p-3

/* ---- Error-free transformations (no FMA: Knuth's TwoSum, Dekker's product) ---- */

/* s + e = a + b exactly. */
static inline void de__two_sum(f64 a, f64 b, f64 *s, f64 *e) {
    f64 x = a + b;
    f64 bb = x - a;
    *e = (a - (x - bb)) + (b - bb);
    *s = x;
}

/* hi + lo = a, each with at most 26 significant bits (Veltkamp). */
static inline void de__split(f64 a, f64 *hi, f64 *lo) {
    f64 c = 134217729.0 * a; /* 2^27 + 1 */
    f64 h = c - (c - a);
    *hi = h;
    *lo = a - h;
}

/* p + e = a * b exactly (no overflow or underflow in this file's uses). */
static inline void de__two_prod(f64 a, f64 b, f64 *p, f64 *e) {
    f64 ah, al, bh, bl;
    f64 x = a * b;
    de__split(a, &ah, &al);
    de__split(b, &bh, &bl);
    *e = ((ah * bh - x) + ah * bl + al * bh) + al * bl;
    *p = x;
}

static inline f64 de__pow2(int k) { /* 2^k, -1022 <= k <= 1023 */
    return de__dm_from64((u64)(1023 + k) << 52);
}

/* ---- Range reduction ---- */

/* Bits i .. i+31 of 2/pi; bit j (j >= 1) has weight 2^-j. Bits at j <= 0 are zero. */
static u32 de__two_over_pi_bits(int i) {
    if (i < 1) {
        if (i + 31 < 1) {
            return 0;
        }
        return de__two_over_pi_bits(1) >> (1 - i);
    }
    int w = (i - 1) >> 5;
    int sh = (i - 1) & 31;
    u64 hi = de__two_over_pi[w];
    u64 lo = (w + 1 < 40) ? de__two_over_pi[w + 1] : 0;
    return (u32)((((hi << 32) | lo) << sh) >> 32);
}

static int de__clz64(u64 v) { /* v != 0 */
    int n = 0;
    if ((v >> 32) == 0) {
        n += 32;
        v <<= 32;
    }
    if ((v >> 48) == 0) {
        n += 16;
        v <<= 16;
    }
    if ((v >> 56) == 0) {
        n += 8;
        v <<= 8;
    }
    if ((v >> 60) == 0) {
        n += 4;
        v <<= 4;
    }
    if ((v >> 62) == 0) {
        n += 2;
        v <<= 2;
    }
    if ((v >> 63) == 0) {
        n += 1;
    }
    return n;
}

/*
 * Payne-Hanek for a finite |x| >= 2^20 * pi/2. |x| = m * 2^e with m a 53-bit integer. The bits
 * of 2/pi at index i <= e - 2 add m * 2^(e - i), a multiple of 4, so they do not change the
 * quadrant: the reduction needs only the window B of 192 bits starting at index e - 1. Then
 * x * 2/pi = m * B / 2^190 (mod 4), with an error below m * 2^(e - (e + 190)) < 2^-137.
 */
static int de__payne_hanek(f64 x, f64 *y0, f64 *y1) {
    u64 u = de__dm_bits64(x);
    bool neg = (u >> 63) != 0;
    int e = (int)((u >> 52) & 0x7FF) - 1075;
    u64 m = (u & 0x000FFFFFFFFFFFFFull) | 0x0010000000000000ull;

    u32 b[6]; /* little-endian limbs of B */
    for (int k = 0; k < 6; k++) {
        b[5 - k] = de__two_over_pi_bits(e - 1 + 32 * k);
    }
    u32 ml[2] = {(u32)m, (u32)(m >> 32)};
    u32 p[8] = {0};
    for (int i = 0; i < 2; i++) {
        u64 carry = 0;
        for (int j = 0; j < 6; j++) {
            u64 t = (u64)ml[i] * b[j] + p[i + j] + carry;
            p[i + j] = (u32)t;
            carry = t >> 32;
        }
        p[i + 6] = (u32)carry;
    }

    /* Quadrant: bits 190..191. Fraction: bits 0..189, as f2:f1:f0 (f2 holds bits 128..189). */
    int q = (int)((p[5] >> 30) & 3u);
    u64 f2 = ((u64)(p[5] & 0x3FFFFFFFu) << 32) | p[4];
    u64 f1 = ((u64)p[3] << 32) | p[2];
    u64 f0 = ((u64)p[1] << 32) | p[0];
    bool negf = false;
    if (f2 >> 61) { /* fraction >= 1/2: use fraction - 1, next quadrant */
        /* 2^190 - F: two's complement on 192 bits, then the low 190 bits. */
        f0 = ~f0;
        f1 = ~f1;
        f2 = ~f2;
        f0 += 1;
        if (f0 == 0) {
            f1 += 1;
            if (f1 == 0) {
                f2 += 1;
            }
        }
        f2 &= 0x3FFFFFFFFFFFFFFFull;
        q = (q + 1) & 3;
        negf = true;
    }

    /* Normalize so that bit 191 (top of f2) is set; count the shift. */
    int lz = 0;
    while (f2 == 0) {
        if (f1 == 0 && f0 == 0) { /* cannot happen for x != 0; keep it total */
            *y0 = 0.0;
            *y1 = 0.0;
            return q;
        }
        f2 = f1;
        f1 = f0;
        f0 = 0;
        lz += 64;
    }
    int s = de__clz64(f2);
    if (s) {
        f2 = (f2 << s) | (f1 >> (64 - s));
        f1 = (f1 << s) | (f0 >> (64 - s));
    }
    lz += s;

    /* F / 2^190 = hi * 2^(-51 - lz) + lo * 2^(-104 - lz) + (dropped bits). */
    u64 hi = f2 >> 11;
    u64 lo = ((f2 & 0x7FFu) << 42) | (f1 >> 22);
    f64 hd = (f64)hi * de__pow2(-51 - lz);
    f64 ld = (f64)lo * de__pow2(-104 - lz);

    /* r = (hd + ld) * pi/2, as a double-double. */
    f64 ph, pe;
    de__two_prod(hd, DE__PIO2_HI, &ph, &pe);
    f64 tail = pe + (hd * DE__PIO2_LO + ld * DE__PIO2_HI);
    f64 r0, r1;
    de__two_sum(ph, tail, &r0, &r1);
    if (negf != neg) {
        r0 = -r0;
        r1 = -r1;
    }
    if (neg) {
        q = (-q) & 3;
    }
    *y0 = r0;
    *y1 = r1;
    return q;
}

int de__dm_rem_pio2(f64 x, f64 *y0, f64 *y1) {
    f64 ax = de__dm_fabs64(x);
    if (ax <= DE__PIO4_HI) {
        *y0 = x;
        *y1 = 0.0;
        return 0;
    }
    if (ax < DE__CW_LIMIT) {
        f64 nd = (x * DE__INV_PIO2 + DE__ROUND_MAGIC) - DE__ROUND_MAGIC;
        int n = (int)nd;
        f64 r = x - nd * DE__PIO2_1; /* exact (Sterbenz) */
        f64 s1, e1, s2, e2;
        de__two_sum(r, -(nd * DE__PIO2_2), &s1, &e1);
        de__two_sum(s1, -(nd * DE__PIO2_3), &s2, &e2);
        f64 lo = (e1 + e2) - nd * DE__PIO2_3T;
        de__two_sum(s2, lo, y0, y1);
        return n & 3;
    }
    return de__payne_hanek(x, y0, y1);
}

/* ---- Kernels ---- */

f64 de__dm_ksin(f64 y0, f64 y1) {
    f64 z = y0 * y0;
    f64 v = z * y0;
    f64 r = DE__S1 + z * (DE__S2 + z * (DE__S3 + z * (DE__S4 + z * DE__S5)));
    /* sin(y0 + y1) = sin(y0) + y1 * cos(y0) + O(y1^2). */
    return y0 + (v * DE__S0 + (v * (z * r) + y1 * (1.0 - 0.5 * z)));
}

f64 de__dm_kcos(f64 y0, f64 y1) {
    f64 z = y0 * y0;
    f64 r = z * (DE__C0 + z * (DE__C1 + z * (DE__C2 + z * (DE__C3 + z * (DE__C4 + z * DE__C5)))));
    /* cos(y0 + y1) = cos(y0) - y1 * sin(y0) + O(y1^2). 1 - z/2 is formed with its rounding
     * error kept ((1 - w) - hz), which matters near |y0| = pi/4. */
    f64 hz = 0.5 * z;
    f64 w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + (z * r - y0 * y1));
}

void de__dm_sincos(f64 x, f64 *s, f64 *c) {
    if (de__dm_fabs64(x) < 0x1p-27) { /* sin x = x, cos x = 1 after rounding; keeps -0 */
        if (s) {
            *s = x;
        }
        if (c) {
            *c = 1.0;
        }
        return;
    }
    f64 y0, y1;
    int n = de__dm_rem_pio2(x, &y0, &y1);
    if (s) {
        switch (n) {
        case 0:
            *s = de__dm_ksin(y0, y1);
            break;
        case 1:
            *s = de__dm_kcos(y0, y1);
            break;
        case 2:
            *s = -de__dm_ksin(y0, y1);
            break;
        default:
            *s = -de__dm_kcos(y0, y1);
            break;
        }
    }
    if (c) {
        switch (n) {
        case 0:
            *c = de__dm_kcos(y0, y1);
            break;
        case 1:
            *c = -de__dm_ksin(y0, y1);
            break;
        case 2:
            *c = -de__dm_kcos(y0, y1);
            break;
        default:
            *c = de__dm_ksin(y0, y1);
            break;
        }
    }
}

f64 de__dm_tan(f64 x) {
    if (de__dm_fabs64(x) < 0x1p-27) {
        return x;
    }
    f64 y0, y1;
    int n = de__dm_rem_pio2(x, &y0, &y1);
    f64 s = de__dm_ksin(y0, y1);
    f64 c = de__dm_kcos(y0, y1);
    /* Odd quadrants: tan(r + pi/2) = -cos(r) / sin(r). sin(r) != 0 here: r = 0 needs x = 0. */
    return (n & 1) ? -c / s : s / c;
}

f64 de__dm_atan(f64 t) {
    f64 a = de__dm_fabs64(t);
    bool neg = (de__dm_bits64(t) >> 63) != 0;
    if (a < 0x1p-27) { /* atan t = t after rounding; keeps +-0 */
        return t;
    }
    bool inv = a > 1.0;
    if (inv) {
        a = 1.0 / a; /* atan a = pi/2 - atan(1/a); 1/inf = 0 */
    }
    f64 base_hi = 0.0, base_lo = 0.0, u = a;
    if (a > DE__TAN_PIO8) { /* atan a = pi/4 + atan((a - 1) / (a + 1)) */
        u = (a - 1.0) / (a + 1.0);
        base_hi = DE__PIO4_HI;
        base_lo = DE__PIO4_LO;
    }
    f64 p = u;
    if (de__dm_fabs64(u) >= 0x1p-27) {
        f64 z = u * u;
        f64 poly =
            DE__A0 +
            z * (DE__A1 +
                 z * (DE__A2 +
                      z * (DE__A3 +
                           z * (DE__A4 +
                                z * (DE__A5 +
                                     z * (DE__A6 +
                                          z * (DE__A7 +
                                               z * (DE__A8 + z * (DE__A9 + z * DE__A10)))))))));
        p = u + u * z * poly;
    }
    f64 r = base_hi + (base_lo + p);
    if (inv) {
        r = (DE__PIO2_HI - r) + DE__PIO2_LO;
    }
    return neg ? -r : r;
}

f64 de__dm_exp(f64 t) {
    if (de__dm_fabs64(t) < 0x1p-54) {
        return 1.0 + t;
    }
    f64 kd = (t * DE__INV_LN2 + DE__ROUND_MAGIC) - DE__ROUND_MAGIC;
    int k = (int)kd;
    f64 r = (t - kd * DE__LN2_HI) - kd * DE__LN2_LO; /* first difference exact (Sterbenz) */
    f64 poly = DE__E0 +
               r * (DE__E1 +
                    r * (DE__E2 +
                         r * (DE__E3 +
                              r * (DE__E4 +
                                   r * (DE__E5 +
                                        r * (DE__E6 +
                                             r * (DE__E7 +
                                                  r * (DE__E8 + r * (DE__E9 + r * DE__E10)))))))));
    f64 er = 1.0 + (r + r * r * poly);
    return er * de__pow2(k);
}

f64 de__dm_log(f64 x) {
    u64 u = de__dm_bits64(x);
    int k = (int)((u >> 52) & 0x7FF) - 1023;
    f64 m = de__dm_from64((u & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull); /* [1, 2) */
    if (m > DE__SQRT2) {
        m *= 0.5;
        k += 1;
    }
    f64 f = m - 1.0; /* exact (Sterbenz) */
    f64 s = f / (2.0 + f);
    f64 z = s * s;
    f64 poly =
        DE__L0 +
        z * (DE__L1 +
             z * (DE__L2 + z * (DE__L3 + z * (DE__L4 + z * (DE__L5 + z * (DE__L6 + z * DE__L7))))));
    f64 lm = 2.0 * s + s * z * poly;
    f64 kd = (f64)k;
    return kd * DE__LN2_HI + (kd * DE__LN2_LO + lm);
}

/* ---- f64 public functions ---- */

f64 de_dsin64(f64 x) {
    if (de__dm_isnan64(x)) {
        return de__dm_nan64();
    }
    x = de__dm_daz64(x);
    if (de__dm_isinf64(x)) {
        return de__dm_nan64();
    }
    f64 s;
    de__dm_sincos(x, &s, NULL);
    return s;
}

f64 de_dcos64(f64 x) {
    if (de__dm_isnan64(x)) {
        return de__dm_nan64();
    }
    x = de__dm_daz64(x);
    if (de__dm_isinf64(x)) {
        return de__dm_nan64();
    }
    f64 c;
    de__dm_sincos(x, NULL, &c);
    return c;
}

f64 de_dsqrt64(f64 x) {
    if (de__dm_isnan64(x)) {
        return de__dm_nan64();
    }
    x = de__dm_daz64(x);
    if ((de__dm_bits64(x) >> 63) && x != 0.0) { /* negative, not -0 */
        return de__dm_nan64();
    }
    return sqrt(x);
}
