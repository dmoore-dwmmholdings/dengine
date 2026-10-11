/* transform.c: de_transform, the large-world TRS transform (SPEC §5.5, DENG-244).
 *
 * Position f64, rotation and scale f32. Everything that touches a position runs in f64,
 * including the quaternion rotation: rotating a 100 km vector in f32 would be off by
 * millimetres. The rotation is normalized in f64 first, so an f32 quaternion whose length is
 * 1 +- 1e-7 does not stretch the result by 1e-7 (1 cm at 100 km).
 *
 * mul, inverse, point, dir and lerp use only + - * / and sqrt, and this file is compiled with
 * the deterministic flags (src/core/CMakeLists.txt: no FP contraction), so simulation code may
 * call them. They do not call the fast de_quat_* functions (those may dispatch to SIMD with FMA).
 * Only from_affine and the *_relative matrix builders call fast math (de_m4_trs,
 * de_affine_from_trs, de_m4_decompose); they serve rendering and tools. */
#include <dengine/de_math.h>

#include <math.h> /* sqrt only */

/* f64 quaternion, internal. */
typedef struct de__dquat {
    f64 x, y, z, w;
} de__dquat;

static de_dvec3 de__dv3(f64 x, f64 y, f64 z) {
    de_dvec3 r = {x, y, z};
    return r;
}

static de_quat de__quat(f32 x, f32 y, f32 z, f32 w) {
    de_quat q;
    q.x = x;
    q.y = y;
    q.z = z;
    q.w = w;
    return q;
}

static de__dquat de__dq(f64 x, f64 y, f64 z, f64 w) {
    de__dquat q = {x, y, z, w};
    return q;
}

static de_quat de__dq_round(de__dquat q) {
    return de__quat((f32)q.x, (f32)q.y, (f32)q.z, (f32)q.w);
}

/* q / |q| in f64. A zero (or NaN) quaternion gives the identity, like de_quat_normalize.
 * normalize(conj(q)) == conj(normalize(q)) bit for bit (only signs differ), which
 * de_transform_inverse relies on. */
static de__dquat de__dq_normalize(f64 x, f64 y, f64 z, f64 w) {
    f64 n2 = x * x + y * y + z * z + w * w;
    if (!(n2 > 0.0))
        return de__dq(0.0, 0.0, 0.0, 1.0);
    f64 inv = 1.0 / sqrt(n2);
    return de__dq(x * inv, y * inv, z * inv, w * inv);
}

static de__dquat de__dq_from(de_quat q) {
    return de__dq_normalize((f64)q.x, (f64)q.y, (f64)q.z, (f64)q.w);
}

