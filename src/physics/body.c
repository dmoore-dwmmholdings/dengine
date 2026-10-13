/* body.c: the preallocated body pool and the direct body API (DENG-132). SPEC §13.1, §13.2. */
#include "physics/physics_internal.h"
#include "physics/shapes/shape_internal.h"

/* ---- Pool ------------------------------------------------------------------------------ */

/* One allocation holds every array; each array starts at a 16-byte boundary. */
#define DE__POOL_ALIGN 16u

static size_t pool_align_up(size_t x) {
    return (x + (DE__POOL_ALIGN - 1u)) & ~(size_t)(DE__POOL_ALIGN - 1u);
}

typedef struct pool_field {
    void **dst;
    size_t elem;
} pool_field;

static u32 pool_fields(de__body_pool *pool, pool_field *f) {
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
    DE__FIELD(local_com);
    DE__FIELD(local_inertia);
    DE__FIELD(local_inv_inertia);
    DE__FIELD(linear_damping);
    DE__FIELD(angular_damping);
    DE__FIELD(gravity_scale);
    DE__FIELD(shape);
    DE__FIELD(filter);
    DE__FIELD(material);
    DE__FIELD(owner);
    DE__FIELD(world_aabb);
    DE__FIELD(friction_axis);
    DE__FIELD(friction_secondary);
#undef DE__FIELD
    return n;
}

