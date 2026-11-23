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
    /* Borrowing cannot retain a view into a refillable reader's mutable buffer. */
    char buffered[] = "\x81\xa1x\x2a";
    mpack_reader_init(&r, buffered, sizeof buffered - 1, sizeof buffered - 1);
    ASSERT_EQ(mpack_expect_map(&r), 1u);
    field = (de_ep_kv_raw){.key = {"old", 3}, .value = {"old", 3}};
    EXPECT_FALSE(de__ed_raw_map_entry(&r, &field));
    EXPECT_TRUE(field.key.ptr == NULL && field.value.ptr == NULL);
    EXPECT_EQ(field.key.len, 0u);
    EXPECT_EQ(field.value.len, 0u);
    EXPECT_EQ(mpack_reader_destroy(&r), mpack_error_unsupported);
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

/* ---------- DENG-571: prefab overrides with variants and nesting (protocol 1.18) ---------- */

typedef struct pf_world {
    de_component health, target;
} pf_world;

static pf_world prefab_world(safety_fx *f) {
    pf_world p;
    p.health = register_health(f);
    ASSERT_TRUE(DE_HANDLE_VALID(
        de_type_register(f->engine.types, &DE_TYPE_DESC(TTarget, 1, target_fields))));
    p.target = de_component_register(f->engine.world, de_type_find(f->engine.types, "TTarget"),
                                     DE_COMP_SERIALIZE);
    ASSERT_TRUE(DE_HANDLE_VALID(p.target));
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
    de__ed_edit_register(&f->ed);
    return p;
}

static de_entity spawn_prefab(safety_fx *f, const char *asset) {
    char params[256], response[2048];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    const de_ep_prefab_instantiate_params q = {.asset = de_ep_cstr(asset), .scene = 1};
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
    return e;
}

static de_entity first_child(safety_fx *f, de_entity e) {
    return de_entity_children_first(f->engine.world, e);
}

/* One prefab call with {entity, path?, target?, revision?, tx?}. The response is parsed into
 * `tree` over `response`; the caller destroys the tree. */
typedef struct pf_call {
    const char *path, *target;
    bool has_revision;
    u64 revision;
    u32 tx;
} pf_call;

static mpack_node_t prefab_call(safety_fx *f, de_ep_method method, de_entity e, pf_call c,
                                mpack_tree_t *tree, char response[2048]) {
    static char params[512];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    mpack_build_map(&w);
    mpack_write_cstr(&w, "entity");
    mpack_write_u64(&w, e.bits);
    if (c.path != NULL) {
        mpack_write_cstr(&w, "path");
        mpack_write_cstr(&w, c.path);
    }
    if (c.target != NULL) {
        mpack_write_cstr(&w, "target");
        mpack_write_cstr(&w, c.target);
    }
    if (c.has_revision) {
        mpack_write_cstr(&w, "revision");
        mpack_write_u64(&w, c.revision);
    }
    if (c.tx != 0) {
        mpack_write_cstr(&w, "tx");
        mpack_write_u32(&w, c.tx);
    }
    mpack_complete_map(&w);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    return request(f, method, (de_ep_raw){params, size}, tree, response);
}

/* Applies or reverts and expects success; returns the transaction id. */
static u32 prefab_ok(safety_fx *f, de_ep_method method, de_entity e, pf_call c) {
    char response[2048];
    mpack_tree_t tree;
    mpack_node_t root = prefab_call(f, method, e, c, &tree, response);
    if (mpack_node_map_contains_cstr(root, "error")) {
        mpack_node_t m = mpack_node_map_cstr(mpack_node_map_cstr(root, "error"), "message");
        printf("prefab error: %.*s\n", (int)mpack_node_strlen(m), mpack_node_str(m));
    }
    ASSERT_TRUE(mpack_node_map_contains_cstr(root, "result"));
    const u32 tx = mpack_node_u32(mpack_node_map_cstr(mpack_node_map_cstr(root, "result"), "tx"));
    EXPECT_TRUE(tx != 0);
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    return tx;
}

