/* DENG-828: input.loadActionMaps and input.monitor.subscribe (DIDE protocol 1.24, DIDE-137;
 * capabilities input.actionMaps and input.monitor), through the real handlers, a real de_input
 * driven by injected events, and the generated protocol readers and writers. */
#include "de_test.h"
#if !defined(DE_SHIPPING)
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include "input/action_internal.h"
#include "input/input_internal.h"
#include <dengine/de_input.h>
#include <stdlib.h>

#define STORAGE  (2u << 20)
#define RESPONSE (256u << 10)
#define DT       (1.0f / 60.0f)

static const char k_maps[] = "[contexts.driving]\n"
                             "maps = [\"gameplay\", \"vehicle\"]\n"
                             "\n"
                             "[contexts.photo]\n"
                             "maps = [\"photo\"]\n"
                             "blocks_lower = false\n"
                             "\n"
                             "[maps.gameplay.actions.jump]\n"
                             "bindings = [{ key = \"space\" }]\n"
                             "\n"
                             "[maps.gameplay.actions.move]\n"
                             "type = \"axis2d\"\n"
                             "bindings = [{ key = \"W\", composite = \"up\" }]\n"
                             "\n"
                             "[maps.gameplay.actions.interact]\n"
                             "interaction = \"hold\"\n"
                             "hold_seconds = 0.5\n"
                             "bindings = [{ key = \"e\" }]\n"
                             "\n"
                             "[maps.gameplay.actions.fire]\n"
                             "interaction = \"tap\"\n"
                             "tap_seconds = 0.2\n"
                             "bindings = [{ key = \"f\" }]\n"
                             "\n"
                             "[maps.vehicle.actions.horn]\n"
                             "interaction = \"release\"\n"
                             "bindings = [{ key = \"h\" }]\n"
                             "\n"
                             "[maps.photo.actions.snap]\n"
                             "bindings = [{ key = \"p\" }]\n"
                             "\n"
                             "[maps.menu.actions.confirm]\n"
                             "bindings = [{ key = \"return\" }]\n";

typedef struct in_fx {
    de_engine engine;
    struct de__host host;
    de__editor editor;
    unsigned char *storage;
    char *response;
    char *params;
    mpack_tree_t tree;
    bool parsed;
    char note[RESPONSE];
    char out[RESPONSE]; /* the editor's notification buffer */
    mpack_tree_t note_tree;
    bool note_parsed;
} in_fx;

static void in_event_key(de_input *in, de_key k, bool down) {
    static u64 t = 1000000;
    de__input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = DE_RAW_KEY;
    ev.time_ns = (t += 1000);
    ev.key.code = (u16)k;
    ev.key.down = down;
    de__input_inject(in, &ev);
}

static void in_fx_setup(in_fx *f) {
    memset(f, 0, sizeof *f);
    de_input_desc d;
    memset(&d, 0, sizeof d);
    ASSERT_EQ(de_input_create(&d, &f->engine.input), DE_OK);
    f->editor.e = &f->engine;
    f->editor.h = &f->host;
    f->editor.note = f->out;
    f->editor.note_cap = sizeof f->out;
    f->editor.mode = DE_EP_WORLD_MODE_EDIT;
    f->storage = (unsigned char *)malloc(STORAGE);
    f->response = (char *)malloc(RESPONSE);
    f->params = (char *)malloc((2u << 20) + 4096u);
    ASSERT_TRUE(f->storage != NULL && f->response != NULL && f->params != NULL);
    de__ed_input_register(&f->editor);
}
static void in_fx_teardown(in_fx *f) {
    if (f->parsed)
        mpack_tree_destroy(&f->tree);
    if (f->note_parsed)
        mpack_tree_destroy(&f->note_tree);
    for (u32 i = 0; i < f->editor.reset_count; ++i)
        f->editor.resets[i](&f->editor);
    de_input_destroy(f->engine.input);
    free(f->storage);
    free(f->response);
    free(f->params);
}

