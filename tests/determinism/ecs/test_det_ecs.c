/* test_det_ecs.c: ECS determinism scenario tests and harness self tests (DENG-353; SPEC §19.4,
 * §5.7 rule 7).
 *
 * The golden file ecs_spawn_move.checksums is a cross-platform contract: the per-tick
 * de_world_checksum of the scenario with seed DET_ECS_SPAWN_MOVE_SEED must be the same on every
 * platform, architecture, compiler and worker count. A mismatch is a determinism bug, never a
 * reason to regenerate the file. Regenerate it only when the scenario itself changes on purpose:
 *     DE_DET_UPDATE_GOLDEN=1 ctest --preset dev -R det.ecs_spawn_move.golden
 * Every run of det.ecs_spawn_move.golden also writes what it computed to
 * <build>/determinism/ecs_spawn_move.checksums (DE_DET_OUT_DIR overrides the directory), which the
 * `determinism` CI workflow uploads and compares across platforms. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv, fopen */
#endif
#include "de_test.h"

#include "scenario_spawn_move.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_DET_OUT_DIR
#error "DE_DET_OUT_DIR must be defined by tests/determinism/ecs/CMakeLists.txt"
#endif
#ifndef DE_DET_GOLDEN_DIR
#error "DE_DET_GOLDEN_DIR must be defined by tests/determinism/ecs/CMakeLists.txt"
#endif

#define TICKS        1000u
#define MANY_WORKERS 8u

static u64 g_base[TICKS], g_run[TICKS], g_file[TICKS + 8u];

static const char *out_dir(void) {
    const char *env = getenv("DE_DET_OUT_DIR");
    return (env != NULL && env[0] != '\0') ? env : DE_DET_OUT_DIR;
}

static void out_path(char *buf, size_t n, const char *file) {
    snprintf(buf, n, "%s/%s", out_dir(), file);
}

static void run_scenario(u32 workers, u64 *out) {
    ASSERT_EQ(det_ecs_spawn_move.ticks, TICKS);
    const de_result r = de_det_run(&det_ecs_spawn_move, DET_ECS_SPAWN_MOVE_SEED, workers, out);
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        fprintf(stderr, "de_det_run: %s\n", e != NULL ? e->message : "(no message)");
    }
    ASSERT_EQ(r, DE_OK);
}

/* The fixture resets the test-only injection switch, also after a failed check. */
typedef struct det_fx {
    int unused;
} det_fx;
static void det_fx_setup(det_fx *f) {
    (void)f;
    det_ecs_inject.kind = DET_INJECT_NONE;
    det_ecs_inject.from_tick = 0;
}
static void det_fx_teardown(det_fx *f) {
    det_fx_setup(f);
}

/* ---- det.ecs_spawn_move.workers_1_vs_n ----------------------------------------------------- */

TEST_F(ecs_spawn_move, workers_1_vs_n, det_fx) {
    (void)f;
    /* Baseline: no task system (every system and range serially on this thread). */
    run_scenario(0, g_base);
    /* The world changes every tick: almost every checksum is new (a stuck world would make the
     * comparison meaningless). */
    u32 changes = 0;
    for (u32 t = 1; t < TICKS; ++t)
        changes += g_base[t] != g_base[t - 1];
    EXPECT_GE(changes, TICKS - 10u);

    static const u32 workers[2] = {1u, MANY_WORKERS};
    for (u32 w = 0; w < 2u; ++w) {
        for (u32 rep = 0; rep < 3u; ++rep) {
            run_scenario(workers[w], g_run);
            char what[64];
            snprintf(what, sizeof what, "workers 0 vs %u (run %u)", workers[w], rep + 1u);
            EXPECT_EQ(de_det_report(what, g_base, g_run, TICKS), DE_DET_NO_DIVERGENCE);
        }
    }
    /* And the serial baseline itself repeats, here through the session API. The population
     * stays in the hundreds (spawns, children and despawns balance), so every tick has parallel
     * ranges and commands to apply. */
    de_det_session s;
    ASSERT_EQ(de_det_session_begin(&s, &det_ecs_spawn_move, DET_ECS_SPAWN_MOVE_SEED, 0), DE_OK);
    u32 min_alive = 0xFFFFFFFFu, max_alive = 0;
    const de_query_term any_vel = {de_component_find(s.world, "DVel"), DE_READ};
    const de_query_desc qd = {&any_vel, 1, NULL, 0};
    const de_query alive = de_query_create(s.world, &qd);
    ASSERT_TRUE(DE_HANDLE_VALID(alive));
    for (u32 t = 0; t < TICKS; ++t) {
        ASSERT_EQ(de_det_session_step(&s, &g_run[t]), DE_OK);
        const u32 n = de_query_count(s.world, alive);
        if (t >= 100u && n < min_alive)
            min_alive = n;
        if (n > max_alive)
            max_alive = n;
    }
    de_det_session_end(&s);
    fprintf(stderr, "ecs_spawn_move: %u to %u moving entities (minimum after tick 100)\n",
            min_alive, max_alive);
    EXPECT_GE(min_alive, 200u);
    EXPECT_EQ(de_det_report("workers 0 vs 0", g_base, g_run, TICKS), DE_DET_NO_DIVERGENCE);
}

