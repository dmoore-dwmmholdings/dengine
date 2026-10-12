/*
 * attach.c: de_physics_attach, the only place where physics meets the ECS (DENG-132, SPEC §13.1).
 *
 * Registers the field types (de_body_type, de_transform, de_material_phys, de_collision_filter),
 * the components DeRigidBody, DeCollider and DeCharacter (DE_COMP_SNAPSHOT), and the resource
 * type DePhysicsWorld, then inserts the resource. A type that is already registered under the
 * same name (another module, or an earlier attach) is reused. The FixedUpdate systems are S23.
 */
#include "physics/physics_internal.h"

#include <stddef.h>

/* Runtime handles: kept in snapshots (rollback restores them with the physics state), never
 * serialized (a handle means nothing in another process) and not editable. */
#define DE__HANDLE_FIELD_FLAGS (DE_FIELD_SNAPSHOT | DE_FIELD_EDITOR_HIDDEN | DE_FIELD_READONLY)

static de_result last_error_or(de_result fallback) {
    const de_error *e = de_error_last();
    return e != NULL && e->result != DE_OK ? e->result : fallback;
}

/* The id of type `desc->name`, registering it when the registry does not have it yet. */
static de_result ensure_type(de_type_registry *types, const de_type_desc *desc, de_type_id *out) {
    *out = de_type_find(types, desc->name);
    if (DE_HANDLE_VALID(*out))
        return DE_OK;
    const de_result r = de_type_register_ex(types, desc, out, NULL);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "de_physics_attach: registering %s", desc->name);
    return DE_OK;
}

