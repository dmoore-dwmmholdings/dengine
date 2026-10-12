/*
 * sat_box.c: the box polytope template and box-box SAT over 15 axes (DENG-134).
 *
 * Box template: vertex v has the signs (+ when the bit is set) x: bit 0, y: bit 1, z: bit 2.
 * Faces 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z, each four half-edges counter-clockwise seen from
 * outside (generated once and checked by physics_sat.box_template).
 */
#include "physics/narrow/narrowphase.h"

static const de__hull_face box_faces[6] = {{0, 4}, {4, 4}, {8, 4}, {12, 4}, {16, 4}, {20, 4}};

/* {next, twin, origin, face} */
static const de__hull_edge box_edges[24] = {
    {1, 22, 1, 0},  {2, 10, 3, 0},  {3, 17, 7, 0},  {0, 13, 5, 0},  /* +X: 1 3 7 5 */
    {5, 15, 0, 1},  {6, 19, 4, 1},  {7, 8, 6, 1},   {4, 20, 2, 1},  /* -X: 0 4 6 2 */
    {9, 6, 2, 2},   {10, 18, 6, 2}, {11, 1, 7, 2},  {8, 21, 3, 2},  /* +Y: 2 6 7 3 */
    {13, 23, 0, 3}, {14, 3, 1, 3},  {15, 16, 5, 3}, {12, 4, 4, 3},  /* -Y: 0 1 5 4 */
    {17, 14, 4, 4}, {18, 2, 5, 4},  {19, 9, 7, 4},  {16, 5, 6, 4},  /* +Z: 4 5 7 6 */
    {21, 7, 0, 5},  {22, 11, 2, 5}, {23, 0, 3, 5},  {20, 12, 1, 5}, /* -Z: 0 2 3 1 */
};

void de__np_box_poly_init(de__np_box_poly *bp, de_vec3 he) {
    for (u32 v = 0; v < 8; ++v)
        bp->v[v] =
            de__v3((v & 1u) ? he.x : -he.x, (v & 2u) ? he.y : -he.y, (v & 4u) ? he.z : -he.z);
    const f32 e[3] = {he.x, he.y, he.z};
    for (u32 f = 0; f < 6; ++f) {
        const u32 axis = f / 2u;
        const f32 s = (f & 1u) ? -1.0f : 1.0f;
        de_vec3 n = de__v3(0.0f, 0.0f, 0.0f);
        if (axis == 0)
            n.x = s;
        else if (axis == 1)
            n.y = s;
        else
            n.z = s;
        bp->p[f].n = n;
        bp->p[f].d = -e[axis];
    }
    bp->poly.verts = bp->v;
    bp->poly.planes = bp->p;
    bp->poly.faces = box_faces;
    bp->poly.edges = box_edges;
    bp->poly.vcount = 8;
    bp->poly.fcount = 6;
    bp->poly.ecount = 24;
    bp->poly.centroid = de__v3(0.0f, 0.0f, 0.0f);
    bp->poly.flat = false;
}

u32 de__np_box_edge(u32 v0, u32 v1) {
    for (u32 e = 0; e < 24; ++e) {
        const u32 o = box_edges[e].origin, d = box_edges[box_edges[e].next].origin;
        if ((o == v0 && d == v1) || (o == v1 && d == v0)) {
            const u32 t = box_edges[e].twin;
            return t < e ? t : e;
        }
    }
    return 0;
}

/* ---- Box-box --------------------------------------------------------------------------- */

static f32 comp(de_vec3 v, u32 i) {
    return i == 0 ? v.x : (i == 1 ? v.y : v.z);
}