/* Expects a structured error: code and data.reason. */
static void prefab_fails(safety_fx *f, de_ep_method method, de_entity e, pf_call c,
                         de_ep_error code, const char *reason) {
    char response[2048];
    mpack_tree_t tree;
    mpack_node_t root = prefab_call(f, method, e, c, &tree, response);
    ASSERT_TRUE(mpack_node_map_contains_cstr(root, "error"));
    mpack_node_t err = mpack_node_map_cstr(root, "error");
    EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(err, "code")), (i32)code);
    mpack_node_t data = mpack_node_map_cstr(err, "data");
    mpack_node_t r = mpack_node_map_cstr(data, "reason");
    EXPECT_EQ(mpack_node_strlen(r), strlen(reason));
    EXPECT_TRUE(memcmp(mpack_node_str(r), reason, strlen(reason)) == 0);
    EXPECT_TRUE(mpack_node_map_contains_cstr(data, "revision"));
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
}

static bool node_streq(mpack_node_t n, const char *s) {
    return mpack_node_type(n) == mpack_type_str && mpack_node_strlen(n) == strlen(s) &&
           memcmp(mpack_node_str(n), s, strlen(s)) == 0;
}

/* The overrides of `e`'s subtree: how many, and the entry for (entity, path) if any. */
typedef struct pf_found {
    u32 count;
    bool found;
    char source[64];
    char chain[4][64];
    u64 ids[4];
    u32 levels;
    u64 revision;
} pf_found;

static pf_found overrides_of(safety_fx *f, de_entity e, de_entity entity, const char *path) {
    pf_found out;
    memset(&out, 0, sizeof out);
    char response[2048];
    mpack_tree_t tree;
    mpack_node_t root = prefab_call(f, DE_EP_METHOD_PREFAB_OVERRIDES, e, (pf_call){0}, &tree,
                                    response);
    ASSERT_TRUE(mpack_node_map_contains_cstr(root, "result"));
    mpack_node_t res = mpack_node_map_cstr(root, "result");
    if (mpack_node_map_contains_cstr(res, "revision"))
        out.revision = mpack_node_u64(mpack_node_map_cstr(res, "revision"));
    mpack_node_t list = mpack_node_map_cstr(res, "overrides");
    out.count = (u32)mpack_node_array_length(list);
    for (u32 i = 0; i < out.count; ++i) {
        mpack_node_t o = mpack_node_array_at(list, i);
        if (!mpack_node_map_contains_cstr(o, "entity") ||
            mpack_node_u64(mpack_node_map_cstr(o, "entity")) != entity.bits ||
            !node_streq(mpack_node_map_cstr(o, "path"), path))
            continue;
        out.found = true;
        mpack_node_t s = mpack_node_map_cstr(o, "source");
        snprintf(out.source, sizeof out.source, "%.*s", (int)mpack_node_strlen(s),
                 mpack_node_str(s));
        mpack_node_t chain = mpack_node_map_cstr(o, "chain");
        mpack_node_t ids = mpack_node_map_cstr(o, "ids");
        out.levels = (u32)mpack_node_array_length(chain);
        for (u32 k = 0; k < out.levels && k < 4; ++k) {
            mpack_node_t c = mpack_node_array_at(chain, k);
            snprintf(out.chain[k], sizeof out.chain[k], "%.*s", (int)mpack_node_strlen(c),
                     mpack_node_str(c));
            out.ids[k] = mpack_node_u64(mpack_node_array_at(ids, k));
        }
    }
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    return out;
}

static u64 checksum(safety_fx *f) {
    return de_world_checksum(f->engine.world, 0xFFFFFFFFu);
}

