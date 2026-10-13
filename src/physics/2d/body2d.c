/* body2d.c: the preallocated 2D body pool and the 2D body API (DENG-252). SPEC §13.2, §13.3. */
#include "physics/2d/p2d_internal.h"

/* ---- Pool ------------------------------------------------------------------------------ */

/* One allocation holds every array; each array starts at a 16-byte boundary (like 3D). */
#define DE__POOL2D_ALIGN 16u

static size_t pool_align_up(size_t x) {
    return (x + (DE__POOL2D_ALIGN - 1u)) & ~(size_t)(DE__POOL2D_ALIGN - 1u);
}

typedef struct pool_field {
    void **dst;
    size_t elem;
} pool_field;

static u32 pool_fields(de__body2d_pool *pool, pool_field *f) {
    u32 n = 0;
#define DE__FIELD(member) f[n].dst = (void **)&pool->member, f[n].elem = sizeof(*pool->member), ++n
    DE__FIELD(free_list);
    DE__FIELD(generation);
    DE__FIELD(alive);
    DE__FIELD(type);
    DE__FIELD(flags);
    DE__FIELD(position);
    DE__FIELD(rotation);
    DE__FIELD(linear_velocity);
    DE__FIELD(angular_velocity);
    DE__FIELD(mass);
    DE__FIELD(inv_mass);
    DE__FIELD(inertia);
    DE__FIELD(inv_inertia);
    DE__FIELD(local_com);
    DE__FIELD(linear_damping);
    DE__FIELD(angular_damping);
    DE__FIELD(gravity_scale);
    DE__FIELD(shape);
    DE__FIELD(filter);
    DE__FIELD(material);
    DE__FIELD(owner);
    DE__FIELD(world_aabb);
#undef DE__FIELD
    return n;
}

de_result de__body2d_pool_init(de__body2d_pool *pool, const de_allocator *a, u32 capacity) {
    memset(pool, 0, sizeof *pool);
    pool_field f[32];
    const u32 n = pool_fields(pool, f);
    size_t size = 0;
    for (u32 i = 0; i < n; ++i)
        size = pool_align_up(size) + f[i].elem * (size_t)capacity;
    size = pool_align_up(size);
    u8 *block = (u8 *)DE_ALLOC(a, size, DE__POOL2D_ALIGN, DE_MEM_PHYSICS);
    if (block == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_physics2d_create: body pool of %u bodies",
                       capacity);
    memset(block, 0, size);
    size_t off = 0;
    for (u32 i = 0; i < n; ++i) {
        off = pool_align_up(off);
        *f[i].dst = block + off;
        off += f[i].elem * (size_t)capacity;
    }
    pool->block = block;
    pool->block_size = size;
    pool->capacity = capacity;
    return DE_OK;
}

void de__body2d_pool_free(de__body2d_pool *pool, const de_allocator *a) {
    if (pool->block != NULL)
        DE_FREE(a, pool->block, pool->block_size);
    memset(pool, 0, sizeof *pool);
}

/* ---- AABBs ----------------------------------------------------------------------------- */

void de__body2d_refresh_aabb(de_physics2d_world *p, u32 index) {
    de__body2d_pool *b = &p->bodies;
    const de__shape2d *s = de__shape2d_get(p, b->shape[index]);
    b->world_aabb[index] = s != NULL
                               ? de__shape2d_world_aabb(s, b->position[index], b->rotation[index])
                               : de__aabb2d_empty();
}

void de__bodies2d_refresh_aabbs(de_physics2d_world *p) {
    for (u32 i = 0; i < p->bodies.high_water; ++i)
        if (p->bodies.alive[i])
            de__body2d_refresh_aabb(p, i);
}

/* ---- Mass ------------------------------------------------------------------------------ */

