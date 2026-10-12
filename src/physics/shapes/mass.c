/*
 * mass.c: mass properties per unit density (DENG-132). Analytic for the primitives, tetrahedra
 * for hulls (covariance form), parallel axis for compounds. Everything in f64.
 *
 * Inertia tensors are about the center of mass, in the shape's local axes, for density 1 (so the
 * volume is the mass). Capsules and cylinders lie along local Z.
 */
#include "physics/shapes/shape_internal.h"

#define DE__PI_D 3.14159265358979323846

static void diag(de__unit_mass *out, f64 vol, f64 ixx, f64 iyy, f64 izz) {
    memset(out, 0, sizeof *out);
    out->volume = vol;
    out->inertia[0] = ixx;
    out->inertia[4] = iyy;
    out->inertia[8] = izz;
}

void de__mass_sphere(f64 r, de__unit_mass *out) {
    const f64 v = 4.0 / 3.0 * DE__PI_D * r * r * r;
    const f64 i = 0.4 * v * r * r;
    diag(out, v, i, i, i);
}

void de__mass_capsule(f64 r, f64 h, de__unit_mass *out) {
    /* Cylinder of height 2h plus two hemispheres. Each hemisphere's center of mass is 3r/8 from
     * its flat face; moving its inertia from there to the capsule center (parallel axis) gives
     * ms * (2r^2/5 + h^2 + 3hr/4) for the pair about a transverse axis. */
    const f64 mc = DE__PI_D * r * r * 2.0 * h;
    const f64 ms = 4.0 / 3.0 * DE__PI_D * r * r * r;
    const f64 izz = mc * r * r * 0.5 + ms * 0.4 * r * r;
    const f64 ixx =
        mc * (4.0 * h * h / 12.0 + r * r * 0.25) + ms * (0.4 * r * r + h * h + 0.75 * h * r);
    diag(out, mc + ms, ixx, ixx, izz);
}

void de__mass_box(de_dvec3 e, de__unit_mass *out) {
    const f64 v = 8.0 * e.x * e.y * e.z;
    const f64 x2 = e.x * e.x, y2 = e.y * e.y, z2 = e.z * e.z;
    diag(out, v, v / 3.0 * (y2 + z2), v / 3.0 * (x2 + z2), v / 3.0 * (x2 + y2));
}

void de__mass_cylinder(f64 r, f64 h, de__unit_mass *out) {
    const f64 v = DE__PI_D * r * r * 2.0 * h;
    const f64 ixx = v * (3.0 * r * r + 4.0 * h * h) / 12.0;
    diag(out, v, ixx, ixx, v * r * r * 0.5);
}

/* Inertia (about the origin of the covariance's frame) from a covariance C = integral x x^T:
 * I = trace(C) * Id - C. */
static void inertia_from_cov(const f64 c[9], f64 out[9]) {
    const f64 tr = c[0] + c[4] + c[8];
    for (u32 i = 0; i < 9; ++i)
        out[i] = -c[i];
    out[0] += tr;
    out[4] += tr;
    out[8] += tr;
}

void de__mass_hull(const de__hull *hull, de__unit_mass *out) {
    memset(out, 0, sizeof *out);
    /* Reference point: the vertex centroid, inside the hull, keeps the tetrahedra well-scaled. */
    const de_dvec3 ref = de__d3_from(hull->centroid);
    f64 vol = 0.0;
    de_dvec3 acc = de__d3(0.0, 0.0, 0.0);
    f64 cov[9] = {0};
    for (u32 f = 0; f < hull->face_count; ++f) {
        const de__hull_face *face = &hull->faces[f];
        const u32 e0 = face->first_edge;
        const de_dvec3 a = de__d3_sub(de__d3_from(hull->vertices[hull->edges[e0].origin]), ref);
        for (u32 k = 1; k + 1 < face->edge_count; ++k) {
            const de_dvec3 b =
                de__d3_sub(de__d3_from(hull->vertices[hull->edges[e0 + k].origin]), ref);
            const de_dvec3 c =
                de__d3_sub(de__d3_from(hull->vertices[hull->edges[e0 + k + 1].origin]), ref);
            /* Tetrahedron (ref, a, b, c): det = 6 * signed volume (> 0 for outward faces).
             * Its covariance is det / 120 * (S S^T + a a^T + b b^T + c c^T), S = a + b + c. */
            const f64 det = de__d3_dot(a, de__d3_cross(b, c));
            const de_dvec3 s = de__d3_add(de__d3_add(a, b), c);
            vol += det / 6.0;
            acc = de__d3_add(acc, de__d3_scale(s, det / 24.0));
            const f64 va[3] = {a.x, a.y, a.z}, vb[3] = {b.x, b.y, b.z}, vc[3] = {c.x, c.y, c.z};
            const f64 vs[3] = {s.x, s.y, s.z};
            for (u32 col = 0; col < 3; ++col)
                for (u32 row = 0; row < 3; ++row)
                    cov[col * 3 + row] += det / 120.0 *
                                          (vs[row] * vs[col] + va[row] * va[col] +
                                           vb[row] * vb[col] + vc[row] * vc[col]);
        }
    }
    if (!(vol > 0.0))
        return;
    const de_dvec3 com_rel = de__d3_scale(acc, 1.0 / vol);
    /* Move the covariance from the reference point to the center of mass. */
    const f64 cr[3] = {com_rel.x, com_rel.y, com_rel.z};
    for (u32 col = 0; col < 3; ++col)
        for (u32 row = 0; row < 3; ++row)
            cov[col * 3 + row] -= vol * cr[row] * cr[col];
    out->volume = vol;
    out->com = de__d3_add(ref, com_rel);
    inertia_from_cov(cov, out->inertia);
}

