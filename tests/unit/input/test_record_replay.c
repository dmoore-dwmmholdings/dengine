/* test_record_replay.c: input recording and replay per fixed tick (DENG-89): quantization, the
 * .deir format, live == replay, schema checks, corrupt files, the replay override, and the
 * checked-in test vector (tests/determinism/input/). The scripted session comes from
 * tests/determinism/input/input_script.c. */
#include "input_script.h"
#include "input_test_util.h"

#include "input/record_format.h"

#include "de_alloc_guard.h"
#include "de_det_harness.h"
#include "de_test.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_INPUT_TEST_OUT_DIR
#error "DE_INPUT_TEST_OUT_DIR must be defined by tests/unit/input/CMakeLists.txt"
#endif
#ifndef DE_INPUT_GOLDEN_DIR
#error "DE_INPUT_GOLDEN_DIR must be defined by tests/unit/input/CMakeLists.txt"
#endif

#define TICKS INPUT_SCRIPT_TICKS
#define N     INPUT_SCRIPT_ACTIONS

static void mount_dirs(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("recout", DE_INPUT_TEST_OUT_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("recgold", DE_INPUT_GOLDEN_DIR, DE_MOUNT_READ_ONLY, 0), DE_OK);
    mounted = true;
}

static de_input *script_input(void) {
    de_input *in = input_test_create(NULL);
    if (in != NULL && !input_script_schema(in)) {
        de_input_destroy(in);
        return NULL;
    }
    return in;
}

/* Bitwise equality of two tick states (fields, not padding). */
static bool state_eq(const de__action_state *a, const de__action_state *b) {
    return a->triggered == b->triggered && a->held == b->held &&
           memcmp(&a->value, &b->value, sizeof a->value) == 0;
}

/* A recorded live session: per-tick tick views and input checksums. */
typedef struct session {
    de_input_recording *rec;
    de__action_state states[TICKS][N];
    u64 sums[TICKS];
    u32 record_failures;
} session;

static void capture(de_input *in, u32 index, session *s) {
    de_action a[N];
    input_script_actions(in, a);
    for (u32 k = 0; k < N; ++k)
        (void)de__action_tick_state_get(in, a[k], &s->states[index][k]);
    s->sums[index] = de_input_tick_checksum(in);
}

static void live_tick(de_input *in, u64 tick, u32 index, void *user) {
    session *s = (session *)user;
    capture(in, index, s);
    if (s->rec != NULL && de_input_record_tick(in, s->rec, tick) != DE_OK)
        s->record_failures++;
}

static session g_live, g_replay;

/** Records the scripted session into g_live (rec kept in g_live.rec). */
static de_input *record_script(void) {
    de_input *in = script_input();
    if (in == NULL)
        return NULL;
    memset(&g_live, 0, sizeof g_live);
    de_input_record_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.tick_rate = INPUT_SCRIPT_TICK_RATE;
    rd.reserve_ticks = TICKS;
    if (de_input_record_begin(in, &rd, &g_live.rec) != DE_OK) {
        de_input_destroy(in);
        return NULL;
    }
    input_script_run_live(in, TICKS, live_tick, &g_live);
    return in;
}

/** Replays `rec` (loaded on `in`) for every scripted tick into g_replay, feeding the script's
 * events (different frames) as live noise that must not reach the tick view. */
static void replay_all(de_input *in, de_input_recording *rec) {
    memset(&g_replay, 0, sizeof g_replay);
    for (u32 i = 0; i < TICKS; ++i) {
        input_script_events(in, 5000u + i); /* live noise */
        de__input_begin_frame_dt(in, INPUT_TEST_DT);
        de__actions_frame_update(in, INPUT_TEST_DT);
        EXPECT_TRUE(de_input_replay_tick(in, rec, INPUT_SCRIPT_FIRST_TICK + i));
        de__actions_fixed_tick(in, 7000u + i);
        capture(in, i, &g_replay);
    }
}

/* ---- Quantization -------------------------------------------------------------------------- */

static f32 f32_below(f32 v) {
    u32 b;
    memcpy(&b, &v, sizeof b);
    b -= 1u; /* positive v: the next float toward 0 */
    memcpy(&v, &b, sizeof v);
    return v;
}

