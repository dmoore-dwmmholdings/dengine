#include "de_test.h"
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include "asset/backend_internal.h"
#include <dengine/de_platform.h>

/* Exercise the production handler/queue with real asset requests and a held backend.
 * Only clock and transport are controlled: no ten-second sleeps or GPU timing assertions. */
static u64 test_now;
static u32 sends;
static bool fail_send;
static char captured[32768];
static size_t captured_size;
static u64 model_clock(void) {
    return test_now;
}
static bool model_send(de__editor *ed, mpack_writer_t *w, u8 kind);
#define de_time_now_ns         model_clock
#define de__ed_send            model_send
#define de__ed_assets_register model_register
#define de__ed_assets_shutdown model_shutdown
#include "host/editor/editor_assets.c"
#undef de_time_now_ns
#undef de__ed_send
#undef de__ed_assets_register
#undef de__ed_assets_shutdown

static bool model_send(de__editor *ed, mpack_writer_t *w, u8 kind) {
    (void)kind;
    captured_size = mpack_writer_buffer_used(w);
    ASSERT_LE(captured_size, sizeof captured);
    memcpy(captured, w->buffer, captured_size);
    ASSERT_EQ(mpack_writer_destroy(w), mpack_ok);
    ++sends;
    if (fail_send) {
        ed->connected = false;
        for (u32 i = 0; i < ed->reset_count; ++i)
            ed->resets[i](ed);
    }
    return !fail_send;
}
typedef struct info_fx {
    de_engine engine;
    de__editor editor;
    u64 arena[8192];
    char output[32768];
    u32 slot, submits;
    de_render_model model;
    de_render_model_node node;
} info_fx;
static void held_submit(void *user, de_asset_system *s, u32 slot, de__load_priority priority) {
    (void)s;
    (void)priority;
    info_fx *f = user;
    f->slot = slot;
    ++f->submits;
}
static void held_cancel(void *user, de_asset_system *s, u32 slot) {
    (void)user;
    (void)s;
    (void)slot;
}
static void held_priority(void *user, de_asset_system *s, u32 slot, de__load_priority priority) {
    (void)user;
    (void)s;
    (void)slot;
    (void)priority;
}
static void info_fx_setup(info_fx *f) {
    test_now = 100;
    sends = 0;
    fail_send = false;
    captured_size = 0;
    ASSERT_EQ(de_log_init(&(de_log_desc){.no_console_sink = true}), DE_OK);
    ASSERT_EQ(de_fs_mount("modelinfo", DE_MODEL_INFO_DATA, DE_MOUNT_READ_ONLY, 0), DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.no_builtin_types = true},
                                     &f->engine.assets),
              DE_OK);
    ASSERT_TRUE(DE_HANDLE_VALID(
        de_asset_type_register(f->engine.assets, &(de_asset_type_desc){.name = "model"})));
    de__load_backend backend = {held_submit, held_cancel, held_priority, f, NULL};
    de__asset_set_backend(f->engine.assets, &backend);
    ASSERT_EQ(de_asset_manifest_load(f->engine.assets, "modelinfo:/assets.toml"), DE_OK);
    f->editor.e = &f->engine;
    f->editor.arena = (u8 *)f->arena;
    f->editor.arena_cap = sizeof f->arena;
    f->editor.out = f->output;
    f->editor.out_cap = sizeof f->output;
    f->editor.connected = f->editor.authed = true;
    f->editor.epoch = 5;
    model_register(&f->editor);
    f->node = (de_render_model_node){.name = "Root", .parent = DE_COOKED_NONE};
    f->model = (de_render_model){.node_count = 1, .nodes = &f->node};
}
static void info_fx_teardown(info_fx *f) {
    model_shutdown(&f->editor);
    if (f->slot) {
        de__slot *slot = de__slot_get(f->engine.assets, f->slot);
        u32 refs = 999;
        de__slot_refs(slot, &refs, NULL);
        EXPECT_EQ(refs, 0u);
        de_atomic_store_ptr(&slot->object, NULL, DE_ATOMIC_RELEASE);
    }
    de_asset_system_destroy(f->engine.assets);
    de_log_shutdown();
}
static void named_query(info_fx *f, u64 request, const char *name) {
    char params[128];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    mpack_start_map(&w, 1);
    mpack_write_cstr(&w, "id");
    mpack_write_cstr(&w, name);
    mpack_finish_map(&w);
    size_t bytes = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    de_ep_request req = {.id = request, .params = {params, bytes}};
    de_ep_arena a = {(u8 *)f->arena, sizeof f->arena, 0};
    f->editor.deferred = false;
    de__ed_respond_begin(&f->editor, &w);
    f->editor.handlers[DE_EP_METHOD_ASSET_MODEL_INFO](&f->editor, &req, &a, &w);
    if (f->editor.deferred)
        ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    else {
        de__ed_respond_limit(&f->editor, &w, request);
        (void)model_send(&f->editor, &w, DE_EP_KIND_RESPONSE);
    }
}
static void query(info_fx *f, u64 request) {
    named_query(f, request, "hero");
}
static void complete(info_fx *f, de_result result) {
    if (result == DE_OK)
        de_atomic_store_ptr(&de__slot_get(f->engine.assets, f->slot)->object, &f->model,
                            DE_ATOMIC_RELEASE);
    de__slot_complete(f->engine.assets, f->slot, result, sizeof f->model);
    de__load_update(f->engine.assets);
}
static void response(u64 request, int error) {
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, captured, captured_size);
    mpack_tree_parse(&tree);
    mpack_node_t root = mpack_tree_root(&tree);
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(root, "id")), request);
    EXPECT_EQ(mpack_node_map_contains_cstr(root, "error"), error != 0);
    if (error)
        EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(mpack_node_map_cstr(root, "error"), "code")),
                  error);
    if (!error) {
        mpack_node_t result = mpack_node_map_cstr(root, "result");
        mpack_node_t nodes = mpack_node_map_cstr(result, "nodes");
        EXPECT_EQ(mpack_node_array_length(nodes), 1u);
        char name[32];
        mpack_node_copy_cstr(mpack_node_map_cstr(mpack_node_array_at(nodes, 0), "name"), name,
                             sizeof name);
        EXPECT_STREQ(name, "Root");
    }
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
}
TEST_F(model_info_async, held_load_does_not_wait_and_serializes_on_completion, info_fx) {
    query(f, 7);
    ASSERT_TRUE(f->editor.deferred);
    ASSERT_EQ(sends, 0u);
    for (u32 frame = 0; frame < 8; ++frame)
        model_info_tick(&f->editor);
    EXPECT_EQ(sends, 0u);
    EXPECT_EQ(f->submits, 1u);
    /* No new 2-second cutoff: previously valid 2..10-second loads still succeed. */
    test_now += 4000000000ULL;
    model_info_tick(&f->editor);
    EXPECT_EQ(sends, 0u);
    complete(f, DE_OK);
    model_info_tick(&f->editor);
    EXPECT_EQ(sends, 1u);
    response(7, false);
    query(f, 8);
    EXPECT_FALSE(f->editor.deferred);
    response(8, false);
}
TEST_F(model_info_async, capacity_deadline_and_failure, info_fx) {
    for (u32 i = 0; i < ED_LOADS; ++i) {
        query(f, i + 1);
        ASSERT_TRUE(f->editor.deferred);
    }
    EXPECT_EQ(f->submits, 1u);
    query(f, 99);
    EXPECT_FALSE(f->editor.deferred);
    response(99, DE_EP_ERR_BUSY);
    test_now += 9999999999ULL;
    model_info_tick(&f->editor);
    EXPECT_EQ(sends, 1u);
    ++test_now;
    model_info_tick(&f->editor);
    EXPECT_EQ(sends, ED_LOADS + 1);
    response(32, true);
}
TEST_F(model_info_async, failed_load_returns_legacy_not_found, info_fx) {
    query(f, 9);
    complete(f, DE_ERR_IO);
    model_info_tick(&f->editor);
    response(9, true);
}
TEST_F(model_info_async, oversized_cold_and_warm_responses_return_busy, info_fx) {
    /* Force the ordinary fixed-buffer overflow with valid metadata, without a large model. */
    static const char long_name[] =
        "A model node name that makes this otherwise valid response exceed the small test "
        "output buffer, while leaving enough room for the ordinary typed Busy error.";
    f->node.name = long_name;
    f->editor.out_cap = 128;
    query(f, 10);
    ASSERT_TRUE(f->editor.deferred);
    complete(f, DE_OK);
    model_info_tick(&f->editor);
    EXPECT_EQ(sends, 1u);
    response(10, DE_EP_ERR_BUSY);
    for (u32 i = 0; i < ED_LOADS; ++i)
        EXPECT_EQ(g_assets.model_info[i].load.id, 0u);
    u32 refs = 999;
    de__slot_refs(de__slot_get(f->engine.assets, f->slot), &refs, NULL);
    EXPECT_EQ(refs, 0u);
    /* The same metadata still takes the synchronous warm path and the shared error handler. */
    query(f, 11);
    EXPECT_FALSE(f->editor.deferred);
    EXPECT_EQ(sends, 2u);
    response(11, DE_EP_ERR_BUSY);
}
TEST_F(model_info_async, epoch_reset_shutdown_and_reentrant_send_release_once, info_fx) {
    query(f, 1);
    ++f->editor.epoch;
    model_info_tick(&f->editor);
    EXPECT_EQ(sends, 0u);
    query(f, 2);
    model_info_reset(&f->editor);
    model_info_reset(&f->editor);
    query(f, 3);
    model_shutdown(&f->editor);
    model_shutdown(&f->editor);
    query(f, 4);
    query(f, 5);
    complete(f, DE_OK);
    fail_send = true;
    model_info_tick(&f->editor);
    EXPECT_EQ(sends, 1u);
    EXPECT_FALSE(f->editor.connected);
    for (u32 i = 0; i < ED_LOADS; ++i)
        EXPECT_EQ(g_assets.model_info[i].load.id, 0u);
}

TEST_F(model_info_async, warm_query_succeeds_with_full_pending_queue, info_fx) {
    named_query(f, 100, "warm");
    complete(f, DE_OK);
    model_info_tick(&f->editor);
    response(100, false);
    const u32 warm_slot = f->slot;
    for (u32 i = 0; i < ED_LOADS; ++i)
        query(f, i + 1);
    const u32 cold_slot = f->slot;
    named_query(f, 101, "warm");
    EXPECT_FALSE(f->editor.deferred);
    response(101, false);
    u32 refs = 999;
    de__slot *warm = de__slot_get(f->engine.assets, warm_slot);
    de__slot_refs(warm, &refs, NULL);
    EXPECT_EQ(refs, 0u);
    de_atomic_store_ptr(&warm->object, NULL, DE_ATOMIC_RELEASE);
    f->slot = cold_slot;
}
