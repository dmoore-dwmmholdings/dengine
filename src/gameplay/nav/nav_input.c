/*
 * nav_input.c: navmesh build input from an ECS world (DENG-76). Every static, non-trigger
 * DeCollider whose layer is in the build's collision_mask (0 = every layer) adds its surface:
 * the authored primitives (sphere, capsule, box, convex hull) directly, a live physics shape
 * (DE_COLLIDER_SHAPE_HANDLE) through de_shape_triangulate when the world has a DePhysicsWorld.
 * "Static" is no DeRigidBody, or a DE_BODY_STATIC one. The pose is the entity's
 * DeGlobalTransform (else DeTransform) times the collider offset; scale is not applied.
 *
 * Components are found by name, so a world without physics just gives no input. Main thread,
 * outside schedules (it creates a query).
 */
#include "gameplay/nav/nav_internal.h"

#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_math.h>

#include <string.h>

#ifndef DE_HAS_PHYSICS
#define DE_HAS_PHYSICS 0
#endif
#if DE_HAS_PHYSICS
#include <dengine/de_physics.h>
#endif

bool de__nav_entity_pose(de_world *w, de_entity e, de__nav_pose *out) {
    const de_builtin_component_ids ids = de_builtin_components(w);
    const de_transform *t = NULL;
    if (DE_HANDLE_VALID(ids.global_transform))
        t = (const de_transform *)de_entity_get_const(w, e, ids.global_transform);
    if (t == NULL && DE_HANDLE_VALID(ids.transform))
        t = (const de_transform *)de_entity_get_const(w, e, ids.transform);
    if (t == NULL)
        return false;
    *out = de__nav_pose_from(t);
    return true;
}

#if DE_HAS_PHYSICS

enum { T_COLLIDER, T_BODY, T_GLOBAL, T_LOCAL, T_HULL, T_COUNT };

typedef struct gather_ctx {
    de__nav_soup *soup;
    u32 mask;
    const de_physics_world *physics;
    i32 col[T_COUNT]; /* column of each term, -1 when the component is not registered */
    de__nav_pose pose; /* for the triangle callback */
} gather_ctx;

static void add_shape_tri(void *user, const de_vec3 tri[3]) {
    gather_ctx *g = (gather_ctx *)user;
    de__nav_soup_add_tri(g->soup, de__nav_pose_apply(&g->pose, tri[0]),
                         de__nav_pose_apply(&g->pose, tri[1]),
                         de__nav_pose_apply(&g->pose, tri[2]));
}

static const void *column(const de_query_chunk *ch, const gather_ctx *g, int term, u32 row,
                          size_t stride) {
    if (g->col[term] < 0 || ch->columns[g->col[term]] == NULL)
        return NULL;
    return (const u8 *)ch->columns[g->col[term]] + stride * row;
}

static void gather_chunk(const de_query_chunk *ch, void *user) {
    gather_ctx *g = (gather_ctx *)user;
    for (u32 i = 0; i < ch->count; ++i) {
        const DeCollider *c = (const DeCollider *)column(ch, g, T_COLLIDER, i, sizeof(DeCollider));
        if (c == NULL || c->is_trigger)
            continue;
        if (g->mask != 0 && (c->filter.layer & g->mask) == 0)
            continue;
        const DeRigidBody *b = (const DeRigidBody *)column(ch, g, T_BODY, i, sizeof(DeRigidBody));
        if (b != NULL && b->type != DE_BODY_STATIC)
            continue;
        const de_transform *t =
            (const de_transform *)column(ch, g, T_GLOBAL, i, sizeof(de_transform));
        if (t == NULL)
            t = (const de_transform *)column(ch, g, T_LOCAL, i, sizeof(de_transform));
        if (t == NULL)
            continue;
        const de__nav_pose body = de__nav_pose_from(t);
        const de__nav_pose off = de__nav_pose_from(&c->offset);
        g->pose = de__nav_pose_mul(&body, &off);
        switch (c->shape_kind) {
        case DE_COLLIDER_SHAPE_SPHERE:
            de__nav_soup_add_sphere(g->soup, &g->pose, c->radius);
            break;
        case DE_COLLIDER_SHAPE_CAPSULE:
            de__nav_soup_add_capsule(g->soup, &g->pose, c->radius, c->half_height);
            break;
        case DE_COLLIDER_SHAPE_BOX:
            de__nav_soup_add_box(g->soup, &g->pose, c->half_extents);
            break;
        case DE_COLLIDER_SHAPE_CONVEX: {
            const DeConvexHull *h =
                (const DeConvexHull *)column(ch, g, T_HULL, i, sizeof(DeConvexHull));
            if (h != NULL && h->point_count >= 4u && h->point_count <= DE_CONVEX_HULL_MAX_POINTS)
                de__nav_soup_add_hull(g->soup, &g->pose, h->points, h->point_count);
            break;
        }
        case DE_COLLIDER_SHAPE_HANDLE:
        default:
            if (g->physics != NULL && DE_HANDLE_VALID(c->shape))
                (void)de_shape_triangulate(g->physics, c->shape, add_shape_tri, g);
            break;
        }
    }
}

static const de_physics_world *find_physics(de_world *w) {
    const de_type_id t = de_type_find(de_world_types(w), "DePhysicsWorld");
    if (!DE_HANDLE_VALID(t))
        return NULL;
    const de_resource_id r = de_resource_find(w, t);
    if (!DE_HANDLE_VALID(r))
        return NULL;
    const DePhysicsWorld *pw = (const DePhysicsWorld *)de_resource_get(w, r);
    return pw != NULL ? pw->world : NULL;
}

void de__nav_gather_world(de_nav *n, de__nav_soup *soup, u32 mask) {
    de_world *w = n->world;
    if (w == NULL)
        return;
    const de_component collider = de_component_find(w, "DeCollider");
    if (!DE_HANDLE_VALID(collider))
        return;
    const de_builtin_component_ids ids = de_builtin_components(w);
    const de_component comps[T_COUNT] = {collider, de_component_find(w, "DeRigidBody"),
                                         ids.global_transform, ids.transform,
                                         de_component_find(w, "DeConvexHull")};
    gather_ctx g;
    memset(&g, 0, sizeof g);
    g.soup = soup;
    g.mask = mask;
    g.physics = find_physics(w);
    de_query_term terms[T_COUNT];
    u32 count = 0;
    for (int i = 0; i < T_COUNT; ++i) {
        g.col[i] = -1;
        if (!DE_HANDLE_VALID(comps[i]))
            continue;
        terms[count].component = comps[i];
        terms[count].access = i == T_COLLIDER ? DE_READ : DE_OPTIONAL_READ;
        g.col[i] = (i32)count++;
    }
    de_query_desc qd;
    memset(&qd, 0, sizeof qd);
    qd.terms = terms;
    qd.term_count = count;
    const de_query q = de_query_create(w, &qd);
    if (!DE_HANDLE_VALID(q)) {
        DE_WARN(n->log, "nav: cannot query colliders (inside a schedule?)");
        return;
    }
    de_query_each(w, q, gather_chunk, &g);
    de_query_destroy(w, q);
}

#else /* !DE_HAS_PHYSICS */

void de__nav_gather_world(de_nav *n, de__nav_soup *soup, u32 mask) {
    (void)n;
    (void)soup;
    (void)mask;
}

#endif
