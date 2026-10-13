/* test_physics_ecs.c: the physics.step system (DENG-223): ECS -> physics sync, static colliders,
 * kinematic targets, sleeping, characters, entity joints, joint break events, no allocation per
 * tick. */
#include "ecs_test_util.h"

/* Live bodies equal the map entries: no orphans. */
static void expect_no_orphans(const pe_env *e) {
    const de__pecs *s = de__pecs_get(e->p);
    EXPECT_EQ(de_body_count(e->p), s->count);
    for (u32 k = 0; k < s->count; ++k) {
        EXPECT_TRUE(de_entity_alive(e->w, s->map[k].entity));
        EXPECT_TRUE(de_body_valid(e->p, s->map[k].body));
        if (k > 0)
            EXPECT_LT(de_entity_index(s->map[k - 1].entity), de_entity_index(s->map[k].entity));
    }
}

TEST(physics_ecs, spawn_and_despawn_same_tick) {
    pe_env e;
    pe_init(&e, 0);
    const de_shape box = st_box(e.p, 0.5f, 0.5f, 0.5f);
    const de_entity ground = pe_ground(&e);
    de_entity ents[10];
    for (u32 i = 0; i < 10; ++i)
        ents[i] = pe_spawn_body(&e, DE_BODY_DYNAMIC, box, (f64)i * 2.0, 0.0, 1.0);
    EXPECT_EQ(de_body_count(e.p), 0u);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_EQ(de_body_count(e.p), 11u);
    for (u32 i = 0; i < 10; ++i) {
        const de_body b = de_physics_entity_body(e.p, ents[i]);
        ASSERT_TRUE(de_body_valid(e.p, b));
        EXPECT_EQ(pe_rb(&e, ents[i])->body.id, b.id);
        EXPECT_EQ(e.p->bodies.owner[pe_index(&e, ents[i])].bits, ents[i].bits);
        EXPECT_EQ((u32)e.p->bodies.type[pe_index(&e, ents[i])], (u32)DE_BODY_DYNAMIC);
    }
    EXPECT_EQ((u32)e.p->bodies.type[pe_index(&e, ground)], (u32)DE_BODY_STATIC);
    /* Bodies are created in archetype / row order: spawn order here. */
    for (u32 i = 1; i < 10; ++i)
        EXPECT_LT(pe_index(&e, ents[i - 1]), pe_index(&e, ents[i]));
    expect_no_orphans(&e);

    /* Despawn three; despawn one more and spawn a new entity that reuses its index. */
    de_body gone[4];
    for (u32 i = 0; i < 4; ++i)
        gone[i] = de_physics_entity_body(e.p, ents[i]);
    for (u32 i = 0; i < 4; ++i)
        de_entity_despawn(e.p->ecs_world, ents[i]);
    const de_entity reused = pe_spawn_body(&e, DE_BODY_DYNAMIC, box, 50.0, 0.0, 1.0);
    EXPECT_EQ(de_entity_index(reused), de_entity_index(ents[3]));
    ASSERT_EQ(pe_tick(&e), DE_OK);
    for (u32 i = 0; i < 4; ++i)
        EXPECT_FALSE(de_body_valid(e.p, gone[i]));
    EXPECT_TRUE(de_body_valid(e.p, de_physics_entity_body(e.p, reused)));
    EXPECT_FALSE(de_body_valid(e.p, de_physics_entity_body(e.p, ents[3])));
    EXPECT_EQ(de_body_count(e.p), 8u);
    expect_no_orphans(&e);

    /* Despawn everything: no body is left. */
    for (u32 i = 4; i < 10; ++i)
        de_entity_despawn(e.w, ents[i]);
    de_entity_despawn(e.w, reused);
    de_entity_despawn(e.w, ground);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_EQ(de_body_count(e.p), 0u);
    expect_no_orphans(&e);
    pe_free(&e);
}