TEST_F(editor_prefab_overrides, lists_the_subtree_with_variant_and_nested_sources, safety_fx) {
    const pf_world p = prefab_world(f);
    const de_entity s = spawn_prefab(f, "props/stack");
    const de_entity inner = first_child(f, s);      /* 0x11, props/crate's root */
    const de_entity leaf = first_child(f, inner);   /* 0x12, props/crate's child */
    ASSERT_TRUE(inner.bits != 0 && leaf.bits != 0);
    EXPECT_EQ(overrides_of(f, s, s, "THealth.max").count, 0u);
    ((TTarget *)de_entity_get(f->engine.world, leaf, p.target))->speed = 3;
    ((THealth *)de_entity_get(f->engine.world, inner, p.health))->max = 61;
    pf_found o = overrides_of(f, s, leaf, "TTarget.speed");
    EXPECT_EQ(o.count, 2u);
    ASSERT_TRUE(o.found);
    EXPECT_STREQ(o.source, "props/stack"); /* the stack sets speed = 2 itself */
    ASSERT_EQ(o.levels, 2u);
    EXPECT_STREQ(o.chain[0], "props/stack");
    EXPECT_STREQ(o.chain[1], "props/crate");
    EXPECT_EQ(o.ids[0], 0x12u);
    EXPECT_EQ(o.ids[1], 0x2u);
    o = overrides_of(f, s, inner, "THealth.max");
    ASSERT_TRUE(o.found);
    EXPECT_STREQ(o.source, "props/crate"); /* inherited from the nested crate */
    /* The query covers the entity's subtree only. */
    EXPECT_EQ(overrides_of(f, inner, inner, "THealth.max").count, 2u);
    o = overrides_of(f, leaf, leaf, "TTarget.speed");
    EXPECT_EQ(o.count, 1u);
    EXPECT_TRUE(o.found);
    /* A variant: max is the variant's own, current comes from its base. */
    const de_entity v = spawn_prefab(f, "props/crate_heavy");
    THealth *h = de_entity_get(f->engine.world, v, p.health);
    h->max = 1;
    h->current = 2;
    o = overrides_of(f, v, v, "THealth.max");
    EXPECT_EQ(o.count, 2u);
    EXPECT_STREQ(o.source, "props/crate_heavy");
    ASSERT_EQ(o.levels, 2u);
    EXPECT_STREQ(o.chain[1], "props/crate");
    EXPECT_STREQ(overrides_of(f, v, v, "THealth.current").source, "props/crate");
}

TEST_F(editor_prefab_overrides, compares_floats_exactly_without_round_trip_overrides, safety_fx) {
    const pf_world p = prefab_world(f);
    const de_entity v = spawn_prefab(f, "props/crate_heavy");
    const de_entity w2 = spawn_prefab(f, "props/crate_heavy");
    /* 80.3 is not exact in f32 or f64: the cooked value and the editor's agree bit for bit. */
    EXPECT_EQ(overrides_of(f, v, v, "THealth.max").count, 0u);
    THealth *h = de_entity_get(f->engine.world, v, p.health);
    h->max = 80.3f;
    EXPECT_EQ(overrides_of(f, v, v, "THealth.max").count, 0u);
    const f32 next = nextafterf(80.3f, 100.0f);
    h->max = next;
    EXPECT_TRUE(overrides_of(f, v, v, "THealth.max").found); /* one ulp is an override */
    prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, v, (pf_call){.path = "THealth.max"});
    EXPECT_EQ(overrides_of(f, v, v, "THealth.max").count, 0u);
    const f32 other = ((THealth *)de_entity_get(f->engine.world, w2, p.health))->max;
    EXPECT_TRUE(memcmp(&other, &next, sizeof next) == 0);
    EXPECT_EQ(overrides_of(f, w2, w2, "THealth.max").count, 0u);
}

TEST_F(editor_prefab_overrides, apply_to_source_is_one_undo_step_for_every_instance, safety_fx) {
    const pf_world p = prefab_world(f);
    const de_entity a = spawn_prefab(f, "props/crate");
    const de_entity b = spawn_prefab(f, "props/crate");
    const de_entity c = spawn_prefab(f, "props/crate");
    const de_entity v = spawn_prefab(f, "props/crate_heavy");
    ((THealth *)de_entity_get(f->engine.world, b, p.health))->current = 33; /* b's own */
    ((THealth *)de_entity_get(f->engine.world, a, p.health))->current = 70;
    const u64 before = checksum(f);
    const u64 rev = overrides_of(f, a, a, "THealth.current").revision;
    const u32 tx = prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a,
                             (pf_call){.path = "THealth.current", .has_revision = true,
                                       .revision = rev});
    EXPECT_EQ(overrides_of(f, a, a, "THealth.current").count, 0u);
    EXPECT_EQ(overrides_of(f, a, a, "THealth.current").revision, rev + 1);
    EXPECT_FLOAT_EQ(((THealth *)de_entity_get(f->engine.world, b, p.health))->current, 33, 0);
    EXPECT_TRUE(overrides_of(f, b, b, "THealth.current").found);
    EXPECT_FLOAT_EQ(((THealth *)de_entity_get(f->engine.world, c, p.health))->current, 70, 0);
    THealth *vh = de_entity_get(f->engine.world, v, p.health);
    EXPECT_FLOAT_EQ(vh->current, 70, 0); /* the variant inherits current */
    EXPECT_FLOAT_EQ(vh->max, 80.3f, 0);  /* and keeps its own max */
    const u64 after = checksum(f);
    EXPECT_TRUE(after != before);
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    EXPECT_EQ(checksum(f), before);
    EXPECT_TRUE(overrides_of(f, a, a, "THealth.current").found); /* the base is back too */
    transaction(f, DE_EP_METHOD_EDIT_REDO, tx);
    EXPECT_EQ(checksum(f), after);
    EXPECT_EQ(overrides_of(f, c, c, "THealth.current").count, 0u);
}

