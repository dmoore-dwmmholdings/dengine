/*
 * test_render_thread.c: the pipelined render thread and its handoff (DENG-175; SPEC §8).
 * Executable test_render_world, CTest names render_world.render_thread.<test>.
 *
 *   - pipelined_one_frame_ahead: a render thread that only finishes frame N after the simulation
 *     has started frame N + 1 (an explicit wait, then a short sleep). The simulation is never
 *     more than one frame ahead: Extract of N + 1 finds frame N rendered and N + 1 not, and the
 *     simulation never starts N + 2 while N renders. The render thread sees each buffer with the
 *     digest Extract left, before and after its sleep (nobody writes it meanwhile). The world has
 *     a task system, so the Extract systems run on workers. TSan-clean (an S12 exit check).
 *   - unpipelined_equivalence: a scripted 100-frame scene (moves, spawns, despawns, component
 *     changes, light changes, a camera that rebases the origin) gives the same buffer digests
 *     with `r.pipelined` 1 and 0, frame by frame.
 *
 * Every wait in the hooks waits on explicit state with a bound (no fixed iteration counts); a
 * timeout is a failure, never a hang. Checks on the render thread go into atomics; the test
 * thread asserts on them (the test framework is single-threaded).
 */
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_platform.h>
#include <dengine/de_render.h>

#include "platform/de_atomic.h"
#include "render/world/render_world.h"

#include <stdio.h>
#include <string.h>

#include "test_render_world.h"

#define WAIT_TIMEOUT_NS (20ull * 1000000000ull)
#define SLOW_RENDER_NS  (2ull * 1000000ull)

typedef struct rtt {
    rwt fx;
} rtt;

static void rtt_setup(rtt *x) {
    (void)x;
}

static void rtt_teardown(rtt *x) {
    if (x->fx.r != NULL || x->fx.w != NULL)
        rwt_stop(&x->fx);
}

static de_transform at(f64 x, f64 y, f64 z) {
    de_transform t = de_transform_identity();
    t.position = (de_dvec3){x, y, z};
    return t;
}

/* ---- pipelined_one_frame_ahead --------------------------------------------------------------- */

#define AHEAD_FRAMES 40u

typedef struct ahead_state {
    de_atomic_u64 sim_frame;      /* the frame the simulation is in (main thread, release) */
    u64 digest[AHEAD_FRAMES + 2]; /* written by Extract's thread before the kick */
    u64 last_frame;               /* frames the test runs (set before the first kick) */
    de_atomic_u32 timeouts;       /* hook: the simulation did not start N + 1 in time */
    de_atomic_u32 too_far;        /* hook: the simulation was at N + 2 or later */
    de_atomic_u32 bad_state;      /* hook: the buffer was not RENDERING */
    de_atomic_u32 bad_digest;     /* hook: the buffer changed, or is not the one extracted */
    de_atomic_u32 overlapped;     /* hook: frames that rendered while the next one simulated */
    de_atomic_u32 hooked;         /* hook: frames seen */
} ahead_state;

static void ahead_hook(de_renderer *r, const de__render_world_frame *fr, void *user) {
    (void)r;
    ahead_state *s = (ahead_state *)user;
    const u64 n = fr->frame;
    de_atomic_fetch_add_u32(&s->hooked, 1, DE_ATOMIC_RELAXED);
    if (de_atomic_load_u32(&fr->state, DE_ATOMIC_RELAXED) != DE__RW_RENDERING)
        de_atomic_fetch_add_u32(&s->bad_state, 1, DE_ATOMIC_RELAXED);
    if (n > AHEAD_FRAMES || rwt_digest(fr) != s->digest[n])
        de_atomic_fetch_add_u32(&s->bad_digest, 1, DE_ATOMIC_RELAXED);
    if (n >= s->last_frame)
        return; /* nothing simulates after the last frame */
    /* Wait until the simulation has started frame n + 1: rendering n overlaps it. */
    const u64 deadline = de_time_now_ns() + WAIT_TIMEOUT_NS;
    while (de_atomic_load_u64(&s->sim_frame, DE_ATOMIC_ACQUIRE) < n + 1u) {
        if (de_time_now_ns() > deadline) {
            de_atomic_fetch_add_u32(&s->timeouts, 1, DE_ATOMIC_RELAXED);
            return;
        }
        de_sleep_ns(50000);
    }
    de_atomic_fetch_add_u32(&s->overlapped, 1, DE_ATOMIC_RELAXED);
    /* Slow render: the simulation gets time to run ahead, if it could. It cannot: Extract of
     * n + 1 waits for this frame, so it never starts n + 2. */
    de_sleep_ns(SLOW_RENDER_NS);
    if (de_atomic_load_u64(&s->sim_frame, DE_ATOMIC_ACQUIRE) > n + 1u)
        de_atomic_fetch_add_u32(&s->too_far, 1, DE_ATOMIC_RELAXED);
    if (rwt_digest(fr) != s->digest[n])
        de_atomic_fetch_add_u32(&s->bad_digest, 1, DE_ATOMIC_RELAXED);
}

