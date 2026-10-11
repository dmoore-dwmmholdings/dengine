/* dmath.c: deterministic math, f32 part (DENG-243, SPEC §5.4, §5.7).
 *
 * Every function converts its f32 inputs to f64 (exact), computes with the f64 kernels of
 * dmath64.c and rounds once (de__dm_f32_result). floor, ceil, round and fmod are exact and work
 * on the bits. See dmath_reduce.h for the numeric model (denormals, NaN, rounding). Compiled with
 * -ffp-contract=off (de_mark_deterministic_sources); no <math.h> function except sqrt and sqrtf.
 */
#include "core/math/dmath_reduce.h"

#include <math.h> /* sqrt, sqrtf only (correctly rounded IEEE operations) */

#define DE__PI_HI   0x1.921fb54442d18p+1
#define DE__PI_LO   0x1.1a62633145c07p-53
#define DE__PIO2_D  0x1.921fb54442d18p+0
#define DE__PIO4_D  0x1.921fb54442d18p-1
#define DE__3PIO4_D 0x1.2d97c7f3321d2p+1
/* exp overflows f32 above ln(FLT_MAX) = 88.72..., and is below FLT_MIN under -87.33...: outside
 * [-104, 89] the result is known without computing it. */
#define DE__EXP_HI 89.0
#define DE__EXP_LO -104.0

static inline f32 de__signed_zero(bool neg) {
    return de__dm_from32(neg ? 0x80000000u : 0u);
}
static inline f32 de__signed_inf(bool neg) {
    return de__dm_from32(neg ? 0xFF800000u : 0x7F800000u);
}

/* ---- Trigonometry ---- */

f32 de_dsin(f32 x) {
    if (de__dm_isnan32(x) || de__dm_isinf32(x)) {
        return de__dm_nan32();
    }
    f64 s;
    de__dm_sincos((f64)de__dm_daz32(x), &s, NULL);
    return de__dm_f32_result(s);
}

f32 de_dcos(f32 x) {
    if (de__dm_isnan32(x) || de__dm_isinf32(x)) {
        return de__dm_nan32();
    }
    f64 c;
    de__dm_sincos((f64)de__dm_daz32(x), NULL, &c);
    return de__dm_f32_result(c);
}

void de_dsincos(f32 x, f32 *s, f32 *c) {
    f32 rs, rc;
    if (de__dm_isnan32(x) || de__dm_isinf32(x)) {
        rs = rc = de__dm_nan32();
    } else {
        f64 ds, dc;
        de__dm_sincos((f64)de__dm_daz32(x), &ds, &dc);
        rs = de__dm_f32_result(ds);
        rc = de__dm_f32_result(dc);
    }
    if (s) {
        *s = rs;
    }
    if (c) {
        *c = rc;
    }
}

f32 de_dtan(f32 x) {
    if (de__dm_isnan32(x) || de__dm_isinf32(x)) {
        return de__dm_nan32();
    }
    return de__dm_f32_result(de__dm_tan((f64)de__dm_daz32(x)));
}

f32 de_dasin(f32 x) {
    if (de__dm_isnan32(x)) {
        return de__dm_nan32();
    }
    f64 d = (f64)de__dm_daz32(x);
    f64 a = de__dm_fabs64(d);
    if (a > 1.0) {
        return de__dm_nan32();
    }
    if (a == 1.0) {
        return de__dm_f32_result(d > 0.0 ? DE__PIO2_D : -DE__PIO2_D);
    }
    /* asin x = atan(x / sqrt(1 - x^2)); 1 - x^2 as (1 - x)(1 + x) keeps it accurate near 1. */
    return de__dm_f32_result(de__dm_atan(d / sqrt((1.0 - d) * (1.0 + d))));
}

f32 de_dacos(f32 x) {
    if (de__dm_isnan32(x)) {
        return de__dm_nan32();
    }
    f64 d = (f64)de__dm_daz32(x);
    if (de__dm_fabs64(d) > 1.0) {
        return de__dm_nan32();
    }
    if (d == -1.0) {
        return de__dm_f32_result(DE__PI_HI);
    }
    /* acos x = 2 atan(sqrt((1 - x) / (1 + x))), accurate at both ends. */
    return de__dm_f32_result(2.0 * de__dm_atan(sqrt((1.0 - d) / (1.0 + d))));
}

f32 de_datan(f32 x) {
    if (de__dm_isnan32(x)) {
        return de__dm_nan32();
    }
    return de__dm_f32_result(de__dm_atan((f64)de__dm_daz32(x)));
}