TEST(record_replay, quantization_rounding) {
    EXPECT_EQ(de__quantize_axis(0.0f), 0);
    EXPECT_EQ(de__quantize_axis(-0.0f), 0);
    EXPECT_EQ(de__quantize_axis(1.0f), 32767);
    EXPECT_EQ(de__quantize_axis(-1.0f), -32767);
    EXPECT_EQ(de__quantize_axis(7.5f), 32767); /* clamped */
    EXPECT_EQ(de__quantize_axis(-1e30f), -32767);
    volatile f32 zero = 0.0f;
    const f32 nan = zero / zero;
    EXPECT_EQ(de__quantize_axis(nan), 0);
    /* 0.5 * 32767 = 16383.5 exactly: half rounds away from zero, both signs. */
    EXPECT_EQ(de__quantize_axis(0.5f), 16384);
    EXPECT_EQ(de__quantize_axis(-0.5f), -16384);
    /* Just below the half: down. A float add-half-and-truncate (t + 0.5f in f32) gets this
     * wrong for values whose product is close to n + 0.5; the exact path does not. */
    EXPECT_EQ(de__quantize_axis(f32_below(0.5f)), 16383);
    EXPECT_EQ(de__quantize_axis(-f32_below(0.5f)), -16383);
    EXPECT_EQ(de__quantize_axis(1e-30f), 0);
    /* Every quantized value survives dequantize -> quantize (the tick view is stable). */
    for (i32 q = -32767; q <= 32767; ++q) {
        const i16 back = de__quantize_axis(de__dequantize_axis((i16)q));
        if (back != q) {
            EXPECT_EQ(back, q);
            break;
        }
    }
    EXPECT_FLOAT_EQ(de__dequantize_axis(32767), 1.0, 0.0);
    EXPECT_FLOAT_EQ(de__dequantize_axis(-32767), -1.0, 0.0);
}

TEST(record_replay, quantized_tick_view) {
    de_input *in = script_input();
    ASSERT_TRUE(in != NULL);
    de_action a[N];
    input_script_actions(in, a);
    EXPECT_FALSE(de_input_tick_quantization(in));
    ASSERT_EQ(de_input_set_tick_quantization(in, true), DE_OK);
    EXPECT_TRUE(de_input_tick_quantization(in));
    /* A pad stick deflection that is not a multiple of 1/32767: the tick view holds the
     * quantized value, the frame view the exact one. */
    input_test_pad_add(in, INPUT_SCRIPT_PAD, DE_PLATFORM_PAD_PS5, 0, "pad");
    input_test_frame(in);
    input_test_pad_axis(in, INPUT_SCRIPT_PAD, DE_AXIS_RIGHT_TRIGGER, 0.123456f);
    input_test_pad_axis(in, INPUT_SCRIPT_PAD, DE_AXIS_LEFT_TRIGGER, 0.3f);
    input_test_frame(in);
    de__actions_frame_update(in, INPUT_TEST_DT);
    de__actions_fixed_tick(in, 1);
    de__action_state fr, tk;
    ASSERT_TRUE(de__action_frame_state_get(in, a[7], &fr)); /* throttle: left trigger * 0.5 */
    ASSERT_TRUE(de__action_tick_state_get(in, a[7], &tk));
    EXPECT_FLOAT_EQ(fr.value.x, 0.15, 1e-6);
    EXPECT_EQ(de__quantize_axis(fr.value.x), 4915); /* 0.15 * 32767 = 4915.05 */
    EXPECT_TRUE(memcmp(&tk.value.x, &(f32){4915.0f / 32767.0f}, sizeof(f32)) == 0);
    EXPECT_FALSE(tk.triggered); /* axes carry no edges in the quantized stream */
    /* Fire is a button bound to the right trigger (0.12 < 0.5): not held, value 0. */
    ASSERT_TRUE(de__action_tick_state_get(in, a[1], &tk));
    EXPECT_FALSE(tk.held);
    EXPECT_FLOAT_EQ(tk.value.x, 0.0, 0.0);
    /* Off again: the tick view is the latched frame view. */
    ASSERT_EQ(de_input_set_tick_quantization(in, false), DE_OK);
    EXPECT_FALSE(de_input_tick_quantization(in));
    de__actions_fixed_tick(in, 2);
    ASSERT_TRUE(de__action_tick_state_get(in, a[7], &tk));
    EXPECT_TRUE(memcmp(&tk.value.x, &fr.value.x, sizeof(f32)) == 0);
    de_input_destroy(in);
}

/* ---- Live == replay ------------------------------------------------------------------------ */