TEST(physics_ecs, component_changes) {
    pe_env e;
    pe_init(&e, 0);
    const de_shape box = st_box(e.p, 0.5f, 0.5f, 0.5f);
    pe_ground(&e);
    const de_entity a = pe_spawn_body(&e, DE_BODY_DYNAMIC, box, 0.0, 0.0, 3.0);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    const de_body b0 = de_physics_entity_body(e.p, a);
    ASSERT_TRUE(de_body_valid(e.p, b0));

    /* In-place fields keep the body. */
    DeRigidBody *rb = (DeRigidBody *)de_entity_get(e.w, a, e.rb);
    rb->linear_damping = 0.25f;
    rb->lock_rot_z = true;
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_EQ(de_physics_entity_body(e.p, a).id, b0.id);
    u32 i = pe_index(&e, a);
    EXPECT_TRUE(e.p->bodies.linear_damping[i] == 0.25f);
    EXPECT_TRUE((e.p->bodies.flags[i] & DE_BODY_FLAG_LOCK_ROT_Z) != 0);
    EXPECT_TRUE((e.p->bodies.flags[i] & DE_BODY_FLAG_ALLOW_SLEEP) != 0);

    /* Mass re-creates the body (new handle, the new mass). */
    rb = (DeRigidBody *)de_entity_get(e.w, a, e.rb);
    rb->mass = 7.0f;
    ASSERT_EQ(pe_tick(&e), DE_OK);
    const de_body b1 = de_physics_entity_body(e.p, a);
    EXPECT_NE(b1.id, b0.id);
    EXPECT_FALSE(de_body_valid(e.p, b0));
    EXPECT_EQ(pe_rb(&e, a)->body.id, b1.id);
    de_mass_props mp;
    ASSERT_TRUE(de_body_get_mass_props(e.p, b1, &mp));
    EXPECT_FLOAT_EQ(mp.mass, 7.0f, 1e-5);

    /* A mutable get that changes nothing keeps the body. */
    (void)de_entity_get(e.w, a, e.rb);
    (void)de_entity_get(e.w, a, e.col);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_EQ(de_physics_entity_body(e.p, a).id, b1.id);

    /* Removing DeCollider removes the body; adding it back makes a new one. */
    de_entity_remove(e.w, a, e.col);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_FALSE(de_body_valid(e.p, de_physics_entity_body(e.p, a)));
    EXPECT_FALSE(de_body_valid(e.p, b1));
    pe_add_collider(&e, a, box);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_TRUE(de_body_valid(e.p, de_physics_entity_body(e.p, a)));

    /* Removing DeRigidBody leaves a static collider (found by the rescan). */
    de_entity_remove(e.w, a, e.rb);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    i = pe_index(&e, a);
    ASSERT_TRUE(i != 0xFFFFFFFFu);
    EXPECT_EQ((u32)e.p->bodies.type[i], (u32)DE_BODY_STATIC);

    /* An invalid shape gives no body (and no crash); fixing it creates one. */
    const de_entity bad = pe_spawn_body(&e, DE_BODY_DYNAMIC, (de_shape){0}, 5.0, 0.0, 1.0);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_FALSE(de_body_valid(e.p, de_physics_entity_body(e.p, bad)));
    de_error_clear();
    ((DeCollider *)de_entity_get(e.w, bad, e.col))->shape = box;
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_TRUE(de_body_valid(e.p, de_physics_entity_body(e.p, bad)));

    /* A change that cannot make a body (negative mass) drops it once, without a retry every
     * tick; a valid value makes it again. */
    ((DeRigidBody *)de_entity_get(e.w, bad, e.rb))->mass = -1.0f;
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_FALSE(de_body_valid(e.p, de_physics_entity_body(e.p, bad)));
    const u64 failed = de__pecs_get(e.p)->stats.failed;
    for (u32 t = 0; t < 3; ++t)
        ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_EQ(de__pecs_get(e.p)->stats.failed, failed);
    ((DeRigidBody *)de_entity_get(e.w, bad, e.rb))->mass = 2.0f;
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_TRUE(de_body_valid(e.p, de_physics_entity_body(e.p, bad)));
    expect_no_orphans(&e);
    pe_free(&e);
}