static mpack_node_t call(in_fx *f, de_ep_method m, const void *params) {
    mpack_writer_t w;
    mpack_writer_init(&w, f->params, (2u << 20) + 4096u);
    if (m == DE_EP_METHOD_INPUT_LOAD_ACTION_MAPS)
        de_ep_write_input_load_action_maps_params(
            &w, (const de_ep_input_load_action_maps_params *)params);
    else
        de_ep_write_input_monitor_subscribe_params(
            &w, (const de_ep_input_monitor_subscribe_params *)params);
    const size_t psize = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    const de_ep_request req = {.id = 7, .method = de_ep_cstr("input"), .params = {f->params, psize}};
    ASSERT_TRUE(f->editor.handlers[m] != NULL);
    de_ep_arena a = {f->storage, STORAGE, 0};
    mpack_writer_init(&w, f->response, RESPONSE);
    f->editor.handlers[m](&f->editor, &req, &a, &w);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    if (f->parsed)
        mpack_tree_destroy(&f->tree);
    mpack_tree_init_data(&f->tree, f->response, size);
    mpack_tree_parse(&f->tree);
    f->parsed = true;
    ASSERT_EQ(mpack_tree_error(&f->tree), mpack_ok);
    mpack_node_t root = mpack_tree_root(&f->tree);
    ASSERT_EQ(mpack_node_u64(mpack_node_map_cstr(root, "id")), 7u);
    return root;
}
static mpack_node_t result_of(mpack_node_t root) {
    ASSERT_FALSE(mpack_node_map_contains_cstr(root, "error"));
    return mpack_node_map_cstr(root, "result");
}
static void expect_error(mpack_node_t root, de_ep_error code) {
    EXPECT_FALSE(mpack_node_map_contains_cstr(root, "result"));
    mpack_node_t error = mpack_node_map_cstr(root, "error");
    EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(error, "code")), (i32)code);
}
static bool is_str(mpack_node_t n, const char *s) {
    return mpack_node_strlen(n) == strlen(s) && memcmp(mpack_node_str(n), s, strlen(s)) == 0;
}
static bool ep_is(de_ep_str a, const char *s) {
    return a.len == strlen(s) && memcmp(a.ptr, s, a.len) == 0;
}

static mpack_node_t load(in_fx *f, const char *path, const char *text) {
    de_ep_input_load_action_maps_params q = {.path = de_ep_cstr(path)};
    if (text != NULL) {
        q.has_text = true;
        q.text = de_ep_cstr(text);
    }
    return call(f, DE_EP_METHOD_INPUT_LOAD_ACTION_MAPS, &q);
}
static mpack_node_t subscribe(in_fx *f, bool has_rate, f32 rate) {
    de_ep_input_monitor_subscribe_params q = {.has_rate_hz = has_rate, .rate_hz = rate};
    return call(f, DE_EP_METHOD_INPUT_MONITOR_SUBSCRIBE, &q);
}

/* One game frame: input, action evaluation, then the editor ticks. */
static void frame(in_fx *f) {
    de__input_begin_frame_dt(f->engine.input, DT);
    de__actions_frame_update(f->engine.input, DT);
    ++f->host.loop_frames;
    for (u32 i = 0; i < f->editor.tick_count; ++i)
        f->editor.ticks[i](&f->editor);
}

static const de_ep_input_action_state *action(const de_ep_input_monitor_sample *s,
                                              const char *name) {
    for (u32 i = 0; i < s->actions.count; ++i)
        if (ep_is(s->actions.items[i].name, name))
            return &s->actions.items[i];
    return NULL;
}

static u32 load_maps(in_fx *f) {
    mpack_node_t r = result_of(load(f, "input/game.actions.toml", k_maps));
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "maps")), 4u);
    EXPECT_EQ(mpack_node_u32(mpack_node_map_cstr(r, "actions")), 7u);
    return mpack_node_u32(mpack_node_map_cstr(r, "actions"));
}

static u32 sub_id(in_fx *f, f32 rate) {
    mpack_node_t r = result_of(subscribe(f, true, rate));
    return mpack_node_u32(mpack_node_map_cstr(r, "subscription"));
}