TEST(record_replay, live_equals_replay_1000_ticks) {
    mount_dirs();
    de_input *live = record_script();
    ASSERT_TRUE(live != NULL);
    EXPECT_EQ(g_live.record_failures, 0u);
    EXPECT_EQ(de_input_recording_tick_count(g_live.rec), TICKS);
    EXPECT_EQ(de_input_recording_first_tick(g_live.rec), (u64)INPUT_SCRIPT_FIRST_TICK);
    EXPECT_EQ(de_input_recording_last_tick(g_live.rec), (u64)INPUT_SCRIPT_FIRST_TICK + TICKS - 1u);
    ASSERT_EQ(de_input_record_save(g_live.rec, "recout:/live_equals_replay.deir"), DE_OK);
    const u64 rec_sum = de_input_recording_checksum(g_live.rec);
    EXPECT_NE(rec_sum, 0u);

    de_input *in = script_input();
    ASSERT_TRUE(in != NULL);
    de_input_recording *rec = NULL;
    ASSERT_EQ(de_input_replay_load(in, "recout:/live_equals_replay.deir", &rec), DE_OK);
    EXPECT_EQ(de_input_recording_tick_count(rec), TICKS);
    EXPECT_EQ(de_input_recording_tick_rate(rec), INPUT_SCRIPT_TICK_RATE);
    EXPECT_EQ(de_input_recording_schema(rec), de_input_schema_hash(in));
    EXPECT_EQ(de_input_recording_checksum(rec), rec_sum); /* saving again gives the same bytes */
    replay_all(in, rec);

    u32 first_bad = TICKS;
    for (u32 i = 0; i < TICKS && first_bad == TICKS; ++i) {
        for (u32 k = 0; k < N; ++k) {
            if (!state_eq(&g_live.states[i][k], &g_replay.states[i][k])) {
                first_bad = i;
                break;
            }
        }
    }
    EXPECT_EQ(first_bad, TICKS);
    EXPECT_EQ(de_det_report("live vs replay", g_live.sums, g_replay.sums, TICKS),
              DE_DET_NO_DIVERGENCE);
    EXPECT_FALSE(de_input_replay_tick(in, rec, INPUT_SCRIPT_FIRST_TICK + TICKS));
    de_input_recording_destroy(rec);
    de_input_destroy(in);
    de_input_recording_destroy(g_live.rec);
    de_input_destroy(live);
}

TEST(record_replay, script_covers_every_action) {
    de_input *live = record_script();
    ASSERT_TRUE(live != NULL);
    for (u32 k = 0; k < N; ++k) {
        u32 held = 0, triggered = 0, nonzero = 0;
        for (u32 i = 0; i < TICKS; ++i) {
            held += g_live.states[i][k].held;
            triggered += g_live.states[i][k].triggered;
            nonzero += g_live.states[i][k].value.x != 0.0f || g_live.states[i][k].value.y != 0.0f;
        }
        if (held <= 10u || nonzero <= 10u)
            fprintf(stderr, "action %u: held %u, nonzero %u, triggered %u\n", (unsigned)k,
                    (unsigned)held, (unsigned)nonzero, (unsigned)triggered);
        EXPECT_GT(held, 10u);
        EXPECT_GT(nonzero, 10u);
        if (k == 0 || k == 1 || k == 2 || k == 3 || k == 4 || k == 8)
            EXPECT_GT(triggered, 2u); /* every button interaction fires */
        else
            EXPECT_EQ(triggered, 0u); /* axes: no edges in the quantized stream */
    }
    de_input_recording_destroy(g_live.rec);
    de_input_destroy(live);
}

/* ---- Format -------------------------------------------------------------------------------- */

static de_input *one_button_input(const char *map, const char *name) {
    de_input *in = input_test_create(NULL);
    if (in == NULL)
        return NULL;
    const de_action_map m = de_action_map_create(in, map);
    de_action_map_push(in, m);
    de_binding b;
    memset(&b, 0, sizeof b);
    b.source = DE_BIND_KEY;
    b.code = DE_KEY_SPACE;
    b.composite_slot = DE_COMPOSITE_NONE;
    de_action_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.type = DE_ACTION_BUTTON;
    d.bindings = &b;
    d.binding_count = 1;
    (void)de_action_add(in, m, &d);
    return in;
}

static void tick_once(de_input *in, de_input_recording *rec, u64 tick) {
    input_test_frame(in);
    de__actions_frame_update(in, INPUT_TEST_DT);
    de__actions_fixed_tick(in, tick);
    ASSERT_EQ(de_input_record_tick(in, rec, tick), DE_OK);
}

