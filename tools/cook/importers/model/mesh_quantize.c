/*
 * mesh_quantize.c: vertex quantization of the mesh stage (DENG-103, SPEC §11.6): the DMSH v2
 * vertex formats (render/formats/de_cooked_mesh.h) and their encoders. See model_internal.h.
 *
 * Every encoder computes in f64 and rounds once, and every decision (a format, a code) depends
 * only on the input values, so the same mesh always gives the same bytes. The decoders are the
 * header's (de_cooked_mesh_position, de_cooked_oct_decode, ...): the tests decode what this
 * file encodes with them.
 */
#include "model_internal.h"

#include <dengine/de_base.h>

#include "render/formats/de_cooked_mesh.h"

#include <math.h>
#include <string.h>

static void put_u16(u8 *p, u16 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

/* round(x) for x in a range that fits an i32, halves away from zero (no fenv dependence). */
static i32 round_i32(f64 x) {
    return (i32)(x >= 0.0 ? floor(x + 0.5) : -floor(-x + 0.5));
}

static u16 unorm16(f64 v, f64 offset, f64 scale) {
    if (!(scale > 0.0))
        return 0;
    i32 q = round_i32((v - offset) / scale);
    return (u16)(q < 0 ? 0 : q > 65535 ? 65535 : q);
}

static u8 unorm8(f64 v) {
    if (!(v > 0.0))
        return 0;
    if (v >= 1.0)
        return 255;
    return (u8)round_i32(v * 255.0);
}

/* ---- Octahedral unit vectors ----------------------------------------------------------------- */

void de_model_oct_encode(const f32 n[3], i32 sign_bit, i16 out[2]) {
    f64 x = n[0], y = n[1], z = n[2];
    f64 l1 = fabs(x) + fabs(y) + fabs(z);
    if (!(l1 > 0.0) || !isfinite(l1)) {
        /* No direction: +Z, which decodes from (0, 0). */
        x = 0.0;
        y = 0.0;
        z = 1.0;
        l1 = 1.0;
    }
    f64 px = x / l1, py = y / l1;
    if (z < 0.0) {
        f64 tx = (1.0 - fabs(py)) * (px >= 0.0 ? 1.0 : -1.0);
        f64 ty = (1.0 - fabs(px)) * (py >= 0.0 ? 1.0 : -1.0);
        px = tx;
        py = ty;
    }
    f64 len = sqrt(x * x + y * y + z * z);
    f64 ux = x / len, uy = y / len, uz = z / len;
    f64 fx = floor(px * 32767.0), fy = floor(py * 32767.0);
    f64 best = -2.0;
    i32 bx = 0, by = 0;
    /* Candidates around the exact code, in a fixed order (ties keep the first): x in {fx, fx + 1};
     * y in {fy, fy + 1}, or with a sign bit, the two codes in {fy - 1 .. fy + 2} whose bit 0 is
     * the sign. The decoder clears bit 0 of a signed y, so it is cleared here too. */
    for (i32 i = 0; i < 2; ++i)
        for (i32 j = -1; j < 3; ++j) {
            i32 cx = (i32)fx + i, cy = (i32)fy + j;
            if (cx < -32767 || cx > 32767 || cy < -32767 || cy > 32767)
                continue;
            u16 ybits = (u16)(i16)cy;
            if (sign_bit < 0 && (j < 0 || j > 1))
                continue;
            if (sign_bit >= 0 && (u32)(ybits & 1u) != (u32)sign_bit)
                continue;
            if (sign_bit >= 0)
                ybits = (u16)(ybits & 0xFFFEu);
            f32 d[3];
            de_cooked_oct_decode((i16)cx, (i16)ybits, d);
            f64 dot = d[0] * ux + d[1] * uy + d[2] * uz;
            if (dot > best) {
                best = dot;
                bx = cx;
                by = cy;
            }
        }
    out[0] = (i16)bx;
    out[1] = (i16)by;
}

/* ---- Weights --------------------------------------------------------------------------------- */

void de_model_weights_unorm8(const f32 w[4], u8 out[4]) {
    f64 v[4], sum = 0.0;
    for (u32 k = 0; k < 4; ++k) {
        v[k] = w[k] > 0.0f && isfinite(w[k]) ? (f64)w[k] : 0.0;
        sum += v[k];
    }
    if (!(sum > 0.0)) {
        out[0] = 255;
        out[1] = out[2] = out[3] = 0;
        return;
    }
    u32 total = 0;
    f64 frac[4];
    for (u32 k = 0; k < 4; ++k) {
        f64 s = v[k] / sum * 255.0;
        f64 f = floor(s);
        out[k] = (u8)(f > 255.0 ? 255.0 : f);
        frac[k] = s - f;
        total += out[k];
    }
    /* Hand out what rounding down lost, largest remainder first (ties: lower index). */
    while (total < 255u) {
        u32 best = 0;
        for (u32 k = 1; k < 4; ++k)
            if (frac[k] > frac[best])
                best = k;
        out[best]++;
        frac[best] = -1.0;
        total++;
    }
}

/* ---- Choosing the formats -------------------------------------------------------------------- */

static u32 vtx(const u32 *order, u32 i) {
    return order != NULL ? order[i] : i;
}

/* Position as decoded with offset/scale (the header's f32 expression). */
static void decode_pos(const f32 offset[3], const f32 scale[3], const u16 q[3], f32 out[3]) {
    for (u32 k = 0; k < 3; ++k) {
        f32 t = (f32)q[k] * scale[k];
        out[k] = offset[k] + t;
    }
}

static void choose_positions(const de_model_mesh *m, const u32 *order, u32 count, f32 max_error,
                             de_model_quant *q) {
    q->format[DE_MODEL_ATTR_POSITION] = DE_COOKED_VF_F32X3;
    for (u32 k = 0; k < 3; ++k) {
        q->pos_offset[k] = 0.0f;
        q->pos_scale[k] = 1.0f;
    }
    if (count == 0 || !(max_error > 0.0f))
        return;
    f32 lo[3], hi[3];
    for (u32 k = 0; k < 3; ++k) {
        lo[k] = INFINITY;
        hi[k] = -INFINITY;
    }
    for (u32 i = 0; i < count; ++i) {
        const f32 *p = m->position + 3u * (u64)vtx(order, i);
        for (u32 k = 0; k < 3; ++k) {
            if (!isfinite(p[k]))
                return;
            lo[k] = p[k] < lo[k] ? p[k] : lo[k];
            hi[k] = p[k] > hi[k] ? p[k] : hi[k];
        }
    }
    f32 offset[3], scale[3];
    for (u32 k = 0; k < 3; ++k) {
        f64 step = ((f64)hi[k] - (f64)lo[k]) / 65535.0;
        if (!(step <= (f64)max_error))
            return;
        offset[k] = lo[k];
        scale[k] = (f32)step;
    }
    /* The check that matters: every decoded position within max_error of its source. */
    for (u32 i = 0; i < count; ++i) {
        const f32 *p = m->position + 3u * (u64)vtx(order, i);
        u16 c[3];
        for (u32 k = 0; k < 3; ++k)
            c[k] = unorm16(p[k], offset[k], scale[k]);
        f32 d[3];
        decode_pos(offset, scale, c, d);
        f64 e = 0.0;
        for (u32 k = 0; k < 3; ++k)
            e += ((f64)d[k] - p[k]) * ((f64)d[k] - p[k]);
        if (!(sqrt(e) <= (f64)max_error))
            return;
    }
    q->format[DE_MODEL_ATTR_POSITION] = DE_COOKED_VF_UNORM16X4;
    memcpy(q->pos_offset, offset, sizeof offset);
    memcpy(q->pos_scale, scale, sizeof scale);
}

static void choose_uv(const de_model_mesh *m, const u32 *order, u32 count, u32 set,
                      de_model_quant *q) {
    u32 attr = set == 0 ? DE_MODEL_ATTR_UV0 : DE_MODEL_ATTR_UV1;
    q->format[attr] = DE_COOKED_VF_F32X2;
    q->uv_offset[set][0] = q->uv_offset[set][1] = 0.0f;
    q->uv_scale[set][0] = q->uv_scale[set][1] = 1.0f;
    f32 lo[2] = {INFINITY, INFINITY}, hi[2] = {-INFINITY, -INFINITY};
    for (u32 i = 0; i < count; ++i) {
        const f32 *t = m->uv[set] + 2u * (u64)vtx(order, i);
        for (u32 k = 0; k < 2; ++k) {
            if (!isfinite(t[k]))
                return;
            lo[k] = t[k] < lo[k] ? t[k] : lo[k];
            hi[k] = t[k] > hi[k] ? t[k] : hi[k];
        }
    }
    if (count == 0)
        return;
    for (u32 k = 0; k < 2; ++k)
        if (!((f64)hi[k] - (f64)lo[k] <= (f64)DE_COOKED_UV_UNORM_MAX_RANGE))
            return;
    q->format[attr] = DE_COOKED_VF_UNORM16X2;
    for (u32 k = 0; k < 2; ++k) {
        q->uv_offset[set][k] = lo[k];
        q->uv_scale[set][k] = (f32)(((f64)hi[k] - (f64)lo[k]) / 65535.0);
    }
}

void de_model_quant_choose(const de_model_mesh *m, const u32 *order, u32 count,
                           f32 max_position_error, de_model_quant *q) {
    memset(q, 0, sizeof *q);
    choose_positions(m, order, count, max_position_error, q);
    if (m->normal != NULL)
        q->format[DE_MODEL_ATTR_NORMAL] = DE_COOKED_VF_OCT16;
    if (m->tangent != NULL)
        q->format[DE_MODEL_ATTR_TANGENT] = DE_COOKED_VF_OCT16_SIGN;
    for (u32 set = 0; set < 2; ++set)
        if (m->uv[set] != NULL)
            choose_uv(m, order, count, set, q);
    if (m->color0 != NULL)
        q->format[DE_MODEL_ATTR_COLOR0] = DE_COOKED_VF_UNORM8X4;
    if (m->weights0 != NULL)
        q->format[DE_MODEL_ATTR_WEIGHTS0] = DE_COOKED_VF_UNORM8X4;
    if (m->joints0 != NULL) {
        bool wide = false;
        for (u32 i = 0; i < count && !wide; ++i)
            for (u32 k = 0; k < 4; ++k)
                wide = wide || m->joints0[4u * (u64)vtx(order, i) + k] > 255u;
        q->format[DE_MODEL_ATTR_JOINTS0] = wide ? DE_COOKED_VF_U16X4 : DE_COOKED_VF_U8X4;
    }
}

/* ---- Encoding -------------------------------------------------------------------------------- */

void de_model_quant_position(const de_model_quant *q, const de_model_mesh *m, u32 v, f32 out[3]) {
    const f32 *p = m->position + 3u * (u64)v;
    if (q->format[DE_MODEL_ATTR_POSITION] == DE_COOKED_VF_F32X3) {
        memcpy(out, p, 3 * sizeof(f32));
        return;
    }
    u16 c[3];
    for (u32 k = 0; k < 3; ++k)
        c[k] = unorm16(p[k], q->pos_offset[k], q->pos_scale[k]);
    decode_pos(q->pos_offset, q->pos_scale, c, out);
}

void de_model_quant_encode(const de_model_quant *q, const de_model_mesh *m, u32 attr, u32 v,
                           u8 *out) {
    u64 i = v;
    switch (attr) {
    case DE_MODEL_ATTR_POSITION:
        if (q->format[attr] == DE_COOKED_VF_F32X3) {
            for (u32 k = 0; k < 3; ++k)
                de_cooked_put_f32(out + 4 * k, m->position[3 * i + k]);
        } else {
            for (u32 k = 0; k < 3; ++k)
                put_u16(out + 2 * k,
                        unorm16(m->position[3 * i + k], q->pos_offset[k], q->pos_scale[k]));
            put_u16(out + 6, 0);
        }
        break;
    case DE_MODEL_ATTR_NORMAL: {
        i16 c[2];
        de_model_oct_encode(m->normal + 3 * i, -1, c);
        put_u16(out, (u16)c[0]);
        put_u16(out + 2, (u16)c[1]);
        break;
    }
    case DE_MODEL_ATTR_TANGENT: {
        i16 c[2];
        de_model_oct_encode(m->tangent + 4 * i, m->tangent[4 * i + 3] < 0.0f ? 1 : 0, c);
        put_u16(out, (u16)c[0]);
        put_u16(out + 2, (u16)c[1]);
        break;
    }
    case DE_MODEL_ATTR_UV0:
    case DE_MODEL_ATTR_UV1: {
        u32 set = attr == DE_MODEL_ATTR_UV0 ? 0u : 1u;
        const f32 *t = m->uv[set] + 2 * i;
        if (q->format[attr] == DE_COOKED_VF_F32X2) {
            de_cooked_put_f32(out, t[0]);
            de_cooked_put_f32(out + 4, t[1]);
        } else {
            for (u32 k = 0; k < 2; ++k)
                put_u16(out + 2 * k, unorm16(t[k], q->uv_offset[set][k], q->uv_scale[set][k]));
        }
        break;
    }
    case DE_MODEL_ATTR_COLOR0:
        for (u32 k = 0; k < 4; ++k)
            out[k] = unorm8(m->color0[4 * i + k]);
        break;
    case DE_MODEL_ATTR_JOINTS0:
        for (u32 k = 0; k < 4; ++k) {
            u16 j = m->joints0[4 * i + k];
            if (q->format[attr] == DE_COOKED_VF_U16X4)
                put_u16(out + 2 * k, j);
            else
                out[k] = (u8)j;
        }
        break;
    case DE_MODEL_ATTR_WEIGHTS0:
        de_model_weights_unorm8(m->weights0 + 4 * i, out);
        break;
    default:
        break;
    }
}