/* Fills mass, inverse mass, center of mass and inertia of body `i` from its shape. */
static void body_set_mass(de__body2d_pool *b, u32 i, const de__shape2d *s, f32 mass_override) {
    b->mass[i] = 0.0f;
    b->inv_mass[i] = 0.0f;
    b->inertia[i] = 0.0f;
    b->inv_inertia[i] = 0.0f;
    b->local_com[i] = de__v2(0.0f, 0.0f);
    if (s == NULL || !s->has_area)
        return;
    const de__unit_mass2d *u = &s->unit_mass;
    b->local_com[i] = de__v2((f32)u->com.x, (f32)u->com.y);
    if (b->type[i] != DE_BODY_DYNAMIC)
        return;
    f64 density = (f64)b->material[i].density;
    f64 mass = density * u->area;
    if (mass_override > 0.0f) {
        mass = (f64)mass_override;
        density = mass / u->area;
    }
    const f64 inertia = u->inertia * density;
    b->mass[i] = (f32)mass;
    b->inv_mass[i] = (f32)(1.0 / mass);
    b->inertia[i] = (f32)inertia;
    if ((b->flags[i] & DE_BODY2D_FLAG_FIXED_ROTATION) == 0 && inertia > 0.0)
        b->inv_inertia[i] = (f32)(1.0 / inertia);
}

/* ---- Public API ------------------------------------------------------------------------ */

static bool transform_finite(const de_transform2d *t) {
    return de__f64_finite(t->x) && de__f64_finite(t->y) && de__rot2d_finite(t->rotation);
}

de_body de_body2d_create(de_physics2d_world *p, const de_body2d_desc *desc) {
    de_body none = {0};
    if (p == NULL || desc == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_body2d_create: NULL world or desc");
        return none;
    }
    if ((u32)desc->type > (u32)DE_BODY_DYNAMIC) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_body2d_create: unknown body type %u", (u32)desc->type);
        return none;
    }
    if (!transform_finite(&desc->transform) || !de__v2_finite(desc->linear_velocity) ||
        !de__f32_finite(desc->angular_velocity)) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_body2d_create: non-finite transform or velocity");
        return none;
    }
    if (!de__f32_finite(desc->mass) || desc->mass < 0.0f ||
        !de__f32_finite(desc->material.density) || desc->material.density < 0.0f ||
        !de__f32_finite(desc->linear_damping) || !de__f32_finite(desc->angular_damping) ||
        !de__f32_finite(desc->gravity_scale)) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_body2d_create: bad mass, density or damping");
        return none;
    }
    de__shape2d *s = NULL;
    if (desc->shape.id != 0) {
        s = de__shape2d_get(p, desc->shape);
        if (s == NULL) {
            DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_body2d_create: stale shape 0x%08x",
                    desc->shape.id);
            return none;
        }
    }
    if (desc->type == DE_BODY_DYNAMIC && (s == NULL || !s->has_area)) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                "de_body2d_create: a dynamic body needs a solid shape (not a segment or a "
                "chain)");
        return none;
    }
    de__body2d_pool *b = &p->bodies;
    u32 i;
    if (b->free_count > 0) {
        i = b->free_list[--b->free_count];
    } else if (b->high_water < b->capacity) {
        i = b->high_water++;
        b->generation[i] = 1;
    } else {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_body2d_create: body pool full (max_bodies %u)",
                b->capacity);
        return none;
    }

    b->type[i] = (u8)desc->type;
    b->flags[i] = desc->flags;
    b->position[i] = de__d2_make(desc->transform.x, desc->transform.y);
    b->rotation[i] = de__rot2d_normalize(desc->transform.rotation);
    if (desc->type == DE_BODY_STATIC) {
        b->linear_velocity[i] = de__v2(0.0f, 0.0f);
        b->angular_velocity[i] = 0.0f;
    } else {
        b->linear_velocity[i] = desc->linear_velocity;
        b->angular_velocity[i] = desc->angular_velocity;
    }
    b->linear_damping[i] = desc->linear_damping;
    b->angular_damping[i] = desc->angular_damping;
    b->gravity_scale[i] = desc->gravity_scale;
    b->shape[i].id = s != NULL ? desc->shape.id : 0u;
    b->filter[i] = desc->filter;
    if (desc->filter.layer == 0 && desc->filter.mask == 0 && desc->filter.group == 0) {
        b->filter[i].layer = DE_COLLISION_FILTER_DEFAULT_LAYER;
        b->filter[i].mask = DE_COLLISION_FILTER_DEFAULT_MASK;
    }
    b->material[i] = desc->material;
    if (b->material[i].density == 0.0f)
        b->material[i].density = DE_PHYSICS2D_DEFAULT_DENSITY;
    b->owner[i] = desc->owner;
    body_set_mass(b, i, s, desc->mass);
    if (s != NULL)
        ++s->refcount;
    b->alive[i] = 1;
    ++b->live_count;
    de__body2d_refresh_aabb(p, i);

    for (u32 e = 0; e < DE__P2D_EXT_COUNT; ++e) {
        const de__p2d_ext *x = &p->ext[e];
        if (x->state != NULL && x->vtbl != NULL && x->vtbl->body_added != NULL)
            x->vtbl->body_added(p, x->state, i);
    }
    return de__body2d_handle(p, i);
}