/* ---- det.ecs_spawn_move.golden ------------------------------------------------------------- */

TEST_F(ecs_spawn_move, golden, det_fx) {
    (void)f;
    run_scenario(MANY_WORKERS, g_run);

    char path[1024];
    out_path(path, sizeof path, "ecs_spawn_move.checksums");
    EXPECT_EQ(de_det_write_checksums(path, det_ecs_spawn_move.name, DET_ECS_SPAWN_MOVE_SEED, g_run,
                                     TICKS),
              DE_OK);

    char golden[1024];
    snprintf(golden, sizeof golden, "%s/ecs_spawn_move.checksums", DE_DET_GOLDEN_DIR);
    const char *update = getenv("DE_DET_UPDATE_GOLDEN");
    if (update != NULL && update[0] == '1') {
        ASSERT_EQ(de_det_write_checksums(golden, det_ecs_spawn_move.name, DET_ECS_SPAWN_MOVE_SEED,
                                         g_run, TICKS),
                  DE_OK);
        fprintf(stderr, "determinism: rewrote %s\n", golden);
    }

    u32 n = 0;
    const de_result r = de_det_read_checksums(golden, g_file, TICKS + 8u, &n);
    ASSERT_EQ(r, DE_OK);
    ASSERT_EQ(n, TICKS);
    EXPECT_EQ(de_det_report("golden vs this build", g_file, g_run, TICKS), DE_DET_NO_DIVERGENCE);
}

/* ---- det.ecs_spawn_move.rollback ----------------------------------------------------------- */

/* Snapshot at tick 400, run 200 ticks, restore, run the same 200 ticks again: both legs must
 * equal the straight run (the spawner's random stream lives in a snapshot component). */
TEST_F(ecs_spawn_move, rollback, det_fx) {
    (void)f;
    run_scenario(0, g_base);

    enum { TAKE = 400, LEG = 200 };
    de_det_session s;
    ASSERT_EQ(de_det_session_begin(&s, &det_ecs_spawn_move, DET_ECS_SPAWN_MOVE_SEED, 2u), DE_OK);
    for (u32 t = 0; t < (u32)TAKE; ++t)
        ASSERT_EQ(de_det_session_step(&s, &g_run[t]), DE_OK);
    EXPECT_EQ(de_det_report("straight vs session", g_base, g_run, TAKE), DE_DET_NO_DIVERGENCE);

    de_world_snapshot snap;
    ASSERT_EQ(de_world_snapshot_take(s.world, NULL, &snap), DE_OK);
    EXPECT_EQ(snap.tick, (u64)TAKE);
    for (u32 leg = 0; leg < 2u; ++leg) {
        if (leg == 1u)
            ASSERT_EQ(de_world_snapshot_restore(s.world, &snap), DE_OK);
        EXPECT_EQ(de_world_tick(s.world), (u64)TAKE);
        for (u32 t = TAKE; t < (u32)(TAKE + LEG); ++t)
            ASSERT_EQ(de_det_session_step(&s, &g_run[t]), DE_OK);
        char what[48];
        snprintf(what, sizeof what, "straight vs rollback leg %u", leg);
        EXPECT_EQ(de_det_report(what, g_base + TAKE, g_run + TAKE, LEG), DE_DET_NO_DIVERGENCE);
    }
    de_world_snapshot_free(NULL, &snap);
    de_det_session_end(&s);
}

/* ---- det.harness_detects_divergence --------------------------------------------------------- */

/* Hash-map iteration order injected into the wind (outside the schedule) from tick 320, a wind
 * tick. The harness must report exactly that tick, serially and with workers. */