TEST_F(editor_input, advertises_the_capabilities, in_fx) {
    bool maps = false, monitor = false;
    for (u32 i = 0; i < f->editor.cap_count; ++i) {
        maps |= strcmp(f->editor.caps[i], "input.actionMaps") == 0;
        monitor |= strcmp(f->editor.caps[i], "input.monitor") == 0;
    }
    EXPECT_TRUE(maps);
    EXPECT_TRUE(monitor);
    EXPECT_STREQ(DE_EP_PROTOCOL_VERSION, "1.24"); /* pinned dide 406153b1 */
}

/* The DIDE conformance probes at 1.24. */
TEST_F(editor_input, conformance_probes, in_fx) {
    expect_error(subscribe(f, true, 0.0f), DE_EP_ERR_INVALID_PARAMS);
    expect_error(subscribe(f, true, 121.0f), DE_EP_ERR_INVALID_PARAMS);
    const u32 id = mpack_node_u32(mpack_node_map_cstr(result_of(subscribe(f, false, 0)),
                                                      "subscription"));
    EXPECT_TRUE(id >= 0x30000u);
    EXPECT_TRUE(de__ed_input_unsubscribe(id));
    EXPECT_FALSE(de__ed_input_unsubscribe(id));
    expect_error(load(f, "dide-conformance.toml", "[maps"), DE_EP_ERR_INVALID_PARAMS);
    expect_error(load(f, "dide-conformance/no-such-input.toml", NULL), DE_EP_ERR_NOT_FOUND);
}

TEST_F(editor_input, load_rejects_bad_requests, in_fx) {
    expect_error(load(f, "/abs/input.toml", ""), DE_EP_ERR_INVALID_PARAMS);
    expect_error(load(f, "a/../input.toml", ""), DE_EP_ERR_INVALID_PARAMS);
    expect_error(load(f, "bad.toml", "[maps.m.actions.a]\ntype = \"axis9d\"\n"),
                 DE_EP_ERR_INVALID_PARAMS);
    mpack_node_t root = load(f, "bad.toml", "[maps.m.actions.a]\ninteraction = \"squeeze\"\n");
    expect_error(root, DE_EP_ERR_INVALID_PARAMS);
    mpack_node_t msg = mpack_node_map_cstr(mpack_node_map_cstr(root, "error"), "message");
    EXPECT_TRUE(mpack_node_strlen(msg) > 0); /* the parser's message */
    const size_t n = (1u << 20) + 1u;
    char *big = (char *)malloc(n + 1);
    ASSERT_TRUE(big != NULL);
    memset(big, ' ', n);
    big[n] = '\0';
    expect_error(load(f, "big.toml", big), DE_EP_ERR_INVALID_PARAMS);
    free(big);
    EXPECT_TRUE(de__actions_state_const(f->engine.input) == NULL ||
                de__actions_state_const(f->engine.input)->action_count == 0);
}

TEST_F(editor_input, load_counts_maps_and_actions_in_every_mode, in_fx) {
    f->editor.mode = DE_EP_WORLD_MODE_PAUSED;
    load_maps(f);
    f->editor.mode = DE_EP_WORLD_MODE_PLAY;
    load_maps(f); /* a reload of the same file */
    EXPECT_EQ(de__actions_state_const(f->engine.input)->action_count, 7u);
    EXPECT_TRUE(DE_HANDLE_VALID(de_action_map_find(f->engine.input, "vehicle")));
}

TEST_F(editor_input, rate_limits_and_play_only, in_fx) {
    load_maps(f);
    const u32 id = sub_id(f, 120.0f);
    frame(f);
    EXPECT_EQ(de__ed_input_samples(id), 0u); /* Edit */
    f->editor.mode = DE_EP_WORLD_MODE_PAUSED;
    frame(f);
    EXPECT_EQ(de__ed_input_samples(id), 0u);
    f->editor.mode = DE_EP_WORLD_MODE_PLAY;
    frame(f);
    EXPECT_EQ(de__ed_input_samples(id), 1u); /* the first eligible tick */
    f->editor.mode = DE_EP_WORLD_MODE_SIMULATE;
    const u32 slow = sub_id(f, 1.0f);
    frame(f);
    frame(f);
    EXPECT_EQ(de__ed_input_samples(slow), 1u); /* 1 Hz: one sample in two quick frames */
    for (u32 i = 0; i < f->editor.reset_count; ++i)
        f->editor.resets[i](&f->editor);
    EXPECT_FALSE(de__ed_input_unsubscribe(id)); /* a reset ends them */
}

