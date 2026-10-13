/* test_det_input.c: the input recording test vector and its determinism scenario (DENG-89; SPEC
 * §19.4, §5.7 rule 7, §15).
 *
 * Golden files (this directory), a cross-platform contract:
 *   input_replay.deir             the scripted session (input_script.c) recorded live
 *   input_replay_ticks.checksums  de_input_tick_checksum after each recorded tick
 *   input_replay.checksums        the world checksum after each tick of the `input_replay`
 *                                 scenario: the recording replayed into a world whose FixedUpdate
 *                                 system integrates the actions into a snapshot component
 *
 * Tests (CTest det.input_replay.*):
 *   live_rerecord     records the script live and checks that the bytes equal input_replay.deir
 *                     and the per-tick input checksums equal input_replay_ticks.checksums: the
 *                     whole live path (events, action evaluation with de_dmath, quantization,
 *                     encoding) is the same on every platform
 *   golden            the scenario on 0 workers against input_replay.checksums
 *   workers_1_vs_n    the scenario on 0, 1 and 4 workers gives the same checksums
 * Each writes what it computed to <build>/determinism/ (DE_DET_OUT_DIR), which the determinism
 * workflow diffs across platforms.
 *
 * Regenerate only when the script, the schema, the format or the quantization changes on
 * purpose, on one platform, in this order:
 *     DE_DET_UPDATE_GOLDEN=1 ctest --preset dev -R det.input_replay.live_rerecord
 *     DE_DET_UPDATE_GOLDEN=1 ctest --preset dev -R det.input_replay.golden */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv, fopen */
#endif
#include "de_det_harness.h"
#include "input_script.h"

#include "input/record_format.h"

#include "de_test.h"

#include <dengine/de_reflect.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_DET_OUT_DIR
#error "DE_DET_OUT_DIR must be defined by tests/determinism/input/CMakeLists.txt"
#endif
#ifndef DE_DET_GOLDEN_DIR
#error "DE_DET_GOLDEN_DIR must be defined by tests/determinism/input/CMakeLists.txt"
#endif

#define TICKS     INPUT_SCRIPT_TICKS
#define ROOT      "detinput"
#define DEIR_PATH ROOT ":/input_replay.deir"

static const char *out_dir(void) {
    const char *env = getenv("DE_DET_OUT_DIR");
    return (env != NULL && env[0] != '\0') ? env : DE_DET_OUT_DIR;
}

static bool updating(void) {
    const char *u = getenv("DE_DET_UPDATE_GOLDEN");
    return u != NULL && u[0] == '1';
}

static void mount_golden(void) {
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount(ROOT, DE_DET_GOLDEN_DIR,
                              updating() ? DE_MOUNT_WRITABLE : DE_MOUNT_READ_ONLY, 0),
                  DE_OK);
        mounted = true;
    }
}

/* Writes `sums` to <out>/<name>.checksums and, when updating, to the golden directory; otherwise
 * compares them with the golden file. */
static void check_golden(const char *name, const u64 *sums, u32 count) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.checksums", out_dir(), name);
    EXPECT_EQ(de_det_write_checksums(path, name, 0, sums, count), DE_OK);
    snprintf(path, sizeof path, "%s/%s.checksums", DE_DET_GOLDEN_DIR, name);
    if (updating()) {
        ASSERT_EQ(de_det_write_checksums(path, name, 0, sums, count), DE_OK);
        fprintf(stderr, "determinism: rewrote %s\n", path);
        return;
    }
    static u64 golden[TICKS + 8u];
    u32 n = 0;
    ASSERT_EQ(de_det_read_checksums(path, golden, TICKS + 8u, &n), DE_OK);
    ASSERT_EQ(n, count);
    EXPECT_EQ(de_det_report(name, golden, sums, count), DE_DET_NO_DIVERGENCE);
}

/* ---- det.input_replay.live_rerecord ------------------------------------------------------- */

typedef struct live_ctx {
    de_input_recording *rec;
    u64 sums[TICKS];
    u32 failures;
} live_ctx;

static void live_tick(de_input *in, u64 tick, u32 index, void *user) {
    live_ctx *c = (live_ctx *)user;
    c->sums[index] = de_input_tick_checksum(in);
    if (de_input_record_tick(in, c->rec, tick) != DE_OK)
        c->failures++;
}