TEST(physics_ecs, falls_and_writes_back) {
    pe_env e;
    pe_init(&e, 0);
    pe_ground(&e);
    const de_entity s = pe_spawn_body(&e, DE_BODY_DYNAMIC, st_sphere(e.p, 0.5f), 0.0, 0.0, 5.0);
    for (u32 t = 0; t < 30; ++t)
        ASSERT_EQ(pe_tick(&e), DE_OK);
    const de_body b = de_physics_entity_body(e.p, s);
    de_transform bt;
    ASSERT_TRUE(de_body_get_transform(e.p, b, &bt));
    const de_transform *xf = pe_xf(&e, s);
    ST_LT(xf->position.z, 4.0); /* fell with gravity_scale 1 (construct hook) */
    EXPECT_MEMEQ(&xf->position, &bt.position, sizeof bt.position);
    EXPECT_MEMEQ(&xf->rotation, &bt.rotation, sizeof bt.rotation);
    EXPECT_TRUE(xf->scale.x == 1.0f); /* scale untouched */
    de_vec3 lin, ang;
    ASSERT_TRUE(de_body_get_velocity(e.p, b, &lin, &ang));
    EXPECT_MEMEQ(&pe_rb(&e, s)->linear_velocity, &lin, sizeof lin);
    ST_LT(lin.z, -1.0f);

    /* Lands and rests on the ground (top at z = 0). */
    for (u32 t = 0; t < 120; ++t)
        ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_FLOAT_EQ(pe_xf(&e, s)->position.z, 0.5, 0.02);
    pe_free(&e);
}

TEST(physics_ecs, user_velocity_and_teleport) {
    pe_env e;
    pe_init(&e, 0);
    de_physics_set_gravity(e.p, de__v3(0.0f, 0.0f, 0.0f));
    const de_entity s = pe_spawn_body(&e, DE_BODY_DYNAMIC, st_sphere(e.p, 0.5f), 0.0, 0.0, 0.0);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    DeRigidBody *rb = (DeRigidBody *)de_entity_get(e.w, s, e.rb);
    rb->linear_velocity = de__v3(6.0f, 0.0f, 0.0f);
    for (u32 t = 0; t < 10; ++t)
        ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_FLOAT_EQ(pe_xf(&e, s)->position.x, 1.0, 1e-4);
    EXPECT_FLOAT_EQ(pe_rb(&e, s)->linear_velocity.x, 6.0f, 1e-5);

    /* Teleport through DeTransform. */
    de_transform *xf = (de_transform *)de_entity_get(e.w, s, e.xf);
    xf->position = de__d3(100.0, 50.0, 0.0);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_FLOAT_EQ(pe_xf(&e, s)->position.x, 100.1, 1e-4);
    EXPECT_FLOAT_EQ(pe_xf(&e, s)->position.y, 50.0, 1e-9);
    pe_free(&e);
}

TEST(physics_ecs, kinematic_targets) {
    pe_env e;
    pe_init(&e, 0);
    pe_ground(&e);
    const de_entity k =
        pe_spawn_body(&e, DE_BODY_KINEMATIC, st_box(e.p, 1.0f, 1.0f, 0.1f), 0.0, 0.0, 0.5);
    const de_entity box =
        pe_spawn_body(&e, DE_BODY_DYNAMIC, st_box(e.p, 0.2f, 0.2f, 0.2f), 0.0, 0.0, 0.85);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    const de_body kb = de_physics_entity_body(e.p, k);
    for (u32 t = 1; t <= 60; ++t) {
        de_transform *xf = (de_transform *)de_entity_get(e.w, k, e.xf);
        xf->position = de__d3(0.02 * (f64)t, 0.0, 0.5 + 0.005 * (f64)t);
        xf->rotation = st_quat_axis_angle(de__v3(0.0f, 0.0f, 1.0f), 0.01f * (f32)t);
        const de_transform target = *xf;
        ASSERT_EQ(pe_tick(&e), DE_OK);
        de_transform bt;
        ASSERT_TRUE(de_body_get_transform(e.p, kb, &bt));
        EXPECT_MEMEQ(&bt.position, &target.position, sizeof bt.position);
        EXPECT_FLOAT_EQ(bt.rotation.z, target.rotation.z, 1e-6);
        de_vec3 lin, ang;
        ASSERT_TRUE(de_body_get_velocity(e.p, kb, &lin, &ang));
        EXPECT_TRUE(lin.x == 0.0f && lin.z == 0.0f && ang.z == 0.0f); /* back to DeRigidBody's */
    }
    EXPECT_EQ(de_physics_entity_body(e.p, k).id, kb.id);
    /* The box rode the platform: carried along x and up. */
    ST_GT(pe_xf(&e, box)->position.x, 0.6);
    ST_GT(pe_xf(&e, box)->position.z, 1.0);

    /* A kinematic body with a DeRigidBody velocity moves by itself and writes back its pose. */
    DeRigidBody *rb = (DeRigidBody *)de_entity_get(e.w, k, e.rb);
    rb->linear_velocity = de__v3(0.0f, 1.0f, 0.0f);
    for (u32 t = 0; t < 30; ++t)
        ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_FLOAT_EQ(pe_xf(&e, k)->position.y, 0.5, 1e-4);
    pe_free(&e);
}

