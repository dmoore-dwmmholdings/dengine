/* test_authoring.c: physics authoring for DIDE (DENG-766): physmat and ragdoll asset parsing,
 * combine modes, DeCollider shape_kind and material_asset, the joint components with limits in a
 * simple sim, and DeRagdoll spawning from a registered asset. */
#include "ecs_test_util.h"
#include "joint_test_util.h"

#include <stdlib.h>

#define DEG (JT_PI / 180.0f)

static f32 absf(f32 v) {
    return v < 0.0f ? -v : v;
}

/* ================================ Materials ============================================ */

TEST(physics_authoring, physmat_parse) {
    static const char k_text[] = "version = 1\n"
                                 "friction = 0.8\n"
                                 "restitution = 0.25\n"
                                 "density = 500\n"
                                 "friction_combine = \"minimum\"\n"
                                 "restitution_combine = \"average\"\n";
    de_material_phys m;
    ASSERT_EQ(de_physics_material_parse(k_text, (u32)strlen(k_text), &m), DE_OK);
    EXPECT_TRUE(absf(m.friction - 0.8f) < 1e-6f);
    EXPECT_TRUE(absf(m.restitution - 0.25f) < 1e-6f);
    EXPECT_TRUE(absf(m.density - 500.0f) < 1e-3f);
    EXPECT_EQ((u32)m.friction_combine, (u32)DE_COMBINE_MINIMUM);
    EXPECT_EQ((u32)m.restitution_combine, (u32)DE_COMBINE_AVERAGE);

    /* Defaults (DIDE's): friction 0.5, geometric / maximum. */
    static const char k_min[] = "version = 1\n";
    ASSERT_EQ(de_physics_material_parse(k_min, (u32)strlen(k_min), &m), DE_OK);
    EXPECT_TRUE(absf(m.friction - 0.5f) < 1e-6f);
    EXPECT_EQ((u32)m.friction_combine, (u32)DE_COMBINE_GEOMETRIC);
    EXPECT_EQ((u32)m.restitution_combine, (u32)DE_COMBINE_MAXIMUM);

    static const char k_bad_mode[] = "friction_combine = \"loudest\"\n";
    EXPECT_EQ(de_physics_material_parse(k_bad_mode, (u32)strlen(k_bad_mode), &m), DE_ERR_PARSE);
    static const char k_neg[] = "friction = -1.0\n";
    EXPECT_EQ(de_physics_material_parse(k_neg, (u32)strlen(k_neg), &m),
              DE_ERR_INVALID_ARGUMENT);
    static const char k_type[] = "friction = \"high\"\n";
    EXPECT_EQ(de_physics_material_parse(k_type, (u32)strlen(k_type), &m), DE_ERR_PARSE);
    de_error_clear();
}

TEST(physics_authoring, combine_rules) {
    /* The mode later in the list wins; DEFAULT takes the other mode, or the given default. */
    EXPECT_TRUE(absf(de__combine(DE_COMBINE_AVERAGE, DE_COMBINE_AVERAGE, 0.2f, 0.6f,
                                 DE_COMBINE_GEOMETRIC) -
                     0.4f) < 1e-6f);
    EXPECT_TRUE(absf(de__combine(DE_COMBINE_MINIMUM, DE_COMBINE_AVERAGE, 0.2f, 0.6f,
                                 DE_COMBINE_GEOMETRIC) -
                     0.2f) < 1e-6f);
    EXPECT_TRUE(absf(de__combine(DE_COMBINE_MINIMUM, DE_COMBINE_MULTIPLY, 0.5f, 0.6f,
                                 DE_COMBINE_GEOMETRIC) -
                     0.3f) < 1e-6f);
    EXPECT_TRUE(absf(de__combine(DE_COMBINE_MAXIMUM, DE_COMBINE_MINIMUM, 0.2f, 0.6f,
                                 DE_COMBINE_GEOMETRIC) -
                     0.6f) < 1e-6f);
    EXPECT_TRUE(absf(de__combine(DE_COMBINE_DEFAULT, DE_COMBINE_DEFAULT, 0.25f, 1.0f,
                                 DE_COMBINE_GEOMETRIC) -
                     0.5f) < 1e-6f);
    EXPECT_TRUE(absf(de__combine(DE_COMBINE_DEFAULT, DE_COMBINE_DEFAULT, 0.25f, 1.0f,
                                 DE_COMBINE_MAXIMUM) -
                     1.0f) < 1e-6f);
    EXPECT_TRUE(absf(de__combine(DE_COMBINE_DEFAULT, DE_COMBINE_MINIMUM, 0.25f, 1.0f,
                                 DE_COMBINE_MAXIMUM) -
                     0.25f) < 1e-6f);
}