static de_result register_types(de_type_registry *types, de_type_id ids[4]) {
    if (!DE_HANDLE_VALID(de_type_find(types, "de_body_type"))) {
        static const de_enum_value k_body_type[] = {
            {"DE_BODY_STATIC", DE_BODY_STATIC},
            {"DE_BODY_KINEMATIC", DE_BODY_KINEMATIC},
            {"DE_BODY_DYNAMIC", DE_BODY_DYNAMIC},
        };
        const de_result r = de_enum_register(types, "de_body_type", k_body_type, 3, false);
        if (r != DE_OK)
            return DE_FAIL_WRAP(r, "de_physics_attach: registering de_body_type");
    }

    static const de_field_info k_transform[] = {
        DE_FIELD_EX(de_transform, position, DE_FK_DVEC3, DE_FIELD_DEFAULT, {0, 0, "m", 0, 0, 0}),
        DE_FIELD(de_transform, rotation, DE_FK_QUAT, DE_FIELD_DEFAULT),
        DE_FIELD(de_transform, scale, DE_FK_VEC3, DE_FIELD_DEFAULT),
    };
    static const de_field_info k_material[] = {
        DE_FIELD(de_material_phys, friction, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(de_material_phys, restitution, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD_EX(de_material_phys, density, DE_FK_F32, DE_FIELD_DEFAULT,
                    {0, 0, "kg/m^3", "0 = 1000 (water)", 0, 0}),
    };
    static const de_field_info k_filter[] = {
        DE_FIELD(de_collision_filter, layer, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(de_collision_filter, mask, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(de_collision_filter, group, DE_FK_U32, DE_FIELD_DEFAULT),
    };
    DE_TRY(ensure_type(types, &DE_TYPE_DESC(de_transform, 1, k_transform), &ids[0]));
    DE_TRY(ensure_type(types, &DE_TYPE_DESC(de_material_phys, 1, k_material), &ids[1]));
    DE_TRY(ensure_type(types, &DE_TYPE_DESC(de_collision_filter, 1, k_filter), &ids[2]));

    /* DePhysicsWorld holds a pointer: no reflected fields, so it is never serialized. */
    de_type_desc world_desc;
    memset(&world_desc, 0, sizeof world_desc);
    world_desc.name = "DePhysicsWorld";
    world_desc.size = (u32)sizeof(DePhysicsWorld);
    world_desc.align = (u32)DE_ALIGNOF(DePhysicsWorld);
    world_desc.version = 1;
    world_desc.is_pod = true;
    DE_TRY(ensure_type(types, &world_desc, &ids[3]));
    return DE_OK;
}

static de_result register_components(de_type_registry *types, de_type_id out[3]) {
    static const de_field_info k_rigid_body[] = {
        DE_FIELD_EX(DeRigidBody, type, DE_FK_ENUM, DE_FIELD_DEFAULT,
                    {0, 0, 0, 0, "de_body_type", 0}),
        DE_FIELD_EX(DeRigidBody, mass, DE_FK_F32, DE_FIELD_DEFAULT,
                    {0, 0, "kg", "0 = from the collider's volume and density", 0, 0}),
        DE_FIELD_EX(DeRigidBody, linear_velocity, DE_FK_VEC3, DE_FIELD_DEFAULT,
                    {0, 0, "m/s", 0, 0, 0}),
        DE_FIELD_EX(DeRigidBody, angular_velocity, DE_FK_VEC3, DE_FIELD_DEFAULT,
                    {0, 0, "rad/s", 0, 0, 0}),
        DE_FIELD(DeRigidBody, linear_damping, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DeRigidBody, angular_damping, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DeRigidBody, gravity_scale, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DeRigidBody, ccd, DE_FK_BOOL, DE_FIELD_DEFAULT),
        DE_FIELD(DeRigidBody, allow_sleep, DE_FK_BOOL, DE_FIELD_DEFAULT),
        DE_FIELD(DeRigidBody, lock_rot_x, DE_FK_BOOL, DE_FIELD_DEFAULT),
        DE_FIELD(DeRigidBody, lock_rot_y, DE_FK_BOOL, DE_FIELD_DEFAULT),
        DE_FIELD(DeRigidBody, lock_rot_z, DE_FK_BOOL, DE_FIELD_DEFAULT),
        DE_FIELD(DeRigidBody, body, DE_FK_U32, DE__HANDLE_FIELD_FLAGS),
    };
    static const de_field_info k_collider[] = {
        DE_FIELD(DeCollider, shape, DE_FK_U32, DE__HANDLE_FIELD_FLAGS),
        DE_FIELD_EX(DeCollider, offset, DE_FK_STRUCT, DE_FIELD_DEFAULT,
                    {0, 0, 0, 0, "de_transform", 0}),
        DE_FIELD_EX(DeCollider, material, DE_FK_STRUCT, DE_FIELD_DEFAULT,
                    {0, 0, 0, 0, "de_material_phys", 0}),
        DE_FIELD_EX(DeCollider, filter, DE_FK_STRUCT, DE_FIELD_DEFAULT,
                    {0, 0, 0, 0, "de_collision_filter", 0}),
        DE_FIELD(DeCollider, is_trigger, DE_FK_BOOL, DE_FIELD_DEFAULT),
    };
    static const de_field_info k_character[] = {
        DE_FIELD_EX(DeCharacter, radius, DE_FK_F32, DE_FIELD_DEFAULT, {0, 0, "m", 0, 0, 0}),
        DE_FIELD_EX(DeCharacter, height, DE_FK_F32, DE_FIELD_DEFAULT, {0, 0, "m", 0, 0, 0}),
        DE_FIELD_EX(DeCharacter, step_height, DE_FK_F32, DE_FIELD_DEFAULT, {0, 0, "m", 0, 0, 0}),
        DE_FIELD_EX(DeCharacter, max_slope_rad, DE_FK_F32, DE_FIELD_DEFAULT,
                    {0, 0, "rad", 0, 0, 0}),
        DE_FIELD_EX(DeCharacter, skin_width, DE_FK_F32, DE_FIELD_DEFAULT, {0, 0, "m", 0, 0, 0}),
        DE_FIELD_EX(DeCharacter, desired_velocity, DE_FK_VEC3, DE_FIELD_DEFAULT,
                    {0, 0, "m/s", 0, 0, 0}),
        /* Outputs written by the S23 character system: snapshot only. */
        DE_FIELD(DeCharacter, velocity, DE_FK_VEC3, DE_FIELD_SNAPSHOT | DE_FIELD_READONLY),
        DE_FIELD(DeCharacter, grounded, DE_FK_BOOL, DE_FIELD_SNAPSHOT | DE_FIELD_READONLY),
        DE_FIELD(DeCharacter, ground_normal, DE_FK_VEC3, DE_FIELD_SNAPSHOT | DE_FIELD_READONLY),
        DE_FIELD(DeCharacter, ground_entity, DE_FK_ENTITY, DE_FIELD_SNAPSHOT | DE_FIELD_READONLY),
    };
    DE_TRY(ensure_type(types, &DE_TYPE_DESC(DeRigidBody, 1, k_rigid_body), &out[0]));
    DE_TRY(ensure_type(types, &DE_TYPE_DESC(DeCollider, 1, k_collider), &out[1]));
    DE_TRY(ensure_type(types, &DE_TYPE_DESC(DeCharacter, 1, k_character), &out[2]));
    return DE_OK;
}

de_result de_physics_attach(de_physics_world *p, de_world *w, de_type_registry *types) {
    if (p == NULL || w == NULL || types == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_physics_attach: NULL argument");
    de_type_id field_types[4], comp_types[3];
    DE_TRY(register_types(types, field_types));
    DE_TRY(register_components(types, comp_types));

    /* Registration order is part of the determinism contract (component ids): fixed here. */
    de_component comps[3];
    for (u32 i = 0; i < 3; ++i) {
        comps[i] = de_component_register(w, comp_types[i], DE_COMP_SNAPSHOT);
        if (!DE_HANDLE_VALID(comps[i]))
            return DE_FAIL_WRAP(last_error_or(DE_ERR_INVALID_ARGUMENT),
                                "de_physics_attach: registering component %u", i);
    }
    DePhysicsWorld init;
    init.world = p;
    const de_resource_id res = de_resource_insert(w, field_types[3], &init);
    if (!DE_HANDLE_VALID(res))
        return DE_FAIL_WRAP(last_error_or(DE_ERR_INVALID_ARGUMENT),
                            "de_physics_attach: inserting the DePhysicsWorld resource");
    p->ecs_world = w;
    p->comp_rigid_body = comps[0];
    p->comp_collider = comps[1];
    p->comp_character = comps[2];
    p->res_physics_world = res;
    return DE_OK;
}
