/* triangulate.c: the read-only shape surface accessor (DENG-76), for code that needs a shape's
 * triangles, such as navmesh generation (src/gameplay/nav/).
 *
 * Every kind gives a closed or open surface in the shape's local frame, front faces
 * counter-clockwise seen from outside (cross(b - a, c - a) points out; up for a heightfield):
 *   sphere, capsule, cylinder  8 segments around Z, on rings at 45 degree steps. The ring radius
 *                              is scaled by 1 / cos(22.5 deg), so the polygon contains the circle;
 *   box                        12 triangles;
 *   convex                     a fan per hull face;
 *   mesh                       its triangles as given;
 *   heightfield                two per cell, holes skipped;
 *   compound                   each child's triangles, placed with the child transform.
 * No trigonometry and no libm: the module is deterministic (SPEC §5.7). */
#include "physics/physics_internal.h"
#include "physics/shapes/shape_internal.h"

#include <dengine/de_physics.h>

#define TRI_SEGMENTS 8u
/* 1 / cos(22.5 deg): an 8-gon with this circumradius contains the unit circle. */
#define TRI_RING_SCALE 1.0823922f

/* cos and sin of k * 45 degrees, k = 0..7. */
static const f32 k_cos8[TRI_SEGMENTS] = {1.0f, 0.70710678f, 0.0f, -0.70710678f,
                                         -1.0f, -0.70710678f, 0.0f, 0.70710678f};
static const f32 k_sin8[TRI_SEGMENTS] = {0.0f, 0.70710678f, 1.0f, 0.70710678f,
                                         0.0f, -0.70710678f, -1.0f, -0.70710678f};

typedef struct tri_out {
    de_shape_triangle_fn fn;
    void *user;
    bool placed; /* apply rot / pos (compound child) */
    de_quat rot;
    de_vec3 pos;
} tri_out;

static void emit(const tri_out *o, de_vec3 a, de_vec3 b, de_vec3 c) {
    de_vec3 t[3] = {a, b, c};
    if (o->placed)
        for (u32 i = 0; i < 3; ++i)
            t[i] = de__v3_add(de__quat_rotate(o->rot, t[i]), o->pos);
    o->fn(o->user, t);
}

/* A surface of revolution around Z through `n` rings (z, radius), bottom to top. A ring of
 * radius 0 is a pole: its quads become triangles. */
static void emit_lathe(const tri_out *o, const f32 (*rings)[2], u32 n) {
    for (u32 i = 0; i + 1 < n; ++i) {
        const f32 z0 = rings[i][0], r0 = rings[i][1] * TRI_RING_SCALE;
        const f32 z1 = rings[i + 1][0], r1 = rings[i + 1][1] * TRI_RING_SCALE;
        for (u32 k = 0; k < TRI_SEGMENTS; ++k) {
            const u32 k1 = (k + 1u) % TRI_SEGMENTS;
            const de_vec3 p00 = de__v3(r0 * k_cos8[k], r0 * k_sin8[k], z0);
            const de_vec3 p01 = de__v3(r0 * k_cos8[k1], r0 * k_sin8[k1], z0);
            const de_vec3 p11 = de__v3(r1 * k_cos8[k1], r1 * k_sin8[k1], z1);
            const de_vec3 p10 = de__v3(r1 * k_cos8[k], r1 * k_sin8[k], z1);
            if (r0 > 0.0f)
                emit(o, p00, p01, p11);
            if (r1 > 0.0f)
                emit(o, p00, p11, p10);
        }
    }
}