/* Counts the rows of a changed component (Update system). */
typedef struct change_count {
    u32 rows;
} change_count;

static void count_chunk(const de_query_chunk *chunk, void *user) {
    ((change_count *)user)->rows += chunk->count;
}

static void count_system(de_system_ctx *ctx) {
    change_count *c = (change_count *)ctx->user;
    de_query_each(ctx->world, ctx->queries[0], count_chunk, c);
    de_query_each(ctx->world, ctx->queries[1], count_chunk, c);
}

TEST(physics_ecs, sleeping_bodies_are_left_alone) {
    pe_env e;
    pe_init(&e, 0);
    pe_ground(&e);
    const de_entity a =
        pe_spawn_body(&e, DE_BODY_DYNAMIC, st_box(e.p, 0.5f, 0.5f, 0.5f), 0.0, 0.0, 0.5);
    const de_entity b =
        pe_spawn_body(&e, DE_BODY_DYNAMIC, st_box(e.p, 0.5f, 0.5f, 0.5f), 3.0, 0.0, 0.5);

    change_count cc = {0};
    const de_query_term terms[] = {{e.xf, DE_READ}};
    const de_query_filter fx[] = {{e.xf, DE_CHANGED}, {e.rb, DE_WITH}};
    const de_query_term terms_rb[] = {{e.rb, DE_READ}};
    const de_query_filter frb[] = {{e.rb, DE_CHANGED}};
    de_query_desc q[2];
    memset(q, 0, sizeof q);
    q[0].terms = terms;
    q[0].term_count = 1;
    q[0].filters = fx;
    q[0].filter_count = 2;
    q[1].terms = terms_rb;
    q[1].term_count = 1;
    q[1].filters = frb;
    q[1].filter_count = 1;
    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "test.count_changes";
    sd.schedule = DE_SCHEDULE_UPDATE;
    sd.fn = count_system;
    sd.user = &cc;
    sd.queries = q;
    sd.query_count = 2;
    ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(e.w, &sd)));

    u32 t = 0;
    for (; t < 300 && !(de_body_is_sleeping(e.p, de_physics_entity_body(e.p, a)) &&
                        de_body_is_sleeping(e.p, de_physics_entity_body(e.p, b)));
         ++t) {
        ASSERT_EQ(pe_tick(&e), DE_OK);
        ASSERT_EQ(de_world_run_schedule(e.w, DE_SCHEDULE_UPDATE, ST_DT, 0.0f), DE_OK);
    }
    EXPECT_LT(t, 300u);
    /* Asleep: the sync and the write-back touch neither the bodies nor the components, even when
     * a game takes a mutable pointer without changing anything. */
    ASSERT_EQ(pe_tick(&e), DE_OK); /* the components see the sleep tick's zero velocity */
    ASSERT_EQ(de_world_run_schedule(e.w, DE_SCHEDULE_UPDATE, ST_DT, 0.0f), DE_OK);
    cc.rows = 0;
    const u64 writes = de__pecs_get(e.p)->stats.writes;
    for (u32 k = 0; k < 60; ++k) {
        if (k == 10) {
            (void)de_entity_get(e.w, a, e.rb);
            (void)de_entity_get(e.w, a, e.xf);
        }
        ASSERT_EQ(pe_tick(&e), DE_OK);
        ASSERT_EQ(de_world_run_schedule(e.w, DE_SCHEDULE_UPDATE, ST_DT, 0.0f), DE_OK);
        EXPECT_TRUE(de_body_is_sleeping(e.p, de_physics_entity_body(e.p, a)));
        EXPECT_TRUE(de_body_is_sleeping(e.p, de_physics_entity_body(e.p, b)));
    }
    EXPECT_EQ(de__pecs_get(e.p)->stats.writes, writes);
    EXPECT_EQ(cc.rows, 2u); /* only the game's own two marks */

    /* A real change wakes the body. */
    DeRigidBody *rb = (DeRigidBody *)de_entity_get(e.w, a, e.rb);
    rb->linear_velocity = de__v3(0.0f, 0.0f, 3.0f);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_FALSE(de_body_is_sleeping(e.p, de_physics_entity_body(e.p, a)));
    EXPECT_TRUE(de_body_is_sleeping(e.p, de_physics_entity_body(e.p, b)));
    pe_free(&e);
}