TEST_F(editor_input, sample_has_devices_actions_and_contexts, in_fx) {
    load_maps(f);
    de_input *in = f->engine.input;
    de_action_map_push(in, de_action_map_find(in, "menu"));
    ASSERT_EQ(de_input_context_push(in, de_name_from("driving")), DE_OK);
    ASSERT_EQ(de_input_context_push(in, de_name_from("photo")), DE_OK);
    f->editor.mode = DE_EP_WORLD_MODE_PLAY;
    const u32 id = sub_id(f, 1.0f); /* the tick sends once, then not for 1 s */
    in_event_key(in, DE_KEY_W, true);
    in_event_key(in, DE_KEY_SPACE, true);
    frame(f);
    de_ep_input_monitor_sample s;
    ASSERT_TRUE(de__ed_input_sample(&f->editor, id, &s));
    EXPECT_EQ(s.subscription, id);
    EXPECT_EQ(s.keys.count, 2u);
    EXPECT_EQ(s.actions.count, 7u);
    const de_ep_input_action_state *jump = action(&s, "jump");
    ASSERT_TRUE(jump != NULL);
    EXPECT_TRUE(ep_is(jump->map, "gameplay"));
    EXPECT_TRUE(ep_is(jump->type, "button"));
    EXPECT_TRUE(ep_is(jump->interaction, "press"));
    EXPECT_EQ(jump->phase, DE_EP_INPUT_ACTION_PHASE_PERFORMED);
    EXPECT_FALSE(jump->has_progress);
    EXPECT_EQ(jump->value.count, 1u);
    const de_ep_input_action_state *move = action(&s, "move");
    ASSERT_TRUE(move != NULL);
    EXPECT_EQ(move->value.count, 2u);
    EXPECT_FLOAT_EQ(move->value.items[1], 1.0f, 1e-5f);
    /* Top first: photo (passthrough), driving (gameplay under vehicle), then the menu map. */
    ASSERT_EQ(s.contexts.count, 3u);
    EXPECT_TRUE(ep_is(s.contexts.items[0].name, "photo"));
    EXPECT_FALSE(s.contexts.items[0].blocks_lower);
    EXPECT_TRUE(ep_is(s.contexts.items[1].name, "driving"));
    ASSERT_EQ(s.contexts.items[1].maps.count, 2u);
    EXPECT_TRUE(ep_is(s.contexts.items[1].maps.items[0], "gameplay"));
    EXPECT_TRUE(ep_is(s.contexts.items[1].maps.items[1], "vehicle"));
    EXPECT_TRUE(s.contexts.items[1].blocks_lower);
    EXPECT_TRUE(ep_is(s.contexts.items[2].name, "menu"));
    EXPECT_TRUE(s.contexts.items[2].blocks_lower);

    /* It encodes as an input.monitor notification. */
    mpack_writer_t w;
    mpack_writer_init(&w, f->note, sizeof f->note);
    de_ep_write_notify_input_monitor(&w, 1, &s);
    const size_t n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_init_data(&f->note_tree, f->note, n);
    mpack_tree_parse(&f->note_tree);
    f->note_parsed = true;
    ASSERT_EQ(mpack_tree_error(&f->note_tree), mpack_ok);
    EXPECT_TRUE(is_str(mpack_node_map_cstr(mpack_tree_root(&f->note_tree), "topic"),
                       "input.monitor"));
}