TEST(input_replay, live_rerecord) {
    mount_golden();
    de_input_desc d;
    memset(&d, 0, sizeof d);
    de_input *in = NULL;
    ASSERT_EQ(de_input_create(&d, &in), DE_OK);
    ASSERT_TRUE(input_script_schema(in));
    static live_ctx c;
    memset(&c, 0, sizeof c);
    de_input_record_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.tick_rate = INPUT_SCRIPT_TICK_RATE;
    rd.reserve_ticks = TICKS;
    ASSERT_EQ(de_input_record_begin(in, &rd, &c.rec), DE_OK);
    input_script_run_live(in, TICKS, live_tick, &c);
    EXPECT_EQ(c.failures, 0u);
    EXPECT_EQ(de_input_recording_tick_count(c.rec), TICKS);

    char out[1024];
    snprintf(out, sizeof out, "%s/input_replay.deir", out_dir());
    const u64 size = de__recording_file_size(c.rec);
    u8 *bytes = (u8 *)malloc((size_t)size);
    ASSERT_TRUE(bytes != NULL);
    de__recording_write_file(c.rec, bytes);
    FILE *f = fopen(out, "wb");
    if (f != NULL) {
        fwrite(bytes, 1, (size_t)size, f);
        fclose(f);
    }
    if (updating()) {
        ASSERT_EQ(de_input_record_save(c.rec, DEIR_PATH), DE_OK);
        fprintf(stderr, "determinism: rewrote %s/input_replay.deir (%llu bytes)\n",
                DE_DET_GOLDEN_DIR, (unsigned long long)size);
    } else {
        void *golden = NULL;
        u64 golden_size = 0;
        ASSERT_EQ(de_file_read_all(DEIR_PATH, NULL, &golden, &golden_size), DE_OK);
        EXPECT_EQ(golden_size, size);
        if (golden_size == size)
            EXPECT_MEMEQ(golden, bytes, (size_t)size);
        DE_FREE(de_heap_allocator(), golden, (size_t)golden_size + 1u);
    }
    free(bytes);
    check_golden("input_replay_ticks", c.sums, TICKS);
    de_input_recording_destroy(c.rec);
    de_input_destroy(in);
}

/* ---- The replay scenario ------------------------------------------------------------------ */

/* Simulation state fed by the replayed actions (a snapshot component, so it is checksummed). */
typedef struct DInputSim {
    u64 input_sum; /* de_input_tick_checksum of the tick */
    f32 px, py;    /* move integrated */
    f32 yaw, pitch;
    f32 throttle;
    f32 scroll;
    u32 triggers; /* triggered actions, counted */
    u32 held;     /* held bits of this tick */
} DInputSim;

typedef struct det_input_state {
    de_input *in;
    de_input_recording *rec;
    de_component sim;
    de_action a[INPUT_SCRIPT_ACTIONS];
    u32 failures;
} det_input_state;

enum { A_JUMP, A_FIRE, A_DASH, A_CROUCH, A_USE, A_MOVE, A_LOOK, A_THROTTLE, A_CONFIRM, A_SCROLL };

static void sim_chunk(const de_query_chunk *ch, void *user) {
    const det_input_state *st = (const det_input_state *)user;
    const de_input *in = st->in;
    DInputSim *s = (DInputSim *)ch->columns[0];
    for (u32 i = 0; i < ch->count; ++i) {
        s[i].input_sum = de_input_tick_checksum(in);
        const de_vec2 mv = de_action_value2(in, st->a[A_MOVE]);
        s[i].px += mv.x * 0.25f;
        s[i].py += mv.y * 0.25f;
        const de_vec2 lk = de_action_value2(in, st->a[A_LOOK]);
        s[i].yaw += lk.x * 0.125f;
        s[i].pitch += lk.y * 0.125f;
        s[i].throttle = s[i].throttle * 0.5f + de_action_value(in, st->a[A_THROTTLE]);
        s[i].scroll += de_action_value(in, st->a[A_SCROLL]);
        u32 held = 0;
        for (u32 k = 0; k < INPUT_SCRIPT_ACTIONS; ++k) {
            if (de_action_triggered(in, st->a[k]))
                s[i].triggers++;
            if (de_action_held(in, st->a[k]))
                held |= 1u << k;
        }
        s[i].held = held;
    }
}

static void sim_system(de_system_ctx *ctx) {
    de_query_each(ctx->world, ctx->queries[0], sim_chunk, ctx->user);
}