static void run_ahead(rwt *f) {
    rwt_start(f, 2, true);
    /* Start the task workers before the test (their first tasks), so their startup does not
     * delay the first frames. */
    for (u32 i = 0; i < 3; ++i)
        ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    const de_entity cam = rwt_spawn_camera(f, at(0.0, 0.0, 2.0), 0);
    de_entity moving[32];
    for (u32 i = 0; i < 32; ++i)
        moving[i] = rwt_spawn_mesh(f, at((f64)i, 0.0, 0.0), i + 1u);
    for (u32 i = 0; i < 200; ++i)
        (void)rwt_spawn_mesh(f, at((f64)i, 5.0, 0.0), 100u + i);
    for (u32 i = 0; i < 4; ++i)
        (void)rwt_spawn_light(f, at((f64)i, 0.0, 4.0), DE_LIGHT_POINT);

    static ahead_state s;
    memset(&s, 0, sizeof s);
    de_atomic_init_u64(&s.sim_frame, 0);
    s.last_frame = AHEAD_FRAMES;
    de__render_world_set_frame_hook(f->r, ahead_hook, &s);
    de__render_world *rw = de__rw_of(f->r);
    ASSERT_TRUE(rw != NULL);

    for (u64 n = 1; n <= AHEAD_FRAMES; ++n) {
        /* "Update" of frame n: the simulation is in frame n now. */
        de_atomic_store_u64(&s.sim_frame, n, DE_ATOMIC_RELEASE);
        for (u32 k = 0; k < 32; ++k)
            rwt_move(f, moving[k], (de_dvec3){(f64)k + 0.5 * (f64)n, 0.0, 0.0});
        rwt_move(f, cam, (de_dvec3){400.0 * (f64)n, 0.0, 2.0}); /* rebases now and then */
        ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 0.0f, 0.0f), DE_OK);
        ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_EXTRACT, 0.0f, 0.0f), DE_OK);
        /* Extract waited for frame n - 1 and nothing later is kicked: exactly n - 1 rendered. */
        EXPECT_EQ(de__render_world_rendered(f->r), n - 1u);
        ASSERT_TRUE(rw->filling != NULL);
        ASSERT_EQ(rw->filling->frame, n);
        s.digest[n] = rwt_digest(rw->filling);
        ASSERT_EQ(de__render_extract_and_kick(f->r, f->w), DE_OK);
    }
    de__render_world_wait_idle(f->r);
    EXPECT_EQ(de__render_world_rendered(f->r), (u64)AHEAD_FRAMES);
    EXPECT_EQ(de_atomic_load_u32(&s.hooked, DE_ATOMIC_RELAXED), AHEAD_FRAMES);
    EXPECT_EQ(de_atomic_load_u32(&s.timeouts, DE_ATOMIC_RELAXED), 0u);
    EXPECT_EQ(de_atomic_load_u32(&s.too_far, DE_ATOMIC_RELAXED), 0u);
    EXPECT_EQ(de_atomic_load_u32(&s.bad_state, DE_ATOMIC_RELAXED), 0u);
    EXPECT_EQ(de_atomic_load_u32(&s.bad_digest, DE_ATOMIC_RELAXED), 0u);
    /* Every frame but the last rendered while the next one simulated. */
    EXPECT_EQ(de_atomic_load_u32(&s.overlapped, DE_ATOMIC_RELAXED), AHEAD_FRAMES - 1u);
    de__render_world_set_frame_hook(f->r, NULL, NULL);
}

TEST_F(render_thread, pipelined_one_frame_ahead, rtt) {
    run_ahead(&f->fx);
}

/* ---- unpipelined_equivalence ----------------------------------------------------------------- */

#define SCRIPT_FRAMES 100u
#define SCRIPT_MESHES 200u
#define SCRIPT_LIGHTS 8u

typedef struct digest_log {
    u64 digest[SCRIPT_FRAMES + 1];
    de_atomic_u32 count;
    de_atomic_u32 out_of_order;
} digest_log;

static void log_hook(de_renderer *r, const de__render_world_frame *fr, void *user) {
    (void)r;
    digest_log *l = (digest_log *)user;
    const u32 i = de_atomic_load_u32(&l->count, DE_ATOMIC_RELAXED);
    if (fr->frame != (u64)i + 1u || i >= SCRIPT_FRAMES) {
        de_atomic_fetch_add_u32(&l->out_of_order, 1, DE_ATOMIC_RELAXED);
        return;
    }
    l->digest[i] = rwt_digest(fr);
    de_atomic_store_u32(&l->count, i + 1u, DE_ATOMIC_RELEASE);
}