/* a * b (Hamilton): b first, then a. Same formula as de_quat_mul. */
static de__dquat de__dq_mul(de__dquat a, de__dquat b) {
    return de__dq(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                  a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                  a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                  a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}

static de_dvec3 de__dv3_cross(de_dvec3 a, de_dvec3 b) {
    return de__dv3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

/* v rotated by the unit quaternion q: v + w*t + u x t with u = (x, y, z), t = 2 (u x v). */
static de_dvec3 de__dq_rotate(de__dquat q, de_dvec3 v) {
    de_dvec3 u = de__dv3(q.x, q.y, q.z);
    de_dvec3 t = de__dv3_cross(u, v);
    t = de__dv3(t.x * 2.0, t.y * 2.0, t.z * 2.0);
    de_dvec3 c = de__dv3_cross(u, t);
    return de__dv3(v.x + q.w * t.x + c.x, v.y + q.w * t.y + c.y, v.z + q.w * t.z + c.z);
}

/* scale (f32) * p (f64), component-wise, in f64. */
static de_dvec3 de__dv3_scale_by(de_dvec3 p, de_vec3 s) {
    return de__dv3(p.x * (f64)s.x, p.y * (f64)s.y, p.z * (f64)s.z);
}

/* rotate(q, s * p): the linear part of a transform applied to p. Every function that maps a
 * position goes through this one helper, so mul(inverse(t), t) cancels exactly. */
static de_dvec3 de__linear(de_quat q, de_vec3 s, de_dvec3 p) {
    return de__dq_rotate(de__dq_from(q), de__dv3_scale_by(p, s));
}

de_transform de_transform_identity(void) {
    de_transform t;
    t.position = de__dv3(0.0, 0.0, 0.0);
    t.rotation = de__quat(0.0f, 0.0f, 0.0f, 1.0f);
    t.scale.x = 1.0f;
    t.scale.y = 1.0f;
    t.scale.z = 1.0f;
    return t;
}

de_transform de_transform_mul(const de_transform *parent, const de_transform *child) {
    de_transform r;
    de_dvec3 off = de__linear(parent->rotation, parent->scale, child->position);
    r.position =
        de__dv3(parent->position.x + off.x, parent->position.y + off.y, parent->position.z + off.z);
    r.rotation =
        de__dq_round(de__dq_mul(de__dq_from(parent->rotation), de__dq_from(child->rotation)));
    /* No shear: the scale is composed component-wise (see the note in de_math.h). */
    r.scale.x = parent->scale.x * child->scale.x;
    r.scale.y = parent->scale.y * child->scale.y;
    r.scale.z = parent->scale.z * child->scale.z;
    return r;
}

de_transform de_transform_inverse(const de_transform *t) {
    de_transform r;
    /* conj is a sign flip, exact in f32, so mul(inverse(t), t) rebuilds the same f64 rotation. */
    r.rotation = de__quat(-t->rotation.x, -t->rotation.y, -t->rotation.z, t->rotation.w);
    r.scale.x = 1.0f / t->scale.x;
    r.scale.y = 1.0f / t->scale.y;
    r.scale.z = 1.0f / t->scale.z;
    de_dvec3 p = de__linear(r.rotation, r.scale, t->position);
    r.position = de__dv3(-p.x, -p.y, -p.z);
    return r;
}

de_dvec3 de_transform_point(const de_transform *t, de_dvec3 p) {
    de_dvec3 off = de__linear(t->rotation, t->scale, p);
    return de__dv3(t->position.x + off.x, t->position.y + off.y, t->position.z + off.z);
}

de_vec3 de_transform_dir(const de_transform *t, de_vec3 d) {
    de_dvec3 r = de__linear(t->rotation, t->scale, de__dv3((f64)d.x, (f64)d.y, (f64)d.z));
    de_vec3 out = {(f32)r.x, (f32)r.y, (f32)r.z};
    return out;
}

de_transform de_transform_lerp(const de_transform *a, const de_transform *b, f32 t) {
    de_transform r;
    r.position = de_dv3_lerp(a->position, b->position, (f64)t);

    /* nlerp along the shortest path: q and -q are the same rotation. */
    de_quat qa = a->rotation, qb = b->rotation;
    f64 ax = qa.x, ay = qa.y, az = qa.z, aw = qa.w;
    f64 bx = qb.x, by = qb.y, bz = qb.z, bw = qb.w;
    if (ax * bx + ay * by + az * bz + aw * bw < 0.0) {
        bx = -bx;
        by = -by;
        bz = -bz;
        bw = -bw;
    }
    f64 td = (f64)t, ud = 1.0 - td;
    r.rotation = de__dq_round(de__dq_normalize(ax * ud + bx * td, ay * ud + by * td,
                                               az * ud + bz * td, aw * ud + bw * td));

    f32 u = 1.0f - t;
    r.scale.x = a->scale.x * u + b->scale.x * t;
    r.scale.y = a->scale.y * u + b->scale.y * t;
    r.scale.z = a->scale.z * u + b->scale.z * t;
    return r;
}

bool de_transform_from_affine(const de_affine *a, de_dvec3 origin, de_transform *out) {
    de_mat4 m = de_affine_to_m4(a);
    de_vec3 t, s;
    de_quat r;
    bool ok = de_m4_decompose(&m, &t, &r, &s);
    out->position = de__dv3(origin.x + (f64)t.x, origin.y + (f64)t.y, origin.z + (f64)t.z);
    out->rotation = r;
    out->scale = s;
    return ok;
}

de_mat4 de_transform_to_m4_relative(const de_transform *t, de_dvec3 origin) {
    return de_m4_trs(de_dv3_relative(t->position, origin), t->rotation, t->scale);
}

de_affine de_transform_to_affine_relative(const de_transform *t, de_dvec3 origin) {
    return de_affine_from_trs(de_dv3_relative(t->position, origin), t->rotation, t->scale);
}
