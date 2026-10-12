/* compound.c: compound shape (DENG-132): referenced children with local transforms, a child
 * BVH, and parallel-axis mass properties. One nesting level. */
#include "physics/shapes/shape_internal.h"

de_result de__compound_build(de_physics_world *p, const de_shape_desc *d, de__shape *s) {
    if (d->children == NULL || d->child_count == 0 ||
        d->child_count > DE_SHAPE_MAX_COMPOUND_CHILDREN)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape_create: compound needs 1..%u children",
                       DE_SHAPE_MAX_COMPOUND_CHILDREN);
    const u32 n = d->child_count;
    for (u32 i = 0; i < n; ++i) {
        const de__shape *c = de__shape_get(p, d->children[i]);
        if (c == NULL)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape_create: child %u is not a live shape",
                           i);
        if (c->kind == DE_SHAPE_COMPOUND)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_shape_create: child %u is a compound (one nesting level)", i);
        if (d->child_xforms != NULL) {
            const de_transform *t = &d->child_xforms[i];
            if (!de__d3_finite(t->position) || !de__f32_finite(t->rotation.x) ||
                !de__f32_finite(t->rotation.y) || !de__f32_finite(t->rotation.z) ||
                !de__f32_finite(t->rotation.w))
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                               "de_shape_create: child transform %u is not finite", i);
        }
    }

    const de_allocator *a = p->alloc;
    const size_t size = sizeof(de__compound_child) * (size_t)n;
    de__compound_child *ch =
        (de__compound_child *)DE_ALLOC(a, size, DE_ALIGNOF(de__compound_child), DE_MEM_PHYSICS);
    if (ch == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_shape_create: compound of %u", n);
    memset(ch, 0, size);
    s->data = ch;
    s->data_size = size;
    /* child_count stays 0 until the references are taken, so a failed build releases nothing. */
    s->u.compound.children = ch;
    s->u.compound.child_count = 0;

    de_aabb *bounds =
        (de_aabb *)DE_ALLOC(a, sizeof(de_aabb) * (size_t)n, DE_ALIGNOF(de_aabb), DE_MEM_PHYSICS);
    u32 *prims = bounds != NULL
                     ? (u32 *)DE_ALLOC(a, sizeof(u32) * (size_t)n, DE_ALIGNOF(u32), DE_MEM_PHYSICS)
                     : NULL;
    if (prims == NULL) {
        if (bounds != NULL)
            DE_FREE(a, bounds, sizeof(de_aabb) * (size_t)n);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_shape_create: compound scratch");
    }

    bool volume = true;
    de__unit_mass acc;
    memset(&acc, 0, sizeof acc);
    de_aabb all;
    all.min = de__v3(FLT_MAX, FLT_MAX, FLT_MAX);
    all.max = de__v3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    f64 rmax = 0.0;
    for (u32 i = 0; i < n; ++i) {
        de__compound_child *c = &ch[i];
        c->shape = de__shape_get(p, d->children[i]);
        c->handle = d->children[i];
        if (d->child_xforms != NULL) {
            const de_dvec3 pos = d->child_xforms[i].position;
            c->position = de__v3((f32)pos.x, (f32)pos.y, (f32)pos.z);
            c->rotation = de_dnormalize_quat(d->child_xforms[i].rotation);
        } else {
            c->position = de__v3(0.0f, 0.0f, 0.0f);
            c->rotation = de_dnormalize_quat((de_quat){0.0f, 0.0f, 0.0f, 1.0f});
        }
        c->bounds = de__shape_world_aabb(c->shape, de__d3_from(c->position), c->rotation);
        bounds[i] = c->bounds;
        prims[i] = i;
        all.min = de__v3_min(all.min, c->bounds.min);
        all.max = de__v3_max(all.max, c->bounds.max);
        const de_dvec3 pd = de__d3_from(c->position);
        rmax = de__maxd(rmax, de_dsqrt64(de__d3_dot(pd, pd)) + (f64)c->shape->bounding_radius);
        if (c->shape->has_volume)
            de__mass_compound_add(&acc, &c->shape->unit_mass, c->position, c->rotation);
        else
            volume = false;
    }
    const de_result r = de__bvh_build(a, bounds, prims, n, 2, &s->u.compound.bvh);
    DE_FREE(a, bounds, sizeof(de_aabb) * (size_t)n);
    DE_FREE(a, prims, sizeof(u32) * (size_t)n);
    if (r != DE_OK)
        return r;

    s->local_aabb = all;
    s->bounding_radius = de__f32_round_up(rmax);
    s->has_volume = volume;
    if (volume) {
        de__mass_compound_finish(&acc);
        s->unit_mass = acc;
        s->has_volume = acc.volume > 0.0;
    }
    for (u32 i = 0; i < n; ++i)
        ++ch[i].shape->refcount;
    s->u.compound.child_count = n;
    return DE_OK;
}

void de__compound_release(de__shape *s) {
    de__compound *c = &s->u.compound;
    for (u32 i = 0; i < c->child_count; ++i) {
        DE_ASSERT(c->children[i].shape->refcount > 0);
        --c->children[i].shape->refcount;
    }
    c->child_count = 0;
}