static de_result replay_setup(de_world *w, u64 seed, void *state) {
    (void)seed;
    det_input_state *st = (det_input_state *)state;
    de_input_desc d;
    memset(&d, 0, sizeof d);
    de_result r = de_input_create(&d, &st->in);
    if (r != DE_OK)
        return r;
    if (!input_script_schema(st->in))
        return DE_FAIL(DE_ERR_INTERNAL, "input_replay: schema");
    input_script_actions(st->in, st->a);
    r = de_input_attach(st->in, w);
    if (r != DE_OK)
        return r;

    static const de_field_info k_sim[] = {
        DE_FIELD(DInputSim, input_sum, DE_FK_U64, DE_FIELD_DEFAULT),
        DE_FIELD(DInputSim, px, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DInputSim, py, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DInputSim, yaw, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DInputSim, pitch, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DInputSim, throttle, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DInputSim, scroll, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(DInputSim, triggers, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(DInputSim, held, DE_FK_U32, DE_FIELD_DEFAULT),
    };
    const de_type_id t = de_type_register(de_world_types(w), &DE_TYPE_DESC(DInputSim, 1, k_sim));
    st->sim = de_component_register(w, t, DE_COMP_SNAPSHOT);
    if (!DE_HANDLE_VALID(st->sim))
        return DE_FAIL_WRAP(DE_ERR_INVALID_ARGUMENT, "input_replay: component registration");
    de_entity e;
    r = de_entity_spawn_with(w, &st->sim, 1, &e);
    if (r != DE_OK)
        return r;

    const de_query_term terms[1] = {{st->sim, DE_WRITE}};
    const de_query_desc q = {terms, 1, NULL, 0};
    de_resource_access acc;
    acc.id = st->in->resource;
    acc.access = DE_READ;
    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "det.input_sim";
    sd.schedule = DE_SCHEDULE_FIXED_UPDATE;
    sd.flags = DE_SYSTEM_DETERMINISTIC;
    sd.fn = sim_system;
    sd.user = st;
    sd.queries = &q;
    sd.query_count = 1;
    sd.resources = &acc;
    sd.resource_count = 1;
    if (!DE_HANDLE_VALID(de_system_add(w, &sd)))
        return DE_FAIL_WRAP(DE_ERR_INVALID_ARGUMENT, "input_replay: system");

    return de_input_replay_load(st->in, DEIR_PATH, &st->rec);
}

static void replay_tick(de_world *w, u64 tick_index, void *state) {
    (void)w;
    det_input_state *st = (det_input_state *)state;
    if (!de_input_replay_tick(st->in, st->rec, INPUT_SCRIPT_FIRST_TICK + tick_index))
        st->failures++;
}

static void replay_teardown(de_world *w, void *state) {
    (void)w;
    det_input_state *st = (det_input_state *)state;
    de_input_recording_destroy(st->rec);
    if (st->in != NULL) {
        de_input_detach(st->in);
        de_input_destroy(st->in);
    }
}

static const de_det_scenario det_input_replay = {
    "input_replay",                     /* name */
    replay_setup,                       /* setup */
    replay_tick,                        /* tick */
    replay_teardown,                    /* teardown */
    TICKS,                              /* ticks */
    (u32)sizeof(det_input_state),       /* state_size */
    1.0f / (f32)INPUT_SCRIPT_TICK_RATE, /* dt */
    0u,                                 /* checksum mask: DE_COMP_SNAPSHOT */
};

static u64 g_run[TICKS];
static u64 g_other[TICKS];

TEST(input_replay, golden) {
    mount_golden();
    ASSERT_EQ(de_det_run(&det_input_replay, 0, 0, g_run), DE_OK);
    check_golden("input_replay", g_run, TICKS);
}

TEST(input_replay, workers_1_vs_n) {
    mount_golden();
    ASSERT_EQ(de_det_run(&det_input_replay, 0, 0, g_run), DE_OK);
    static const u32 workers[] = {1u, 4u};
    for (u32 i = 0; i < 2u; ++i) {
        ASSERT_EQ(de_det_run(&det_input_replay, 0, workers[i], g_other), DE_OK);
        char what[64];
        snprintf(what, sizeof what, "input_replay workers 0 vs %u", (unsigned)workers[i]);
        EXPECT_EQ(de_det_report(what, g_run, g_other, TICKS), DE_DET_NO_DIVERGENCE);
    }
}