TEST(physics_ecs, static_colliders_and_offset) {
    pe_env e;
    pe_init(&e, 0);
    pe_ground(&e);
    /* A dynamic box whose collider sits 0.5 m above the entity origin. */
    const de_entity a = pe_spawn_xf(&e, 0.0, 0.0, 2.0);
    DeRigidBody *rb = (DeRigidBody *)de_entity_add(e.w, a, e.rb);
    rb->type = DE_BODY_DYNAMIC;
    DeCollider *c = pe_add_collider(&e, a, st_box(e.p, 0.5f, 0.5f, 0.5f));
    c->offset.position = de__d3(0.0, 0.0, 0.5);
    /* A static step: DeCollider only. */
    const de_entity step = pe_spawn_static(&e, st_box(e.p, 1.0f, 1.0f, 0.25f), 5.0, 0.0, 0.25);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    const de__pecs_entry *en = de__pecs_find(de__pecs_get(e.p), a);
    ASSERT_TRUE(en != NULL);
    EXPECT_TRUE(DE_HANDLE_VALID(en->owned_shape));
    de_mass_props mp;
    ASSERT_TRUE(de_body_get_mass_props(e.p, en->body, &mp));
    EXPECT_FLOAT_EQ(mp.center.z, 0.5f, 1e-5);
    EXPECT_EQ((u32)e.p->bodies.type[pe_index(&e, step)], (u32)DE_BODY_STATIC);
    for (u32 t = 0; t < 150; ++t)
        ASSERT_EQ(pe_tick(&e), DE_OK);
    /* The box (center 0.5 above the origin, half height 0.5) rests on z = 0: origin at z = 0. */
    EXPECT_FLOAT_EQ(pe_xf(&e, a)->position.z, 0.0, 0.02);

    /* Moving a static collider teleports it. */
    de_transform *xf = (de_transform *)de_entity_get(e.w, step, e.xf);
    xf->position = de__d3(-5.0, 0.0, 0.25);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    de_transform bt;
    ASSERT_TRUE(de_body_get_transform(e.p, de_physics_entity_body(e.p, step), &bt));
    EXPECT_TRUE(bt.position.x == -5.0);

    /* Despawning frees the owned compound too. */
    const u32 shapes = e.p->shape_live_count;
    de_entity_despawn(e.w, a);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_EQ(e.p->shape_live_count, shapes - 1u);
    pe_free(&e);
}

TEST(physics_ecs, characters_via_ecs) {
    pe_env e;
    pe_init(&e, 0);
    pe_ground(&e);
    const de_entity c = pe_spawn_xf(&e, 0.0, 0.0, 1.0);
    DeCharacter *ch = (DeCharacter *)de_entity_add(e.w, c, e.ch);
    ch->radius = 0.3f;
    ch->height = 1.8f;
    ASSERT_EQ(pe_tick(&e), DE_OK);
    const de_body proxy = de_physics_entity_body(e.p, c);
    ASSERT_TRUE(de_body_valid(e.p, proxy));
    EXPECT_EQ(e.p->bodies.owner[pe_index(&e, c)].bits, c.bits);
    for (u32 t = 0; t < 60; ++t)
        ASSERT_EQ(pe_tick(&e), DE_OK);
    const DeCharacter *cr = (const DeCharacter *)de_entity_get_const(e.w, c, e.ch);
    EXPECT_TRUE(cr->grounded);
    EXPECT_FLOAT_EQ(pe_xf(&e, c)->position.z, 0.9, 0.03);

    for (u32 t = 0; t < 60; ++t) {
        ((DeCharacter *)de_entity_get(e.w, c, e.ch))->desired_velocity = de__v3(2.0f, 0.0f, 0.0f);
        ASSERT_EQ(pe_tick(&e), DE_OK);
        de_transform bt;
        ASSERT_TRUE(de_body_get_transform(e.p, proxy, &bt));
        EXPECT_MEMEQ(&pe_xf(&e, c)->position, &bt.position, sizeof bt.position);
    }
    EXPECT_FLOAT_EQ(pe_xf(&e, c)->position.x, 2.0, 0.05);
    EXPECT_TRUE(((const DeCharacter *)de_entity_get_const(e.w, c, e.ch))->grounded);

    /* A character with DeRigidBody + DeCollider too is still a character. */
    DeRigidBody *rb = (DeRigidBody *)de_entity_add(e.w, c, e.rb);
    rb->type = DE_BODY_DYNAMIC;
    pe_add_collider(&e, c, st_sphere(e.p, 0.3f));
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_EQ(de_physics_entity_body(e.p, c).id, proxy.id);

    de_entity_despawn(e.w, c);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_FALSE(de_body_valid(e.p, proxy));
    expect_no_orphans(&e);
    pe_free(&e);
}