static u32 lcg(u32 *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

/* The scripted scene; every decision comes from the frame number and one LCG. */
static void run_script(rwt *f, bool pipelined, digest_log *log) {
    rwt_start(f, 2, pipelined);
    memset(log, 0, sizeof *log);
    de_atomic_init_u32(&log->count, 0);
    de_atomic_init_u32(&log->out_of_order, 0);
    de__render_world_set_frame_hook(f->r, log_hook, log);

    const de_entity cam = rwt_spawn_camera(f, at(0.0, 0.0, 1.8), 0);
    (void)rwt_spawn_camera(f, at(50.0, 0.0, 10.0), 3); /* a render-to-texture camera */
    const de_component env_c = f->ids.environment;
    de_entity env = DE_ENTITY_NULL;
    ASSERT_EQ(de_entity_spawn_with(f->w, &env_c, 1, &env), DE_OK);
    ((DeEnvironment *)de_entity_get(f->w, env, env_c))->rotation = 0.5f;
    de_entity mesh[SCRIPT_MESHES];
    u32 seed = 7u;
    for (u32 i = 0; i < SCRIPT_MESHES; ++i)
        mesh[i] = rwt_spawn_mesh(f, at((f64)(lcg(&seed) % 2000u), (f64)(lcg(&seed) % 2000u), 0.0),
                                 i + 1u);
    de_entity light[SCRIPT_LIGHTS];
    for (u32 i = 0; i < SCRIPT_LIGHTS; ++i)
        light[i] = rwt_spawn_light(f, at((f64)i * 10.0, 0.0, 5.0), (de_light_kind)(i % 5u));

    for (u32 n = 1; n <= SCRIPT_FRAMES; ++n) {
        rwt_move(f, cam, (de_dvec3){37.0 * (f64)n, 3.0 * (f64)n, 1.8});
        for (u32 i = n % 7u; i < SCRIPT_MESHES; i += 7u)
            if (de_entity_alive(f->w, mesh[i]))
                rwt_move(f, mesh[i], (de_dvec3){(f64)i + 0.25 * (f64)n, (f64)n, 0.5});
        if (n % 5u == 0) { /* replace one */
            const u32 i = lcg(&seed) % SCRIPT_MESHES;
            if (de_entity_alive(f->w, mesh[i]))
                de_entity_despawn(f->w, mesh[i]);
            mesh[i] = rwt_spawn_mesh(f, at((f64)(lcg(&seed) % 500u), 0.0, 0.0), 5000u + n);
        }
        if (n % 9u == 0) {
            const u32 i = lcg(&seed) % SCRIPT_MESHES;
            DeMeshRenderer *m =
                (DeMeshRenderer *)de_entity_get(f->w, mesh[i], f->ids.mesh_renderer);
            if (m != NULL)
                m->materials[1].id = 7000u + n;
        }
        if (n % 13u == 0) { /* a component removed, then added back later */
            const u32 i = lcg(&seed) % SCRIPT_MESHES;
            if (de_entity_has(f->w, mesh[i], f->ids.mesh_renderer))
                de_entity_remove(f->w, mesh[i], f->ids.mesh_renderer);
            else if (de_entity_alive(f->w, mesh[i]))
                ((DeMeshRenderer *)de_entity_add(f->w, mesh[i], f->ids.mesh_renderer))->mesh.id =
                    9000u + n;
        }
        if (n % 11u == 0) {
            DeLight *l = (DeLight *)de_entity_get(f->w, light[n % SCRIPT_LIGHTS], f->ids.light);
            l->color = (de_vec3){1.0f, 0.5f, (f32)n / 100.0f};
        }
        if (n == 50u)
            de_entity_despawn(f->w, light[2]);
        if (n == 60u)
            light[2] = rwt_spawn_light(f, at(-20.0, -20.0, 3.0), DE_LIGHT_SPOT);
        ASSERT_EQ(rwt_frame(f), DE_OK);
    }
    de__render_world_wait_idle(f->r);
    EXPECT_EQ(de_atomic_load_u32(&log->count, DE_ATOMIC_ACQUIRE), SCRIPT_FRAMES);
    EXPECT_EQ(de_atomic_load_u32(&log->out_of_order, DE_ATOMIC_RELAXED), 0u);
    EXPECT_EQ(de__render_world_rendered(f->r), pipelined ? (u64)SCRIPT_FRAMES : 0u);
    de__render_world_set_frame_hook(f->r, NULL, NULL);
    rwt_stop(f);
}

TEST_F(render_thread, unpipelined_equivalence, rtt) {
    static digest_log piped, inline_;
    run_script(&f->fx, true, &piped);
    run_script(&f->fx, false, &inline_);
    u32 first_diff = SCRIPT_FRAMES;
    for (u32 i = 0; i < SCRIPT_FRAMES && first_diff == SCRIPT_FRAMES; ++i)
        if (piped.digest[i] != inline_.digest[i])
            first_diff = i;
    if (first_diff != SCRIPT_FRAMES)
        printf("first differing frame: %u\n", first_diff + 1u);
    EXPECT_EQ(first_diff, SCRIPT_FRAMES);
    /* The script is not trivial: digests change from frame to frame. */
    EXPECT_NE(piped.digest[10], piped.digest[11]);
}