de_result de__body_pool_init(de__body_pool *pool, const de_allocator *a, u32 capacity) {
    memset(pool, 0, sizeof *pool);
    pool_field f[32];
    const u32 n = pool_fields(pool, f);
    size_t size = 0;
    for (u32 i = 0; i < n; ++i)
        size = pool_align_up(size) + f[i].elem * (size_t)capacity;
    size = pool_align_up(size);
    u8 *block = (u8 *)DE_ALLOC(a, size, DE__POOL_ALIGN, DE_MEM_PHYSICS);
    if (block == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_physics_create: body pool of %u bodies", capacity);
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

void de__body_pool_free(de__body_pool *pool, const de_allocator *a) {
    if (pool->block != NULL)
        DE_FREE(a, pool->block, pool->block_size);
    memset(pool, 0, sizeof *pool);
}

/* ---- AABBs ----------------------------------------------------------------------------- */

static de_aabb aabb_empty(void) {
    de_aabb b;
    b.min = de__v3(FLT_MAX, FLT_MAX, FLT_MAX);
    b.max = de__v3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    return b;
}

void de__body_refresh_aabb(de_physics_world *p, u32 index) {
    de__body_pool *b = &p->bodies;
    const de__shape *s = de__shape_get(p, b->shape[index]);
    b->world_aabb[index] =
        s != NULL ? de__shape_world_aabb(s, b->position[index], b->rotation[index]) : aabb_empty();
}

void de__bodies_refresh_aabbs(de_physics_world *p) {
    for (u32 i = 0; i < p->bodies.high_water; ++i)
        if (p->bodies.alive[i])
            de__body_refresh_aabb(p, i);
}

/* ---- Mass ------------------------------------------------------------------------------ */

static void mat3_from_d(const f64 m[9], de_mat3 *out) {
    for (u32 i = 0; i < 9; ++i)
        out->m[i] = (f32)m[i];
}

/* Fills mass, inverse mass, center of mass and inertia of body `i` from its shape. */
static void body_set_mass(de__body_pool *b, u32 i, const de__shape *s, f32 mass_override) {
    memset(&b->local_inertia[i], 0, sizeof b->local_inertia[i]);
    memset(&b->local_inv_inertia[i], 0, sizeof b->local_inv_inertia[i]);
    b->mass[i] = 0.0f;
    b->inv_mass[i] = 0.0f;
    b->local_com[i] = de__v3(0.0f, 0.0f, 0.0f);
    if (s == NULL || !s->has_volume)
        return;
    const de__unit_mass *u = &s->unit_mass;
    b->local_com[i] = de__v3((f32)u->com.x, (f32)u->com.y, (f32)u->com.z);
    if (b->type[i] != DE_BODY_DYNAMIC)
        return;
    f64 density = (f64)b->material[i].density;
    f64 mass = density * u->volume;
    if (mass_override > 0.0f) {
        mass = (f64)mass_override;
        density = mass / u->volume;
    }
    f64 inertia[9], inv[9];
    for (u32 k = 0; k < 9; ++k)
        inertia[k] = u->inertia[k] * density;
    de__mat3_inverse_d(inertia, inv);
    b->mass[i] = (f32)mass;
    b->inv_mass[i] = (f32)(1.0 / mass);
    mat3_from_d(inertia, &b->local_inertia[i]);
    mat3_from_d(inv, &b->local_inv_inertia[i]);
}

/* ---- Public API ------------------------------------------------------------------------ */

static bool quat_finite(de_quat q) {
    return de__f32_finite(q.x) && de__f32_finite(q.y) && de__f32_finite(q.z) && de__f32_finite(q.w);
}

/* A zero quaternion is the identity; otherwise q / |q| computed in f64. */
static de_quat quat_normalized(de_quat q) {
    return de_dnormalize_quat(q);
}

de_body de_body_create(de_physics_world *p, const de_body_desc *desc) {
    de_body none = {0};
    if (p == NULL || desc == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_body_create: NULL world or desc");
        return none;
    }
    if ((u32)desc->type > (u32)DE_BODY_DYNAMIC) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_body_create: unknown body type %u", (u32)desc->type);
        return none;
    }
    if (!de__d3_finite(desc->transform.position) || !quat_finite(desc->transform.rotation) ||
        !de__v3_finite(desc->linear_velocity) || !de__v3_finite(desc->angular_velocity)) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_body_create: non-finite transform or velocity");
        return none;
    }
    if (!de__f32_finite(desc->mass) || desc->mass < 0.0f ||
        !de__f32_finite(desc->material.density) || desc->material.density < 0.0f ||
        !de__f32_finite(desc->linear_damping) || !de__f32_finite(desc->angular_damping) ||
        !de__f32_finite(desc->gravity_scale)) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_body_create: bad mass, density or damping");
        return none;
    }
    /* DENG-217: friction and restitution feed the solver: finite and >= 0. */
    if (!de__f32_finite(desc->material.friction) || desc->material.friction < 0.0f ||
        !de__f32_finite(desc->material.restitution) || desc->material.restitution < 0.0f ||
        !de__f32_finite(desc->friction_secondary) || desc->friction_secondary < 0.0f ||
        !de__v3_finite(desc->friction_axis)) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_body_create: bad friction or restitution");
        return none;
    }
    de__shape *s = NULL;
    if (desc->shape.id != 0) {
        s = de__shape_get(p, desc->shape);
        if (s == NULL) {
            DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_body_create: stale shape 0x%08x", desc->shape.id);
            return none;
        }
    }
    if (desc->type == DE_BODY_DYNAMIC && (s == NULL || !s->has_volume)) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                "de_body_create: a dynamic body needs a shape with volume (not a mesh, a "
                "heightfield or a compound holding one)");
        return none;
    }
    de__body_pool *b = &p->bodies;
    u32 i;
    if (b->free_count > 0) {
        i = b->free_list[--b->free_count];
    } else if (b->high_water < b->capacity) {
        i = b->high_water++;
        b->generation[i] = 1;
    } else {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_body_create: body pool full (max_bodies %u)",
                b->capacity);
        return none;
    }

    b->type[i] = (u8)desc->type;
    b->flags[i] = desc->flags;
    b->position[i] = desc->transform.position;
    b->rotation[i] = quat_normalized(desc->transform.rotation);
    if (desc->type == DE_BODY_STATIC) {
        b->linear_velocity[i] = de__v3(0.0f, 0.0f, 0.0f);
        b->angular_velocity[i] = de__v3(0.0f, 0.0f, 0.0f);
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
        b->material[i].density = DE_PHYSICS_DEFAULT_DENSITY;
    b->owner[i] = desc->owner;
    /* Zero stays zero (isotropic); anything else becomes a unit axis. */
    b->friction_axis[i] = de_dnormalize_v3(desc->friction_axis);
    b->friction_secondary[i] = desc->friction_secondary;
    body_set_mass(b, i, s, desc->mass);
    if (s != NULL)
        ++s->refcount;
    b->alive[i] = 1;
    ++b->live_count;
    de__body_refresh_aabb(p, i);

    for (u32 e = 0; e < DE__PHYS_EXT_COUNT; ++e) {
        const de__phys_ext *x = &p->ext[e];
        if (x->state != NULL && x->vtbl != NULL && x->vtbl->body_added != NULL)
            x->vtbl->body_added(p, x->state, i);
    }
    return de__body_handle(p, i);
}

