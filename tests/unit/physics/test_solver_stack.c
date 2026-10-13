/* test_solver_stack.c: the soft-step solver on stacks (DENG-217, SPEC §13.1, §13.2): a stack of
 * 10 unit boxes stands for 10 s, a 1:100 mass-ratio pair rests, warm starting lowers the residual
 * velocity of the stack, the constraint interface (class order, stages, write flags, capacity)
 * behaves as constraint.h says, the step allocates nothing after warm-up, and worker counts do not
 * change the result. */
#include "solver_test_util.h"

#include <dengine/de_task.h>

#include <stdio.h>
#include <stdlib.h>

/* ---- stack scene ------------------------------------------------------------------------ */

#define STACK_N 10u

typedef struct stack_scene {
    de_physics_world *p;
    de_body ground;
    de_body boxes[STACK_N];
} stack_scene;

/* Ground top at z = 0; unit boxes resting on each other (1 mm apart). */
static void stack_build(stack_scene *s, de_physics_world *p, u32 n) {
    memset(s, 0, sizeof *s);
    s->p = p;
    const de_shape g = st_box(p, 10.0f, 10.0f, 0.5f);
    const de_shape c = st_box(p, 0.5f, 0.5f, 0.5f);
    s->ground = st_body(p, DE_BODY_STATIC, g, st_xform(0, 0, -0.5, st_quat_identity()), 0.6f, 0.0f);
    for (u32 i = 0; i < n; ++i)
        s->boxes[i] = st_body(p, DE_BODY_DYNAMIC, c,
                              st_xform(0, 0, 0.5 + 1.001 * (f64)i, st_quat_identity()), 0.6f, 0.0f);
}

/* Sum of |v| + |w| of the boxes. */
static f64 stack_motion(const stack_scene *s, u32 n) {
    f64 sum = 0.0;
    for (u32 i = 0; i < n; ++i)
        sum += (f64)st_lenf(st_vel(s->p, s->boxes[i])) + (f64)st_lenf(st_angvel(s->p, s->boxes[i]));
    return sum;
}

/* Soft contacts give under load (stiffness m_eff (2 pi hertz)^2): a stack of ten unit boxes
 * settles about 3.5 cm in its first second. The settling also starts a slow, lightly damped
 * rocking of the stack on its soft contacts (about 1 cm at the top in the first 3 s, period about
 * 2 s), which dies out. Drift = where the top box ends up after 10 s, sideways from its start. */
