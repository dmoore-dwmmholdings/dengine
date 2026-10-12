/* shape.c: shape slots, de_shape_create / destroy and the shape getters (DENG-132). SPEC §13.1. */
#include "physics/shapes/shape_internal.h"

/* ---- Slots ----------------------------------------------------------------------------- */

de__shape *de__shape_get(const de_physics_world *p, de_shape h) {
    if (p == NULL || h.id == 0)
        return NULL;
    const u32 i = de__handle_index(h.id);
    if (i >= p->shape_slot_count)
        return NULL;
    const de__shape_slot *slot = &p->shape_slots[i];
    if (slot->shape == NULL || slot->generation != de__handle_gen(h.id))
        return NULL;
    return slot->shape;
}

/* Makes room for one more slot. Slot arrays may move; shapes never do. */
static de_result slots_reserve(de_physics_world *p) {
    if (p->shape_free_count > 0 || p->shape_slot_count < p->shape_slot_capacity)
        return DE_OK;
    const u32 limit = DE__HANDLE_INDEX_MASK - 1u;
    if (p->shape_slot_capacity >= limit)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_shape_create: too many shapes (%u)", limit);
    u32 cap = p->shape_slot_capacity < 16u ? 16u : p->shape_slot_capacity * 2u;
    if (cap > limit)
        cap = limit;
    const u32 old = p->shape_slot_capacity;
    /* New arrays, then copy: a failure leaves the old ones untouched. */
    de__shape_slot *slots = (de__shape_slot *)DE_ALLOC(p->alloc, sizeof(de__shape_slot) * cap,
                                                       DE_ALIGNOF(de__shape_slot), DE_MEM_PHYSICS);
    u32 *free_list = slots != NULL ? (u32 *)DE_ALLOC(p->alloc, sizeof(u32) * cap, DE_ALIGNOF(u32),
                                                     DE_MEM_PHYSICS)
                                   : NULL;
    if (free_list == NULL) {
        if (slots != NULL)
            DE_FREE(p->alloc, slots, sizeof(de__shape_slot) * cap);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_shape_create: shape slots");
    }
    memset(slots, 0, sizeof(de__shape_slot) * cap);
    if (old > 0) {
        memcpy(slots, p->shape_slots, sizeof(de__shape_slot) * old);
        memcpy(free_list, p->shape_free, sizeof(u32) * old);
        DE_FREE(p->alloc, p->shape_slots, sizeof(de__shape_slot) * old);
        DE_FREE(p->alloc, p->shape_free, sizeof(u32) * old);
    }
    p->shape_slots = slots;
    p->shape_free = free_list;
    p->shape_slot_capacity = cap;
    return DE_OK;
}

/* Frees what a shape owns (not the de__shape itself) and drops its children's references. */
static void shape_free_data(de_physics_world *p, de__shape *s) {
    switch (s->kind) {
    case DE_SHAPE_MESH:
        de__bvh_free(p->alloc, &s->u.mesh.bvh);
        break;
    case DE_SHAPE_COMPOUND:
        de__compound_release(s);
        de__bvh_free(p->alloc, &s->u.compound.bvh);
        break;
    default:
        break;
    }
    if (s->data != NULL)
        DE_FREE(p->alloc, s->data, s->data_size);
    s->data = NULL;
    s->data_size = 0;
}

void de__shapes_free_all(de_physics_world *p) {
    /* Compounds first (they hold references to other shapes), then the rest; slot order. */
    for (u32 pass = 0; pass < 2; ++pass) {
        for (u32 i = 0; i < p->shape_slot_count; ++i) {
            de__shape *s = p->shape_slots[i].shape;
            if (s == NULL || (pass == 0) != (s->kind == DE_SHAPE_COMPOUND))
                continue;
            shape_free_data(p, s);
            DE_FREE(p->alloc, s, sizeof *s);
            p->shape_slots[i].shape = NULL;
        }
    }
    if (p->shape_slots != NULL)
        DE_FREE(p->alloc, p->shape_slots, sizeof(de__shape_slot) * p->shape_slot_capacity);
    if (p->shape_free != NULL)
        DE_FREE(p->alloc, p->shape_free, sizeof(u32) * p->shape_slot_capacity);
    p->shape_slots = NULL;
    p->shape_free = NULL;
    p->shape_slot_count = p->shape_slot_capacity = p->shape_free_count = 0;
    p->shape_live_count = 0;
}