f32 de_datan2(f32 y, f32 x) {
    if (de__dm_isnan32(y) || de__dm_isnan32(x)) {
        return de__dm_nan32();
    }
    y = de__dm_daz32(y);
    x = de__dm_daz32(x);
    bool sy = de__dm_signbit32(y), sx = de__dm_signbit32(x);
    f64 dy = (f64)y, dx = (f64)x;
    f64 r;
    if (dy == 0.0) { /* +-0 for x > 0 or +0; +-pi for x < 0 or -0 */
        if (!sx) {
            return y;
        }
        r = DE__PI_HI;
    } else if (dx == 0.0) {
        r = DE__PIO2_D;
    } else if (de__dm_isinf32(x)) {
        if (de__dm_isinf32(y)) {
            r = sx ? DE__3PIO4_D : DE__PIO4_D;
        } else {
            if (!sx) {
                return de__signed_zero(sy);
            }
            r = DE__PI_HI;
        }
    } else if (de__dm_isinf32(y)) {
        r = DE__PIO2_D;
    } else {
        /* The ratio of two f32 magnitudes is within [2^-254, 2^254]: no overflow in f64. */
        r = de__dm_atan(de__dm_fabs64(dy) / de__dm_fabs64(dx));
        if (sx) {
            r = (DE__PI_HI - r) + DE__PI_LO;
        }
    }
    return de__dm_f32_result(sy ? -r : r);
}

/* ---- Exponential, logarithm, power ---- */

f32 de_dexp(f32 x) {
    if (de__dm_isnan32(x)) {
        return de__dm_nan32();
    }
    f64 d = (f64)de__dm_daz32(x);
    if (d > DE__EXP_HI) {
        return de__signed_inf(false);
    }
    if (d < DE__EXP_LO) {
        return 0.0f;
    }
    return de__dm_f32_result(de__dm_exp(d));
}

f32 de_dlog(f32 x) {
    if (de__dm_isnan32(x)) {
        return de__dm_nan32();
    }
    x = de__dm_daz32(x);
    if (x == 0.0f) {
        return de__signed_inf(true);
    }
    if (de__dm_signbit32(x)) {
        return de__dm_nan32();
    }
    if (de__dm_isinf32(x)) {
        return x;
    }
    return de__dm_f32_result(de__dm_log((f64)x));
}

/* 0: y is not an integer, 1: odd integer, 2: even integer. y finite and not zero. */
static int de__int_kind(f32 y) {
    u32 u = de__dm_bits32(y);
    int e = (int)((u >> 23) & 0xFF) - 127;
    if (e < 0) {
        return 0;
    }
    if (e > 23) {
        return 2;
    }
    u32 m = (u & 0x007FFFFFu) | 0x00800000u;
    u32 frac_mask = (e == 23) ? 0u : (0x00FFFFFFu >> (e + 1));
    if (m & frac_mask) {
        return 0;
    }
    return ((m >> (23 - e)) & 1u) ? 1 : 2;
}

f32 de_dpow(f32 x, f32 y) {
    /* Special cases follow C11 Annex F (pow), with denormals read as zero. */
    x = de__dm_isnan32(x) ? x : de__dm_daz32(x);
    y = de__dm_isnan32(y) ? y : de__dm_daz32(y);
    if (y == 0.0f || x == 1.0f) {
        return 1.0f;
    }
    if (de__dm_isnan32(x) || de__dm_isnan32(y)) {
        return de__dm_nan32();
    }
    bool xneg = de__dm_signbit32(x);
    bool yneg = de__dm_signbit32(y);
    if (de__dm_isinf32(y)) {
        f64 ax = de__dm_fabs64((f64)x);
        if (ax == 1.0) {
            return 1.0f;
        }
        return ((ax < 1.0) == yneg) ? de__signed_inf(false) : 0.0f;
    }
    int kind = de__int_kind(y);
    bool odd = kind == 1;
    if (x == 0.0f) {
        if (yneg) {
            return de__signed_inf(odd && xneg);
        }
        return de__signed_zero(odd && xneg);
    }
    if (de__dm_isinf32(x)) {
        bool neg = odd && xneg;
        return yneg ? de__signed_zero(neg) : de__signed_inf(neg);
    }
    if (xneg && kind == 0) {
        return de__dm_nan32();
    }
    bool rneg = xneg && odd;
    f64 t = (f64)y * de__dm_log(de__dm_fabs64((f64)x));
    if (t > DE__EXP_HI) {
        return de__signed_inf(rneg);
    }
    if (t < DE__EXP_LO) {
        return de__signed_zero(rneg);
    }
    f64 r = de__dm_exp(t);
    return de__dm_f32_result(rneg ? -r : r);
}