TEST(physics_ecs, entity_joints_and_break_events) {
    pe_env e;
    pe_init(&e, 0);
    const de_shape box = st_box(e.p, 0.25f, 0.25f, 0.25f);
    const de_entity anchor = pe_spawn_body(&e, DE_BODY_STATIC, box, 0.0, 0.0, 5.0);
    const de_entity bob = pe_spawn_body(&e, DE_BODY_DYNAMIC, box, 0.0, 0.0, 4.0);
    const de_entity heavy = pe_spawn_body(&e, DE_BODY_DYNAMIC, box, 3.0, 0.0, 4.0);
    const de_entity anchor2 = pe_spawn_body(&e, DE_BODY_STATIC, box, 3.0, 0.0, 5.0);

    de_joint_desc jd;
    memset(&jd, 0, sizeof jd);
    jd.kind = DE_JOINT_BALL;
    jd.a = anchor;
    jd.b = bob;
    jd.frame_a.position = de__d3(0.0, 0.0, -1.0);
    /* No body yet: refused. */
    EXPECT_FALSE(DE_HANDLE_VALID(de_joint_create(e.p, &jd)));
    de_error_clear();

    ASSERT_EQ(pe_tick(&e), DE_OK);
    const de_joint j = de_joint_create(e.p, &jd);
    ASSERT_TRUE(DE_HANDLE_VALID(j));
    /* A breakable joint holding the heavy body (breaks at 50 N; the weight is about 1225 N). */
    jd.a = anchor2;
    jd.b = heavy;
    jd.break_force = 50.0f;
    const de_joint jb = de_joint_create(e.p, &jd);
    ASSERT_TRUE(DE_HANDLE_VALID(jb));

    u32 breaks = 0;
    for (u32 t = 0; t < 60; ++t) {
        ASSERT_EQ(pe_tick(&e), DE_OK);
        const de_joint_break_event *ev;
        while ((ev = (const de_joint_break_event *)de_event_next(e.w, &e.breaks)) != NULL) {
            EXPECT_EQ(ev->joint.id, jb.id);
            EXPECT_EQ(ev->a.bits, anchor2.bits);
            EXPECT_EQ(ev->b.bits, heavy.bits);
            ST_GT(ev->force, 50.0f);
            ++breaks;
        }
    }
    EXPECT_EQ(breaks, 1u);
    /* The ball joint held the bob 1 m under the anchor. */
    EXPECT_FLOAT_EQ(pe_xf(&e, bob)->position.z, 4.0, 0.01);
    ST_LT(pe_xf(&e, heavy)->position.z, 3.0); /* fell after the break */

    /* Despawning a jointed entity destroys the joint with the body. */
    de_entity_despawn(e.w, bob);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_FALSE(de_joint_valid(e.p, j));
    pe_free(&e);
}

