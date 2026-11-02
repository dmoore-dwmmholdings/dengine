#include "de_test.h"
#if !defined(DE_SHIPPING)
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include <dengine/de_asset.h>
#include <dengine/de_ecs.h>
#include <dengine/de_math.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>
#include <math.h>
#include <stdio.h>

/* Actual editor handlers and CPU gizmo input; no renderer, window or transport. */
typedef struct safety_fx {
    de_engine engine;
    de__host host;
    de__editor ed;
    de_builtin_component_ids ids;
    de_task_system *tasks;
    unsigned char arena[65536];
    char note[32768];
} safety_fx;

static void safety_fx_setup(safety_fx *f) {
    memset(f, 0, sizeof *f);
    ASSERT_EQ(de_log_init(&(de_log_desc){.no_console_sink = true}), DE_OK);
    ASSERT_EQ(de_type_registry_create(NULL, &f->engine.types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->engine.types}, &f->engine.world), DE_OK);
    f->ids = de_builtin_components(f->engine.world);
    f->ed.e = &f->engine;
    f->ed.h = &f->host;
    f->ed.mode = DE_EP_WORLD_MODE_EDIT;
    f->ed.arena = f->arena;
    f->ed.arena_cap = sizeof f->arena;
    f->ed.note = f->note;
    f->ed.note_cap = sizeof f->note;
}
static void safety_fx_teardown(safety_fx *f) {
    for (u32 i = 0; i < f->ed.reset_count; ++i)
        f->ed.resets[i](&f->ed);
    de_world_destroy(f->engine.world);
    if (f->engine.assets)
        de_asset_system_destroy(f->engine.assets);
    if (f->tasks)
        de_task_system_destroy(f->tasks);
    de_type_registry_destroy(f->engine.types);
    de_log_shutdown();
}
typedef struct SnapshotNested {
    i32 number;
    bool enabled;
} SnapshotNested;
typedef struct THealth {
    f32 current, max;
    char label[24];
    de_vec3 offset;
    SnapshotNested nested;
} THealth;
#if DE_HAS_COOKER
typedef struct TTarget {
    de_entity target;
    u32 texture;
    f32 speed;
} TTarget;
#endif
static const de_field_info health_fields[] = {
    DE_FIELD(THealth, current, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD(THealth, max, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD(THealth, label, DE_FK_STRING, DE_FIELD_DEFAULT),
    DE_FIELD(THealth, offset, DE_FK_VEC3, DE_FIELD_DEFAULT),
    DE_FIELD_EX(THealth, nested, DE_FK_STRUCT, DE_FIELD_DEFAULT, {0, 0, 0, 0, "SnapshotNested", 0}),
};
static const de_field_info nested_fields[] = {
    DE_FIELD(SnapshotNested, number, DE_FK_I32, DE_FIELD_DEFAULT),
    DE_FIELD(SnapshotNested, enabled, DE_FK_BOOL, DE_FIELD_DEFAULT),
};
static de_component register_health(safety_fx *f) {
    ASSERT_TRUE(DE_HANDLE_VALID(
        de_type_register(f->engine.types, &DE_TYPE_DESC(SnapshotNested, 1, nested_fields))));
    ASSERT_TRUE(DE_HANDLE_VALID(
        de_type_register(f->engine.types, &DE_TYPE_DESC(THealth, 1, health_fields))));
    const de_component c = de_component_register(
        f->engine.world, de_type_find(f->engine.types, "THealth"), DE_COMP_SERIALIZE);
    ASSERT_TRUE(DE_HANDLE_VALID(c));
    return c;
}
#if DE_HAS_COOKER
static const de_field_info target_fields[] = {
    DE_FIELD(TTarget, target, DE_FK_ENTITY, DE_FIELD_DEFAULT),
    DE_FIELD_EX(TTarget, texture, DE_FK_ASSET, DE_FIELD_DEFAULT | DE_FIELD_ASSET_REF,
                {0, 0, 0, 0, "blob", 0}),
    DE_FIELD(TTarget, speed, DE_FK_F32, DE_FIELD_DEFAULT),
};
#endif
static mpack_node_t request(safety_fx *f, de_ep_method method, de_ep_raw params, mpack_tree_t *tree,
                            char response[2048]) {
    const de_ep_request req = {
        .id = 57, .method = de_ep_cstr(de_ep_method_name(method)), .params = params};
    de_ep_arena arena = {f->arena, sizeof f->arena, 0};
    mpack_writer_t w;
    mpack_writer_init(&w, response, 2048);
    ASSERT_TRUE(f->ed.handlers[method] != NULL);
    f->ed.handlers[method](&f->ed, &req, &arena, &w);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_init_data(tree, response, size);
    mpack_tree_parse(tree);
    mpack_node_t root = mpack_tree_root(tree);
    ASSERT_EQ(mpack_node_u64(mpack_node_map_cstr(root, "id")), 57u);
    EXPECT_FALSE(f->ed.deferred);
    return root;
}
TEST(editor_raw, map_entries_preserve_complete_scalar_string_and_nested_encodings) {
    static const de_ep_raw values[] = {
        {"\x2a", 1},
        {"\xf0", 1},
        {"\xc3", 1},
        {"\xc0", 1},
        {"\xca\x3f\x80\x00\x00", 5},
        {"\xcb\x3f\xf0\x00\x00\x00\x00\x00\x00", 9},
        {"\xa3"
         "abc",
         4},
        {"\xc4\x02\x01\x02", 4},
#if MPACK_EXTENSIONS
        {"\xd4\x01\x2a", 3},
#endif
        {"\x93\xc3\xa1"
         "x"
         "\x81\xa1"
         "n"
         "\x7f",
         8},
    };
    char data[512];
    mpack_writer_t w;
    mpack_writer_init(&w, data, sizeof data);
    const u32 count = (u32)(sizeof values / sizeof values[0]);
    mpack_start_map(&w, count);
    for (u32 i = 0; i < count; ++i) {
        mpack_write_cstr(&w, "key");
        mpack_write_object_bytes(&w, values[i].ptr, values[i].len);
    }
    mpack_finish_map(&w);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_reader_t r;
    mpack_reader_init_data(&r, data, size);
    ASSERT_EQ(mpack_expect_map(&r), count);
    for (u32 i = 0; i < count; ++i) {
        de_ep_kv_raw field;
        const bool read = de__ed_raw_map_entry(&r, &field);
        if (!read)
            printf("entry %u: %s\n", i, mpack_error_to_string(mpack_reader_error(&r)));
        ASSERT_TRUE(read);
        ASSERT_EQ(field.key.len, 3u);
        EXPECT_MEMEQ(field.key.ptr, "key", 3);
        ASSERT_EQ(field.value.len, values[i].len);
        EXPECT_MEMEQ(field.value.ptr, values[i].ptr, values[i].len);
        EXPECT_TRUE(field.value.ptr >= data && field.value.ptr + field.value.len <= data + size);
    }
    mpack_done_map(&r);
    EXPECT_EQ(mpack_reader_remaining(&r, NULL), 0u);
    EXPECT_EQ(mpack_reader_destroy(&r), mpack_ok);
}
TEST(editor_raw, truncated_or_non_string_entries_clear_outputs_and_fail) {
    static const char data[] = "\x81\xa1"
                               "x"
                               "\x92\x81\xa1"
                               "y"
                               "\xa3"
                               "abc"
                               "\xc3";
    for (size_t size = 1; size < sizeof data - 1; ++size) {
        mpack_reader_t r;
        mpack_reader_init_data(&r, data, size);
        ASSERT_EQ(mpack_expect_map(&r), 1u);
        de_ep_kv_raw field = {.key = {"old", 3}, .value = {"old", 3}};
        EXPECT_FALSE(de__ed_raw_map_entry(&r, &field));
        EXPECT_TRUE(field.key.ptr == NULL && field.value.ptr == NULL);
        EXPECT_EQ(field.key.len, 0u);
        EXPECT_EQ(field.value.len, 0u);
        EXPECT_NE(mpack_reader_destroy(&r), mpack_ok);
    }
    mpack_reader_t r;
    mpack_reader_init_data(&r, "\x81\x01\x2a", 3);
    ASSERT_EQ(mpack_expect_map(&r), 1u);
    de_ep_kv_raw field;
    EXPECT_FALSE(de__ed_raw_map_entry(&r, &field));
    EXPECT_EQ(mpack_reader_destroy(&r), mpack_error_type);
#if !MPACK_EXTENSIONS
    /* The vendored protocol intentionally disables MessagePack extension types. */
    mpack_reader_init_data(&r,
                           "\x81\xa1"
                           "x"
                           "\xd4\x01\x2a",
                           6);
    ASSERT_EQ(mpack_expect_map(&r), 1u);
    EXPECT_FALSE(de__ed_raw_map_entry(&r, &field));
    EXPECT_TRUE(field.value.ptr == NULL);
    EXPECT_EQ(mpack_reader_destroy(&r), mpack_error_unsupported);
#endif
}
static void transaction(safety_fx *f, de_ep_method method, u32 tx) {
    char params[64], response[2048];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    const de_ep_tx_params q = {.tx = tx};
    de_ep_write_tx_params(&w, &q);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_t tree;
    mpack_node_t root = request(f, method, (de_ep_raw){params, size}, &tree, response);
    EXPECT_TRUE(mpack_node_map_contains_cstr(root, "result"));
    EXPECT_FALSE(mpack_node_map_contains_cstr(root, "error"));
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
}
TEST_F(editor_snapshots, despawn_undo_redo_preserves_complete_component_values, safety_fx) {
    const de_component health = register_health(f);
    const de_entity e = de_entity_spawn(f->engine.world);
    THealth *h = de_entity_add(f->engine.world, e, health);
    ASSERT_TRUE(h != NULL);
    h->current = 50;
    h->max = 100;
    memcpy(h->label, "persistent", sizeof "persistent");
    h->offset = (de_vec3){1, 2, 3};
    h->nested = (SnapshotNested){9, true};
    de__ed_edit_register(&f->ed);
    char params[512], response[2048];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    const de_ep_edit_begin_params begin = {.label = de_ep_cstr("despawn"), .scene = 1};
    de_ep_write_edit_begin_params(&w, &begin);
    size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_t tree;
    mpack_node_t root =
        request(f, DE_EP_METHOD_EDIT_BEGIN, (de_ep_raw){params, size}, &tree, response);
    ASSERT_FALSE(mpack_node_map_contains_cstr(root, "error"));
    const u32 tx = mpack_node_u32(mpack_node_map_cstr(mpack_node_map_cstr(root, "result"), "tx"));
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    de_ep_edit_op op = {.op = de_ep_cstr("despawn"), .has_entity = true, .entity = e.bits};
    const de_ep_edit_apply_params apply = {.tx = tx, .ops = {&op, 1}};
    mpack_writer_init(&w, params, sizeof params);
    de_ep_write_edit_apply_params(&w, &apply);
    size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    root = request(f, DE_EP_METHOD_EDIT_APPLY, (de_ep_raw){params, size}, &tree, response);
    EXPECT_FALSE(mpack_node_map_contains_cstr(root, "error"));
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    EXPECT_FALSE(de_entity_alive(f->engine.world, e));
    transaction(f, DE_EP_METHOD_EDIT_COMMIT, tx);
    for (u32 i = 0; i < 2; ++i) {
        transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
        ASSERT_TRUE(de_entity_alive(f->engine.world, e));
        h = de_entity_get(f->engine.world, e, health);
        ASSERT_TRUE(h != NULL);
        EXPECT_FLOAT_EQ(h->current, 50, 0);
        EXPECT_FLOAT_EQ(h->max, 100, 0);
        EXPECT_STREQ(h->label, "persistent");
        EXPECT_FLOAT_EQ(h->offset.x, 1, 0);
        EXPECT_FLOAT_EQ(h->offset.y, 2, 0);
        EXPECT_FLOAT_EQ(h->offset.z, 3, 0);
        EXPECT_EQ(h->nested.number, 9);
        EXPECT_TRUE(h->nested.enabled);
        transaction(f, DE_EP_METHOD_EDIT_REDO, tx);
        EXPECT_FALSE(de_entity_alive(f->engine.world, e));
    }
}
#if DE_HAS_COOKER
static void revert(safety_fx *f, de_entity e, const char *path, bool found) {
    char params[256], response[2048];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    const de_ep_override_path_params q = {.entity = e.bits, .path = de_ep_cstr(path)};
    de_ep_write_override_path_params(&w, &q);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_t tree;
    mpack_node_t root =
        request(f, DE_EP_METHOD_PREFAB_REVERT_OVERRIDE, (de_ep_raw){params, size}, &tree, response);
    EXPECT_EQ(mpack_node_map_contains_cstr(root, "result"), found);
    EXPECT_EQ(mpack_node_map_contains_cstr(root, "error"), !found);
    if (found && mpack_node_map_contains_cstr(root, "error")) {
        mpack_node_t error = mpack_node_map_cstr(root, "error");
        mpack_node_t message = mpack_node_map_cstr(error, "message");
        printf("revert error %d: %.*s\n", mpack_node_i32(mpack_node_map_cstr(error, "code")),
               (int)mpack_node_strlen(message), mpack_node_str(message));
    }
    if (!found)
        EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(mpack_node_map_cstr(root, "error"), "code")),
                  DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
}
TEST_F(editor_safety, prefab_revert_restores_only_the_field_and_rejects_missing_data, safety_fx) {
    const de_component health = register_health(f);
    ASSERT_TRUE(DE_HANDLE_VALID(
        de_type_register(f->engine.types, &DE_TYPE_DESC(TTarget, 1, target_fields))));
    ASSERT_TRUE(DE_HANDLE_VALID(de_component_register(
        f->engine.world, de_type_find(f->engine.types, "TTarget"), DE_COMP_SERIALIZE)));
    ASSERT_EQ(de_fs_mount("safe676", DE_EDITOR_SAFETY_DATA_DIR, 0, 0), DE_OK);
    de_fp_env_init_thread();
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 1}, &f->tasks), DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.tasks = f->tasks,
                                                             .types = f->engine.types,
                                                             .io_threads = 1,
                                                             .cache_dir = "nocache676:/cache"},
                                     &f->engine.assets),
              DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->engine.assets, "safe676:/assets.toml"), DE_OK);
    de__ed_scene_register(&f->ed);
    char params[256], response[2048];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    const de_ep_prefab_instantiate_params q = {.asset = de_ep_cstr("props/crate"), .scene = 1};
    de_ep_write_prefab_instantiate_params(&w, &q);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_t tree;
    mpack_node_t root =
        request(f, DE_EP_METHOD_PREFAB_INSTANTIATE, (de_ep_raw){params, size}, &tree, response);
    ASSERT_FALSE(mpack_node_map_contains_cstr(root, "error"));
    const de_entity e = {
        mpack_node_u64(mpack_node_map_cstr(mpack_node_map_cstr(root, "result"), "entity"))};
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    ASSERT_TRUE(de_entity_alive(f->engine.world, e));
    THealth *h = de_entity_get(f->engine.world, e, health);
    ASSERT_TRUE(h != NULL);
    h->current = 7;
    h->max = 13;
    revert(f, e, "THealth.current", true);
    EXPECT_FLOAT_EQ(h->current, 50, 0);
    EXPECT_FLOAT_EQ(h->max, 13, 0);
    memcpy(h->label, "changed", sizeof "changed");
    h->offset = (de_vec3){1, 2, 3};
    h->nested = (SnapshotNested){17, true};
    revert(f, e, "THealth.label", true);
    EXPECT_STREQ(h->label, "");
    revert(f, e, "THealth.offset", true);
    EXPECT_FLOAT_EQ(h->offset.x, 0, 0);
    EXPECT_FLOAT_EQ(h->offset.y, 0, 0);
    EXPECT_FLOAT_EQ(h->offset.z, 0, 0);
    revert(f, e, "THealth.nested", true);
    EXPECT_EQ(h->nested.number, 0);
    EXPECT_FALSE(h->nested.enabled);
    EXPECT_FLOAT_EQ(h->current, 50, 0);
    EXPECT_FLOAT_EQ(h->max, 13, 0);
    revert(f, e, "THealth.missing", false);
    de_entity_remove(f->engine.world, e, health);
    revert(f, e, "THealth.current", false);
    h = de_entity_add(f->engine.world, e, health);
    ASSERT_TRUE(h != NULL);
    h->current = 9;
    h->max = 17;
    revert(f, e, "THealth.current", true);
    EXPECT_FLOAT_EQ(h->current, 50, 0);
    EXPECT_FLOAT_EQ(h->max, 17, 0);
}
#endif

#endif