/* A ball with restitution 1 dropped on ground with restitution 0 (default rule): rebound height
 * after the first bounce. The ball's material comes from a registered asset (DeCollider.material_asset). */
static f64 bounce_height(de_combine_mode ball_mode) {
    pe_env e;
    pe_init(&e, 0);
    pe_ground(&e);
    de_material_phys m;
    memset(&m, 0, sizeof m);
    m.friction = 0.5f;
    m.restitution = 1.0f;
    m.restitution_combine = ball_mode;
    const de_name name = de_name_from("physics/materials/bouncy.physmat.toml");
    EXPECT_EQ(de_physics_material_register(e.p, name, &m), DE_OK);
    de_material_phys found;
    EXPECT_TRUE(de_physics_material_find(e.p, name, &found));
    const de_entity ball = pe_spawn_xf(&e, 0.0, 0.0, 2.0);
    DeRigidBody *rb = (DeRigidBody *)de_entity_add(e.w, ball, e.rb);
    rb->type = DE_BODY_DYNAMIC;
    DeCollider *c = pe_add_collider(&e, ball, (de_shape){0});
    c->shape_kind = DE_COLLIDER_SHAPE_SPHERE;
    c->radius = 0.25f;
    c->material_asset = name;
    f64 peak = 0.0;
    bool hit = false;
    for (u32 t = 0; t < 150; ++t) {
        ASSERT_EQ(pe_tick(&e), DE_OK);
        const f64 z = pe_xf(&e, ball)->position.z;
        if (z < 0.4)
            hit = true;
        if (hit && z > peak)
            peak = z;
    }
    EXPECT_TRUE(hit);
    pe_free(&e);
    return peak;
}

TEST(physics_authoring, restitution_combine_via_material_asset) {
    const f64 maxed = bounce_height(DE_COMBINE_MAXIMUM); /* max(1, 0) = 1 */
    const f64 minned = bounce_height(DE_COMBINE_MINIMUM); /* min(1, 0) = 0 */
    fprintf(stderr, "  rebound: maximum %.3f m, minimum %.3f m\n", maxed, minned);
    EXPECT_TRUE(maxed > 1.2);
    EXPECT_TRUE(minned < 0.4);
}

/* ================================ Collider shapes ====================================== */

TEST(physics_authoring, collider_shape_kinds_rest_on_ground) {
    pe_env e;
    pe_init(&e, 0);
    pe_ground(&e);
    de_entity ents[4];
    const de_collider_shape kinds[4] = {DE_COLLIDER_SHAPE_SPHERE, DE_COLLIDER_SHAPE_CAPSULE,
                                        DE_COLLIDER_SHAPE_BOX, DE_COLLIDER_SHAPE_CONVEX};
    const f64 rest[4] = {0.5, 0.5, 0.5, 0.5};
    for (u32 i = 0; i < 4; ++i) {
        ents[i] = pe_spawn_xf(&e, (f64)i * 3.0, 0.0, 1.5);
        DeRigidBody *rb = (DeRigidBody *)de_entity_add(e.w, ents[i], e.rb);
        rb->type = DE_BODY_DYNAMIC;
        if (kinds[i] == DE_COLLIDER_SHAPE_CONVEX) {
            DeConvexHull *h =
                (DeConvexHull *)de_entity_add(e.w, ents[i], e.p->comp_authoring[0]);
            h->point_count = 8;
            for (u32 k = 0; k < 8; ++k)
                h->points[k] = (de_vec3){(k & 1u) ? 0.5f : -0.5f, (k & 2u) ? 0.5f : -0.5f,
                                         (k & 4u) ? 0.5f : -0.5f};
        }
        DeCollider *c = pe_add_collider(&e, ents[i], (de_shape){0});
        c->shape_kind = kinds[i];
        c->radius = 0.5f;
        c->half_height = 0.0f; /* capsule as a sphere-ended stub: rests at its radius */
        c->half_extents = (de_vec3){0.5f, 0.5f, 0.5f};
    }
    for (u32 t = 0; t < 180; ++t)
        ASSERT_EQ(pe_tick(&e), DE_OK);
    for (u32 i = 0; i < 4; ++i) {
        const f64 z = pe_xf(&e, ents[i])->position.z;
        fprintf(stderr, "  kind %u rests at z = %.3f\n", (u32)kinds[i], z);
        EXPECT_TRUE(z > rest[i] - 0.05 && z < rest[i] + 0.05);
    }
    EXPECT_EQ(de_body_count(e.p), 5u);

    /* A new radius rebuilds the shape: the sphere grows and rises. */
    DeCollider *c = (DeCollider *)de_entity_get(e.w, ents[0], e.col);
    c->radius = 1.0f;
    for (u32 t = 0; t < 120; ++t)
        ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_TRUE(pe_xf(&e, ents[0])->position.z > 0.95);

    /* A convex collider without enough points gets no body; the rest still step. */
    const de_entity bad = pe_spawn_xf(&e, 20.0, 0.0, 1.0);
    DeCollider *bc = pe_add_collider(&e, bad, (de_shape){0});
    bc->shape_kind = DE_COLLIDER_SHAPE_CONVEX;
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_FALSE(DE_HANDLE_VALID(de_physics_entity_body(e.p, bad)));
    de_error_clear();
    pe_free(&e);
}