void de_body2d_destroy(de_physics2d_world *p, de_body h) {
    u32 i;
    if (!de__body2d_resolve(p, h, &i))
        return;
    for (u32 e = 0; e < DE__P2D_EXT_COUNT; ++e) {
        const de__p2d_ext *x = &p->ext[e];
        if (x->state != NULL && x->vtbl != NULL && x->vtbl->body_removed != NULL)
            x->vtbl->body_removed(p, x->state, i);
    }
    de__body2d_pool *b = &p->bodies;
    de__shape2d *s = de__shape2d_get(p, b->shape[i]);
    if (s != NULL) {
        DE_ASSERT(s->refcount > 0);
        --s->refcount;
    }
    b->alive[i] = 0;
    b->generation[i] = de__gen_next(b->generation[i]);
    b->free_list[b->free_count++] = i;
    --b->live_count;
}

bool de_body2d_valid(const de_physics2d_world *p, de_body h) {
    u32 i;
    return de__body2d_resolve(p, h, &i);
}

u32 de_body2d_count(const de_physics2d_world *p) {
    return p != NULL ? p->bodies.live_count : 0;
}

bool de_body2d_get_transform(const de_physics2d_world *p, de_body h, de_transform2d *out) {
    u32 i;
    if (out == NULL || !de__body2d_resolve(p, h, &i))
        return false;
    out->x = p->bodies.position[i].x;
    out->y = p->bodies.position[i].y;
    out->rotation = p->bodies.rotation[i];
    return true;
}

bool de_body2d_set_transform(de_physics2d_world *p, de_body h, const de_transform2d *t) {
    u32 i;
    if (t == NULL || !de__body2d_resolve(p, h, &i) || !transform_finite(t))
        return false;
    p->bodies.position[i] = de__d2_make(t->x, t->y);
    p->bodies.rotation[i] = de__rot2d_normalize(t->rotation);
    de__body2d_refresh_aabb(p, i);
    return true;
}

bool de_body2d_get_velocity(const de_physics2d_world *p, de_body h, de_vec2 *linear, f32 *angular) {
    u32 i;
    if (!de__body2d_resolve(p, h, &i))
        return false;
    if (linear != NULL)
        *linear = p->bodies.linear_velocity[i];
    if (angular != NULL)
        *angular = p->bodies.angular_velocity[i];
    return true;
}

bool de_body2d_set_velocity(de_physics2d_world *p, de_body h, de_vec2 linear, f32 angular) {
    u32 i;
    if (!de__body2d_resolve(p, h, &i) || !de__v2_finite(linear) || !de__f32_finite(angular))
        return false;
    if (p->bodies.type[i] == DE_BODY_STATIC)
        return true;
    p->bodies.linear_velocity[i] = linear;
    p->bodies.angular_velocity[i] = angular;
    return true;
}

bool de_body2d_get_mass_props(const de_physics2d_world *p, de_body h, de_mass_props2d *out) {
    u32 i;
    if (out == NULL || !de__body2d_resolve(p, h, &i))
        return false;
    const de__shape2d *s = de__shape2d_get(p, p->bodies.shape[i]);
    out->mass = p->bodies.mass[i];
    out->area = s != NULL && s->has_area ? (f32)s->unit_mass.area : 0.0f;
    out->center = p->bodies.local_com[i];
    out->inertia = p->bodies.inertia[i];
    return true;
}