/* ---- Primitive builders ---------------------------------------------------------------- */

static bool positive_finite(f32 x) {
    return de__f32_finite(x) && x > 0.0f;
}

static de_aabb aabb_sym(f32 x, f32 y, f32 z) {
    de_aabb b;
    b.min = de__v3(-x, -y, -z);
    b.max = de__v3(x, y, z);
    return b;
}

static f32 radius_up(f64 sq) {
    return de__f32_round_up(de_dsqrt64(sq));
}

static de_result build_primitive(const de_shape_desc *d, de__shape *s) {
    s->has_volume = true;
    switch (d->kind) {
    case DE_SHAPE_SPHERE:
        if (!positive_finite(d->radius))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape_create: sphere radius must be > 0");
        s->u.sphere.radius = d->radius;
        s->margin = d->radius;
        s->local_aabb = aabb_sym(d->radius, d->radius, d->radius);
        s->bounding_radius = d->radius;
        de__mass_sphere(d->radius, &s->unit_mass);
        return DE_OK;
    case DE_SHAPE_CAPSULE:
        if (!positive_finite(d->radius) || !de__f32_finite(d->half_height) || d->half_height < 0.0f)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_shape_create: capsule needs radius > 0 and half_height >= 0");
        s->u.capsule.radius = d->radius;
        s->u.capsule.half_height = d->half_height;
        s->margin = d->radius;
        s->local_aabb =
            aabb_sym(d->radius, d->radius, de__f32_round_up((f64)d->half_height + (f64)d->radius));
        s->bounding_radius = s->local_aabb.max.z;
        de__mass_capsule(d->radius, d->half_height, &s->unit_mass);
        return DE_OK;
    case DE_SHAPE_BOX: {
        const de_vec3 e = d->half_extents;
        if (!positive_finite(e.x) || !positive_finite(e.y) || !positive_finite(e.z))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_shape_create: box half_extents must be > 0");
        s->u.box.half_extents = e;
        s->local_aabb = aabb_sym(e.x, e.y, e.z);
        s->bounding_radius = radius_up((f64)e.x * e.x + (f64)e.y * e.y + (f64)e.z * e.z);
        de__mass_box(de__d3_from(e), &s->unit_mass);
        return DE_OK;
    }
    case DE_SHAPE_CYLINDER:
        if (!positive_finite(d->radius) || !positive_finite(d->half_height))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_shape_create: cylinder needs radius > 0 and half_height > 0");
        s->u.cylinder.radius = d->radius;
        s->u.cylinder.half_height = d->half_height;
        s->local_aabb = aabb_sym(d->radius, d->radius, d->half_height);
        s->bounding_radius =
            radius_up((f64)d->radius * d->radius + (f64)d->half_height * d->half_height);
        de__mass_cylinder(d->radius, d->half_height, &s->unit_mass);
        return DE_OK;
    default:
        return DE_FAIL(DE_ERR_INTERNAL, "build_primitive: not a primitive");
    }
}

static de_result build_convex(de_physics_world *p, const de_shape_desc *d, de__shape *s) {
    if (d->points == NULL || d->point_count < 4 || d->point_count > DE_SHAPE_MAX_HULL_INPUT)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_shape_create: convex needs 4..%u points (got %u)",
                       DE_SHAPE_MAX_HULL_INPUT, d->point_count);
    if (!de__f32_finite(d->hull_tolerance) || d->hull_tolerance < 0.0f)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape_create: bad hull_tolerance");
    for (u32 i = 0; i < d->point_count; ++i)
        if (!de__v3_finite(d->points[i]))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape_create: point %u is not finite", i);
    const f32 tol = d->hull_tolerance > 0.0f ? d->hull_tolerance
                                             : de__hull_auto_tolerance(d->points, d->point_count);
    DE_TRY(de__hull_build(p->alloc, d->points, d->point_count, tol, &s->u.hull, &s->data,
                          &s->data_size));
    const de__hull *h = &s->u.hull;
    de_vec3 lo = h->vertices[0], hi = h->vertices[0];
    f64 r2 = 0.0;
    for (u32 i = 0; i < h->vertex_count; ++i) {
        const de_vec3 v = h->vertices[i];
        lo = de__v3_min(lo, v);
        hi = de__v3_max(hi, v);
        const f64 q = (f64)v.x * v.x + (f64)v.y * v.y + (f64)v.z * v.z;
        r2 = de__maxd(r2, q);
    }
    s->local_aabb.min = lo;
    s->local_aabb.max = hi;
    s->bounding_radius = radius_up(r2);
    de__mass_hull(h, &s->unit_mass);
    s->has_volume = s->unit_mass.volume > 0.0;
    return DE_OK;
}