/* ================================ Joints =============================================== */

/* A 1 m beam on a hinge about world +Y at (0, 0, 5) to a static anchor; gravity swings it.
 * Returns the largest |hinge angle| over 2 s. */
static f32 hinge_swing(bool limit) {
    pe_env e;
    pe_init(&e, 0);
    const de_entity anchor = pe_spawn_static(&e, st_box(e.p, 0.05f, 0.05f, 0.05f), 0.0, 0.0, 5.0);
    const de_entity beam =
        pe_spawn_body(&e, DE_BODY_DYNAMIC, st_box(e.p, 0.5f, 0.1f, 0.1f), 0.5, 0.0, 5.0);
    const de_quat hy = st_quat_axis_angle((de_vec3){0, 0, 1}, 0.5f * JT_PI); /* X -> world Y */
    DeHingeJoint *h = (DeHingeJoint *)de_entity_add(e.w, beam, e.p->comp_authoring[1]);
    h->target = anchor;
    h->frame = st_xform(-0.5, 0, 0, hy);
    h->target_frame = st_xform(0, 0, 0, hy);
    h->enable_limit = limit;
    h->limit_min = -30.0f * DEG;
    h->limit_max = 30.0f * DEG;
    f32 worst = 0.0f;
    for (u32 t = 0; t < 120; ++t) {
        ASSERT_EQ(pe_tick(&e), DE_OK);
        const de_body b = de_physics_entity_body(e.p, beam);
        f32 tw, sy, sz;
        jt_angles(st_quat_identity(), hy, st_rot(e.p, b), hy, &tw, &sy, &sz);
        worst = absf(tw) > worst ? absf(tw) : worst;
    }
    const DeHingeJoint *hc =
        (const DeHingeJoint *)de_entity_get_const(e.w, beam, e.p->comp_authoring[1]);
    EXPECT_TRUE(de_joint_valid(e.p, hc->joint));
    /* The beam stays on its pivot. */
    const de_dvec3 tip = jt_world_point(e.p, de_physics_entity_body(e.p, beam),
                                        (de_dvec3){-0.5, 0, 0});
    EXPECT_TRUE(jt_dist(tip, (de_dvec3){0, 0, 5}) < 0.02);

    /* Removing the component destroys the joint. */
    const de_joint j = hc->joint;
    de_entity_remove(e.w, beam, e.p->comp_authoring[1]);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_FALSE(de_joint_valid(e.p, j));
    pe_free(&e);
    return worst;
}

TEST(physics_authoring, hinge_component_limit_holds) {
    const f32 free_swing = hinge_swing(false);
    const f32 limited = hinge_swing(true);
    fprintf(stderr, "  hinge: free %.1f deg, limited %.1f deg\n", (f64)(free_swing / DEG),
            (f64)(limited / DEG));
    EXPECT_TRUE(free_swing > 60.0f * DEG);
    EXPECT_TRUE(limited < 32.0f * DEG);
}

