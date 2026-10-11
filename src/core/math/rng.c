/* rng.c: de_rng, deterministic random number streams (DENG-243, SPEC §5.7 rule 6).
 *
 * PCG32 (PCG-XSH-RR 64/32, M. E. O'Neill, "PCG: A Family of Simple Fast Space-Efficient
 * Statistically Good Algorithms for Random Number Generation", 2014): a 64-bit LCG whose output
 * is a permuted 32-bit value. Integer arithmetic only, so the sequence is the same on every
 * platform and compiler. Seeding follows the paper's pcg32_srandom_r, so seed 42 / stream 54
 * reproduces the reference demo output.
 */
#include <dengine/de_math.h>

#define DE__PCG_MULT 6364136223846793005ull

static inline void de__pcg_step(de_rng *r) {
    r->state = r->state * DE__PCG_MULT + r->inc;
}

de_rng de_rng_seed(u64 seed, u64 stream) {
    de_rng r;
    r.state = 0u;
    r.inc = (stream << 1u) | 1u;
    de__pcg_step(&r);
    r.state += seed;
    de__pcg_step(&r);
    return r;
}

u32 de_rng_u32(de_rng *r) {
    u64 old = r->state;
    de__pcg_step(r);
    u32 xorshifted = (u32)(((old >> 18u) ^ old) >> 27u);
    u32 rot = (u32)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
}

f32 de_rng_f32(de_rng *r) {
    /* The top 24 bits, scaled by 2^-24: every value is exact in f32 and below 1. */
    return (f32)(de_rng_u32(r) >> 8) * 0x1p-24f;
}

f64 de_rng_f64(de_rng *r) {
    /* 53 bits from two outputs (27 high, 26 low), scaled by 2^-53. */
    u64 hi = de_rng_u32(r) >> 5;
    u64 lo = de_rng_u32(r) >> 6;
    return (f64)((hi << 26) | lo) * 0x1p-53;
}

u32 de_rng_range(de_rng *r, u32 lo, u32 hi_exclusive) {
    if (hi_exclusive <= lo) {
        return lo;
    }
    /* Lemire, "Fast Random Integer Generation in an Interval" (2019): multiply-shift, rejecting
     * the low products that would bias the result. The rejection loop consumes a deterministic
     * number of outputs for a given state. */
    u32 range = hi_exclusive - lo;
    u64 m = (u64)de_rng_u32(r) * range;
    u32 low = (u32)m;
    if (low < range) {
        u32 threshold = (0u - range) % range; /* 2^32 mod range */
        while (low < threshold) {
            m = (u64)de_rng_u32(r) * range;
            low = (u32)m;
        }
    }
    return lo + (u32)(m >> 32);
}

void de_rng_advance(de_rng *r, u64 delta) {
    /* Brown, "Random Number Generation with Arbitrary Strides" (1994): the LCG step applied
     * delta times is state * A + C, built by squaring in O(log delta). */
    u64 cur_mult = DE__PCG_MULT, cur_plus = r->inc;
    u64 acc_mult = 1u, acc_plus = 0u;
    while (delta > 0u) {
        if (delta & 1u) {
            acc_mult *= cur_mult;
            acc_plus = acc_plus * cur_mult + cur_plus;
        }
        cur_plus = (cur_mult + 1u) * cur_plus;
        cur_mult *= cur_mult;
        delta >>= 1u;
    }
    r->state = acc_mult * r->state + acc_plus;
}
