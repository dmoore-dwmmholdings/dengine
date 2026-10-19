/* DENG-260: reflection, resource and schedule registration for the 2D world. */
#include "physics/2d/ecs2d/ecs2d_internal.h"
#include <stddef.h>
typedef struct DePhysics2DWorld {
    de_physics2d_world *world;
} DePhysics2DWorld;
static de_result type(de_type_registry *r, const de_type_desc *d, de_type_id *out) {
    *out = de_type_find(r, d->name);
    return out->id ? DE_OK : de_type_register_ex(r, d, out, NULL);
}
static void rb_construct(void *v) {
    DeRigidBody2D *r = v;
    memset(r, 0, sizeof *r);
    r->gravity_scale = 1;
}
static de_result components(de_world *w, de__ecs2d *s) {
    de_type_registry *r = de_world_types(w);
    de_type_id id;
    static const de_field_info material[] = {
        DE_FIELD(de_material_phys, friction, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(de_material_phys, restitution, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(de_material_phys, density, DE_FK_F32, DE_FIELD_DEFAULT)};
    static const de_field_info filter[] = {
        DE_FIELD(de_collision_filter, layer, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(de_collision_filter, mask, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(de_collision_filter, group, DE_FK_U32, DE_FIELD_DEFAULT)};
    DE_TRY(type(r, &DE_TYPE_DESC(de_material_phys, 1, material), &id));
    DE_TRY(type(r, &DE_TYPE_DESC(de_collision_filter, 1, filter), &id));
    static const de_field_info rb[] = {
        DE_FIELD(DeRigidBody2D, type, DE_FK_I32, DE_FIELD_DEFAULT),
        DE_FIELD(DeRigidBody2D, mass, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DeRigidBody2D, linear_velocity, DE_FK_VEC2, DE_FIELD_DEFAULT),
        DE_FIELD(DeRigidBody2D, angular_velocity, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DeRigidBody2D, linear_damping, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DeRigidBody2D, angular_damping, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DeRigidBody2D, gravity_scale, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DeRigidBody2D, fixed_rotation, DE_FK_BOOL, DE_FIELD_DEFAULT),
        DE_FIELD(DeRigidBody2D, bullet, DE_FK_BOOL, DE_FIELD_DEFAULT),
        DE_FIELD(DeRigidBody2D, body, DE_FK_U32, DE_FIELD_SNAPSHOT),
    };
    de_type_desc rb_desc = DE_TYPE_DESC(DeRigidBody2D, 1, rb);
    rb_desc.hooks.construct = rb_construct;
    DE_TRY(type(r, &rb_desc, &id));
    s->rb = de_component_register(w, id, DE_COMP_SNAPSHOT);
    static const de_field_info col[] = {
        DE_FIELD(DeCollider2D, shape, DE_FK_U32, DE_FIELD_SNAPSHOT),
        DE_FIELD(DeCollider2D, kind, DE_FK_I32, DE_FIELD_DEFAULT),
        DE_FIELD(DeCollider2D, half_extents, DE_FK_VEC2, DE_FIELD_DEFAULT),
        DE_FIELD(DeCollider2D, radius, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DeCollider2D, point_count, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(DeCollider2D, is_trigger, DE_FK_BOOL, DE_FIELD_DEFAULT),
        DE_FIELD_EX(DeCollider2D, material, DE_FK_STRUCT, DE_FIELD_DEFAULT,
                    {0, 0, 0, 0, "de_material_phys", 0}),
        DE_FIELD_EX(DeCollider2D, filter, DE_FK_STRUCT, DE_FIELD_DEFAULT,
                    {0, 0, 0, 0, "de_collision_filter", 0}),
        DE_FIELD_EX(DeCollider2D, points, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT,
                    {0, 0, 0, 0, "vec2", 8}),
    };
    de_type_desc col_desc = DE_TYPE_DESC(DeCollider2D, 1, col);
    DE_TRY(type(r, &col_desc, &id));
    s->col = de_component_register(w, id, DE_COMP_SNAPSHOT);
    static const de_field_info character[] = {
        DE_FIELD(DeCharacter2D, size, DE_FK_VEC2, DE_FIELD_DEFAULT),
        DE_FIELD(DeCharacter2D, max_slope_rad, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DeCharacter2D, step_height, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DeCharacter2D, desired_velocity, DE_FK_VEC2, DE_FIELD_DEFAULT),
        DE_FIELD(DeCharacter2D, velocity, DE_FK_VEC2, DE_FIELD_SNAPSHOT),
        DE_FIELD(DeCharacter2D, grounded, DE_FK_BOOL, DE_FIELD_SNAPSHOT),
    };
    de_type_desc character_desc = DE_TYPE_DESC(DeCharacter2D, 1, character);
    DE_TRY(type(r, &character_desc, &id));
    s->character = de_component_register(w, id, DE_COMP_SNAPSHOT);
    de_type_desc resource = {0};
    resource.name = "DePhysics2DWorld";
    resource.size = sizeof(DePhysics2DWorld);
    resource.align = DE_ALIGNOF(DePhysics2DWorld);
    resource.version = 1;
    resource.is_pod = true;
    DE_TRY(type(r, &resource, &id));
    DePhysics2DWorld value = {s->p};
    s->resource = de_resource_insert(w, id, &value);
    static const de_field_info event[] = {
        DE_FIELD(de_contact_event, kind, DE_FK_I32, DE_FIELD_SNAPSHOT),
        DE_FIELD(de_contact_event, a, DE_FK_ENTITY, DE_FIELD_SNAPSHOT),
        DE_FIELD(de_contact_event, b, DE_FK_ENTITY, DE_FIELD_SNAPSHOT),
        DE_FIELD(de_contact_event, point, DE_FK_DVEC3, DE_FIELD_SNAPSHOT),
        DE_FIELD(de_contact_event, normal, DE_FK_VEC3, DE_FIELD_SNAPSHOT),
        DE_FIELD(de_contact_event, impulse, DE_FK_F32, DE_FIELD_SNAPSHOT)};
    DE_TRY(type(r, &DE_TYPE_DESC(de_contact_event, 1, event), &id));
    s->contact = de_event_register(w, id);
    de_builtin_component_ids builtin = de_builtin_components(w);
    s->transform = builtin.transform;
    s->global = builtin.global_transform;
    if (!s->rb.id || !s->col.id || !s->character.id || !s->resource.id || !s->contact.id ||
        !s->transform.id)
        return DE_FAIL(DE_ERR_INTERNAL, "2D ECS registration failed");
    return DE_OK;
}
static void destroy(de_physics2d_world *p, void *v) {
    de__ecs2d *s = v;
    /* Like 3D, the ECS world owns systems; callers must stop schedules before physics teardown. */
    DE_FREE(p->alloc, s->entries, sizeof *s->entries * s->capacity);
    DE_FREE(p->alloc, s, sizeof *s);
}
static void removed(de_physics2d_world *p, void *v, u32 i) {
    de__ecs2d *s = v;
    if (i >= s->capacity)
        return;
    s->entries[i].grave = p->bodies.owner[i];
    s->entries[i].grave_generation = p->bodies.generation[i];
}
static const de__p2d_ext_vtbl ext = {destroy, NULL, removed};
void de_physics2d_attach(de_physics2d_world *p, de_world *w) {
    if (!p || !w) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "NULL 2D ECS attachment");
        return;
    }
    de__ecs2d *s = de__p2d_ext_get(p, DE__P2D_EXT_ECS);
    if (s) {
        if (s->w != w)
            DE_FAIL(DE_ERR_ALREADY_EXISTS, "2D physics attached elsewhere");
        return;
    }
    s = DE_ALLOC(p->alloc, sizeof *s, DE_ALIGNOF(de__ecs2d), DE_MEM_PHYSICS);
    if (!s)
        return;
    memset(s, 0, sizeof *s);
    s->p = p;
    s->w = w;
    s->capacity = p->max_bodies;
    s->entries = DE_ALLOC(p->alloc, sizeof *s->entries * s->capacity, DE_ALIGNOF(de__ecs2d_entry),
                          DE_MEM_PHYSICS);
    if (!s->entries) {
        DE_FREE(p->alloc, s, sizeof *s);
        return;
    }
    memset(s->entries, 0, sizeof *s->entries * s->capacity);
    if (components(w, s) != DE_OK) {
        destroy(p, s);
        return;
    }
    de_query_term terms[] = {{s->transform, DE_WRITE},
                             {s->rb, DE_OPTIONAL_WRITE},
                             {s->col, DE_OPTIONAL_READ},
                             {s->character, DE_OPTIONAL_WRITE}};
    de_query_desc q = {0};
    q.terms = terms;
    q.term_count = 4;
    de_resource_access resource = {s->resource, DE_WRITE};
    de_system_desc d = {0};
    d.name = "physics2d.step";
    d.schedule = DE_SCHEDULE_FIXED_UPDATE;
    d.fn = de__ecs2d_step;
    d.user = s;
    d.flags = DE_SYSTEM_DETERMINISTIC;
    d.queries = &q;
    d.query_count = 1;
    d.resources = &resource;
    d.resource_count = 1;
    d.writes_events = &s->contact;
    d.writes_event_count = 1;
    s->step = de_system_add(w, &d);
    de_query_term interp[] = {{s->transform, DE_READ}, {s->global, DE_OPTIONAL_WRITE}};
    q.terms = interp;
    q.term_count = 2;
    resource.access = DE_READ;
    const char *after[] = {DE_TRANSFORM_PROPAGATE_SYSTEM};
    d.name = "physics2d.interpolate";
    d.schedule = DE_SCHEDULE_POST_UPDATE;
    d.fn = de__ecs2d_interpolate;
    d.flags = 0;
    d.after = after;
    d.after_count = 1;
    d.writes_events = NULL;
    d.writes_event_count = 0;
    s->interpolate = de_system_add(w, &d);
    if (!s->step.id || !s->interpolate.id) {
        if (s->step.id)
            de_system_remove(w, s->step);
        destroy(p, s);
        return;
    }
    de__p2d_ext_set(p, DE__P2D_EXT_ECS, s, &ext);
}