TEST(physics_authoring, cone_and_fixed_components) {
    pe_env e;
    pe_init(&e, 0);
    const de_shape box = st_box(e.p, 0.2f, 0.2f, 0.2f);
    const de_entity anchor = pe_spawn_static(&e, box, 0.0, 0.0, 5.0);
    const de_entity a = pe_spawn_body(&e, DE_BODY_DYNAMIC, box, 1.0, 0.0, 5.0);
    const de_entity b = pe_spawn_body(&e, DE_BODY_DYNAMIC, box, 2.0, 0.0, 5.0);
    DeConeTwistJoint *c = (DeConeTwistJoint *)de_entity_add(e.w, a, e.p->comp_authoring[2]);
    c->target = anchor;
    c->frame = st_xform(-1.0, 0, 0, st_quat_identity());
    c->target_frame = st_xform(0, 0, 0, st_quat_identity());
    c->twist_min = -10.0f * DEG;
    c->twist_max = 10.0f * DEG;
    c->swing_y = 20.0f * DEG;
    c->swing_z = 20.0f * DEG;
    DeFixedJoint *f = (DeFixedJoint *)de_entity_add(e.w, b, e.p->comp_authoring[3]);
    f->target = a;
    f->frame = st_xform(-0.5, 0, 0, st_quat_identity());
    f->target_frame = st_xform(0.5, 0, 0, st_quat_identity());
    f32 worst_swing = 0.0f;
    for (u32 t = 0; t < 120; ++t) {
        ASSERT_EQ(pe_tick(&e), DE_OK);
        const de_body ba = de_physics_entity_body(e.p, a);
        f32 tw, sy, sz;
        jt_angles(st_quat_identity(), st_quat_identity(), st_rot(e.p, ba), st_quat_identity(),
                  &tw, &sy, &sz);
        const f32 s = absf(sy) > absf(sz) ? absf(sy) : absf(sz);
        worst_swing = s > worst_swing ? s : worst_swing;
    }
    fprintf(stderr, "  cone: worst swing %.1f deg\n", (f64)(worst_swing / DEG));
    EXPECT_TRUE(worst_swing < 26.0f * DEG); /* 20 deg limit, soft under a 2-body load */
    const DeConeTwistJoint *cc =
        (const DeConeTwistJoint *)de_entity_get_const(e.w, a, e.p->comp_authoring[2]);
    const DeFixedJoint *fc =
        (const DeFixedJoint *)de_entity_get_const(e.w, b, e.p->comp_authoring[3]);
    EXPECT_TRUE(de_joint_valid(e.p, cc->joint));
    EXPECT_TRUE(de_joint_valid(e.p, fc->joint));
    /* The fixed pair keeps its 1 m spacing. */
    const de_dvec3 pa = pe_xf(&e, a)->position, pb = pe_xf(&e, b)->position;
    EXPECT_TRUE(absf((f32)jt_dist(pa, pb) - 1.0f) < 0.02f);

    /* Despawning the target destroys the joints that point at it. */
    const de_joint fj = fc->joint;
    de_entity_despawn(e.w, a);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_FALSE(de_joint_valid(e.p, fj));
    pe_free(&e);
}

/* ================================ Ragdolls ============================================= */

static const char k_ragdoll[] =
    "version = 1\n"
    "skeleton = \"characters/hero.skel\"\n"
    "self_collision = false\n"
    "[[bones]]\n"
    "name = \"pelvis\"\n"
    "bind = { position = [0.0, 0.0, 1.0], rotation = [0.0, 0.0, 0.0, 1.0] }\n"
    "shape = \"box\"\n"
    "half_extents = [0.15, 0.1, 0.1]\n"
    "mass = 10.0\n"
    "[[bones]]\n"
    "name = \"spine\"\n"
    "parent = 0\n"
    "bind = { position = [0.0, 0.0, 1.35], rotation = [0.0, 0.0, 0.0, 1.0] }\n"
    "shape = \"capsule\"\n"
    "radius = 0.1\n"
    "half_height = 0.15\n"
    "mass = 8.0\n"
    "joint = \"cone_twist\"\n"
    "joint_rotation = [0.0, -0.70710678, 0.0, 0.70710678]\n"
    "limit_min = -0.3\n"
    "limit_max = 0.3\n"
    "swing_y = 0.4\n"
    "swing_z = 0.4\n"
    "[[bones]]\n"
    "name = \"head\"\n"
    "parent = 1\n"
    "bind = { position = [0.0, 0.0, 1.7], rotation = [0.0, 0.0, 0.0, 1.0] }\n"
    "shape = \"sphere\"\n"
    "radius = 0.12\n"
    "mass = 4.0\n"
    "joint = \"hinge\"\n"
    "limit_min = -0.5\n"
    "limit_max = 0.5\n";