TEST(physics_ecs, attach_rules) {
    pe_env e;
    pe_init(&e, 0);
    EXPECT_EQ(de_physics_attach(e.p, e.w, e.types), DE_OK); /* again: harmless */
    EXPECT_TRUE(DE_HANDLE_VALID(de_physics_contact_event_type(e.p)));
    EXPECT_TRUE(DE_HANDLE_VALID(de_physics_joint_break_event_type(e.p)));

    /* Another ECS world: refused. */
    de_type_registry *types2;
    ASSERT_EQ(de_type_registry_create(NULL, &types2), DE_OK);
    de_world_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.types = types2;
    de_world *w2;
    ASSERT_EQ(de_world_create(&wd, &w2), DE_OK);
    EXPECT_EQ(de_physics_attach(e.p, w2, types2), DE_ERR_ALREADY_EXISTS);
    de_error_clear();
    de_world_destroy(w2);
    de_type_registry_destroy(types2);

    /* Not attached: the lookups give nothing. */
    de_physics_world *p2;
    ASSERT_EQ(de_physics_create(NULL, &p2), DE_OK);
    EXPECT_FALSE(DE_HANDLE_VALID(de_physics_contact_event_type(p2)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_physics_entity_body(p2, (de_entity){1})));
    de_physics_destroy(p2);
    EXPECT_FALSE(DE_HANDLE_VALID(de_physics_entity_body(NULL, (de_entity){1})));

    /* A tick with dt 0 does nothing. */
    pe_ground(&e);
    ASSERT_EQ(pe_tick_dt(&e, 0.0f), DE_OK);
    EXPECT_EQ(de_body_count(e.p), 0u);
    pe_free(&e);
    EXPECT_EQ(de_tracking_allocator_stats(&e.track).bytes_live, 0u);
}

TEST(physics_ecs, no_allocation_per_tick) {
    pe_env e;
    pe_init(&e, 2048);
    pe_ground(&e);
    const de_shape box = st_box(e.p, 0.3f, 0.3f, 0.3f);
    const de_shape ball = st_sphere(e.p, 0.3f);
    de_entity ents[64];
    for (u32 i = 0; i < 64; ++i)
        ents[i] = pe_spawn_body(&e, DE_BODY_DYNAMIC, (i & 1u) ? ball : box, (f64)(i % 8u) * 0.7,
                                (f64)(i / 8u) * 0.7, 0.5 + 0.1 * (f64)i);
    const de_entity c = pe_spawn_xf(&e, -5.0, 0.0, 1.0);
    (void)de_entity_add(e.w, c, e.ch);
    for (u32 t = 0; t < 90; ++t) {
        ((DeCharacter *)de_entity_get(e.w, c, e.ch))->desired_velocity = de__v3(1.0f, 0.0f, 0.0f);
        ASSERT_EQ(pe_tick(&e), DE_OK);
        ASSERT_EQ(de_world_run_schedule(e.w, DE_SCHEDULE_POST_UPDATE, ST_DT, 0.5f), DE_OK);
        de_contact_event evs[8];
        (void)pe_read_contacts(&e, evs, 8);
    }
    const de_tracking_stats before = de_tracking_allocator_stats(&e.track);
    for (u32 t = 0; t < 120; ++t) {
        ((DeCharacter *)de_entity_get(e.w, c, e.ch))->desired_velocity =
            de__v3((t & 32u) ? -1.0f : 1.0f, 0.0f, 0.0f);
        if (t == 40) {
            DeRigidBody *rb = (DeRigidBody *)de_entity_get(e.w, ents[5], e.rb);
            rb->linear_velocity = de__v3(0.0f, 0.0f, 4.0f);
        }
        ASSERT_EQ(pe_tick(&e), DE_OK);
        ASSERT_EQ(de_world_run_schedule(e.w, DE_SCHEDULE_POST_UPDATE, ST_DT, 0.5f), DE_OK);
        de_contact_event evs[8];
        (void)pe_read_contacts(&e, evs, 8);
    }
    const de_tracking_stats after = de_tracking_allocator_stats(&e.track);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.realloc_count, before.realloc_count);
    pe_free(&e);
}

/* Two identical runs give the same physics and ECS checksums on every tick. */
static void run_scene(u64 *out, u32 ticks) {
    pe_env e;
    pe_init(&e, 0);
    pe_ground(&e);
    const de_shape box = st_box(e.p, 0.3f, 0.3f, 0.3f);
    for (u32 t = 0; t < ticks; ++t) {
        if (t < 20)
            pe_spawn_body(&e, DE_BODY_DYNAMIC, box, (f64)(t % 4u) * 0.4, 0.0, 1.0 + (f64)t * 0.7);
        (void)pe_tick(&e);
        out[t] = de_physics_checksum(e.p) ^ (de_world_checksum(e.w, DE_COMP_SNAPSHOT) * 31u);
    }
    pe_free(&e);
}

TEST(physics_ecs, repeatable) {
    u64 a[90], b[90];
    run_scene(a, 90);
    run_scene(b, 90);
    EXPECT_MEMEQ(a, b, sizeof a);
}
