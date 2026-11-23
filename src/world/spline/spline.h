/*
 * spline.h: spline components and curve math (DENG-775; the runtime systems are DENG-583).
 *
 * A spline is an entity with DeSpline: a Catmull-Rom or cubic Bezier curve through `count` of
 * the fixed `points` (the entity's local space; tangents are offsets from the position and only
 * Bezier uses them). DeSplineMeshes on the same entity places a mesh along the curve. The layout
 * and the curve rules are the DIDE editor IPC contract, protocol 1.12 (world.splines), and match
 * the DIDE mock engine (mock-engine/src/splines.rs), so the IDE and the engine draw the same curve.
 *
 * Evaluation is in world space: the control points are moved by the entity's world transform
 * first. Catmull-Rom and Bezier curves are affine invariant, so this is the same curve as the
 * local one, transformed, and arc lengths are world metres.
 *
 * The editor service (src/host/editor/editor_spline.c) and, with DENG-583, the runtime placement
 * system use the same de__spline_place(), so `spline.instances` is what renders.
 *
 * Include as "world/spline/spline.h". Internal to the engine.
 */
#ifndef DE_WORLD_SPLINE_H
#define DE_WORLD_SPLINE_H

#include <dengine/de_asset.h>
#include <dengine/de_ecs.h>
#include <dengine/de_math.h>

DE_EXTERN_C_BEGIN

#define DE_SPLINE_MAX_POINTS 64u
/** Polyline points per curve segment. */
#define DE_SPLINE_DENSE 32u
/** The most samples or instances one query returns. */
#define DE_SPLINE_MAX_OUT 4096u
/** The densest polyline: every segment of a closed spline of DE_SPLINE_MAX_POINTS, plus the end. */
#define DE_SPLINE_MAX_POLY (DE_SPLINE_MAX_POINTS * DE_SPLINE_DENSE + 1u)

#define DE_SPLINE_COMPONENT        "DeSpline"
#define DE_SPLINE_MESHES_COMPONENT "DeSplineMeshes"

typedef enum de_spline_kind { DE_SPLINE_CATMULL_ROM = 0, DE_SPLINE_BEZIER = 1 } de_spline_kind;

typedef struct de_spline_point {
    de_vec3 position;    /* local space, m */
    de_vec3 in_tangent;  /* offset from position (Bezier only) */
    de_vec3 out_tangent; /* offset from position (Bezier only) */
} de_spline_point;

typedef struct DeSpline {
    de_spline_kind kind;
    bool closed;
    u32 count; /* points in use; values above DE_SPLINE_MAX_POINTS count as the maximum */
    de_spline_point points[DE_SPLINE_MAX_POINTS];
} DeSpline;

typedef struct DeSplineMeshes {
    de_model mesh;
    f32 spacing; /* m between instances (without deform) */
    f32 length;  /* the mesh's length along +X, m */
    bool deform; /* stretch pieces to fill the curve */
    f32 offset;  /* m from the start */
} DeSplineMeshes;

typedef struct de_spline_ids {
    de_component spline, meshes;
} de_spline_ids;

/** Registers the de_spline_kind enum, de_spline_point and the DeSpline and DeSplineMeshes
 * components in `w` (reusing ones already registered with the same layout). */
de_result de__spline_register(de_world *w, de_spline_ids *out);

/** The curve as a dense polyline, world space, with the arc length at each point. */
typedef struct de_spline_poly {
    de_vec3 p[DE_SPLINE_MAX_POLY];
    f32 at[DE_SPLINE_MAX_POLY];
    u32 n;
    bool closed;
} de_spline_poly;

/** Builds the polyline of `s` moved by `world` (NULL: identity). Fewer than two points give
 * those points at distance 0. */
void de__spline_poly(const DeSpline *s, const de_transform *world, de_spline_poly *out);
static inline f32 de__spline_length(const de_spline_poly *p) {
    return p->n ? p->at[p->n - 1] : 0.0f;
}
/** The point at arc length `d` (clamped to the curve) and the unit direction there. */
void de__spline_at(const de_spline_poly *p, f32 d, de_vec3 *pos, de_vec3 *dir);

/** Samples every `step` metres (`step` >= 0.01; widened so that at most DE_SPLINE_MAX_OUT come
 * back), ending with the curve's end. `pos` and `dir` take 3 floats per sample, `dist` one;
 * each holds DE_SPLINE_MAX_OUT samples. Returns the count. */
u32 de__spline_sample(const de_spline_poly *p, f32 step, f32 *pos, f32 *dir, f32 *dist);

/** Mesh placement (DIDE-R168 AC1). Without deform: an instance every `spacing` m from `offset`,
 * scale 1 (a closed curve does not repeat its start). With deform: round(usable / length) pieces
 * (at least 1), each centred on its piece and stretched along +X to fill it. `xf` takes 10
 * floats per instance (position xyz, rotation xyzw with +X along the curve, scale xyz) for
 * DE_SPLINE_MAX_OUT instances. Returns the count. */
u32 de__spline_place(const de_spline_poly *p, const DeSplineMeshes *m, f32 *xf);

DE_EXTERN_C_END

#endif /* DE_WORLD_SPLINE_H */