static void emit_box(const tri_out *o, de_vec3 h) {
    /* Corner i: bit 0 = +x, bit 1 = +y, bit 2 = +z. Faces counter-clockwise from outside. */
    static const u8 k_faces[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4},
                                     {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
    de_vec3 c[8];
    for (u32 i = 0; i < 8; ++i)
        c[i] = de__v3((i & 1u) ? h.x : -h.x, (i & 2u) ? h.y : -h.y, (i & 4u) ? h.z : -h.z);
    for (u32 f = 0; f < 6; ++f) {
        emit(o, c[k_faces[f][0]], c[k_faces[f][1]], c[k_faces[f][2]]);
        emit(o, c[k_faces[f][0]], c[k_faces[f][2]], c[k_faces[f][3]]);
    }
}

static void emit_hull(const tri_out *o, const de__hull *h) {
    for (u32 f = 0; f < h->face_count; ++f) {
        const de__hull_face *face = &h->faces[f];
        if (face->edge_count < 3)
            continue;
        const de_vec3 a = h->vertices[h->edges[face->first_edge].origin];
        /* Wind each fan triangle so its normal agrees with the face plane. */
        for (u32 e = 1; e + 1 < face->edge_count; ++e) {
            const de_vec3 b = h->vertices[h->edges[face->first_edge + e].origin];
            const de_vec3 c = h->vertices[h->edges[face->first_edge + e + 1].origin];
            const de_vec3 nrm = de__v3_cross(de__v3_sub(b, a), de__v3_sub(c, a));
            if (de__v3_dot(nrm, h->planes[f].n) >= 0.0f)
                emit(o, a, b, c);
            else
                emit(o, a, c, b);
        }
    }
}

static void emit_shape(const tri_out *o, const de__shape *s) {
    switch (s->kind) {
    case DE_SHAPE_SPHERE: {
        const f32 r = s->u.sphere.radius;
        const f32 rings[5][2] = {{-r, 0.0f},
                                 {-r * 0.70710678f, r * 0.70710678f},
                                 {0.0f, r},
                                 {r * 0.70710678f, r * 0.70710678f},
                                 {r, 0.0f}};
        emit_lathe(o, rings, 5);
        break;
    }
    case DE_SHAPE_CAPSULE: {
        const f32 r = s->u.capsule.radius, hh = s->u.capsule.half_height;
        const f32 rings[6][2] = {{-hh - r, 0.0f},
                                 {-hh - r * 0.70710678f, r * 0.70710678f},
                                 {-hh, r},
                                 {hh, r},
                                 {hh + r * 0.70710678f, r * 0.70710678f},
                                 {hh + r, 0.0f}};
        emit_lathe(o, rings, 6);
        break;
    }
    case DE_SHAPE_CYLINDER: {
        const f32 r = s->u.cylinder.radius, hh = s->u.cylinder.half_height;
        const f32 rings[4][2] = {{-hh, 0.0f}, {-hh, r}, {hh, r}, {hh, 0.0f}};
        emit_lathe(o, rings, 4);
        break;
    }
    case DE_SHAPE_BOX:
        emit_box(o, s->u.box.half_extents);
        break;
    case DE_SHAPE_CONVEX:
        emit_hull(o, &s->u.hull);
        break;
    case DE_SHAPE_MESH: {
        const de__mesh *m = &s->u.mesh;
        for (u32 t = 0; t < m->tri_count; ++t)
            emit(o, m->vertices[m->indices[3u * t]], m->vertices[m->indices[3u * t + 1u]],
                 m->vertices[m->indices[3u * t + 2u]]);
        break;
    }
    case DE_SHAPE_HEIGHTFIELD: {
        const de__heightfield *hf = &s->u.hf;
        for (u32 row = 0; row + 1 < hf->rows; ++row)
            for (u32 col = 0; col + 1 < hf->cols; ++col) {
                if (de__hf_cell_is_hole(hf, col, row))
                    continue;
                de_vec3 tri[2][3];
                de__hf_cell_triangles(hf, col, row, tri);
                emit(o, tri[0][0], tri[0][1], tri[0][2]);
                emit(o, tri[1][0], tri[1][1], tri[1][2]);
            }
        break;
    }
    case DE_SHAPE_COMPOUND:
        /* Children are never compounds (one nesting level), so this does not recurse twice. */
        for (u32 i = 0; i < s->u.compound.child_count; ++i) {
            const de__compound_child *c = &s->u.compound.children[i];
            tri_out child = *o;
            child.placed = true;
            child.rot = c->rotation;
            child.pos = c->position;
            emit_shape(&child, c->shape);
        }
        break;
    }
}

de_result de_shape_triangulate(const de_physics_world *p, de_shape s, de_shape_triangle_fn fn,
                               void *user) {
    const de__shape *sh = de__shape_get(p, s);
    if (sh == NULL || fn == NULL)
        return DE_ERR_INVALID_ARGUMENT;
    tri_out o;
    o.fn = fn;
    o.user = user;
    o.placed = false;
    o.rot = (de_quat){0.0f, 0.0f, 0.0f, 1.0f};
    o.pos = de__v3(0.0f, 0.0f, 0.0f);
    emit_shape(&o, sh);
    return DE_OK;
}