TEST_F(editor_input, hold_progress_and_cancel, in_fx) {
    load_maps(f);
    de_input *in = f->engine.input;
    ASSERT_EQ(de_input_context_push(in, de_name_from("driving")), DE_OK);
    f->editor.mode = DE_EP_WORLD_MODE_PLAY;
    const u32 id = sub_id(f, 1.0f); /* the tick sends once, then not for 1 s */
    frame(f);
    in_event_key(in, DE_KEY_E, true);
    for (u32 i = 0; i < 16; ++i) /* about 0.25 s of the 0.5 s hold */
        frame(f);
    de_ep_input_monitor_sample s;
    ASSERT_TRUE(de__ed_input_sample(&f->editor, id, &s));
    const de_ep_input_action_state *a = action(&s, "interact");
    ASSERT_TRUE(a != NULL);
    EXPECT_EQ(a->phase, DE_EP_INPUT_ACTION_PHASE_STARTED);
    EXPECT_TRUE(a->has_progress);
    EXPECT_TRUE(a->progress > 0.3f && a->progress < 0.7f);
    in_event_key(in, DE_KEY_E, false);
    frame(f);
    frame(f); /* the cancel frame is past, but held for this subscription's next sample */
    ASSERT_TRUE(de__ed_input_sample(&f->editor, id, &s));
    EXPECT_EQ(action(&s, "interact")->phase, DE_EP_INPUT_ACTION_PHASE_CANCELED);
    ASSERT_TRUE(de__ed_input_sample(&f->editor, id, &s));
    EXPECT_EQ(action(&s, "interact")->phase, DE_EP_INPUT_ACTION_PHASE_IDLE);

    in_event_key(in, DE_KEY_E, true);
    for (u32 i = 0; i < 40; ++i)
        frame(f);
    ASSERT_TRUE(de__ed_input_sample(&f->editor, id, &s));
    EXPECT_EQ(action(&s, "interact")->phase, DE_EP_INPUT_ACTION_PHASE_PERFORMED);
    EXPECT_FLOAT_EQ(action(&s, "interact")->progress, 1.0f, 1e-6f);
    in_event_key(in, DE_KEY_E, false);
    frame(f);
    ASSERT_TRUE(de__ed_input_sample(&f->editor, id, &s));
    EXPECT_EQ(action(&s, "interact")->phase, DE_EP_INPUT_ACTION_PHASE_IDLE); /* no cancel */
}

TEST_F(editor_input, tap_and_release_pulses_are_held, in_fx) {
    load_maps(f);
    de_input *in = f->engine.input;
    ASSERT_EQ(de_input_context_push(in, de_name_from("driving")), DE_OK);
    f->editor.mode = DE_EP_WORLD_MODE_PLAY;
    const u32 id = sub_id(f, 1.0f); /* the tick sends once, then not for 1 s */
    frame(f);
    in_event_key(in, DE_KEY_F, true);
    in_event_key(in, DE_KEY_H, true);
    frame(f);
    de_ep_input_monitor_sample s;
    ASSERT_TRUE(de__ed_input_sample(&f->editor, id, &s));
    EXPECT_EQ(action(&s, "fire")->phase, DE_EP_INPUT_ACTION_PHASE_STARTED);
    EXPECT_EQ(action(&s, "horn")->phase, DE_EP_INPUT_ACTION_PHASE_STARTED);
    in_event_key(in, DE_KEY_F, false);
    in_event_key(in, DE_KEY_H, false);
    frame(f);
    frame(f);
    frame(f);
    ASSERT_TRUE(de__ed_input_sample(&f->editor, id, &s));
    EXPECT_EQ(action(&s, "fire")->phase, DE_EP_INPUT_ACTION_PHASE_PERFORMED);
    EXPECT_EQ(action(&s, "horn")->phase, DE_EP_INPUT_ACTION_PHASE_PERFORMED);
    ASSERT_TRUE(de__ed_input_sample(&f->editor, id, &s));
    EXPECT_EQ(action(&s, "fire")->phase, DE_EP_INPUT_ACTION_PHASE_IDLE);

    /* A press longer than tap_seconds is canceled. */
    in_event_key(in, DE_KEY_F, true);
    for (u32 i = 0; i < 30; ++i)
        frame(f);
    in_event_key(in, DE_KEY_F, false);
    frame(f);
    ASSERT_TRUE(de__ed_input_sample(&f->editor, id, &s));
    EXPECT_EQ(action(&s, "fire")->phase, DE_EP_INPUT_ACTION_PHASE_CANCELED);
}

#endif /* !DE_SHIPPING */