TEST(physics_authoring, ragdoll_asset_parse) {
    de_ragdoll_asset *a = (de_ragdoll_asset *)calloc(1, sizeof *a);
    ASSERT_TRUE(a != NULL);
    ASSERT_EQ(de_ragdoll_asset_parse(k_ragdoll, (u32)strlen(k_ragdoll), a), DE_OK);
    EXPECT_EQ(a->bone_count, 3u);
    EXPECT_FALSE(a->self_collision);
    EXPECT_TRUE(strcmp(a->skeleton, "characters/hero.skel") == 0);
    EXPECT_EQ(a->bones[0].parent, DE_RAGDOLL_NO_PARENT);
    EXPECT_EQ((u32)a->bones[0].shape, (u32)DE_RAGDOLL_SHAPE_BOX);
    EXPECT_EQ(a->bones[1].parent, 0u);
    EXPECT_EQ((u32)a->bones[1].shape, (u32)DE_RAGDOLL_SHAPE_CAPSULE);
    EXPECT_EQ((u32)a->bones[1].joint, (u32)DE_RAGDOLL_JOINT_CONE_TWIST);
    EXPECT_TRUE(absf(a->bones[1].swing_y - 0.4f) < 1e-6f);
    EXPECT_EQ(a->bones[2].parent, 1u);
    EXPECT_EQ((u32)a->bones[2].joint, (u32)DE_RAGDOLL_JOINT_HINGE);
    EXPECT_TRUE(absf((f32)a->bones[2].bind.position.z - 1.7f) < 1e-6f);

    /* A bone whose parent is not lower, and a second root, are refused. */
    static const char k_bad[] = "[[bones]]\nname = \"a\"\nparent = 0\n";
    EXPECT_EQ(de_ragdoll_asset_parse(k_bad, (u32)strlen(k_bad), a), DE_ERR_PARSE);
    static const char k_two_roots[] = "[[bones]]\nname = \"a\"\n[[bones]]\nname = \"b\"\n";
    EXPECT_EQ(de_ragdoll_asset_parse(k_two_roots, (u32)strlen(k_two_roots), a), DE_ERR_PARSE);
    static const char k_none[] = "version = 1\n";
    EXPECT_NE(de_ragdoll_asset_parse(k_none, (u32)strlen(k_none), a), DE_OK);
    de_error_clear();
    free(a);
}