TEST_F(editor_prefab_overrides, apply_to_an_ancestor_through_variants_and_nesting, safety_fx) {
    const pf_world p = prefab_world(f);
    const de_entity s = spawn_prefab(f, "props/stack");
    const de_entity s2 = spawn_prefab(f, "props/stack");
    const de_entity crate = spawn_prefab(f, "props/crate");
    const de_entity v = spawn_prefab(f, "props/crate_heavy");
    const de_entity inner = first_child(f, s);
    ((THealth *)de_entity_get(f->engine.world, inner, p.health))->max = 60;
    /* Not in the chain of the nested crate. */
    prefab_fails(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, inner,
                 (pf_call){.path = "THealth.max", .target = "props/crate_heavy"},
                 DE_EP_ERR_INVALID_PARAMS, "target");
    const u64 before = checksum(f);
    const u32 tx = prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, inner,
                             (pf_call){.path = "THealth.max", .target = "props/crate"});
    EXPECT_FLOAT_EQ(((THealth *)de_entity_get(f->engine.world, crate, p.health))->max, 60, 0);
    EXPECT_FLOAT_EQ(
        ((THealth *)de_entity_get(f->engine.world, first_child(f, s2), p.health))->max, 60, 0);
    EXPECT_FLOAT_EQ(((THealth *)de_entity_get(f->engine.world, v, p.health))->max, 80.3f, 0);
    EXPECT_FLOAT_EQ(((THealth *)de_entity_get(f->engine.world, s, p.health))->max, 5, 0);
    EXPECT_EQ(overrides_of(f, s, s, "THealth.max").count, 0u);
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    EXPECT_EQ(checksum(f), before);
    transaction(f, DE_EP_METHOD_EDIT_REDO, tx);
    /* A variant's own value goes to the base, and the variant takes it too. */
    ((THealth *)de_entity_get(f->engine.world, v, p.health))->max = 90;
    prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, v,
              (pf_call){.path = "THealth.max", .target = "props/crate"});
    EXPECT_FLOAT_EQ(((THealth *)de_entity_get(f->engine.world, crate, p.health))->max, 90, 0);
    EXPECT_FLOAT_EQ(
        ((THealth *)de_entity_get(f->engine.world, first_child(f, s2), p.health))->max, 90, 0);
    EXPECT_EQ(overrides_of(f, v, v, "THealth.max").count, 0u);
    /* An entity field is mapped to each prefab's own ids. */
    const de_entity leaf = first_child(f, inner);
    TTarget *t = de_entity_get(f->engine.world, leaf, p.target);
    t->target = s; /* the stack's root: not part of the nested crate */
    prefab_fails(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, leaf,
                 (pf_call){.path = "TTarget.target", .target = "props/crate"},
                 DE_EP_ERR_UNSUPPORTED, "outside_prefab");
    prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, leaf, (pf_call){.path = "TTarget.target"});
    const de_entity leaf2 = first_child(f, first_child(f, s2));
    EXPECT_EQ(((TTarget *)de_entity_get(f->engine.world, leaf2, p.target))->target.bits, s2.bits);
}