bool de__np_box_box(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                    de__np_manifold *out) {
    out->count = 0;
    const de_vec3 ea = a->u.box.half_extents, eb = b->u.box.half_extents;
    const f32 *m = rel->r.m; /* R_ij = dot(A axis i, B axis j) = m[j * 3 + i] */
    const de_vec3 t = rel->t;
    const f32 spec = rel->spec;
    f32 absr[9];
    for (u32 k = 0; k < 9; ++k)
        absr[k] = de__absf(m[k]);

    /* Face axes of A. */
    f32 sep_a = -3.0e38f;
    u32 face_a = 0;
    for (u32 i = 0; i < 3; ++i) {
        const f32 rb = eb.x * absr[0 * 3 + i] + eb.y * absr[1 * 3 + i] + eb.z * absr[2 * 3 + i];
        const f32 ti = comp(t, i);
        const f32 s = de__absf(ti) - comp(ea, i) - rb;
        if (s > sep_a) {
            sep_a = s;
            face_a = 2u * i + (ti < 0.0f ? 1u : 0u);
        }
    }
    if (sep_a > spec)
        return false;
    /* Face axes of B. */
    f32 sep_b = -3.0e38f;
    u32 face_b = 0;
    for (u32 j = 0; j < 3; ++j) {
        const f32 tb = m[j * 3 + 0] * t.x + m[j * 3 + 1] * t.y + m[j * 3 + 2] * t.z;
        const f32 ra = ea.x * absr[j * 3 + 0] + ea.y * absr[j * 3 + 1] + ea.z * absr[j * 3 + 2];
        const f32 s = de__absf(tb) - ra - comp(eb, j);
        if (s > sep_b) {
            sep_b = s;
            /* B's face toward A: normal -sign(tb) along axis j. */
            face_b = 2u * j + (tb >= 0.0f ? 1u : 0u);
        }
    }
    if (sep_b > spec)
        return false;
    /* Edge axes A_i x B_j. */
    f32 sep_e = -3.0e38f;
    u32 ei = 0, ej = 0;
    de_vec3 axis = de__v3(0.0f, 0.0f, 0.0f);
    bool have_edge = false;
    for (u32 i = 0; i < 3; ++i) {
        for (u32 j = 0; j < 3; ++j) {
            const de_vec3 ai =
                de__v3(i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f);
            const de_vec3 bj = de__v3(m[j * 3 + 0], m[j * 3 + 1], m[j * 3 + 2]);
            de_vec3 l = de__v3_cross(ai, bj);
            if (de__np_len2(l) < 1e-6f)
                continue; /* parallel edges: covered by the face axes */
            l = de__np_normalize(l, l);
            const f32 ra = ea.x * de__absf(l.x) + ea.y * de__absf(l.y) + ea.z * de__absf(l.z);
            f32 rb = 0.0f;
            for (u32 k = 0; k < 3; ++k) {
                const de_vec3 bk = de__v3(m[k * 3 + 0], m[k * 3 + 1], m[k * 3 + 2]);
                rb += comp(eb, k) * de__absf(de__v3_dot(bk, l));
            }
            const f32 tl = de__v3_dot(t, l);
            const f32 s = de__absf(tl) - ra - rb;
            if (s > sep_e) {
                sep_e = s;
                ei = i;
                ej = j;
                axis = tl < 0.0f ? de__v3(-l.x, -l.y, -l.z) : l;
                have_edge = true;
            }
        }
    }
    if (have_edge && sep_e > spec)
        return false;

    de__np_box_poly pa, pb;
    de__np_box_poly_init(&pa, ea);
    de__np_box_poly_init(&pb, eb);
    de_vec3 bverts[8];
    for (u32 v = 0; v < 8; ++v)
        bverts[v] = de__np_to_a(rel, pb.v[v]);

    const bool ref_b = sep_b > sep_a + DE__NP_FACE_TOL;
    if (have_edge && sep_e > (sep_a > sep_b ? sep_a : sep_b) + DE__NP_EDGE_TOL) {
        /* A's edge parallel to axis ei at A's support along the axis; B's edge parallel to its
         * axis ej at B's support against it. */
        u32 va = 0;
        for (u32 k = 0; k < 3; ++k)
            if (k != ei && comp(axis, k) >= 0.0f)
                va |= 1u << k;
        const de_vec3 lb = de__np_dir_to_b(rel, axis);
        u32 vb = 0;
        for (u32 k = 0; k < 3; ++k)
            if (k != ej && -comp(lb, k) >= 0.0f)
                vb |= 1u << k;
        const u32 ea_id = de__np_box_edge(va, va | (1u << ei));
        const u32 eb_id = de__np_box_edge(vb, vb | (1u << ej));
        de__np_edge_contact(&pa.poly, ea_id, &pb.poly, bverts, eb_id, axis, rel, out);
        return out->count > 0;
    }
    de__np_face_contact(&pa.poly, &pb.poly, bverts, rel, !ref_b, ref_b ? face_b : face_a, out);
    return out->count > 0;
}