TEST(record_replay, delta_encoding_size) {
    de_input *in = one_button_input("gameplay", "jump");
    ASSERT_TRUE(in != NULL);
    de_input_recording *rec = NULL;
    ASSERT_EQ(de_input_record_begin(in, NULL, &rec), DE_OK);
    const u64 empty = de__recording_file_size(rec);
    EXPECT_EQ(empty, (u64)DE__DEIR_HEADER_FIXED + 1u + DE__DEIR_TRAILER);
    /* 100 idle ticks: one chunk header, then 2 bytes per tick (delta, count 0). */
    for (u64 t = 1; t <= 100; ++t)
        tick_once(in, rec, t);
    EXPECT_EQ(de__recording_file_size(rec), empty + DE__DEIR_CHUNK_HEADER + 200u);
    /* A press: delta, count 1, one mask byte, one flags byte. Holding: 2 bytes again. */
    input_test_key(in, DE_KEY_SPACE, true);
    tick_once(in, rec, 101);
    EXPECT_EQ(de__recording_file_size(rec), empty + DE__DEIR_CHUNK_HEADER + 204u);
    tick_once(in, rec, 102); /* held, trigger gone: one change */
    EXPECT_EQ(de__recording_file_size(rec), empty + DE__DEIR_CHUNK_HEADER + 208u);
    tick_once(in, rec, 103); /* unchanged */
    EXPECT_EQ(de__recording_file_size(rec), empty + DE__DEIR_CHUNK_HEADER + 210u);
    /* A gap of 200 ticks costs one more varint byte. */
    tick_once(in, rec, 303);
    EXPECT_EQ(de__recording_file_size(rec), empty + DE__DEIR_CHUNK_HEADER + 213u);
    /* Ticks must increase. */
    EXPECT_EQ(de_input_record_tick(in, rec, 303), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_input_record_tick(in, rec, 5), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_input_recording_tick_count(rec), 104u);
    /* The script: 1000 ticks of 10 busy actions fit well under 16 bytes per tick. */
    de_input_recording_destroy(rec);
    de_input_destroy(in);

    de_input *live = record_script();
    ASSERT_TRUE(live != NULL);
    const u64 size = de__recording_file_size(g_live.rec);
    fprintf(stderr, "script recording: %llu bytes for %u ticks\n", (unsigned long long)size,
            (unsigned)TICKS);
    EXPECT_LT(size, (u64)TICKS * 16u);
    de_input_recording_destroy(g_live.rec);
    de_input_destroy(live);
}