TEST_F(editor_prefab_overrides, revert_is_undoable_and_apply_joins_an_open_transaction,
       safety_fx) {
    const pf_world p = prefab_world(f);
    const de_entity a = spawn_prefab(f, "props/crate");
    const de_entity b = spawn_prefab(f, "props/crate");
    THealth *h = de_entity_get(f->engine.world, a, p.health);
    h->current = 12;
    const u64 changed = checksum(f);
    const u32 tx = prefab_ok(f, DE_EP_METHOD_PREFAB_REVERT_OVERRIDE, a,
                             (pf_call){.path = "THealth.current"});
    EXPECT_FLOAT_EQ(h->current, 50, 0);
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    EXPECT_FLOAT_EQ(h->current, 12, 0);
    EXPECT_EQ(checksum(f), changed);
    /* Apply inside an open transaction: it is undone with it. */
    char params[128], response[2048];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    const de_ep_edit_begin_params begin = {.label = de_ep_cstr("apply"), .scene = 1};
    de_ep_write_edit_begin_params(&w, &begin);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_t tree;
    mpack_node_t root =
        request(f, DE_EP_METHOD_EDIT_BEGIN, (de_ep_raw){params, size}, &tree, response);
    const u32 open = mpack_node_u32(mpack_node_map_cstr(mpack_node_map_cstr(root, "result"), "tx"));
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    EXPECT_EQ(prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a,
                        (pf_call){.path = "THealth.current", .tx = open}),
              open);
    transaction(f, DE_EP_METHOD_EDIT_COMMIT, open);
    EXPECT_FLOAT_EQ(((THealth *)de_entity_get(f->engine.world, b, p.health))->current, 12, 0);
    transaction(f, DE_EP_METHOD_EDIT_UNDO, open);
    EXPECT_FLOAT_EQ(((THealth *)de_entity_get(f->engine.world, b, p.health))->current, 50, 0);
    EXPECT_EQ(checksum(f), changed);
}

TEST_F(editor_prefab_overrides, errors_are_structured, safety_fx) {
    const pf_world p = prefab_world(f);
    const de_entity a = spawn_prefab(f, "props/crate");
    const de_entity loose = de_entity_spawn(f->engine.world);
    const de_ep_method apply = DE_EP_METHOD_PREFAB_APPLY_OVERRIDE;
    const de_ep_method revert_m = DE_EP_METHOD_PREFAB_REVERT_OVERRIDE;
    prefab_fails(f, apply, loose, (pf_call){.path = "THealth.max"}, DE_EP_ERR_NOT_FOUND,
                 "not_instance");
    prefab_fails(f, DE_EP_METHOD_PREFAB_OVERRIDES, loose, (pf_call){0}, DE_EP_ERR_NOT_FOUND,
                 "not_instance");
    prefab_fails(f, apply, a, (pf_call){.path = "nodot"}, DE_EP_ERR_INVALID_PARAMS, "path");
    prefab_fails(f, revert_m, a, (pf_call){.path = "THealth.missing"}, DE_EP_ERR_NOT_FOUND,
                 "no_field");
    prefab_fails(f, apply, a, (pf_call){.path = "THealth.max"}, DE_EP_ERR_CONFLICT,
                 "not_override");
    prefab_fails(f, revert_m, a, (pf_call){.path = "THealth.max"}, DE_EP_ERR_CONFLICT,
                 "not_override");
    ((THealth *)de_entity_get(f->engine.world, a, p.health))->max = 3;
    prefab_fails(f, apply, a, (pf_call){.path = "THealth.max", .has_revision = true,
                                        .revision = 99},
                 DE_EP_ERR_CONFLICT, "stale");
    prefab_fails(f, apply, a, (pf_call){.path = "THealth.max", .target = "props/stack"},
                 DE_EP_ERR_INVALID_PARAMS, "target");
    f->ed.mode = DE_EP_WORLD_MODE_PLAY;
    prefab_fails(f, apply, a, (pf_call){.path = "THealth.max"}, DE_EP_ERR_WRONG_MODE,
                 "play_mode");
    prefab_fails(f, revert_m, a, (pf_call){.path = "THealth.max"}, DE_EP_ERR_WRONG_MODE,
                 "play_mode");
    f->ed.mode = DE_EP_WORLD_MODE_EDIT;
    EXPECT_FLOAT_EQ(((THealth *)de_entity_get(f->engine.world, a, p.health))->max, 3, 0);
}
#endif

#endif