f32 de_dsqrt(f32 x) {
    if (de__dm_isnan32(x)) {
        return de__dm_nan32();
    }
    x = de__dm_daz32(x);
    if (de__dm_signbit32(x) && x != 0.0f) {
        return de__dm_nan32();
    }
    return sqrtf(x); /* IEEE sqrt is correctly rounded on every target */
}

/* ---- Rounding and remainder (exact, on the bits) ---- */

f32 de_dfloor(f32 x) {
    if (de__dm_isnan32(x)) {
        return de__dm_nan32();
    }
    x = de__dm_daz32(x);
    u32 u = de__dm_bits32(x);
    int e = (int)((u >> 23) & 0xFF) - 127;
    if (e >= 23) {
        return x; /* integral, or infinite */
    }
    if (e < 0) { /* |x| < 1 */
        if ((u & 0x7FFFFFFFu) == 0) {
            return x;
        }
        return (u >> 31) ? -1.0f : 0.0f;
    }
    u32 mask = 0x007FFFFFu >> e;
    if ((u & mask) == 0) {
        return x;
    }
    if (u >> 31) {
        u += 0x00800000u >> e;
    }
    return de__dm_from32(u & ~mask);
}

f32 de_dceil(f32 x) {
    if (de__dm_isnan32(x)) {
        return de__dm_nan32();
    }
    x = de__dm_daz32(x);
    u32 u = de__dm_bits32(x);
    int e = (int)((u >> 23) & 0xFF) - 127;
    if (e >= 23) {
        return x;
    }
    if (e < 0) {
        if ((u & 0x7FFFFFFFu) == 0) {
            return x;
        }
        return (u >> 31) ? de__signed_zero(true) : 1.0f;
    }
    u32 mask = 0x007FFFFFu >> e;
    if ((u & mask) == 0) {
        return x;
    }
    if (!(u >> 31)) {
        u += 0x00800000u >> e;
    }
    return de__dm_from32(u & ~mask);
}

f32 de_dround(f32 x) {
    if (de__dm_isnan32(x)) {
        return de__dm_nan32();
    }
    x = de__dm_daz32(x);
    u32 u = de__dm_bits32(x);
    int e = (int)((u >> 23) & 0xFF) - 127;
    if (e >= 23) {
        return x;
    }
    if (e < -1) { /* |x| < 0.5 */
        return de__signed_zero((u >> 31) != 0);
    }
    if (e == -1) { /* 0.5 <= |x| < 1 */
        return (u >> 31) ? -1.0f : 1.0f;
    }
    u32 mask = 0x007FFFFFu >> e;
    if ((u & mask) == 0) {
        return x;
    }
    u += 0x00400000u >> e; /* half a unit, away from zero */
    return de__dm_from32(u & ~mask);
}

f32 de_dfmod(f32 x, f32 y) {
    if (de__dm_isnan32(x) || de__dm_isnan32(y)) {
        return de__dm_nan32();
    }
    x = de__dm_daz32(x);
    y = de__dm_daz32(y);
    if (de__dm_isinf32(x) || y == 0.0f) {
        return de__dm_nan32();
    }
    if (de__dm_isinf32(y) || x == 0.0f) {
        return x;
    }
    u32 ux = de__dm_bits32(x), uy = de__dm_bits32(y);
    u32 sx = ux & 0x80000000u;
    ux &= 0x7FFFFFFFu;
    uy &= 0x7FFFFFFFu;
    if (ux < uy) {
        return x;
    }
    if (ux == uy) {
        return de__dm_from32(sx);
    }
    /* Long division of the significands: mx * 2^ex mod my * 2^ey, exactly. */
    int ex = (int)(ux >> 23), ey = (int)(uy >> 23);
    u32 mx = (ux & 0x007FFFFFu) | 0x00800000u;
    u32 my = (uy & 0x007FFFFFu) | 0x00800000u;
    for (; ex > ey; ex--) {
        if (mx >= my) {
            mx -= my;
        }
        mx <<= 1;
    }
    if (mx >= my) {
        mx -= my;
    }
    if (mx == 0) {
        return de__dm_from32(sx);
    }
    while (mx < 0x00800000u) {
        mx <<= 1;
        ey--;
    }
    if (ey <= 0) { /* denormal remainder: flushed */
        return de__dm_from32(sx);
    }
    return de__dm_from32(sx | ((u32)ey << 23) | (mx & 0x007FFFFFu));
}