TEST_F(harness, detects_divergence, det_fx) {
    (void)f;
    const u64 from = 5u * DET_ECS_WIND_PERIOD;
    run_scenario(0, g_base);
    static const u32 workers[2] = {0u, MANY_WORKERS};
    for (u32 w = 0; w < 2u; ++w) {
        det_ecs_inject.kind = DET_INJECT_HASHMAP_ORDER;
        det_ecs_inject.from_tick = from;
        run_scenario(workers[w], g_run);
        det_ecs_inject.kind = DET_INJECT_NONE;
        fprintf(stderr, "(expected) injected hash-map order, workers %u:\n", workers[w]);
        EXPECT_EQ(de_det_report("baseline vs injected", g_base, g_run, TICKS), (u32)from);
        EXPECT_EQ(de_det_compare(g_base, g_run, TICKS), (u32)from);
    }
    /* Injection switched off again: back to the baseline. */
    run_scenario(0, g_run);
    EXPECT_EQ(de_det_compare(g_base, g_run, TICKS), DE_DET_NO_DIVERGENCE);
}

/* ---- det.harness_compare_and_io ------------------------------------------------------------- */

static void write_text(const char *path, const char *text) {
    FILE *fp = fopen(path, "wb");
    ASSERT_TRUE(fp != NULL);
    ASSERT_EQ(fwrite(text, 1, strlen(text), fp), strlen(text));
    ASSERT_EQ(fclose(fp), 0);
}

TEST(harness, compare_and_io) {
    const u64 a[5] = {1, 2, 3, 4, 5};
    u64 b[5] = {1, 2, 3, 4, 5};
    EXPECT_EQ(de_det_compare(a, b, 5), DE_DET_NO_DIVERGENCE);
    EXPECT_EQ(de_det_compare(a, b, 0), DE_DET_NO_DIVERGENCE);
    b[3] = 40;
    b[4] = 50;
    EXPECT_EQ(de_det_compare(a, b, 5), 3u);
    EXPECT_EQ(de_det_compare(a, b, 3), DE_DET_NO_DIVERGENCE);
    b[0] = 0;
    EXPECT_EQ(de_det_compare(a, b, 5), 0u);
    EXPECT_EQ(de_det_report("(expected) synthetic", a, b, 5), 0u);
    EXPECT_EQ(de_det_compare(NULL, b, 5), 0u);

    /* Round trip, including values that need all 16 digits. Not named *.checksums, so CI does
     * not upload it with the scenario files. */
    const u64 v[4] = {0, 0xFFFFFFFFFFFFFFFFull, 0x0123456789ABCDEFull, 42};
    char path[1024];
    out_path(path, sizeof path, "harness_io.txt");
    ASSERT_EQ(de_det_write_checksums(path, "harness_io", 7, v, 4), DE_OK);
    u64 r[8];
    u32 n = 99;
    ASSERT_EQ(de_det_read_checksums(path, r, 8, &n), DE_OK);
    ASSERT_EQ(n, 4u);
    EXPECT_MEMEQ(r, v, sizeof v);
    /* Capacity too small. */
    EXPECT_EQ(de_det_read_checksums(path, r, 3, &n), DE_ERR_PARSE);
    EXPECT_EQ(n, 3u);

    /* CR LF, blank lines, comments and upper-case digits are accepted. */
    write_text(path, "# header\r\n00000000000000aB\r\n\r\n# mid\n  7  \n");
    ASSERT_EQ(de_det_read_checksums(path, r, 8, &n), DE_OK);
    ASSERT_EQ(n, 2u);
    EXPECT_EQ(r[0], 0xABu);
    EXPECT_EQ(r[1], 7u);

    /* Not one hex value per line. */
    write_text(path, "0123\nxyz\n");
    EXPECT_EQ(de_det_read_checksums(path, r, 8, &n), DE_ERR_PARSE);
    write_text(path, "12 34\n");
    EXPECT_EQ(de_det_read_checksums(path, r, 8, &n), DE_ERR_PARSE);
    write_text(path, "11112222333344445\n"); /* 17 digits */
    EXPECT_EQ(de_det_read_checksums(path, r, 8, &n), DE_ERR_PARSE);

    out_path(path, sizeof path, "does_not_exist.txt");
    EXPECT_EQ(de_det_read_checksums(path, r, 8, &n), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_det_read_checksums(NULL, r, 8, &n), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_det_write_checksums(NULL, "x", 0, v, 4), DE_ERR_INVALID_ARGUMENT);

    /* Bad scenarios are refused, and the session is left zeroed. */
    de_det_session s;
    de_det_scenario bad;
    memset(&bad, 0, sizeof bad);
    bad.name = "bad";
    EXPECT_EQ(de_det_session_begin(&s, &bad, 0, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(s.world == NULL);
    EXPECT_EQ(de_det_session_begin(NULL, &det_ecs_spawn_move, 0, 0), DE_ERR_INVALID_ARGUMENT);
}