TEST(physics_solver_stack, stack_10_stands_10s) {
    de_physics_world *p = st_world(64, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    static stack_scene s;
    stack_build(&s, p, STACK_N);
    const de_dvec3 top0 = st_pos(p, s.boxes[STACK_N - 1u]);
    f64 max_side = 0.0, max_motion_late = 0.0;
    for (u32 t = 0; t < 600; ++t) {
        de_physics_step(p, ST_DT);
        const de_dvec3 top = st_pos(p, s.boxes[STACK_N - 1u]);
        const f64 side =
            de_dsqrt64((top.x - top0.x) * (top.x - top0.x) + (top.y - top0.y) * (top.y - top0.y));
        max_side = side > max_side ? side : max_side;
        if (t >= 480) {
            const f64 m = stack_motion(&s, STACK_N);
            max_motion_late = m > max_motion_late ? m : max_motion_late;
        }
    }
    const de_dvec3 top = st_pos(p, s.boxes[STACK_N - 1u]);
    const f64 drift =
        de_dsqrt64((top.x - top0.x) * (top.x - top0.x) + (top.y - top0.y) * (top.y - top0.y));
    const f64 sag = top0.z - top.z;
    fprintf(stderr, "stack_10: drift %.4f m, sway max %.4f m, sag %.4f m, motion (last 2 s) %.3g\n",
            drift, max_side, sag, max_motion_late);
    ST_LT(drift, 0.01);    /* the AC: top box drift < 1 cm */
    ST_LT(max_side, 0.02); /* no toppling or explosion on the way */
    ST_LT(sag, 0.05);      /* soft-contact compression only */
    for (u32 i = 0; i < STACK_N; ++i) {
        const de_quat q = st_rot(p, s.boxes[i]);
        ST_GT(de__absf(q.w), 0.9999f); /* upright: under 1.6 degrees */
        if (i > 0)                     /* still stacked: each box above the previous one */
            ST_GT(st_pos(p, s.boxes[i]).z - st_pos(p, s.boxes[i - 1u]).z, 0.99);
    }
    ST_LT(max_motion_late, 0.01); /* at rest for the last 2 s */
    de_physics_destroy(p);
}

TEST(physics_solver_stack, mass_ratio_1_to_100_rests) {
    de_physics_world *p = st_world(64, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape g = st_box(p, 10.0f, 10.0f, 0.5f);
    const de_shape c = st_box(p, 0.5f, 0.5f, 0.5f);
    (void)st_body(p, DE_BODY_STATIC, g, st_xform(0, 0, -0.5, st_quat_identity()), 0.6f, 0.0f);
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_DYNAMIC;
    d.shape = c;
    d.gravity_scale = 1.0f;
    d.material.friction = 0.6f;
    d.mass = 1.0f;
    d.transform = st_xform(0, 0, 0.5, st_quat_identity());
    const de_body light = de_body_create(p, &d);
    d.mass = 100.0f; /* 100 kg on 1 kg: the hard order */
    d.transform = st_xform(0, 0, 1.501, st_quat_identity());
    const de_body heavy = de_body_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(light) && DE_HANDLE_VALID(heavy));
    de_dvec3 settled = st_pos(p, heavy);
    f64 max_drift = 0.0;
    for (u32 t = 0; t < 300; ++t) {
        de_physics_step(p, ST_DT);
        if (t == 59)
            settled = st_pos(p, heavy);
        if (t >= 59) {
            const f64 dd = st_len(de__d3_sub(st_pos(p, heavy), settled));
            max_drift = dd > max_drift ? dd : max_drift;
        }
    }
    const de_dvec3 hp = st_pos(p, heavy), lp = st_pos(p, light);
    fprintf(stderr,
            "mass ratio: drift after settling %.3g m, light z %.4f, heavy z %.4f, |v| %.3g %.3g\n",
            max_drift, lp.z, hp.z, (f64)st_lenf(st_vel(p, light)), (f64)st_lenf(st_vel(p, heavy)));
    ST_LT(max_drift, 0.005);  /* rests: no creep after settling */
    ST_GT(lp.z, 0.49);        /* the light box is not pressed into the ground */
    ST_GT(hp.z - lp.z, 0.95); /* nor the heavy box into the light one */
    ST_LT(de__absd(hp.x) + de__absd(hp.y), 0.01);
    ST_LT(st_lenf(st_vel(p, light)), 0.01f);
    ST_LT(st_lenf(st_vel(p, heavy)), 0.01f);
    ST_GT(st_rot(p, heavy).w, 0.9999f);
    de_physics_destroy(p);
}

/* Residual motion of the stack over its last second, with and without warm starting. */
static f64 stack_residual(bool warm) {
    de_physics_world *p = st_world(64, NULL, NULL);
    static stack_scene s;
    stack_build(&s, p, STACK_N);
    de__solver *sv = st_solver(p);
    sv->settings.warm_start = warm;
    f64 sum = 0.0;
    for (u32 t = 0; t < 240; ++t) {
        de_physics_step(p, ST_DT);
        if (t >= 180)
            sum += stack_motion(&s, STACK_N);
    }
    de_physics_destroy(p);
    return sum;
}

TEST(physics_solver_stack, warm_start_reduces_jitter) {
    const f64 warm = stack_residual(true);
    const f64 cold = stack_residual(false);
    fprintf(stderr, "residual motion: warm %.6g, cold %.6g\n", warm, cold);
    ST_LT(warm * 4.0, cold);
}

/* ---- constraint interface ------------------------------------------------------------------ */

typedef struct probe_class {
    u32 gathered, prepared, warm, solved_bias, solved_relax, restituted, stored;
    u32 order_errors;
    u32 last_record;
    u32 substep_seen[8];
    bool refused_static_pair;
} probe_class;

static de_body g_probe_a, g_probe_b, g_probe_static, g_probe_kin;

static u32 idx(de_body b) {
    return de__handle_index(b.id);
}

static void probe_gather(de__solver *s, const de__solver_ctx *ctx, void *state) {
    (void)ctx;
    probe_class *pc = (probe_class *)state;
    ++pc->gathered;
    /* static-kinematic: nothing writable, refused. */
    pc->refused_static_pair =
        de__solver_add_constraint(s, DE__CONSTRAINT_CLASS_JOINT, 0, idx(g_probe_static),
                                  idx(g_probe_kin)) == NULL;
    de__constraint *c =
        de__solver_add_constraint(s, DE__CONSTRAINT_CLASS_JOINT, 7, idx(g_probe_a), idx(g_probe_b));
    ASSERT_TRUE(c != NULL);
    EXPECT_EQ(c->flags, (u32)(DE__CONSTRAINT_WRITE_A | DE__CONSTRAINT_WRITE_B));
    EXPECT_EQ(c->data, 0u);
    EXPECT_EQ(c->color, DE__CONSTRAINT_NO_COLOR);
    EXPECT_NE(c->solver_a, DE__SOLVER_STATIC_SLOT);
    /* A body attached to the world: B is the static slot, never written. */
    c = de__solver_add_constraint(s, DE__CONSTRAINT_CLASS_JOINT, 8, idx(g_probe_a),
                                  DE__CONSTRAINT_NO_BODY);
    ASSERT_TRUE(c != NULL);
    EXPECT_EQ(c->flags, (u32)DE__CONSTRAINT_WRITE_A);
    EXPECT_EQ(c->solver_b, DE__SOLVER_STATIC_SLOT);
    /* Capacity 2: the third is dropped. */
    EXPECT_TRUE(de__solver_add_constraint(s, DE__CONSTRAINT_CLASS_JOINT, 9, idx(g_probe_a),
                                          idx(g_probe_b)) == NULL);
}

static void probe_prepare(const de__solver_ctx *ctx, void *state, de__constraint *c) {
    (void)ctx;
    probe_class *pc = (probe_class *)state;
    ++pc->prepared;
    EXPECT_EQ(c->class_id, (u32)DE__CONSTRAINT_CLASS_JOINT);
}

static void probe_warm(const de__solver_ctx *ctx, void *state, de__constraint *c) {
    (void)c;
    probe_class *pc = (probe_class *)state;
    ++pc->warm;
    if (ctx->substep < 8)
        ++pc->substep_seen[ctx->substep];
}

static void probe_solve(const de__solver_ctx *ctx, void *state, de__constraint *c, bool bias) {
    (void)ctx;
    probe_class *pc = (probe_class *)state;
    if (bias)
        ++pc->solved_bias;
    else
        ++pc->solved_relax;
    /* Records in the order they were added: ids 7 then 8 within each stage. */
    if (pc->last_record == c->id)
        ++pc->order_errors;
    pc->last_record = c->id;
}

static void probe_restitution(const de__solver_ctx *ctx, void *state, de__constraint *c) {
    (void)ctx;
    (void)c;
    ++((probe_class *)state)->restituted;
}

static void probe_store(const de__solver_ctx *ctx, void *state, de__constraint *c) {
    (void)ctx;
    (void)c;
    ++((probe_class *)state)->stored;
}

static const de__constraint_class k_probe = {"probe",           probe_gather, probe_prepare,
                                             probe_warm,        probe_solve,  NULL,
                                             probe_restitution, probe_store,  NULL};

TEST(physics_solver_stack, constraint_interface) {
    de_physics_world *p = st_world(64, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape c = st_box(p, 0.5f, 0.5f, 0.5f);
    g_probe_static = st_body(p, DE_BODY_STATIC, c, st_xform(-10, 0, 0, st_quat_identity()), 0, 0);
    g_probe_kin = st_body(p, DE_BODY_KINEMATIC, c, st_xform(-20, 0, 0, st_quat_identity()), 0, 0);
    g_probe_a = st_body(p, DE_BODY_DYNAMIC, c, st_xform(0, 0, 0, st_quat_identity()), 0, 0);
    g_probe_b = st_body(p, DE_BODY_DYNAMIC, c, st_xform(5, 0, 0, st_quat_identity()), 0, 0);
    static probe_class pc;
    memset(&pc, 0, sizeof pc);
    pc.last_record = 0xFFFFFFFFu;
    EXPECT_EQ(de__solver_register_class(p, DE__CONSTRAINT_CLASS_JOINT, &k_probe, &pc, 2), DE_OK);
    EXPECT_EQ(de__solver_register_class(p, DE__CONSTRAINT_CLASS_JOINT, &k_probe, &pc, 2),
              DE_ERR_ALREADY_EXISTS);
    EXPECT_EQ(de__solver_register_class(p, DE__CONSTRAINT_CLASS_COUNT, &k_probe, &pc, 2),
              DE_ERR_INVALID_ARGUMENT);
    de_physics_step(p, ST_DT);
    const u32 n = 2, sub = p->substeps;
    EXPECT_EQ(pc.gathered, 1u);
    EXPECT_TRUE(pc.refused_static_pair);
    EXPECT_EQ(pc.prepared, n);
    EXPECT_EQ(pc.warm, n * sub);
    EXPECT_EQ(pc.solved_bias, n * sub);
    EXPECT_EQ(pc.solved_relax, n * sub); /* relax NULL: solve without bias */
    EXPECT_EQ(pc.restituted, n);
    EXPECT_EQ(pc.stored, n);
    EXPECT_EQ(pc.order_errors, 0u);
    for (u32 k = 0; k < sub; ++k)
        EXPECT_EQ(pc.substep_seen[k], n);
    de__solver_stats st;
    de__solver_stats_get(p, &st);
    EXPECT_EQ(st.awake_bodies, 3u); /* kinematic + two dynamic */
    EXPECT_EQ(st.class_count[DE__CONSTRAINT_CLASS_JOINT], n);
    EXPECT_EQ(st.dropped, 1u);
    /* Slots are only valid during the step. */
    EXPECT_EQ(de__solver_slot(de__solver_get(p), idx(g_probe_a)), DE__SOLVER_STATIC_SLOT);
    de__solver_unregister_class(p, DE__CONSTRAINT_CLASS_JOINT);
    de_physics_step(p, ST_DT);
    EXPECT_EQ(pc.gathered, 1u);
    de_physics_destroy(p);
}

/* ---- allocation and worker counts ------------------------------------------------------------ */

static de_task_system *tasks_new(u32 workers) {
    if (workers == 0)
        return NULL;
    de_fp_env_init_thread();
    de_task_system_desc d;
    memset(&d, 0, sizeof d);
    d.worker_count = workers;
    d.background_worker_count = 1;
    de_task_system *ts = NULL;
    if (de_task_system_create(&d, &ts) != DE_OK)
        return NULL;
    return ts;
}

/* A stack, a row of bouncing spheres and a sliding box: every solver path. */
static void mixed_scene(de_physics_world *p, stack_scene *s) {
    stack_build(s, p, 6);
    const de_shape ball = st_sphere(p, 0.3f);
    const de_shape cube = st_box(p, 0.4f, 0.4f, 0.4f);
    for (u32 i = 0; i < 8; ++i)
        (void)st_body(p, DE_BODY_DYNAMIC, ball,
                      st_xform(-4.0 + (f64)i, 3.0, 1.0 + 0.25 * (f64)i, st_quat_identity()), 0.4f,
                      0.1f * (f32)i);
    const de_body slider =
        st_body(p, DE_BODY_DYNAMIC, cube, st_xform(3.0, -3.0, 0.4, st_quat_identity()), 0.3f, 0.0f);
    de_body_set_velocity(p, slider, (de_vec3){-4.0f, 1.0f, 0.0f}, (de_vec3){0.0f, 0.0f, 2.0f});
}

TEST(physics_solver_stack, no_alloc_after_warmup) {
    const u32 worker_counts[2] = {0, 4};
    for (u32 w = 0; w < 2; ++w) {
        de_task_system *ts = tasks_new(worker_counts[w]);
        de_tracking_allocator t;
        de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
        de_physics_world *p = st_world(128, ts, &a);
        ASSERT_TRUE(p != NULL);
        static stack_scene s;
        mixed_scene(p, &s);
        de_physics_step(p, ST_DT); /* warm-up: broadphase, contacts, solver */
        const de_tracking_stats before = de_tracking_allocator_stats(&t);
        for (u32 tick = 1; tick < 200; ++tick)
            de_physics_step(p, ST_DT);
        const de_tracking_stats after = de_tracking_allocator_stats(&t);
        EXPECT_EQ(after.alloc_count, before.alloc_count);
        EXPECT_EQ(after.realloc_count, before.realloc_count);
        EXPECT_EQ(after.free_count, before.free_count);
        de_physics_destroy(p);
        EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
        de_task_system_destroy(ts);
    }
}

TEST(physics_solver_stack, worker_count_invariance) {
    u64 sums[3] = {0, 0, 0};
    const u32 worker_counts[3] = {0, 1, 4};
    for (u32 w = 0; w < 3; ++w) {
        de_task_system *ts = tasks_new(worker_counts[w]);
        de_physics_world *p = st_world(128, ts, NULL);
        static stack_scene s;
        mixed_scene(p, &s);
        for (u32 tick = 0; tick < 180; ++tick) {
            de_physics_step(p, ST_DT);
            sums[w] = sums[w] * 31u + de_physics_checksum(p);
        }
        de_physics_destroy(p);
        de_task_system_destroy(ts);
    }
    EXPECT_EQ(sums[0], sums[1]);
    EXPECT_EQ(sums[0], sums[2]);
}

TEST(physics_solver_stack, solver_create_oom) {
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
    de_physics_world *p = st_world(64, NULL, &a);
    ASSERT_TRUE(p != NULL);
    static stack_scene s;
    stack_build(&s, p, 2);
    de__contacts *c = NULL;
    ASSERT_EQ(de__contacts_ensure(p, &c), DE_OK);
    /* Fail each solver allocation in turn: nothing is left behind and the slot stays empty. */
    for (i64 k = 0; k < 8; ++k) {
        de_tracking_allocator_fail_after(&t, k);
        de__solver *sv = NULL;
        const de_result r = de__solver_ensure(p, &sv);
        de_tracking_allocator_fail_after(&t, -1);
        if (r == DE_OK)
            break;
        EXPECT_EQ(r, DE_ERR_OUT_OF_MEMORY);
        EXPECT_TRUE(de__solver_get(p) == NULL);
    }
    ASSERT_TRUE(de__solver_get(p) != NULL);
    de_physics_destroy(p);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
    /* A step whose solver cannot be created moves nothing, keeps the error and goes on. */
    p = st_world(64, NULL, &a);
    stack_build(&s, p, 2);
    ASSERT_EQ(de__contacts_ensure(p, &c), DE_OK);
    const de_dvec3 z0 = st_pos(p, s.boxes[1]);
    de_tracking_allocator_fail_after(&t, 0);
    de_physics_step(p, ST_DT);
    de_tracking_allocator_fail_after(&t, -1);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_physics_tick(p), 1u);
    EXPECT_TRUE(st_pos(p, s.boxes[1]).z == z0.z);
    de_physics_step(p, ST_DT);
    EXPECT_TRUE(de__solver_get(p) != NULL);
    de_physics_destroy(p);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
}