/* ---- Public API ------------------------------------------------------------------------ */

de_shape de_shape_create(de_physics_world *p, const de_shape_desc *desc) {
    de_shape none = {0};
    if (p == NULL || desc == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape_create: NULL world or desc");
        return none;
    }
    de__shape *s = DE_NEW(p->alloc, de__shape, DE_MEM_PHYSICS);
    if (s == NULL) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_shape_create: shape");
        return none;
    }
    memset(s, 0, sizeof *s);
    s->kind = desc->kind;

    de_result r;
    switch (desc->kind) {
    case DE_SHAPE_SPHERE:
    case DE_SHAPE_CAPSULE:
    case DE_SHAPE_BOX:
    case DE_SHAPE_CYLINDER:
        r = build_primitive(desc, s);
        break;
    case DE_SHAPE_CONVEX:
        r = build_convex(p, desc, s);
        break;
    case DE_SHAPE_MESH:
        r = de__mesh_build(p, desc, s);
        break;
    case DE_SHAPE_HEIGHTFIELD:
        r = de__heightfield_build(p, desc, s);
        break;
    case DE_SHAPE_COMPOUND:
        r = de__compound_build(p, desc, s);
        break;
    default:
        r = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape_create: unknown kind %u", (u32)desc->kind);
        break;
    }
    if (r == DE_OK)
        r = slots_reserve(p);
    if (r != DE_OK) {
        shape_free_data(p, s);
        DE_FREE(p->alloc, s, sizeof *s);
        return none;
    }

    u32 i;
    if (p->shape_free_count > 0) {
        i = p->shape_free[--p->shape_free_count];
    } else {
        i = p->shape_slot_count++;
        p->shape_slots[i].generation = 1; /* generations start at 1, like bodies */
    }
    de__shape_slot *slot = &p->shape_slots[i];
    slot->shape = s;
    s->handle.id = de__handle_make(i, slot->generation);
    ++p->shape_live_count;
    return s->handle;
}

de_result de_shape_destroy(de_physics_world *p, de_shape h) {
    de__shape *s = de__shape_get(p, h);
    if (s == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape_destroy: stale or invalid shape 0x%08x",
                       h.id);
    if (s->refcount > 0) {
        DE_WARN(de__physics_log(), "de_shape_destroy: shape 0x%08x is used by %u bodies/compounds",
                h.id, s->refcount);
        return DE_FAIL(DE_ERR_BUSY, "de_shape_destroy: shape 0x%08x is still referenced (%u)", h.id,
                       s->refcount);
    }
    const u32 i = de__handle_index(h.id);
    shape_free_data(p, s);
    DE_FREE(p->alloc, s, sizeof *s);
    de__shape_slot *slot = &p->shape_slots[i];
    slot->shape = NULL;
    slot->generation = de__gen_next(slot->generation);
    p->shape_free[p->shape_free_count++] = i;
    --p->shape_live_count;
    return DE_OK;
}

bool de_shape_valid(const de_physics_world *p, de_shape s) {
    return de__shape_get(p, s) != NULL;
}

bool de_shape_get_kind(const de_physics_world *p, de_shape h, de_shape_kind *out) {
    const de__shape *s = de__shape_get(p, h);
    if (s == NULL || out == NULL)
        return false;
    *out = s->kind;
    return true;
}

bool de_shape_get_local_aabb(const de_physics_world *p, de_shape h, de_aabb *out) {
    const de__shape *s = de__shape_get(p, h);
    if (s == NULL || out == NULL)
        return false;
    *out = s->local_aabb;
    return true;
}

de_result de_shape_get_mass_props(const de_physics_world *p, de_shape h, f32 density,
                                  de_mass_props *out) {
    const de__shape *s = de__shape_get(p, h);
    if (s == NULL || out == NULL || !de__f32_finite(density) || density < 0.0f)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_shape_get_mass_props: bad arguments");
    if (density == 0.0f)
        density = DE_PHYSICS_DEFAULT_DENSITY;
    de__unit_mass_scale(&s->unit_mass, s->has_volume ? (f64)density : 0.0, out);
    return DE_OK;
}
