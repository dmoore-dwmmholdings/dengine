/* action_modifiers.c: binding modifiers (DENG-84), in the fixed order
 *
 *     deadzone -> curve -> scale -> invert
 *
 * The deadzone and the curve work on the magnitude (|v| in 1D, the length in 2D) and keep the
 * sign or direction. The deadzone rescales, so the output starts at 0 right after it:
 * (m - dz) / (1 - dz). Pad axes are "bounded": their magnitude is clamped to 1 after the
 * deadzone (a square stick gate reaches a length of about 1.41 in the corners). The mouse and the
 * gyro are unbounded and keep their size.
 *
 * Math goes through de_dmath (de_dsqrt, de_dpow), so the same raw inputs give the same values on
 * every architecture (the module also builds with -ffp-contract=off). */
#include "action_internal.h"

#include <dengine/de_math.h>

static f32 absf(f32 v) {
    return v < 0.0f ? -v : v;
}

static bool finite_f32(f32 v) {
    return v == v && v <= 3.0e38f && v >= -3.0e38f;
}

f32 de__curve_eval(const de_response_curve *c, f32 m) {
    if (m <= 0.0f)
        m = 0.0f;
    switch (c->type) {
    case DE_CURVE_POWER:
        return m > 0.0f ? de_dpow(m, c->exponent) : 0.0f;
    case DE_CURVE_CUSTOM: {
        const u32 n = c->point_count;
        if (n < 2u || n > DE_CURVE_MAX_POINTS)
            return m;
        if (m <= c->points[0].x)
            return c->points[0].y;
        for (u32 i = 1; i < n; ++i) {
            const de_vec2 a = c->points[i - 1u];
            const de_vec2 b = c->points[i];
            if (m <= b.x) {
                const f32 t = (m - a.x) / (b.x - a.x);
                return a.y + (b.y - a.y) * t;
            }
        }
        return c->points[n - 1u].y;
    }
    case DE_CURVE_LINEAR:
    default:
        return m;
    }
}

de_result de__curve_validate(const de_response_curve *c, const char *what) {
    switch (c->type) {
    case DE_CURVE_LINEAR:
        return DE_OK;
    case DE_CURVE_POWER:
        if (!finite_f32(c->exponent) || !(c->exponent > 0.0f))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: a power curve needs an exponent > 0",
                           what);
        return DE_OK;
    case DE_CURVE_CUSTOM:
        if (c->point_count < 2u || c->point_count > DE_CURVE_MAX_POINTS)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: a custom curve needs 2 to %d points", what,
                           DE_CURVE_MAX_POINTS);
        for (u32 i = 0; i < c->point_count; ++i) {
            if (!finite_f32(c->points[i].x) || !finite_f32(c->points[i].y))
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: curve point %u is not finite", what,
                               (unsigned)i);
            if (i > 0 && !(c->points[i].x > c->points[i - 1u].x))
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                               "%s: curve point x values must increase (point %u)", what,
                               (unsigned)i);
        }
        return DE_OK;
    default:
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: unknown curve type %d", what, (int)c->type);
    }
}

/* Deadzone (rescaled), bounded clamp and curve on a magnitude >= 0. */
static f32 shape_magnitude(const de_binding *b, f32 m, bool bounded) {
    const f32 dz = b->deadzone;
    if (dz > 0.0f)
        m = m <= dz ? 0.0f : (m - dz) / (1.0f - dz);
    if (bounded && m > 1.0f)
        m = 1.0f;
    return de__curve_eval(&b->curve, m);
}

static f32 scale_of(const de_binding *b) {
    return b->scale != 0.0f ? b->scale : 1.0f;
}

f32 de__modifiers_apply_1d(const de_binding *b, f32 v, bool bounded) {
    if (!finite_f32(v))
        return 0.0f;
    const f32 m = shape_magnitude(b, absf(v), bounded);
    f32 out = v < 0.0f ? -m : m;
    out *= scale_of(b);
    if (b->invert)
        out = -out;
    return out;
}

de_vec2 de__modifiers_apply_2d(const de_binding *b, de_vec2 v, bool bounded) {
    de_vec2 out = {0.0f, 0.0f};
    if (!finite_f32(v.x) || !finite_f32(v.y))
        return out;
    const f32 len = de_dsqrt(v.x * v.x + v.y * v.y);
    if (!(len > 0.0f))
        return out;
    const f32 m = shape_magnitude(b, len, bounded);
    const f32 k = (m / len) * scale_of(b);
    out.x = v.x * k;
    out.y = v.y * k;
    if (b->invert)
        out.y = -out.y;
    return out;
}
