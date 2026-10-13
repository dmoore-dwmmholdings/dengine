/*
 * mass2d.c: 2D mass properties per unit density (DENG-252), all in f64. Inertia is the polar
 * moment about the center of mass for density 1 (so the area is the mass).
 *
 * Rounded polygons are exact: the core polygon, plus a strip of width `radius` outside each
 * edge, plus a circular sector of radius `radius` at each vertex (between the normals of its two
 * edges). These pieces do not overlap and together are the rounded shape.
 */
#include "physics/2d/p2d_internal.h"

#define DE__PI_D 3.14159265358979323846

/* Running sums about a reference point o: area, first moment (integral of x - o) and polar
 * second moment (integral of |x - o|^2). */
typedef struct macc {
    de__d2 o;
    f64 area;
    de__d2 first;
    f64 polar;
} macc;

/* Signed triangle (o, a, b) with a, b given relative to o. */
static void acc_triangle(macc *m, de__d2 e1, de__d2 e2) {
    const f64 d = de__d2_cross(e1, e2);
    m->area += 0.5 * d;
    m->first = de__d2_add(m->first, de__d2_scale(de__d2_add(e1, e2), d / 6.0));
    m->polar += d / 12.0 * (de__d2_dot(e1, e1) + de__d2_dot(e1, e2) + de__d2_dot(e2, e2));
}

/* A closed polygon q[0..n) (counter-clockwise adds, clockwise subtracts) by its signed fan. */
static void acc_polygon(macc *m, const de__d2 *q, u32 n) {
    for (u32 i = 0; i < n; ++i)
        acc_triangle(m, de__d2_sub(q[i], m->o), de__d2_sub(q[(i + 1u) % n], m->o));
}

/* A sector at v of radius r and half angle `half` around the unit bisector b. About v: area
 * half * r^2, first moment b * 2/3 r^3 sin(half), polar moment half * r^4 / 2; moved to o. */
static void acc_sector(macc *m, de__d2 v, de__d2 b, f64 r, f64 half, f64 sin_half) {
    const f64 a = half * r * r;
    const de__d2 fv = de__d2_scale(b, 2.0 / 3.0 * r * r * r * sin_half);
    const f64 pv = 0.5 * half * r * r * r * r;
    const de__d2 d = de__d2_sub(v, m->o);
    m->area += a;
    m->first = de__d2_add(m->first, de__d2_add(fv, de__d2_scale(d, a)));
    m->polar += pv + 2.0 * de__d2_dot(d, fv) + a * de__d2_dot(d, d);
}

static void acc_finish(const macc *m, de__unit_mass2d *out) {
    memset(out, 0, sizeof *out);
    if (!(m->area > 0.0))
        return;
    const de__d2 c = de__d2_scale(m->first, 1.0 / m->area);
    out->area = m->area;
    out->com = de__d2_add(m->o, c);
    /* Parallel axis: I_com = I_o - area * |c|^2. */
    out->inertia = m->polar - m->area * de__d2_dot(c, c);
}

void de__mass2d_circle(f64 r, de__d2 center, de__unit_mass2d *out) {
    memset(out, 0, sizeof *out);
    out->area = DE__PI_D * r * r;
    out->com = center;
    out->inertia = 0.5 * out->area * r * r;
}

void de__mass2d_capsule(de__d2 p1, de__d2 p2, f64 r, de__unit_mass2d *out) {
    /* A 2r x L box plus two half discs. Each half disc's centroid is 4r / (3 pi) beyond its flat
     * side, which is L / 2 from the center; the parallel axis theorem gives, for the pair,
     * m_c * (r^2 / 2 + h^2 + 2 h lc) with h = L / 2 and lc = 4r / (3 pi). */
    memset(out, 0, sizeof *out);
    const de__d2 e = de__d2_sub(p2, p1);
    const f64 len = de_dsqrt64(de__d2_dot(e, e));
    const f64 h = 0.5 * len;
    const f64 lc = 4.0 * r / (3.0 * DE__PI_D);
    const f64 circle_area = DE__PI_D * r * r;
    const f64 box_area = 2.0 * r * len;
    out->area = circle_area + box_area;
    out->com = de__d2_scale(de__d2_add(p1, p2), 0.5);
    out->inertia = circle_area * (0.5 * r * r + h * h + 2.0 * h * lc) +
                   box_area * (4.0 * r * r + len * len) / 12.0;
}

void de__mass2d_polygon(const de__polygon2d *poly, de__unit_mass2d *out) {
    const u32 n = poly->count;
    de__d2 v[DE_SHAPE2D_MAX_POLYGON_VERTICES] = {{0.0, 0.0}};
    macc m;
    memset(&m, 0, sizeof m);
    /* Reference: the vertex mean (inside the polygon) keeps the terms well-scaled. */
    for (u32 i = 0; i < n; ++i) {
        v[i] = de__d2_from(poly->vertices[i]);
        m.o = de__d2_add(m.o, v[i]);
    }
    m.o = de__d2_scale(m.o, 1.0 / (f64)n);
    acc_polygon(&m, v, n);

    const f64 r = poly->radius;
    if (r > 0.0) {
        for (u32 i = 0; i < n; ++i) {
            /* Strip outside edge i: v[i], v[i] + r n, v[i+1] + r n, v[i+1] (counter-clockwise). */
            const de__d2 nrm = de__d2_scale(de__d2_from(poly->normals[i]), r);
            const de__d2 a = v[i], b = v[(i + 1u) % n];
            const de__d2 q[4] = {a, de__d2_add(a, nrm), de__d2_add(b, nrm), b};
            acc_polygon(&m, q, 4);
            /* Sector at v[i] between the normals of edges i - 1 and i. */
            const de__d2 n0 = de__d2_from(poly->normals[(i + n - 1u) % n]);
            const de__d2 n1 = de__d2_from(poly->normals[i]);
            const f64 cos_t = de__d2_dot(n0, n1);
            const f64 theta = (f64)de_datan2((f32)de__d2_cross(n0, n1), (f32)cos_t);
            const f64 sin_half = de_dsqrt64(de__maxd(0.0, 0.5 * (1.0 - cos_t)));
            de__d2 bis = de__d2_add(n0, n1);
            bis = de__d2_scale(bis, 1.0 / de_dsqrt64(de__d2_dot(bis, bis)));
            acc_sector(&m, v[i], bis, r, 0.5 * theta, sin_half);
        }
    }
    acc_finish(&m, out);
}

void de__unit_mass2d_scale(const de__unit_mass2d *u, f64 density, de_mass_props2d *out) {
    out->mass = (f32)(u->area * density);
    out->area = (f32)u->area;
    out->center = de__v2((f32)u->com.x, (f32)u->com.y);
    out->inertia = (f32)(u->inertia * density);
}
