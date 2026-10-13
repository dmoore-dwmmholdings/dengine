/* world2d.c: 2D physics world create / destroy and settings (DENG-252). SPEC §13.3. */
#include "physics/2d/p2d_internal.h"

de_result de_physics2d_create(const de_physics2d_desc *desc, de_physics2d_world **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics2d_create: out is NULL");
    *out = NULL;
    de_physics2d_desc d;
    if (desc != NULL)
        d = *desc;
    else
        memset(&d, 0, sizeof d);

    if (d.max_bodies == 0)
        d.max_bodies = DE_PHYSICS_DEFAULT_MAX_BODIES;
    if (d.max_bodies > DE_PHYSICS_MAX_BODIES_LIMIT)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics2d_create: max_bodies %u above %u",
                       d.max_bodies, DE_PHYSICS_MAX_BODIES_LIMIT);
    if ((u32)d.plane > (u32)DE_PHYSICS2D_PLANE_TOP_DOWN)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics2d_create: unknown plane %u",
                       (u32)d.plane);
    if (!de__v2_finite(d.gravity))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics2d_create: gravity is not finite");
    /* Zero means the plane's default: down (-y) in a side view, none seen from above. */
    if (d.gravity.x == 0.0f && d.gravity.y == 0.0f && d.plane == DE_PHYSICS2D_PLANE_SIDE)
        d.gravity = de__v2(0.0f, -9.81f);
    if (d.substeps == 0)
        d.substeps = 4;

    const de_allocator *a = d.allocator != NULL ? d.allocator : de_heap_allocator();
    de_physics2d_world *p = (de_physics2d_world *)DE_ALLOC(
        a, sizeof *p, DE_ALIGNOF(de_physics2d_world), DE_MEM_PHYSICS);
    if (p == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_physics2d_create: world");
    memset(p, 0, sizeof *p);
    p->alloc = a;
    p->tasks = d.tasks;
    p->gravity = d.gravity;
    p->substeps = d.substeps;
    p->max_bodies = d.max_bodies;
    p->plane = d.plane;

    const de_result r = de__body2d_pool_init(&p->bodies, a, d.max_bodies);
    if (r != DE_OK) {
        DE_FREE(a, p, sizeof *p);
        return r;
    }
    *out = p;
    return DE_OK;
}

void de_physics2d_destroy(de_physics2d_world *p) {
    if (p == NULL)
        return;
    /* Extensions first (descending slot order): they may refer to bodies and shapes. */
    for (u32 i = DE__P2D_EXT_COUNT; i-- > 0;) {
        de__p2d_ext *e = &p->ext[i];
        if (e->state != NULL && e->vtbl != NULL && e->vtbl->destroy != NULL)
            e->vtbl->destroy(p, e->state);
        e->state = NULL;
        e->vtbl = NULL;
    }
    de__shapes2d_free_all(p);
    de__body2d_pool_free(&p->bodies, p->alloc);
    const de_allocator *a = p->alloc;
    DE_FREE(a, p, sizeof *p);
}

u64 de_physics2d_tick(const de_physics2d_world *p) {
    return p != NULL ? p->tick : 0;
}

void de_physics2d_set_gravity(de_physics2d_world *p, de_vec2 gravity) {
    if (p == NULL || !de__v2_finite(gravity)) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics2d_set_gravity: bad world or gravity");
        return;
    }
    p->gravity = gravity;
}

de_vec2 de_physics2d_get_gravity(const de_physics2d_world *p) {
    return p != NULL ? p->gravity : de__v2(0.0f, 0.0f);
}

de_physics2d_plane de_physics2d_get_plane(const de_physics2d_world *p) {
    return p != NULL ? p->plane : DE_PHYSICS2D_PLANE_SIDE;
}

de_rot2d de_rot2d_from_angle(f32 radians) {
    de_rot2d r;
    de_dsincos(radians, &r.s, &r.c);
    return r;
}

f32 de_rot2d_angle(de_rot2d r) {
    return de_datan2(r.s, r.c);
}