void de_body_destroy(de_physics_world *p, de_body h) {
    u32 i;
    if (!de__body_resolve(p, h, &i))
        return;
    for (u32 e = 0; e < DE__PHYS_EXT_COUNT; ++e) {
        const de__phys_ext *x = &p->ext[e];
        if (x->state != NULL && x->vtbl != NULL && x->vtbl->body_removed != NULL)
            x->vtbl->body_removed(p, x->state, i);
    }
    de__body_pool *b = &p->bodies;
    de__shape *s = de__shape_get(p, b->shape[i]);
    if (s != NULL) {
        DE_ASSERT(s->refcount > 0);
        --s->refcount;
    }
    b->alive[i] = 0;
    b->generation[i] = de__gen_next(b->generation[i]);
    b->free_list[b->free_count++] = i;
    --b->live_count;
}

bool de_body_valid(const de_physics_world *p, de_body h) {
    u32 i;
    return de__body_resolve(p, h, &i);
}

u32 de_body_count(const de_physics_world *p) {
    return p != NULL ? p->bodies.live_count : 0;
}

bool de_body_get_transform(const de_physics_world *p, de_body h, de_transform *out) {
    u32 i;
    if (out == NULL || !de__body_resolve(p, h, &i))
        return false;
    out->position = p->bodies.position[i];
    out->rotation = p->bodies.rotation[i];
    out->scale = de__v3(1.0f, 1.0f, 1.0f);
    return true;
}

bool de_body_set_transform(de_physics_world *p, de_body h, const de_transform *t) {
    u32 i;
    if (t == NULL || !de__body_resolve(p, h, &i))
        return false;
    if (!de__d3_finite(t->position) || !quat_finite(t->rotation))
        return false;
    p->bodies.position[i] = t->position;
    p->bodies.rotation[i] = quat_normalized(t->rotation);
    de__body_refresh_aabb(p, i);
    return true;
}

bool de_body_get_velocity(const de_physics_world *p, de_body h, de_vec3 *linear, de_vec3 *angular) {
    u32 i;
    if (!de__body_resolve(p, h, &i))
        return false;
    if (linear != NULL)
        *linear = p->bodies.linear_velocity[i];
    if (angular != NULL)
        *angular = p->bodies.angular_velocity[i];
    return true;
}

bool de_body_set_velocity(de_physics_world *p, de_body h, de_vec3 linear, de_vec3 angular) {
    u32 i;
    if (!de__body_resolve(p, h, &i) || !de__v3_finite(linear) || !de__v3_finite(angular))
        return false;
    if (p->bodies.type[i] == DE_BODY_STATIC)
        return true;
    p->bodies.linear_velocity[i] = linear;
    p->bodies.angular_velocity[i] = angular;
    return true;
}

bool de_body_get_mass_props(const de_physics_world *p, de_body h, de_mass_props *out) {
    u32 i;
    if (out == NULL || !de__body_resolve(p, h, &i))
        return false;
    out->mass = p->bodies.mass[i];
    out->center = p->bodies.local_com[i];
    out->inertia = p->bodies.local_inertia[i];
    return true;
}