/* ---- Vectors and quaternions (in f64, rounded once per component) ---- */

static bool de__any_nonfinite(const f32 *v, int n) {
    for (int i = 0; i < n; i++) {
        if ((de__dm_bits32(v[i]) & 0x7F800000u) == 0x7F800000u) {
            return true;
        }
    }
    return false;
}

de_vec3 de_dnormalize_v3(de_vec3 a) {
    f32 in[3] = {a.x, a.y, a.z};
    de_vec3 r;
    if (de__any_nonfinite(in, 3)) {
        r.x = r.y = r.z = de__dm_nan32();
        return r;
    }
    f64 x = (f64)de__dm_daz32(a.x), y = (f64)de__dm_daz32(a.y), z = (f64)de__dm_daz32(a.z);
    f64 l2 = (x * x + y * y) + z * z;
    if (l2 == 0.0) {
        r.x = r.y = r.z = 0.0f;
        return r;
    }
    f64 l = sqrt(l2);
    r.x = de__dm_f32_result(x / l);
    r.y = de__dm_f32_result(y / l);
    r.z = de__dm_f32_result(z / l);
    return r;
}

/* q / |q| for a quaternion held as f64 (x, y, z, w); zero gives the identity. */
static de_quat de__quat_normalize64(f64 x, f64 y, f64 z, f64 w) {
    de_quat r;
    f64 l2 = ((x * x + y * y) + z * z) + w * w;
    if (l2 == 0.0) {
        r.x = r.y = r.z = 0.0f;
        r.w = 1.0f;
        return r;
    }
    f64 l = sqrt(l2);
    r.x = de__dm_f32_result(x / l);
    r.y = de__dm_f32_result(y / l);
    r.z = de__dm_f32_result(z / l);
    r.w = de__dm_f32_result(w / l);
    return r;
}

static de_quat de__quat_nan(void) {
    de_quat r;
    r.x = r.y = r.z = r.w = de__dm_nan32();
    return r;
}

de_quat de_dnormalize_quat(de_quat q) {
    f32 in[4] = {q.x, q.y, q.z, q.w};
    if (de__any_nonfinite(in, 4)) {
        return de__quat_nan();
    }
    return de__quat_normalize64((f64)de__dm_daz32(q.x), (f64)de__dm_daz32(q.y),
                                (f64)de__dm_daz32(q.z), (f64)de__dm_daz32(q.w));
}

de_quat de_dintegrate_quat(de_quat q, de_vec3 angular_velocity, f32 dt) {
    f32 in[8] = {q.x, q.y, q.z, q.w, angular_velocity.x, angular_velocity.y, angular_velocity.z,
                 dt};
    if (de__any_nonfinite(in, 8)) {
        return de__quat_nan();
    }
    f64 qx = (f64)de__dm_daz32(q.x), qy = (f64)de__dm_daz32(q.y);
    f64 qz = (f64)de__dm_daz32(q.z), qw = (f64)de__dm_daz32(q.w);
    f64 wx = (f64)de__dm_daz32(angular_velocity.x), wy = (f64)de__dm_daz32(angular_velocity.y);
    f64 wz = (f64)de__dm_daz32(angular_velocity.z), h = (f64)de__dm_daz32(dt);
    f64 w2 = (wx * wx + wy * wy) + wz * wz;
    if (w2 == 0.0 || h == 0.0) {
        return de__quat_normalize64(qx, qy, qz, qw);
    }
    /* d = (axis * sin(theta/2), cos(theta/2)), theta = |w| dt: the exact rotation of a constant
     * world-space angular velocity over dt. sin(theta/2) / |w| stays accurate as w -> 0. */
    f64 wl = sqrt(w2);
    f64 s, c;
    de__dm_sincos(0.5 * wl * h, &s, &c);
    f64 k = s / wl;
    f64 dx = wx * k, dy = wy * k, dz = wz * k, dw = c;
    /* d * q (Hamilton product): q first, then the world-space increment d. */
    f64 rx = ((dw * qx + dx * qw) + dy * qz) - dz * qy;
    f64 ry = ((dw * qy - dx * qz) + dy * qw) + dz * qx;
    f64 rz = ((dw * qz + dx * qy) - dy * qx) + dz * qw;
    f64 rw = ((dw * qw - dx * qx) - dy * qy) - dz * qz;
    return de__quat_normalize64(rx, ry, rz, rw);
}