TEST(record_replay, record_errors) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    de_input_recording *rec = (de_input_recording *)(uintptr_t)1;
    EXPECT_EQ(de_input_record_begin(in, NULL, &rec), DE_ERR_INVALID_ARGUMENT); /* no actions */
    EXPECT_TRUE(rec == NULL);
    EXPECT_EQ(de_input_record_begin(NULL, NULL, &rec), DE_ERR_INVALID_ARGUMENT);
    de_input_destroy(in);

    in = one_button_input("gameplay", "jump");
    ASSERT_TRUE(in != NULL);
    ASSERT_EQ(de_input_record_begin(in, NULL, &rec), DE_OK);
    tick_once(in, rec, 1);
    /* A new action changes the schema: the next tick is refused, the recording stays valid. */
    const de_action_map m = de_action_map_find(in, "gameplay");
    de_binding b;
    memset(&b, 0, sizeof b);
    b.source = DE_BIND_KEY;
    b.code = DE_KEY_E;
    b.composite_slot = DE_COMPOSITE_NONE;
    de_action_desc d;
    memset(&d, 0, sizeof d);
    d.name = "use";
    d.type = DE_ACTION_BUTTON;
    d.bindings = &b;
    d.binding_count = 1;
    ASSERT_TRUE(DE_HANDLE_VALID(de_action_add(in, m, &d)));
    EXPECT_EQ(de_input_record_tick(in, rec, 2), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_input_recording_tick_count(rec), 1u);
    EXPECT_EQ(de_input_record_save(rec, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_NE(de_input_record_save(rec, "nosuchroot:/x.deir"), DE_OK);
    de_input_recording_destroy(rec);
    /* Recording turned quantization on, and it stays on. */
    EXPECT_TRUE(de_input_tick_quantization(in));
    de_input_destroy(in);
}

/* ---- Schema and corrupt files -------------------------------------------------------------- */

/** The file bytes of a short recording of one_button_input(map, name). */
static u8 *short_file(const char *map, const char *name, u64 *size) {
    de_input *in = one_button_input(map, name);
    if (in == NULL)
        return NULL;
    de_input_recording *rec = NULL;
    if (de_input_record_begin(in, NULL, &rec) != DE_OK)
        return NULL;
    for (u64 t = 10; t < 40; ++t) {
        input_test_key(in, DE_KEY_SPACE, (t % 3) == 0);
        input_test_frame(in);
        de__actions_frame_update(in, INPUT_TEST_DT);
        de__actions_fixed_tick(in, t);
        (void)de_input_record_tick(in, rec, t);
    }
    *size = de__recording_file_size(rec);
    u8 *buf = (u8 *)malloc((size_t)*size);
    if (buf != NULL)
        de__recording_write_file(rec, buf);
    de_input_recording_destroy(rec);
    de_input_destroy(in);
    return buf;
}

static void fix_checksum(u8 *buf, u64 size) {
    de__le64(buf + size - DE__DEIR_TRAILER, de_hash64(buf, (size_t)(size - DE__DEIR_TRAILER), 0));
}

TEST(record_replay, schema_mismatch_rejected) {
    mount_dirs();
    u64 size = 0;
    u8 *file = short_file("gameplay", "jump", &size);
    ASSERT_TRUE(file != NULL);
    de_input_recording *rec = NULL;

    /* Same schema: accepted. */
    de_input *same = one_button_input("gameplay", "jump");
    ASSERT_TRUE(same != NULL);
    ASSERT_EQ(de__input_replay_load_mem(same, file, size, "mem", &rec), DE_OK);
    de_input_recording_destroy(rec);
    de_input_destroy(same);

    /* Another action name, another map name, another type, one more action, no actions. */
    const char *maps[] = {"gameplay", "menu"};
    const char *names[] = {"leap", "jump"};
    for (u32 i = 0; i < 2; ++i) {
        de_input *in = one_button_input(maps[i], names[i]);
        ASSERT_TRUE(in != NULL);
        EXPECT_EQ(de__input_replay_load_mem(in, file, size, "mem", &rec), DE_ERR_INVALID_ARGUMENT);
        EXPECT_TRUE(rec == NULL);
        const de_error *e = de_error_last();
        ASSERT_TRUE(e != NULL);
        EXPECT_TRUE(strstr(e->message, "schema mismatch") != NULL);
        EXPECT_FALSE(de_input_tick_quantization(in)); /* nothing installed */
        de_input_destroy(in);
    }
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map m = de_action_map_create(in, "gameplay");
    de_binding b;
    memset(&b, 0, sizeof b);
    b.source = DE_BIND_PAD_AXIS;
    b.code = DE_AXIS_LEFT_TRIGGER;
    b.composite_slot = DE_COMPOSITE_NONE;
    de_action_desc d;
    memset(&d, 0, sizeof d);
    d.name = "jump";
    d.type = DE_ACTION_AXIS1D;
    d.bindings = &b;
    d.binding_count = 1;
    ASSERT_TRUE(DE_HANDLE_VALID(de_action_add(in, m, &d)));
    EXPECT_EQ(de__input_replay_load_mem(in, file, size, "mem", &rec), DE_ERR_INVALID_ARGUMENT);
    de_input_destroy(in);
    in = input_test_create(NULL);
    EXPECT_EQ(de__input_replay_load_mem(in, file, size, "mem", &rec), DE_ERR_INVALID_ARGUMENT);
    de_input_destroy(in);

    /* The schema hash field patched to another input's hash (checksum fixed) is accepted by that
     * input: the hash is the identity. A matching hash with another type table is refused. */
    de_input *other = one_button_input("gameplay", "leap");
    ASSERT_TRUE(other != NULL);
    de__le64(file + 8, de_input_schema_hash(other));
    fix_checksum(file, size);
    EXPECT_EQ(de__input_replay_load_mem(other, file, size, "mem", &rec), DE_OK);
    de_input_recording_destroy(rec);
    file[DE__DEIR_HEADER_FIXED] = (u8)DE_ACTION_AXIS1D;
    fix_checksum(file, size);
    EXPECT_EQ(de__input_replay_load_mem(other, file, size, "mem", &rec), DE_ERR_INVALID_ARGUMENT);
    de_input_destroy(other);
    free(file);

    /* Through the file system too: the golden recording against a one-action input. */
    in = one_button_input("gameplay", "jump");
    EXPECT_EQ(de_input_replay_load(in, "recgold:/input_replay.deir", &rec),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_input_replay_load(in, "recgold:/missing.deir", &rec), DE_ERR_NOT_FOUND);
    de_input_destroy(in);
}

TEST(record_replay, truncated_file) {
    u64 size = 0;
    u8 *file = short_file("gameplay", "jump", &size);
    ASSERT_TRUE(file != NULL);
    de_input *in = one_button_input("gameplay", "jump");
    ASSERT_TRUE(in != NULL);
    de_input_recording *rec = NULL;

    /* Every truncation is refused. */
    for (u64 n = 0; n < size; ++n) {
        const de_result r = de__input_replay_load_mem(in, file, n, "mem", &rec);
        if (r != DE_ERR_PARSE) {
            EXPECT_EQ(r, DE_ERR_PARSE);
            break;
        }
    }
    /* Every single-bit flip is refused (the checksum, or the header checks). */
    for (u64 i = 0; i < size; ++i) {
        for (u32 bit = 0; bit < 8; ++bit) {
            file[i] ^= (u8)(1u << bit);
            const de_result r = de__input_replay_load_mem(in, file, size, "mem", &rec);
            file[i] ^= (u8)(1u << bit);
            if (r == DE_OK) {
                EXPECT_NE(r, DE_OK);
                de_input_recording_destroy(rec);
                i = size;
                break;
            }
        }
    }
    /* Corrupt content behind a valid checksum: each is a parse error naming the problem. */
    u8 *copy = (u8 *)malloc((size_t)size);
    ASSERT_TRUE(copy != NULL);
    const u32 body = DE__DEIR_HEADER_FIXED + 1u;
    struct {
        u32 offset;
        u8 value;
        de_result want;
    } cases[] = {
        {0, 'X', DE_ERR_PARSE},                              /* magic */
        {4, 2, DE_ERR_VERSION_MISMATCH},                     /* version */
        {6, 1, DE_ERR_PARSE},                                /* flags */
        {20, 0, DE_ERR_PARSE},                               /* action count */
        {32, 99, DE_ERR_PARSE},                              /* tick count */
        {36, 5, DE_ERR_PARSE},                               /* chunk count */
        {16, 0, DE_ERR_PARSE},                               /* tick rate 0 */
        {24, 11, DE_ERR_PARSE},                              /* start tick */
        {DE__DEIR_HEADER_FIXED, 7, DE_ERR_PARSE},            /* action type */
        {body + 8, 0, DE_ERR_PARSE},                         /* chunk record count 0 */
        {body + 12, 0xFF, DE_ERR_PARSE},                     /* chunk byte count */
        {body + DE__DEIR_CHUNK_HEADER, 1, DE_ERR_PARSE},     /* first delta not 0 */
        {body + DE__DEIR_CHUNK_HEADER + 1, 5, DE_ERR_PARSE}, /* changed count > N */
    };
    for (u32 c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        memcpy(copy, file, (size_t)size);
        copy[cases[c].offset] = cases[c].value;
        fix_checksum(copy, size);
        const de_result r = de__input_replay_load_mem(in, copy, size, "mem", &rec);
        if (r != cases[c].want)
            fprintf(stderr, "case %u (offset %u): got %d\n", (unsigned)c, (unsigned)cases[c].offset,
                    (int)r);
        EXPECT_EQ(r, cases[c].want);
        EXPECT_TRUE(rec == NULL);
    }
    /* A truncated file on disk, through de_input_replay_load. */
    mount_dirs();
    ASSERT_EQ(de_file_write_all_atomic("recout:/truncated.deir", file, size - 3u), DE_OK);
    EXPECT_EQ(de_input_replay_load(in, "recout:/truncated.deir", &rec), DE_ERR_PARSE);
    EXPECT_FALSE(de_input_tick_quantization(in));
    free(copy);
    free(file);
    de_input_destroy(in);
}

/* ---- The replay override ------------------------------------------------------------------- */

TEST(record_replay, replay_end_removes_override) {
    u64 size = 0;
    u8 *file = short_file("gameplay", "jump", &size); /* ticks 10 .. 39 */
    ASSERT_TRUE(file != NULL);
    de_input *in = one_button_input("gameplay", "jump");
    ASSERT_TRUE(in != NULL);
    const de_action a = de_action_find(in, de_action_map_find(in, "gameplay"), "jump");
    ASSERT_EQ(de_input_set_tick_quantization(in, true), DE_OK);
    de_input_recording *rec = NULL;
    ASSERT_EQ(de__input_replay_load_mem(in, file, size, "mem", &rec), DE_OK);
    de_input_recording *second = NULL;
    EXPECT_EQ(de__input_replay_load_mem(in, file, size, "mem", &second), DE_ERR_BUSY);
    EXPECT_TRUE(second == NULL);
    struct de__actions *s = de__actions_state(in);
    EXPECT_TRUE(s->tick_override == de__input_replay_override);

    /* Live space is held, but the replay decides: tick 12 is recorded pressed (12 % 3 == 0). */
    input_test_key(in, DE_KEY_SPACE, true);
    input_test_frame(in);
    de__actions_frame_update(in, INPUT_TEST_DT);
    ASSERT_TRUE(de_input_replay_tick(in, rec, 11));
    de__actions_fixed_tick(in, 1);
    de__action_state st;
    ASSERT_TRUE(de__action_tick_state_get(in, a, &st));
    EXPECT_FALSE(st.held); /* recorded: released at tick 11 */
    ASSERT_TRUE(de_input_replay_tick(in, rec, 12));
    de__actions_fixed_tick(in, 2);
    ASSERT_TRUE(de__action_tick_state_get(in, a, &st));
    EXPECT_TRUE(st.held);
    EXPECT_TRUE(st.triggered);
    /* A second FixedUpdate without replay_tick: same state, the trigger is not repeated. */
    de__actions_fixed_tick(in, 3);
    ASSERT_TRUE(de__action_tick_state_get(in, a, &st));
    EXPECT_TRUE(st.held);
    EXPECT_FALSE(st.triggered);
    /* Before the first recorded tick: idle. Seeking backwards and forwards works. */
    ASSERT_TRUE(de_input_replay_tick(in, rec, 3));
    de__actions_fixed_tick(in, 4);
    ASSERT_TRUE(de__action_tick_state_get(in, a, &st));
    EXPECT_FALSE(st.held);
    ASSERT_TRUE(de_input_replay_tick(in, rec, 39));
    ASSERT_TRUE(de_input_replay_tick(in, rec, 15));
    de__actions_fixed_tick(in, 5);
    ASSERT_TRUE(de__action_tick_state_get(in, a, &st));
    EXPECT_TRUE(st.held && st.triggered);

    /* Past the end: false, and the previous override (quantization) is back. */
    EXPECT_FALSE(de_input_replay_tick(in, rec, 40));
    EXPECT_TRUE(s->tick_override == de__input_quantize_override);
    EXPECT_FALSE(de_input_replay_tick(in, rec, 20)); /* ended for good */
    /* Live input reaches the tick view again (space is still held from above). */
    input_test_frame(in);
    de__actions_frame_update(in, INPUT_TEST_DT);
    de__actions_fixed_tick(in, 6);
    ASSERT_TRUE(de__action_tick_state_get(in, a, &st));
    EXPECT_TRUE(st.held);
    de_input_recording_destroy(rec);
    EXPECT_TRUE(s->tick_override == de__input_quantize_override);

    /* Destroying a running replay restores the override too; quantization turned off during the
     * replay stays off afterwards. */
    ASSERT_EQ(de__input_replay_load_mem(in, file, size, "mem", &rec), DE_OK);
    EXPECT_TRUE(de_input_tick_quantization(in));
    ASSERT_EQ(de_input_set_tick_quantization(in, false), DE_OK);
    EXPECT_TRUE(s->tick_override == de__input_replay_override);
    de_input_recording_destroy(rec);
    EXPECT_TRUE(s->tick_override == NULL);
    EXPECT_FALSE(de_input_tick_quantization(in));
    free(file);
    de_input_destroy(in);
}

static void foreign_override(de_input *in, u64 tick, void *user) {
    (void)in;
    (void)tick;
    (void)user;
}

TEST(record_replay, foreign_override_kept) {
    u64 size = 0;
    u8 *file = short_file("gameplay", "jump", &size);
    ASSERT_TRUE(file != NULL);
    de_input *in = one_button_input("gameplay", "jump");
    ASSERT_TRUE(in != NULL);
    de__action_tick_override(in, foreign_override, &size);
    /* Quantization and recording refuse a foreign override; replay saves and restores it. */
    EXPECT_EQ(de_input_set_tick_quantization(in, true), DE_ERR_BUSY);
    EXPECT_EQ(de_input_set_tick_quantization(in, false), DE_OK); /* not quantization: kept */
    EXPECT_TRUE(de__actions_state(in)->tick_override == foreign_override);
    de_input_recording *rec = NULL;
    EXPECT_EQ(de_input_record_begin(in, NULL, &rec), DE_ERR_BUSY);
    ASSERT_EQ(de__input_replay_load_mem(in, file, size, "mem", &rec), DE_OK);
    EXPECT_EQ(de_input_set_tick_quantization(in, true), DE_ERR_BUSY);
    EXPECT_FALSE(de_input_replay_tick(in, rec, 1000));
    struct de__actions *s = de__actions_state(in);
    EXPECT_TRUE(s->tick_override == foreign_override);
    EXPECT_TRUE(s->tick_override_user == (void *)&size);
    de_input_recording_destroy(rec);
    de__action_tick_override(in, NULL, NULL);
    free(file);
    de_input_destroy(in);
}

/* ---- No allocation per tick ---------------------------------------------------------------- */

TEST(record_replay, no_alloc_per_tick) {
    de_tracking_allocator tracker;
    const de_allocator ta = de_tracking_allocator_init(&tracker, de_heap_allocator(), 0);
    de_input *in = input_test_create(&ta);
    ASSERT_TRUE(in != NULL);
    ASSERT_TRUE(input_script_schema(in));
    de_input_record_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.reserve_ticks = 5000; /* several chunks */
    de_input_recording *rec = NULL;
    ASSERT_EQ(de_input_record_begin(in, &rd, &rec), DE_OK);
    /* Warm up the first-use paths (the first frame, pad add). */
    input_script_events(in, 0);
    de__input_begin_frame_dt(in, INPUT_TEST_DT);
    de__actions_frame_update(in, INPUT_TEST_DT);
    const u64 allocs = de_tracking_allocator_stats(&tracker).alloc_count;

    de_alloc_guard g;
    memset(&g, 0, sizeof g);
    g.this_thread_only = true;
    de_alloc_guard_begin(&g);
    for (u32 i = 1; i <= 5000; ++i) {
        input_script_events(in, i);
        de__input_begin_frame_dt(in, INPUT_TEST_DT);
        de__actions_frame_update(in, INPUT_TEST_DT);
        de__actions_fixed_tick(in, i);
        if (de_input_record_tick(in, rec, i) != DE_OK)
            break;
    }
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    EXPECT_EQ(de_tracking_allocator_stats(&tracker).alloc_count, allocs);
    EXPECT_EQ(de_input_recording_tick_count(rec), 5000u);
    EXPECT_GT(rec->chunk_count, 1u);

    /* Replay allocates nothing per tick either (the file is loaded once). */
    const u64 size = de__recording_file_size(rec);
    u8 *file = (u8 *)malloc((size_t)size);
    ASSERT_TRUE(file != NULL);
    de__recording_write_file(rec, file);
    de_input_recording_destroy(rec);
    ASSERT_EQ(de__input_replay_load_mem(in, file, size, "mem", &rec), DE_OK);
    free(file);
    const u64 allocs2 = de_tracking_allocator_stats(&tracker).alloc_count;
    memset(&g, 0, sizeof g);
    g.this_thread_only = true;
    de_alloc_guard_begin(&g);
    for (u32 i = 1; i <= 5000; ++i) {
        (void)de_input_replay_tick(in, rec, i);
        de__actions_fixed_tick(in, i);
        (void)de_input_tick_checksum(in);
    }
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    EXPECT_EQ(de_tracking_allocator_stats(&tracker).alloc_count, allocs2);
    de_input_recording_destroy(rec);
    de_input_destroy(in);
    const de_tracking_stats st = de_tracking_allocator_stats(&tracker);
    EXPECT_EQ(st.free_count, st.alloc_count);
}

/* ---- The checked-in test vector ------------------------------------------------------------ */

TEST(record_replay, golden_recording_checksums) {
    mount_dirs();
    de_input *in = script_input();
    ASSERT_TRUE(in != NULL);
    de_input_recording *rec = NULL;
    ASSERT_EQ(de_input_replay_load(in, "recgold:/input_replay.deir", &rec), DE_OK);
    ASSERT_EQ(de_input_recording_tick_count(rec), TICKS);
    replay_all(in, rec);
    static u64 golden[TICKS + 8u];
    u32 n = 0;
    ASSERT_EQ(de_det_read_checksums(DE_INPUT_GOLDEN_DIR "/input_replay_ticks.checksums", golden,
                                    TICKS + 8u, &n),
              DE_OK);
    ASSERT_EQ(n, TICKS);
    EXPECT_EQ(de_det_report("golden ticks vs replay", golden, g_replay.sums, TICKS),
              DE_DET_NO_DIVERGENCE);
    de_input_recording_destroy(rec);
    de_input_destroy(in);
}
