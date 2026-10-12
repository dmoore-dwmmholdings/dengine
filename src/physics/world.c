/* world.c: physics world create / destroy and settings (DENG-132). SPEC §13.1. */
#include "physics/physics_internal.h"
#include "physics/shapes/shape_internal.h"

/* Registration returns the existing category after the first call (it takes a lock, so this is
 * for create and error paths, never per step). */
de_log_category de__physics_log(void) {
    return de_log_register_category("physics", DE_LOG_INFO);
}

de_result de_physics_create(const de_physics_desc *desc, de_physics_world **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics_create: out is NULL");
    *out = NULL;
    de_physics_desc d;
    if (desc != NULL)
        d = *desc;
    else
        memset(&d, 0, sizeof d);

    if (d.max_bodies == 0)
        d.max_bodies = DE_PHYSICS_DEFAULT_MAX_BODIES;
    if (d.max_bodies > DE_PHYSICS_MAX_BODIES_LIMIT)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics_create: max_bodies %u above %u",
                       d.max_bodies, DE_PHYSICS_MAX_BODIES_LIMIT);
    if (!de__v3_finite(d.gravity))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics_create: gravity is not finite");
    if (d.gravity.x == 0.0f && d.gravity.y == 0.0f && d.gravity.z == 0.0f)
        d.gravity = de__v3(0.0f, 0.0f, -9.81f);
    if (d.substeps == 0)
        d.substeps = 4;

    const de_allocator *a = d.allocator != NULL ? d.allocator : de_heap_allocator();
    de_physics_world *p =
        (de_physics_world *)DE_ALLOC(a, sizeof *p, DE_ALIGNOF(de_physics_world), DE_MEM_PHYSICS);
    if (p == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_physics_create: world");
    memset(p, 0, sizeof *p);
    p->alloc = a;
    p->tasks = d.tasks;
    p->gravity = d.gravity;
    p->substeps = d.substeps;
    p->max_bodies = d.max_bodies;

    const de_result r = de__body_pool_init(&p->bodies, a, d.max_bodies);
    if (r != DE_OK) {
        DE_FREE(a, p, sizeof *p);
        return r;
    }
    *out = p;
    return DE_OK;
}

void de_physics_destroy(de_physics_world *p) {
    if (p == NULL)
        return;
    /* Extensions first (descending slot order): they may refer to bodies and shapes. */
    for (u32 i = DE__PHYS_EXT_COUNT; i-- > 0;) {
        de__phys_ext *e = &p->ext[i];
        if (e->state != NULL && e->vtbl != NULL && e->vtbl->destroy != NULL)
            e->vtbl->destroy(p, e->state);
        e->state = NULL;
        e->vtbl = NULL;
    }
    de__shapes_free_all(p);
    de__body_pool_free(&p->bodies, p->alloc);
    const de_allocator *a = p->alloc;
    DE_FREE(a, p, sizeof *p);
}

u64 de_physics_tick(const de_physics_world *p) {
    return p != NULL ? p->tick : 0;
}

void de_physics_set_gravity(de_physics_world *p, de_vec3 gravity) {
    if (p == NULL || !de__v3_finite(gravity)) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics_set_gravity: bad world or gravity");
        return;
    }
    p->gravity = gravity;
}

de_vec3 de_physics_get_gravity(const de_physics_world *p) {
    return p != NULL ? p->gravity : de__v3(0.0f, 0.0f, 0.0f);
}