void de__mass_compound_add(de__unit_mass *acc, const de__unit_mass *child, de_vec3 pos,
                           de_quat rot) {
    if (!(child->volume > 0.0))
        return;
    const de__dmat3 r = de__dmat3_from_quat(rot);
    const de_dvec3 c = de__d3_add(de__d3_from(pos), de__dmat3_mul(&r, child->com));
    /* R I R^T, then the parallel axis term to the compound origin: V (|c|^2 Id - c c^T). */
    f64 ri[9];
    for (u32 col = 0; col < 3; ++col)
        for (u32 row = 0; row < 3; ++row) {
            f64 s = 0.0;
            for (u32 k = 0; k < 3; ++k)
                s += r.m[k * 3 + row] * child->inertia[col * 3 + k];
            ri[col * 3 + row] = s;
        }
    const f64 v = child->volume;
    const f64 cv[3] = {c.x, c.y, c.z};
    const f64 c2 = c.x * c.x + c.y * c.y + c.z * c.z;
    for (u32 col = 0; col < 3; ++col)
        for (u32 row = 0; row < 3; ++row) {
            f64 s = 0.0;
            for (u32 k = 0; k < 3; ++k)
                s += ri[k * 3 + row] * r.m[k * 3 + col]; /* (R I) R^T */
            acc->inertia[col * 3 + row] += s + v * ((row == col ? c2 : 0.0) - cv[row] * cv[col]);
        }
    acc->volume += v;
    acc->com = de__d3_add(acc->com, de__d3_scale(c, v)); /* volume-weighted sum until finish */
}

void de__mass_compound_finish(de__unit_mass *acc) {
    if (!(acc->volume > 0.0)) {
        memset(acc, 0, sizeof *acc);
        return;
    }
    const de_dvec3 c = de__d3_scale(acc->com, 1.0 / acc->volume);
    const f64 cv[3] = {c.x, c.y, c.z};
    const f64 c2 = c.x * c.x + c.y * c.y + c.z * c.z;
    for (u32 col = 0; col < 3; ++col)
        for (u32 row = 0; row < 3; ++row)
            acc->inertia[col * 3 + row] -=
                acc->volume * ((row == col ? c2 : 0.0) - cv[row] * cv[col]);
    acc->com = c;
}

void de__unit_mass_scale(const de__unit_mass *u, f64 density, de_mass_props *out) {
    out->mass = (f32)(u->volume * density);
    out->center = de__v3((f32)u->com.x, (f32)u->com.y, (f32)u->com.z);
    for (u32 i = 0; i < 9; ++i)
        out->inertia.m[i] = (f32)(u->inertia[i] * density);
}

void de__mat3_inverse_d(const f64 m[9], f64 out[9]) {
    /* Column-major: element (r, c) = m[c * 3 + r]. Cofactor expansion. */
    const f64 a = m[0], b = m[3], c = m[6];
    const f64 d = m[1], e = m[4], f = m[7];
    const f64 g = m[2], h = m[5], i = m[8];
    const f64 A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const f64 det = a * A + b * B + c * C;
    if (det == 0.0 || !de__f64_finite(det)) {
        memset(out, 0, sizeof(f64) * 9);
        return;
    }
    const f64 k = 1.0 / det;
    /* inverse(r, c) = cofactor(c, r) / det */
    out[0] = A * k;
    out[1] = B * k;
    out[2] = C * k;
    out[3] = -(b * i - c * h) * k;
    out[4] = (a * i - c * g) * k;
    out[5] = -(a * h - b * g) * k;
    out[6] = (b * f - c * e) * k;
    out[7] = -(a * f - c * d) * k;
    out[8] = (a * e - b * d) * k;
}
