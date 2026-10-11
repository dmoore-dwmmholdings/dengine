/* quat.c: de_quat operations, scalar reference (SPEC §5.4, DENG-241).
 *
 * Hamilton quaternions (x, y, z, w), w scalar. mul(a, b) applies b first, like matrices, so
 * de_quat_to_m3(a * b) = de_quat_to_m3(a) * de_quat_to_m3(b). */
#include "core/math/math_internal.h"
#include "core/math/math_simd.h"

de_quat de_quat_identity(void) {
    return de__quat(0.0f, 0.0f, 0.0f, 1.0f);
}

de_quat de_quat_axis_angle(de_vec3 axis, f32 radians) {
    f32 h = 0.5f * radians;
    f32 s = sinf(h);
    return de__quat(axis.x * s, axis.y * s, axis.z * s, cosf(h));
}

de_quat de__quat_mul_scalar(de_quat a, de_quat b) {
    return de__quat(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                    a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                    a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}

de_quat de_quat_from_euler(f32 yaw, f32 pitch, f32 roll) {
    /* yaw about +Z, pitch about +X, roll about +Y (forward); q = yaw * pitch * roll. */
    de_quat qy = de_quat_axis_angle(DE_AXIS_UP, yaw);
    de_quat qp = de_quat_axis_angle(DE_AXIS_RIGHT, pitch);
    de_quat qr = de_quat_axis_angle(DE_AXIS_FORWARD, roll);
    return de_quat_mul(de_quat_mul(qy, qp), qr);
}

void de_quat_to_euler(de_quat q, f32 *yaw, f32 *pitch, f32 *roll) {
    /* R = Rz(yaw) * Rx(pitch) * Ry(roll); with c* = cos, s* = sin:
     *   R01 = -sy*cp   R11 = cy*cp   R21 = sp   R20 = -cp*sr   R22 = cp*cr
     * In gimbal lock (cp = 0): R00 = cos(yaw +- roll), R10 = sin(yaw +- roll). */
    de_mat3 m = de_quat_to_m3(q);
    f32 r00 = m.m[0], r10 = m.m[1], r20 = m.m[2];
    f32 r01 = m.m[3], r11 = m.m[4], r21 = m.m[5];
    f32 r22 = m.m[8];
    f32 cp = sqrtf(r01 * r01 + r11 * r11);
    f32 p = atan2f(r21, cp);
    f32 y, r;
    if (cp > 1e-6f) {
        y = atan2f(-r01, r11);
        r = atan2f(-r20, r22);
    } else {
        y = atan2f(r10, r00);
        r = 0.0f;
    }
    if (yaw)
        *yaw = y;
    if (pitch)
        *pitch = p;
    if (roll)
        *roll = r;
}

de_quat de_quat_conj(de_quat q) {
    return de__quat(-q.x, -q.y, -q.z, q.w);
}

f32 de_quat_dot(de_quat a, de_quat b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

de_quat de_quat_inverse(de_quat q) {
    f32 inv = 1.0f / de_quat_dot(q, q);
    return de__quat(-q.x * inv, -q.y * inv, -q.z * inv, q.w * inv);
}

de_quat de_quat_normalize(de_quat q) {
    f32 len = sqrtf(de_quat_dot(q, q));
    if (!(len > 0.0f) || !isfinite(len))
        return de_quat_identity();
    f32 inv = 1.0f / len;
    return de__quat(q.x * inv, q.y * inv, q.z * inv, q.w * inv);
}

de_vec3 de__quat_rotate_scalar(de_quat q, de_vec3 v) {
    /* v' = v + w*t + u x t with u = (x, y, z), t = 2 (u x v): q v q* without building q*. */
    de_vec3 u = de__v3(q.x, q.y, q.z);
    de_vec3 t = de_v3_scale(de_v3_cross(u, v), 2.0f);
    return de_v3_add(de_v3_add(v, de_v3_scale(t, q.w)), de_v3_cross(u, t));
}

static de_quat de__quat_lerp_norm(de_quat a, de_quat b, f32 t) {
    return de_quat_normalize(de__quat(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                                      a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t));
}

de_quat de_quat_nlerp(de_quat a, de_quat b, f32 t) {
    if (de_quat_dot(a, b) < 0.0f)
        b = de__quat(-b.x, -b.y, -b.z, -b.w);
    return de__quat_lerp_norm(a, b, t);
}

de_quat de_quat_slerp(de_quat a, de_quat b, f32 t) {
    f32 d = de_quat_dot(a, b);
    if (d < 0.0f) { /* q and -q are the same rotation: take the shorter arc */
        b = de__quat(-b.x, -b.y, -b.z, -b.w);
        d = -d;
    }
    if (t == 0.0f)
        return a;
    if (t == 1.0f)
        return b;
    /* Nearly parallel: sin(theta) is tiny and the weights lose precision; lerp is accurate. */
    if (d > 0.9995f)
        return de__quat_lerp_norm(a, b, t);
    f32 theta = acosf(d);
    f32 inv_s = 1.0f / sinf(theta);
    f32 wa = sinf((1.0f - t) * theta) * inv_s;
    f32 wb = sinf(t * theta) * inv_s;
    return de__quat(wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z,
                    wa * a.w + wb * b.w);
}

de_mat3 de_quat_to_m3(de_quat q) {
    f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    de_mat3 m;
    /* column 0 */
    m.m[0] = 1.0f - 2.0f * (yy + zz);
    m.m[1] = 2.0f * (xy + wz);
    m.m[2] = 2.0f * (xz - wy);
    /* column 1 */
    m.m[3] = 2.0f * (xy - wz);
    m.m[4] = 1.0f - 2.0f * (xx + zz);
    m.m[5] = 2.0f * (yz + wx);
    /* column 2 */
    m.m[6] = 2.0f * (xz + wy);
    m.m[7] = 2.0f * (yz - wx);
    m.m[8] = 1.0f - 2.0f * (xx + yy);
    return m;
}

de_quat de_quat_from_m3(const de_mat3 *m) {
    /* Shepperd: divide by the largest of 4w^2, 4x^2, 4y^2, 4z^2 so the sqrt argument stays far
     * from zero. Element (r, c) is m[c * 3 + r]. */
    f32 r00 = m->m[0], r10 = m->m[1], r20 = m->m[2];
    f32 r01 = m->m[3], r11 = m->m[4], r21 = m->m[5];
    f32 r02 = m->m[6], r12 = m->m[7], r22 = m->m[8];
    f32 trace = r00 + r11 + r22;
    de_quat q;
    if (trace > 0.0f) {
        f32 s = sqrtf(trace + 1.0f) * 2.0f; /* 4w */
        q = de__quat((r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s);
    } else if (r00 > r11 && r00 > r22) {
        f32 s = sqrtf(1.0f + r00 - r11 - r22) * 2.0f; /* 4x */
        q = de__quat(0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s);
    } else if (r11 > r22) {
        f32 s = sqrtf(1.0f + r11 - r00 - r22) * 2.0f; /* 4y */
        q = de__quat((r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s);
    } else {
        f32 s = sqrtf(1.0f + r22 - r00 - r11) * 2.0f; /* 4z */
        q = de__quat((r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s);
    }
    return de_quat_normalize(q);
}