TEST(physics_authoring, ragdoll_component_spawns_bodies_and_joints) {
    pe_env e;
    pe_init(&e, 0);
    pe_ground(&e);
    de_ragdoll_asset *a = (de_ragdoll_asset *)calloc(1, sizeof *a);
    ASSERT_TRUE(a != NULL);
    ASSERT_EQ(de_ragdoll_asset_parse(k_ragdoll, (u32)strlen(k_ragdoll), a), DE_OK);
    const de_name name = de_name_from("physics/ragdolls/hero.ragdoll.toml");
    ASSERT_EQ(de_physics_ragdoll_asset_register(e.p, name, a), DE_OK);
    EXPECT_TRUE(de_physics_ragdoll_asset_find(e.p, name) != NULL);

    const de_entity ent = pe_spawn_xf(&e, 3.0, 0.0, 0.5);
    DeRagdoll *r = (DeRagdoll *)de_entity_add(e.w, ent, e.p->comp_authoring[4]);
    r->asset = name;
    r->material.friction = 0.6f;
    ASSERT_EQ(pe_tick(&e), DE_OK);
    const DeRagdoll *rc = (const DeRagdoll *)de_entity_get_const(e.w, ent, e.p->comp_authoring[4]);
    const de_ragdoll rag = rc->ragdoll;
    ASSERT_TRUE(de_ragdoll_valid(e.p, rag));
    EXPECT_EQ(de_ragdoll_bone_count(e.p, rag), 3u);
    for (u32 i = 0; i < 3; ++i)
        EXPECT_TRUE(DE_HANDLE_VALID(de_ragdoll_bone_body(e.p, rag, i)));
    EXPECT_TRUE(de_joint_valid(e.p, de_ragdoll_bone_joint(e.p, rag, 1)));
    EXPECT_TRUE(de_joint_valid(e.p, de_ragdoll_bone_joint(e.p, rag, 2)));
    /* Placed by the entity transform: the pelvis at model (0, 0, 1) + (3, 0, 0.5). */
    de_transform pt;
    ASSERT_TRUE(de_body_get_transform(e.p, de_ragdoll_bone_body(e.p, rag, 0), &pt));
    EXPECT_TRUE(jt_dist(pt.position, (de_dvec3){3.0, 0.0, 1.5}) < 0.05);

    /* It falls and settles on the ground; ticks do not respawn it. */
    for (u32 t = 0; t < 180; ++t)
        ASSERT_EQ(pe_tick(&e), DE_OK);
    rc = (const DeRagdoll *)de_entity_get_const(e.w, ent, e.p->comp_authoring[4]);
    EXPECT_EQ(rc->ragdoll.id, rag.id);
    ASSERT_TRUE(de_body_get_transform(e.p, de_ragdoll_bone_body(e.p, rag, 0), &pt));
    EXPECT_TRUE(pt.position.z > 0.0 && pt.position.z < 0.6);

    /* Re-registering the asset respawns it with the new bones. */
    a->bone_count = 2;
    ASSERT_EQ(de_physics_ragdoll_asset_register(e.p, name, a), DE_OK);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_FALSE(de_ragdoll_valid(e.p, rag));
    rc = (const DeRagdoll *)de_entity_get_const(e.w, ent, e.p->comp_authoring[4]);
    const de_ragdoll rag2 = rc->ragdoll;
    ASSERT_TRUE(de_ragdoll_valid(e.p, rag2));
    EXPECT_EQ(de_ragdoll_bone_count(e.p, rag2), 2u);

    /* Despawning the entity destroys the ragdoll; only the ground is left. */
    de_entity_despawn(e.w, ent);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_FALSE(de_ragdoll_valid(e.p, rag2));
    EXPECT_EQ(de_body_count(e.p), 1u);

    /* An unknown asset gets no ragdoll (and no error from the step). */
    const de_entity ghost = pe_spawn_xf(&e, 0.0, 0.0, 1.0);
    DeRagdoll *g = (DeRagdoll *)de_entity_add(e.w, ghost, e.p->comp_authoring[4]);
    g->asset = de_name_from("physics/ragdolls/missing.ragdoll.toml");
    ASSERT_EQ(pe_tick(&e), DE_OK);
    g = (DeRagdoll *)de_entity_get(e.w, ghost, e.p->comp_authoring[4]);
    EXPECT_EQ(g->ragdoll.id, 0u);
    de_error_clear();
    free(a);
    pe_free(&e);
}

/* The authoring components are in reflection under the names DIDE writes. */
TEST(physics_authoring, reflection_names) {
    pe_env e;
    pe_init(&e, 0);
    static const char *const k_types[] = {"DeConvexHull", "DeHingeJoint", "DeConeTwistJoint",
                                          "DeFixedJoint", "DeRagdoll"};
    for (u32 i = 0; i < 5; ++i) {
        EXPECT_TRUE(de_type_get(e.types, de_type_find(e.types, k_types[i])) != NULL);
    }
    const de_type_desc *col = de_type_get(e.types, de_type_find(e.types, "DeCollider"));
    ASSERT_TRUE(col != NULL);
    static const char *const k_fields[] = {"shape_kind", "radius", "half_height", "half_extents",
                                           "material_asset"};
    for (u32 i = 0; i < 5; ++i) {
        bool found = false;
        for (u32 f = 0; f < col->field_count; ++f)
            found = found || strcmp(col->fields[f].name, k_fields[i]) == 0;
        EXPECT_TRUE(found);
    }
    pe_free(&e);
}
