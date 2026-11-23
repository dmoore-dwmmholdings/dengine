#include "de_test.h"
#if !defined(DE_SHIPPING)
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include "asset/hot_reload_internal.h"
#include "asset/load_internal.h"
#include "ecs/ecs_internal.h"
#include <dengine/de_asset.h>
#include <dengine/de_ecs.h>
#include <dengine/de_math.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

/* Actual editor handlers and CPU gizmo input; no renderer, window or transport. */
typedef struct safety_fx {
    de_engine engine;
    de__host host;
    de__editor ed;
    de_builtin_component_ids ids;
    de_task_system *tasks;
    unsigned char arena[65536];
    char note[32768];
    char copy[128]; /* DENG-798: "tmp798:/<dir>", the writable copy of the prefab fixtures */
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
static void copy_remove(safety_fx *f);
static void safety_fx_teardown(safety_fx *f) {
    for (u32 i = 0; i < f->ed.reset_count; ++i)
        f->ed.resets[i](&f->ed);
    copy_remove(f);
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
/* DENG-802: entity ids nested in a struct and in an array. */
typedef struct TRef {
    de_entity a;
    i32 n;
} TRef;
typedef struct TLinks {
    TRef ref;
    de_entity list[3];
} TLinks;
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
static const de_field_info ref_fields[] = {
    DE_FIELD(TRef, a, DE_FK_ENTITY, DE_FIELD_DEFAULT),
    DE_FIELD(TRef, n, DE_FK_I32, DE_FIELD_DEFAULT),
};
static const de_field_info links_fields[] = {
    DE_FIELD_EX(TLinks, ref, DE_FK_STRUCT, DE_FIELD_DEFAULT, {0, 0, 0, 0, "TRef", 0}),
    DE_FIELD_EX(TLinks, list, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT, {0, 0, 0, 0, "entity", 3}),
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
    de_component health, target, links;
} pf_world;

/* ---------- DENG-798: a writable copy of the fixtures, so apply can write the sources -------- */

typedef struct copy_ctx {
    const char *from, *to;
    bool ok;
} copy_ctx;

static bool copy_one(const char *name, bool is_dir, void *user) {
    copy_ctx *c = user;
    if (is_dir)
        return true;
    char from[512], to[512];
    snprintf(from, sizeof from, "%s/%s", c->from, name);
    snprintf(to, sizeof to, "%s/%s", c->to, name);
    void *data = NULL;
    u64 size = 0;
    c->ok = c->ok && de_file_read_all(from, de_heap_allocator(), &data, &size) == DE_OK &&
            de_file_write_all_atomic(to, data, size) == DE_OK;
    if (data != NULL)
        DE_FREE(de_heap_allocator(), data, size + 1);
    return c->ok;
}

static bool delete_one(const char *name, bool is_dir, void *user) {
    char path[512];
    if (!is_dir) {
        snprintf(path, sizeof path, "%s/%s", (const char *)user, name);
        (void)de_file_delete(path);
    }
    return true;
}

static void native_rmdir(const char *vpath) {
    char native[1024];
    if (de_fs_resolve(vpath, native, sizeof native) == DE_OK)
        (void)remove(native);
}

/* Copies tests/unit/asset/data/scenes to a new directory under the build tree and mounts it,
 * writable, as safe676:/. */
static void copy_fixtures(safety_fx *f) {
    static u32 counter;
    ASSERT_EQ(de_fs_mount("src798", DE_EDITOR_SAFETY_DATA_DIR, 0, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("tmp798", DE_EDITOR_PREFAB_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
#if defined(_WIN32)
    const unsigned pid = 0;
#else
    const unsigned pid = (unsigned)getpid();
#endif
    snprintf(f->copy, sizeof f->copy, "tmp798:/%u-%llx-%u", pid,
             (unsigned long long)de_time_now_ns(), ++counter);
    char assets[256], native[1024];
    snprintf(assets, sizeof assets, "%s/assets", f->copy);
    ASSERT_EQ(de_dir_create(f->copy), DE_OK);
    ASSERT_EQ(de_dir_create(assets), DE_OK);
    copy_ctx top = {"src798:", f->copy, true};
    ASSERT_TRUE(copy_one("assets.toml", false, &top));
    copy_ctx all = {"src798:/assets", assets, true};
    ASSERT_EQ(de_dir_list("src798:/assets", copy_one, &all), DE_OK);
    ASSERT_TRUE(all.ok);
    ASSERT_EQ(de_fs_resolve(f->copy, native, sizeof native), DE_OK);
    ASSERT_EQ(de_fs_mount("safe676", native, DE_MOUNT_WRITABLE, 0), DE_OK);
}

static void copy_remove(safety_fx *f) {
    if (f->copy[0] == 0)
        return;
    char assets[256], native[1024];
    snprintf(assets, sizeof assets, "%s/assets", f->copy);
    (void)de_dir_list(assets, delete_one, assets);
    (void)de_dir_list(f->copy, delete_one, f->copy);
    const bool resolved = de_fs_resolve(f->copy, native, sizeof native) == DE_OK;
    native_rmdir(assets);
    native_rmdir(f->copy);
    if (resolved)
        (void)de_fs_unmount("safe676", native);
    (void)de_fs_unmount("tmp798", DE_EDITOR_PREFAB_TMP_DIR);
    (void)de_fs_unmount("src798", DE_EDITOR_SAFETY_DATA_DIR);
    f->copy[0] = 0;
}

static pf_world prefab_world_ex(safety_fx *f, bool hot_reload);
static pf_world prefab_world(safety_fx *f) {
    return prefab_world_ex(f, false);
}

static pf_world prefab_world_ex(safety_fx *f, bool hot_reload) {
    pf_world p;
    p.health = register_health(f);
    ASSERT_TRUE(DE_HANDLE_VALID(
        de_type_register(f->engine.types, &DE_TYPE_DESC(TTarget, 1, target_fields))));
    p.target = de_component_register(f->engine.world, de_type_find(f->engine.types, "TTarget"),
                                     DE_COMP_SERIALIZE);
    ASSERT_TRUE(DE_HANDLE_VALID(p.target));
    ASSERT_TRUE(DE_HANDLE_VALID(
        de_type_register(f->engine.types, &DE_TYPE_DESC(TRef, 1, ref_fields))));
    ASSERT_TRUE(DE_HANDLE_VALID(
        de_type_register(f->engine.types, &DE_TYPE_DESC(TLinks, 1, links_fields))));
    p.links = de_component_register(f->engine.world, de_type_find(f->engine.types, "TLinks"),
                                    DE_COMP_SERIALIZE);
    ASSERT_TRUE(DE_HANDLE_VALID(p.links));
    copy_fixtures(f);
    de_fp_env_init_thread();
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 1}, &f->tasks), DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.tasks = f->tasks,
                                                             .types = f->engine.types,
                                                             .io_threads = 1,
                                                             .cache_dir = "nocache676:/cache",
                                                             .hot_reload = hot_reload},
                                     &f->engine.assets),
              DE_OK);
    if (hot_reload)
        de__asset_reload_set_watching(f->engine.assets, false); /* the test drives it */
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
    const char *path, *target, *kind; /* kind: protocol 1.20 override kind, NULL = absent */
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
    if (c.kind != NULL) {
        mpack_write_cstr(&w, "kind");
        mpack_write_cstr(&w, c.kind);
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
    char kind[32];                  /* protocol 1.20: "" when absent */
    mpack_type_t value_type, base_type;
    u32 value_count, base_count;    /* entries, when the value or base is a map */
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
        if (mpack_node_map_contains_cstr(o, "kind")) {
            mpack_node_t k = mpack_node_map_cstr(o, "kind");
            snprintf(out.kind, sizeof out.kind, "%.*s", (int)mpack_node_strlen(k),
                     mpack_node_str(k));
        }
        mpack_node_t ov = mpack_node_map_cstr(o, "value"), ob = mpack_node_map_cstr(o, "base");
        out.value_type = mpack_node_type(ov);
        out.base_type = mpack_node_type(ob);
        out.value_count = out.value_type == mpack_type_map ? (u32)mpack_node_map_count(ov) : 0;
        out.base_count = out.base_type == mpack_type_map ? (u32)mpack_node_map_count(ob) : 0;
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

/* DENG-802: entity ids nested in struct and array fields map between id spaces like direct ones. */
TEST_F(editor_prefab_overrides, apply_maps_entity_ids_in_struct_and_array_fields, safety_fx) {
    const pf_world p = prefab_world(f);
    const de_entity root = spawn_prefab(f, "props/links");
    const de_entity child = first_child(f, root);
    const de_entity other = spawn_prefab(f, "props/crate");
    const de_entity r2 = spawn_prefab(f, "props/links"); /* another instance takes the apply */
    const de_entity c2 = first_child(f, r2);
    TLinks *l = de_entity_get(f->engine.world, child, p.links);
    ASSERT_TRUE(l != NULL);
    EXPECT_EQ(l->ref.a.bits, root.bits);
    EXPECT_EQ(l->list[1].bits, child.bits);
    /* A reference outside the prefab is refused, nested or not. */
    l->ref.a = other;
    prefab_fails(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, child, (pf_call){.path = "TLinks.ref"},
                 DE_EP_ERR_UNSUPPORTED, "outside_prefab");
    l->ref.a = root;
    l->list[2] = other;
    prefab_fails(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, child, (pf_call){.path = "TLinks.list"},
                 DE_EP_ERR_UNSUPPORTED, "outside_prefab");
    /* Inside the prefab: stored in the prefab's ids, applied to the next instance. */
    l->ref.a = child;
    l->ref.n = 7;
    l->list[0] = child;
    l->list[2] = root;
    const u64 before = checksum(f);
    const u32 tx = prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, child,
                             (pf_call){.path = "TLinks.ref"});
    const u32 tx2 = prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, child,
                              (pf_call){.path = "TLinks.list"});
    EXPECT_FALSE(overrides_of(f, root, child, "TLinks.ref").found);
    EXPECT_FALSE(overrides_of(f, root, child, "TLinks.list").found);
    /* Undo puts everything back; redo applies again. */
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx2);
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    EXPECT_EQ(checksum(f), before);
    EXPECT_TRUE(overrides_of(f, root, child, "TLinks.ref").found);
    EXPECT_TRUE(overrides_of(f, root, child, "TLinks.list").found);
    transaction(f, DE_EP_METHOD_EDIT_REDO, tx);
    transaction(f, DE_EP_METHOD_EDIT_REDO, tx2);
    EXPECT_FALSE(overrides_of(f, root, child, "TLinks.ref").found);
    const TLinks *m = de_entity_get(f->engine.world, c2, p.links);
    ASSERT_TRUE(m != NULL);
    EXPECT_EQ(m->ref.a.bits, c2.bits);
    EXPECT_EQ(m->ref.n, 7);
    EXPECT_EQ(m->list[0].bits, c2.bits);
    EXPECT_EQ(m->list[1].bits, c2.bits);
    EXPECT_EQ(m->list[2].bits, r2.bits);
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

/* DENG-797: the 1.18 wire shapes (ApplyResult, RevertResult, PrefabErrorData, OverrideList), key
 * by key and in order. */
static void expect_keys(mpack_node_t map, const char *const *keys, u32 n) {
    ASSERT_EQ(mpack_node_map_count(map), n);
    for (u32 i = 0; i < n; ++i)
        EXPECT_TRUE(node_streq(mpack_node_map_key_at(map, i), keys[i]));
}

TEST_F(editor_prefab_overrides, results_and_error_data_have_the_1_19_shapes, safety_fx) {
    const pf_world p = prefab_world(f);
    const de_entity a = spawn_prefab(f, "props/crate");
    char response[2048];
    mpack_tree_t tree;
    mpack_node_t root = prefab_call(f, DE_EP_METHOD_PREFAB_OVERRIDES, a, (pf_call){0}, &tree,
                                    response);
    static const char *const list_keys[] = {"overrides", "asset", "root", "revision"};
    expect_keys(mpack_node_map_cstr(root, "result"), list_keys, 4);
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    ((THealth *)de_entity_get(f->engine.world, a, p.health))->max = 3;
    root = prefab_call(f, DE_EP_METHOD_PREFAB_OVERRIDES, a, (pf_call){0}, &tree, response);
    mpack_node_t row = mpack_node_array_at(
        mpack_node_map_cstr(mpack_node_map_cstr(root, "result"), "overrides"), 0);
    static const char *const row_keys[] = {"path", "value", "entity", "base",
                                           "source", "chain", "ids"};
    expect_keys(row, row_keys, 7);
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    root = prefab_call(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a, (pf_call){.path = "THealth.max"},
                       &tree, response);
    /* DENG-801: the generated ApplyResult writer; `files` (1.19) comes last. */
    static const char *const apply_keys[] = {"tx", "revision", "prefabs", "entities", "files"};
    expect_keys(mpack_node_map_cstr(root, "result"), apply_keys, 5);
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    ((THealth *)de_entity_get(f->engine.world, a, p.health))->max = 4;
    root = prefab_call(f, DE_EP_METHOD_PREFAB_REVERT_OVERRIDE, a, (pf_call){.path = "THealth.max"},
                       &tree, response);
    static const char *const revert_keys[] = {"tx", "revision"};
    expect_keys(mpack_node_map_cstr(root, "result"), revert_keys, 2);
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    root = prefab_call(f, DE_EP_METHOD_PREFAB_REVERT_OVERRIDE, a, (pf_call){.path = "THealth.max"},
                       &tree, response);
    static const char *const err_keys[] = {"code", "message", "data"};
    expect_keys(mpack_node_map_cstr(root, "error"), err_keys, 3);
    static const char *const data_keys[] = {"reason", "revision"};
    expect_keys(mpack_node_map_cstr(mpack_node_map_cstr(root, "error"), "data"), data_keys, 2);
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
}

/* DENG-809: protocol 1.20 override kinds. A missing kind or kind "field" behaves as before; the
 * entity kinds are not implemented (DENG-811) and answer UNSUPPORTED reason kind, without
 * changing the world. DENG-810: a component kind with a path that is no component name is
 * INVALID_PARAMS reason kind; an unknown component is NOT_FOUND reason no_component. */
TEST_F(editor_prefab_overrides, entity_kinds_are_unsupported, safety_fx) {
    const pf_world p = prefab_world(f);
    const de_entity a = spawn_prefab(f, "props/crate");
    ((THealth *)de_entity_get(f->engine.world, a, p.health))->max = 3;
    static const char *const kinds[] = {"entity_added", "entity_removed"};
    const de_ep_method methods[] = {DE_EP_METHOD_PREFAB_APPLY_OVERRIDE,
                                    DE_EP_METHOD_PREFAB_REVERT_OVERRIDE};
    for (u32 m = 0; m < 2; ++m) {
        for (u32 k = 0; k < 2; ++k)
            prefab_fails(f, methods[m], a, (pf_call){.path = "THealth", .kind = kinds[k]},
                         DE_EP_ERR_UNSUPPORTED, "kind");
        prefab_fails(f, methods[m], a, (pf_call){.path = "THealth.max", .kind = "component_added"},
                     DE_EP_ERR_INVALID_PARAMS, "kind");
        prefab_fails(f, methods[m], a, (pf_call){.path = "NoSuch", .kind = "component_removed"},
                     DE_EP_ERR_NOT_FOUND, "no_component");
        /* THealth is on the root as the prefab has it: no structural override. */
        prefab_fails(f, methods[m], a, (pf_call){.path = "THealth", .kind = "component_added"},
                     DE_EP_ERR_CONFLICT, "not_override");
        prefab_fails(f, methods[m], a, (pf_call){.path = "THealth", .kind = "component_removed"},
                     DE_EP_ERR_CONFLICT, "not_override");
    }
    /* The refused calls changed nothing, and an explicit field kind still applies. */
    EXPECT_FLOAT_EQ(((THealth *)de_entity_get(f->engine.world, a, p.health))->max, 3.0f, 1e-6f);
    EXPECT_TRUE(prefab_ok(f, DE_EP_METHOD_PREFAB_REVERT_OVERRIDE, a,
                          (pf_call){.path = "THealth.max", .kind = "field"}) != 0);
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

/* ---------- DENG-801: prefab.unpack (protocol 1.19) ---------- */

TEST_F(editor_prefab_overrides, unpack_is_undoable_and_keeps_the_overrides, safety_fx) {
    const pf_world p = prefab_world(f);
    const de_entity a = spawn_prefab(f, "props/crate");
    const de_entity other = spawn_prefab(f, "props/crate");
    ((THealth *)de_entity_get(f->engine.world, a, p.health))->current = 33;
    const u64 before = checksum(f);
    const pf_found had = overrides_of(f, a, a, "THealth.current");
    ASSERT_TRUE(had.found);
    char response[2048];
    mpack_tree_t tree;
    mpack_node_t root = prefab_call(f, DE_EP_METHOD_PREFAB_UNPACK, a, (pf_call){0}, &tree,
                                    response);
    ASSERT_TRUE(mpack_node_map_contains_cstr(root, "result"));
    mpack_node_t res = mpack_node_map_cstr(root, "result");
    const u32 tx = mpack_node_u32(mpack_node_map_cstr(res, "tx"));
    EXPECT_TRUE(tx != 0);
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(res, "root")), a.bits);
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(res, "revision")), had.revision);
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    /* Plain entities now: same world, no instance record, the other instance untouched. */
    EXPECT_EQ(checksum(f), before);
    EXPECT_TRUE(de_entity_alive(f->engine.world, a));
    EXPECT_FLOAT_EQ(((THealth *)de_entity_get(f->engine.world, a, p.health))->current, 33, 0);
    prefab_fails(f, DE_EP_METHOD_PREFAB_OVERRIDES, a, (pf_call){0}, DE_EP_ERR_NOT_FOUND,
                 "not_instance");
    prefab_fails(f, DE_EP_METHOD_PREFAB_UNPACK, a, (pf_call){0}, DE_EP_ERR_NOT_FOUND,
                 "not_instance");
    EXPECT_FALSE(overrides_of(f, other, other, "THealth.current").found);
    /* Undo: the record and the override are back, exactly. */
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    EXPECT_EQ(checksum(f), before);
    const pf_found back = overrides_of(f, a, a, "THealth.current");
    EXPECT_TRUE(back.found);
    EXPECT_EQ(back.revision, had.revision);
    EXPECT_EQ(back.count, had.count);
    EXPECT_STREQ(back.source, had.source);
    /* ...so the override still applies and reverts. */
    const u32 rv = prefab_ok(f, DE_EP_METHOD_PREFAB_REVERT_OVERRIDE, a,
                             (pf_call){.path = "THealth.current"});
    EXPECT_FLOAT_EQ(((THealth *)de_entity_get(f->engine.world, a, p.health))->current, 50, 0);
    transaction(f, DE_EP_METHOD_EDIT_UNDO, rv);
    /* Redo of the unpack, then undo again. */
    transaction(f, DE_EP_METHOD_EDIT_REDO, tx);
    EXPECT_EQ(checksum(f), before);
    prefab_fails(f, DE_EP_METHOD_PREFAB_OVERRIDES, a, (pf_call){0}, DE_EP_ERR_NOT_FOUND,
                 "not_instance");
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    EXPECT_TRUE(overrides_of(f, a, a, "THealth.current").found);
}

TEST_F(editor_prefab_overrides, unpack_a_nesting_instance_drops_one_record, safety_fx) {
    (void)prefab_world(f);
    const de_entity s = spawn_prefab(f, "props/stack");
    const de_entity inner = first_child(f, s);
    const u64 before = checksum(f);
    /* The nested crate is not an instance root of its own. */
    prefab_fails(f, DE_EP_METHOD_PREFAB_UNPACK, inner, (pf_call){0}, DE_EP_ERR_NOT_FOUND,
                 "not_instance");
    const u32 tx = prefab_ok(f, DE_EP_METHOD_PREFAB_UNPACK, s, (pf_call){0});
    EXPECT_EQ(checksum(f), before);
    prefab_fails(f, DE_EP_METHOD_PREFAB_OVERRIDES, inner, (pf_call){0}, DE_EP_ERR_NOT_FOUND,
                 "not_instance");
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    EXPECT_EQ(overrides_of(f, s, s, "THealth.max").count, 0u);
    EXPECT_EQ(checksum(f), before);
}

TEST_F(editor_prefab_overrides, unpack_errors_are_structured, safety_fx) {
    (void)prefab_world(f);
    const de_entity a = spawn_prefab(f, "props/crate");
    const de_entity loose = de_entity_spawn(f->engine.world);
    const de_entity leaf = first_child(f, a);
    const de_ep_method unpack = DE_EP_METHOD_PREFAB_UNPACK;
    prefab_fails(f, unpack, loose, (pf_call){0}, DE_EP_ERR_NOT_FOUND, "not_instance");
    if (leaf.bits != 0) /* a child of an instance root is not a root */
        prefab_fails(f, unpack, leaf, (pf_call){0}, DE_EP_ERR_NOT_FOUND, "not_instance");
    prefab_fails(f, unpack, a, (pf_call){.has_revision = true, .revision = 99}, DE_EP_ERR_CONFLICT,
                 "stale");
    f->ed.mode = DE_EP_WORLD_MODE_PLAY;
    prefab_fails(f, unpack, a, (pf_call){0}, DE_EP_ERR_WRONG_MODE, "play_mode");
    f->ed.mode = DE_EP_WORLD_MODE_EDIT;
    /* Nothing changed: the instance is still one, and the current revision passes the check. */
    const u64 rev = overrides_of(f, a, a, "THealth.max").revision;
    const u32 tx = prefab_ok(f, unpack, a, (pf_call){.has_revision = true, .revision = rev});
    EXPECT_TRUE(tx != 0);
}

/* The 1.19 UnpackResult shape: tx, revision, root, in that order. */
TEST_F(editor_prefab_overrides, unpack_result_has_the_1_19_shape, safety_fx) {
    (void)prefab_world(f);
    const de_entity a = spawn_prefab(f, "props/crate");
    char response[2048];
    mpack_tree_t tree;
    mpack_node_t root = prefab_call(f, DE_EP_METHOD_PREFAB_UNPACK, a, (pf_call){0}, &tree,
                                    response);
    static const char *const keys[] = {"tx", "revision", "root"};
    expect_keys(mpack_node_map_cstr(root, "result"), keys, 3);
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
}

/* ---------- DENG-798: apply writes the prefab sources ---------- */

#define CRATE "safe676:/assets/crate.prefab.toml"
#define HEAVY "safe676:/assets/crate_heavy.prefab.toml"
#define STACK "safe676:/assets/stack.prefab.toml"

typedef struct text {
    char s[4096];
    size_t n;
} text;

static text read_text(const char *vpath) {
    text t;
    memset(&t, 0, sizeof t);
    void *data = NULL;
    u64 size = 0;
    ASSERT_EQ(de_file_read_all(vpath, de_heap_allocator(), &data, &size), DE_OK);
    ASSERT_TRUE(size < sizeof t.s);
    memcpy(t.s, data, (size_t)size);
    t.n = (size_t)size;
    DE_FREE(de_heap_allocator(), data, size + 1);
    return t;
}

static bool text_eq(text a, text b) {
    return a.n == b.n && memcmp(a.s, b.s, a.n) == 0;
}

/* `t` with the one occurrence of `from` (which must be there) replaced by `to`. */
static text replaced(text t, const char *from, const char *to) {
    char *at = strstr(t.s, from);
    ASSERT_TRUE(at != NULL);
    ASSERT_TRUE(strstr(at + 1, from) == NULL);
    text out;
    memset(&out, 0, sizeof out);
    const size_t head = (size_t)(at - t.s), lf = strlen(from), lt = strlen(to);
    memcpy(out.s, t.s, head);
    memcpy(out.s + head, to, lt);
    memcpy(out.s + head + lt, at + lf, t.n - head - lf);
    out.n = t.n - lf + lt;
    return out;
}

/* Undo or redo that must fail with code and data.reason. */
static void transaction_fails(safety_fx *f, de_ep_method method, u32 tx, de_ep_error code,
                              const char *reason) {
    char params[64], response[2048];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    const de_ep_tx_params q = {.tx = tx};
    de_ep_write_tx_params(&w, &q);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_t tree;
    mpack_node_t root = request(f, method, (de_ep_raw){params, size}, &tree, response);
    ASSERT_TRUE(mpack_node_map_contains_cstr(root, "error"));
    mpack_node_t err = mpack_node_map_cstr(root, "error");
    EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(err, "code")), (i32)code);
    EXPECT_TRUE(node_streq(mpack_node_map_cstr(mpack_node_map_cstr(err, "data"), "reason"),
                           reason));
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
}

#if !defined(_WIN32)
#include <sys/stat.h>
static void set_mode(const char *vpath, unsigned mode) {
    char native[1024];
    ASSERT_EQ(de_fs_resolve(vpath, native, sizeof native), DE_OK);
    ASSERT_EQ(chmod(native, (mode_t)mode), 0);
}
#endif

TEST_F(editor_prefab_source, apply_writes_the_source_and_keeps_comments_and_order, safety_fx) {
    const pf_world p = prefab_world(f);
    const text crate = read_text(CRATE);
    const de_entity a = spawn_prefab(f, "props/crate");
    ((THealth *)de_entity_get(f->engine.world, a, p.health))->current = 70;
    char response[2048];
    mpack_tree_t tree;
    mpack_node_t root = prefab_call(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a,
                                    (pf_call){.path = "THealth.current"}, &tree, response);
    ASSERT_TRUE(mpack_node_map_contains_cstr(root, "result"));
    mpack_node_t files = mpack_node_map_cstr(mpack_node_map_cstr(root, "result"), "files");
    /* The variant and the nesting prefab inherit the field: they follow, though neither was
     * spawned (DENG-807). */
    ASSERT_EQ(mpack_node_array_length(files), 3u);
    /* The manifest's source path, one per entry of `prefabs` (DIDE-383). */
    EXPECT_TRUE(node_streq(mpack_node_array_at(files, 0), "crate.prefab.toml"));
    mpack_node_t prefabs = mpack_node_map_cstr(mpack_node_map_cstr(root, "result"), "prefabs");
    EXPECT_EQ(mpack_node_array_length(prefabs), mpack_node_array_length(files));
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    /* Only the value changes: the comment, the key order and the layout stay. */
    EXPECT_TRUE(text_eq(read_text(CRATE),
                        replaced(crate, "current = 50.0\nmax = 50.0", "current = 70.0\nmax = 50.0")));
    /* A field the file does not set yet is added to its table. */
    snprintf(((THealth *)de_entity_get(f->engine.world, a, p.health))->label, 24, "big \"one\"");
    const text before = read_text(CRATE);
    prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a, (pf_call){.path = "THealth.label"});
    EXPECT_TRUE(text_eq(read_text(CRATE), replaced(before, "max = 50.0\n",
                                                   "max = 50.0\nlabel = \"big \\\"one\\\"\"\n")));
}

TEST_F(editor_prefab_source, apply_writes_variant_and_nested_sources, safety_fx) {
    const pf_world p = prefab_world(f);
    const text crate = read_text(CRATE), heavy = read_text(HEAVY), stack = read_text(STACK);
    /* Loaded prefabs that inherit the field follow the base (DENG-571). */
    (void)spawn_prefab(f, "props/crate_heavy");
    (void)spawn_prefab(f, "props/stack");
    const de_entity a = spawn_prefab(f, "props/crate");
    ((THealth *)de_entity_get(f->engine.world, a, p.health))->current = 70;
    prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a, (pf_call){.path = "THealth.current"});
    EXPECT_TRUE(text_eq(read_text(CRATE), replaced(crate, "current = 50.0", "current = 70.0")));
    /* The variant and the nested copy followed the base: their files follow too. */
    EXPECT_TRUE(text_eq(read_text(HEAVY), replaced(heavy, "current = 50.0", "current = 70.0")));
    EXPECT_TRUE(text_eq(read_text(STACK), replaced(stack, "current = 50.0", "current = 70.0")));
    /* An apply to the variant itself writes the variant only. */
    const de_entity v = spawn_prefab(f, "props/crate_heavy");
    ((THealth *)de_entity_get(f->engine.world, v, p.health))->max = 81.5f;
    const text crate2 = read_text(CRATE), stack2 = read_text(STACK), heavy2 = read_text(HEAVY);
    prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, v, (pf_call){.path = "THealth.max"});
    EXPECT_TRUE(text_eq(read_text(HEAVY), replaced(heavy2, "max = 80.3", "max = 81.5")));
    EXPECT_TRUE(text_eq(read_text(CRATE), crate2));
    EXPECT_TRUE(text_eq(read_text(STACK), stack2));
    /* A nested entity applied to the nesting prefab: the stack's own table for 0x12. */
    const de_entity s = spawn_prefab(f, "props/stack");
    const de_entity leaf = first_child(f, first_child(f, s));
    ((TTarget *)de_entity_get(f->engine.world, leaf, p.target))->speed = 4;
    prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, leaf, (pf_call){.path = "TTarget.speed"});
    EXPECT_TRUE(text_eq(read_text(STACK), replaced(stack2, "speed = 2.0", "speed = 4.0")));
}

/* ---------- DENG-807: dependents that were never spawned ---------- */

#define LINKS "safe676:/assets/links.prefab.toml"

static bool files_has(mpack_node_t files, const char *name) {
    for (size_t i = 0; i < mpack_node_array_length(files); ++i)
        if (node_streq(mpack_node_array_at(files, i), name))
            return true;
    return false;
}

static void write_text(const char *vpath, text t) {
    ASSERT_EQ(de_file_write_all_atomic(vpath, t.s, t.n), DE_OK);
}

TEST_F(editor_prefab_source, apply_writes_the_sources_of_unloaded_dependents, safety_fx) {
    const pf_world p = prefab_world(f);
    const text crate = read_text(CRATE), heavy = read_text(HEAVY), stack = read_text(STACK);
    const text links = read_text(LINKS);
    /* Only the base is spawned: the variant and the nesting prefab are not loaded. */
    const de_entity a = spawn_prefab(f, "props/crate");
    ((THealth *)de_entity_get(f->engine.world, a, p.health))->current = 70;
    char response[2048];
    mpack_tree_t tree;
    mpack_node_t root = prefab_call(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a,
                                    (pf_call){.path = "THealth.current"}, &tree, response);
    ASSERT_TRUE(mpack_node_map_contains_cstr(root, "result"));
    mpack_node_t res = mpack_node_map_cstr(root, "result");
    const u32 tx = mpack_node_u32(mpack_node_map_cstr(res, "tx"));
    mpack_node_t files = mpack_node_map_cstr(res, "files");
    EXPECT_EQ(mpack_node_array_length(files), 3u);
    EXPECT_TRUE(files_has(files, "crate.prefab.toml"));
    EXPECT_TRUE(files_has(files, "crate_heavy.prefab.toml"));
    EXPECT_TRUE(files_has(files, "stack.prefab.toml"));
    EXPECT_EQ(mpack_node_array_length(mpack_node_map_cstr(res, "prefabs")), 3u);
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    const text crate1 = read_text(CRATE), heavy1 = read_text(HEAVY), stack1 = read_text(STACK);
    EXPECT_TRUE(text_eq(crate1, replaced(crate, "current = 50.0", "current = 70.0")));
    EXPECT_TRUE(text_eq(heavy1, replaced(heavy, "current = 50.0", "current = 70.0")));
    /* The nested copy (0x11) follows; the stack's own root (0x10) does not. */
    EXPECT_TRUE(text_eq(stack1, replaced(stack, "current = 50.0", "current = 70.0")));
    EXPECT_TRUE(text_eq(read_text(LINKS), links)); /* not a dependent */
    /* Undo restores every file byte for byte; redo writes them again. */
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    EXPECT_TRUE(text_eq(read_text(CRATE), crate));
    EXPECT_TRUE(text_eq(read_text(HEAVY), heavy));
    EXPECT_TRUE(text_eq(read_text(STACK), stack));
    transaction(f, DE_EP_METHOD_EDIT_REDO, tx);
    EXPECT_TRUE(text_eq(read_text(CRATE), crate1));
    EXPECT_TRUE(text_eq(read_text(HEAVY), heavy1));
    EXPECT_TRUE(text_eq(read_text(STACK), stack1));
    /* A dependent that sets the field itself keeps its value: the variant's max. */
    ((THealth *)de_entity_get(f->engine.world, a, p.health))->max = 60;
    prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a, (pf_call){.path = "THealth.max"});
    EXPECT_TRUE(text_eq(read_text(HEAVY), heavy1));
    EXPECT_TRUE(text_eq(read_text(STACK), replaced(stack1, "current = 70.0\nmax = 50.0",
                                                   "current = 70.0\nmax = 60.0")));
}

/* Nothing is written and the world is unchanged; the files are as given. */
static void apply_refused(safety_fx *f, de_entity a, de_ep_error code, const char *reason,
                          text crate, text heavy, text stack) {
    const u64 before = checksum(f);
    prefab_fails(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a, (pf_call){.path = "THealth.current"},
                 code, reason);
    EXPECT_EQ(checksum(f), before);
    EXPECT_TRUE(overrides_of(f, a, a, "THealth.current").found);
    EXPECT_TRUE(text_eq(read_text(CRATE), crate));
    EXPECT_TRUE(text_eq(read_text(STACK), stack));
    if (heavy.n != 0)
        EXPECT_TRUE(text_eq(read_text(HEAVY), heavy));
}

TEST_F(editor_prefab_source, unloaded_dependent_cycles_and_unreadable_sources_are_refused,
       safety_fx) {
    const pf_world p = prefab_world(f);
    const text crate = read_text(CRATE), heavy = read_text(HEAVY), stack = read_text(STACK);
    const text links = read_text(LINKS);
    const de_entity a = spawn_prefab(f, "props/crate");
    ((THealth *)de_entity_get(f->engine.world, a, p.health))->current = 70;
    /* The stack (which nests the crate) and links are variants of each other. */
    const text stack_c = replaced(stack, "[prefab]\n", "[prefab]\nbase = \"props/links\"\n");
    const text links_c = replaced(links, "[[entity]]\nid = \"0x1\"",
                                  "[prefab]\nbase = \"props/stack\"\n\n[[entity]]\nid = \"0x1\"");
    write_text(STACK, stack_c);
    write_text(LINKS, links_c);
    apply_refused(f, a, DE_EP_ERR_UNSUPPORTED, "dependent_cycle", crate, heavy, stack_c);
    EXPECT_TRUE(text_eq(read_text(LINKS), links_c));
    write_text(STACK, stack);
    write_text(LINKS, links);
    /* A missing source may be a dependent: refused before any file is written. */
    ASSERT_EQ(de_file_delete(HEAVY), DE_OK);
    apply_refused(f, a, DE_EP_ERR_INTERNAL, "dependent_unreadable", crate, (text){{0}, 0}, stack);
    write_text(HEAVY, heavy);
#if !defined(_WIN32)
    set_mode(HEAVY, 0);
    apply_refused(f, a, DE_EP_ERR_INTERNAL, "dependent_unreadable", crate, (text){{0}, 0}, stack);
    set_mode(HEAVY, 0644);
#endif
    /* Once readable again, the apply goes through and writes all three. */
    prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a, (pf_call){.path = "THealth.current"});
    EXPECT_TRUE(text_eq(read_text(HEAVY), replaced(heavy, "current = 50.0", "current = 70.0")));
    EXPECT_TRUE(text_eq(read_text(STACK), replaced(stack, "current = 50.0", "current = 70.0")));
}

TEST_F(editor_prefab_source, undo_and_redo_restore_the_exact_bytes, safety_fx) {
    const pf_world p = prefab_world(f);
    const text crate = read_text(CRATE), heavy = read_text(HEAVY), stack = read_text(STACK);
    /* Loaded prefabs that inherit the field follow the base (DENG-571). */
    (void)spawn_prefab(f, "props/crate_heavy");
    (void)spawn_prefab(f, "props/stack");
    const de_entity a = spawn_prefab(f, "props/crate");
    ((THealth *)de_entity_get(f->engine.world, a, p.health))->current = 70;
    const u32 tx =
        prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a, (pf_call){.path = "THealth.current"});
    const text crate1 = read_text(CRATE), heavy1 = read_text(HEAVY), stack1 = read_text(STACK);
    EXPECT_FALSE(text_eq(crate1, crate));
    const u64 after = checksum(f);
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    EXPECT_TRUE(text_eq(read_text(CRATE), crate));
    EXPECT_TRUE(text_eq(read_text(HEAVY), heavy));
    EXPECT_TRUE(text_eq(read_text(STACK), stack));
    const u64 before = checksum(f);
    transaction(f, DE_EP_METHOD_EDIT_REDO, tx);
    EXPECT_TRUE(text_eq(read_text(CRATE), crate1));
    EXPECT_TRUE(text_eq(read_text(HEAVY), heavy1));
    EXPECT_TRUE(text_eq(read_text(STACK), stack1));
    EXPECT_EQ(checksum(f), after);
    /* A file changed outside the editor is not overwritten by undo; nothing changes. */
    ASSERT_EQ(de_file_write_all_atomic(HEAVY, "# mine\n", 7), DE_OK);
    transaction_fails(f, DE_EP_METHOD_EDIT_UNDO, tx, DE_EP_ERR_CONFLICT, "source_changed");
    EXPECT_EQ(checksum(f), after);
    EXPECT_TRUE(text_eq(read_text(CRATE), crate1));
    ASSERT_EQ(de_file_write_all_atomic(HEAVY, heavy1.s, heavy1.n), DE_OK);
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    EXPECT_EQ(checksum(f), before);
    EXPECT_TRUE(text_eq(read_text(HEAVY), heavy));
}

#if !defined(_WIN32)
TEST_F(editor_prefab_source, a_failed_write_is_structured_and_changes_nothing, safety_fx) {
    const pf_world p = prefab_world(f);
    const text crate = read_text(CRATE), heavy = read_text(HEAVY), stack = read_text(STACK);
    /* Loaded prefabs that inherit the field follow the base (DENG-571). */
    (void)spawn_prefab(f, "props/crate_heavy");
    (void)spawn_prefab(f, "props/stack");
    const de_entity a = spawn_prefab(f, "props/crate");
    ((THealth *)de_entity_get(f->engine.world, a, p.health))->current = 70;
    const u64 changed = checksum(f);
    set_mode(HEAVY, 0444); /* the second of three files */
    prefab_fails(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a, (pf_call){.path = "THealth.current"},
                 DE_EP_ERR_INTERNAL, "write");
    EXPECT_EQ(checksum(f), changed);
    EXPECT_TRUE(overrides_of(f, a, a, "THealth.current").found);
    EXPECT_TRUE(text_eq(read_text(CRATE), crate));
    EXPECT_TRUE(text_eq(read_text(HEAVY), heavy));
    EXPECT_TRUE(text_eq(read_text(STACK), stack));
    /* Undo that cannot write: the world and the files stay as they were. */
    set_mode(HEAVY, 0644);
    const u32 tx =
        prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a, (pf_call){.path = "THealth.current"});
    const u64 after = checksum(f);
    const text crate1 = read_text(CRATE), heavy1 = read_text(HEAVY);
    set_mode(HEAVY, 0444);
    transaction_fails(f, DE_EP_METHOD_EDIT_UNDO, tx, DE_EP_ERR_INTERNAL, "write");
    EXPECT_EQ(checksum(f), after);
    EXPECT_EQ(overrides_of(f, a, a, "THealth.current").count, 0u);
    EXPECT_TRUE(text_eq(read_text(CRATE), crate1));
    EXPECT_TRUE(text_eq(read_text(HEAVY), heavy1));
    set_mode(HEAVY, 0644);
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    EXPECT_EQ(checksum(f), changed);
    EXPECT_TRUE(text_eq(read_text(CRATE), crate));
}
#endif

TEST_F(editor_prefab_source, apply_recooks_the_written_prefab, safety_fx) {
    const pf_world p = prefab_world_ex(f, true);
    const de_entity a = spawn_prefab(f, "props/crate");
    ((THealth *)de_entity_get(f->engine.world, a, p.health))->current = 70;
    prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a, (pf_call){.path = "THealth.current"});
    de__asset_reload_stats st;
    const u64 t0 = de_time_now_ns();
    for (;;) {
        de__asset_wake_reset(f->engine.assets);
        de_asset_system_update(f->engine.assets, NULL);
        de__asset_reload_get_stats(f->engine.assets, &st);
        if (st.started != 0 && st.swapped + st.failed + st.superseded + st.discarded >= st.started)
            break;
        ASSERT_TRUE(de_time_now_ns() - t0 < 20000000000ull);
        (void)de__asset_wake_block(f->engine.assets, 10000000u);
    }
    EXPECT_GE(st.swapped, 1u);
    EXPECT_EQ(st.failed, 0u);
    /* The cooked prefab itself has the new value, not only the editor's copy. */
    const de_prefab pf = de_asset_prefab(f->engine.assets, de_asset_id_from("props/crate"));
    de_entity root = DE_ENTITY_NULL;
    ASSERT_EQ(de_prefab_instantiate(f->engine.assets, f->engine.world, pf, DE_ENTITY_NULL, &root),
              DE_OK);
    EXPECT_FLOAT_EQ(((THealth *)de_entity_get(f->engine.world, root, p.health))->current, 70, 0);
}

TEST_F(editor_prefab_source, the_written_bytes_are_deterministic, safety_fx) {
    const pf_world p = prefab_world(f);
    const de_entity a = spawn_prefab(f, "props/crate");
    THealth *h = de_entity_get(f->engine.world, a, p.health);
    h->current = 0.1f; /* not exact: the shortest text that reads back as the same f32 */
    const u32 tx =
        prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a, (pf_call){.path = "THealth.current"});
    const text first = read_text(CRATE), heavy = read_text(HEAVY);
    EXPECT_TRUE(strstr(first.s, "current = 0.1\n") != NULL);
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    h = de_entity_get(f->engine.world, a, p.health);
    h->current = 0.1f;
    prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a, (pf_call){.path = "THealth.current"});
    EXPECT_TRUE(text_eq(read_text(CRATE), first));
    EXPECT_TRUE(text_eq(read_text(HEAVY), heavy));
}

TEST_F(editor_prefab_source, trailing_comments_crlf_and_unsupported_layouts, safety_fx) {
    const pf_world p = prefab_world(f);
    const de_entity a = spawn_prefab(f, "props/crate");
    THealth *h = de_entity_get(f->engine.world, a, p.health);
    /* The file as somebody edited it on Windows, after the cook: comments after values stay. */
    static const char crlf[] =
        "# crate\r\n[[entity]]\r\nid = \"0x1\" # root\r\n[entity.THealth]\r\n"
        "current = 50.0   # hit points\r\nmax = 50.0\r\n\r\n[[entity]]\r\nid = \"0x2\"\r\n"
        "parent = \"0x1\"\r\n[entity.TTarget]\r\ntarget = \"0x1\"\r\ntexture = \"ui/heart\"\r\n"
        "speed = 1.0\r\n";
    ASSERT_EQ(de_file_write_all_atomic(CRATE, crlf, sizeof crlf - 1), DE_OK);
    h->current = 70;
    h->offset = (de_vec3){1, 2.5f, -3};
    prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a, (pf_call){.path = "THealth.current"});
    prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a, (pf_call){.path = "THealth.offset"});
    text want;
    memcpy(want.s, crlf, sizeof crlf);
    want.n = sizeof crlf - 1;
    want = replaced(want, "current = 50.0   # hit points\r\nmax = 50.0\r\n",
                    "current = 70.0   # hit points\r\nmax = 50.0\r\n"
                    "offset = [1.0, 2.5, -3.0]\r\n");
    EXPECT_TRUE(text_eq(read_text(CRATE), want));
    /* Dotted keys are not rewritten: a structured error, and nothing changes. */
    static const char dotted[] = "[[entity]]\nid = \"0x1\"\nTHealth.current = 70.0\n"
                                 "THealth.max = 50.0\n";
    ASSERT_EQ(de_file_write_all_atomic(CRATE, dotted, sizeof dotted - 1), DE_OK);
    h->max = 9;
    const u64 before = checksum(f);
    prefab_fails(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a, (pf_call){.path = "THealth.max"},
                 DE_EP_ERR_UNSUPPORTED, "source_layout");
    EXPECT_EQ(checksum(f), before);
    EXPECT_TRUE(overrides_of(f, a, a, "THealth.max").found);
    EXPECT_EQ(read_text(CRATE).n, sizeof dotted - 1);
}

/* ---------- DENG-803: a reloaded prefab file refreshes the definition store ---------------- */

/* Waits for every queued reload to finish, then tells the editor what the asset events tell it
 * in a running host: the listed assets were reloaded. */
static void reload_and_notify(safety_fx *f, const char *const *assets, u32 count) {
    for (u32 i = 0; i < count; ++i)
        ASSERT_TRUE(de__asset_reload_queue(f->engine.assets, de_asset_id_from(assets[i])));
    de__asset_reload_stats st;
    const u64 t0 = de_time_now_ns();
    for (;;) {
        de__asset_wake_reset(f->engine.assets);
        de_asset_system_update(f->engine.assets, NULL);
        de__asset_reload_get_stats(f->engine.assets, &st);
        if (st.started != 0 && st.swapped + st.failed + st.superseded + st.discarded >= st.started)
            break;
        ASSERT_TRUE(de_time_now_ns() - t0 < 20000000000ull);
        (void)de__asset_wake_block(f->engine.assets, 10000000u);
    }
    EXPECT_EQ(st.failed, 0u);
    for (u32 i = 0; i < count; ++i)
        de__ed_prefab_reloaded(&f->ed, de_asset_id_from(assets[i]));
}

static float health_current(safety_fx *f, const pf_world *p, de_entity e) {
    return ((THealth *)de_entity_get(f->engine.world, e, p->health))->current;
}

TEST_F(editor_prefab_reload, external_edit_updates_definition_instances_and_revision, safety_fx) {
    const pf_world p = prefab_world_ex(f, true);
    const de_entity a = spawn_prefab(f, "props/crate");
    const de_entity b = spawn_prefab(f, "props/crate");
    ((THealth *)de_entity_get(f->engine.world, b, p.health))->max = 99; /* an override */
    const u64 rev = overrides_of(f, a, a, "THealth.current").revision;
    const text crate = read_text(CRATE);
    const text edited = replaced(crate, "current = 50.0", "current = 65.0");
    ASSERT_EQ(de_file_write_all_atomic(CRATE, edited.s, edited.n), DE_OK);
    const char *const changed[] = {"props/crate"};
    reload_and_notify(f, changed, 1);
    /* The instances took the new value; the override stayed. */
    EXPECT_FLOAT_EQ(health_current(f, &p, a), 65, 0);
    EXPECT_FLOAT_EQ(health_current(f, &p, b), 65, 0);
    EXPECT_FLOAT_EQ(((THealth *)de_entity_get(f->engine.world, b, p.health))->max, 99, 0);
    EXPECT_TRUE(overrides_of(f, b, b, "THealth.max").found);
    EXPECT_FALSE(overrides_of(f, a, a, "THealth.current").found);
    /* The revision went up once: the old one is stale. */
    EXPECT_EQ(overrides_of(f, a, a, "THealth.current").revision, rev + 1);
    prefab_fails(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a,
                 (pf_call){.path = "THealth.current", .has_revision = true, .revision = rev},
                 DE_EP_ERR_CONFLICT, "stale");
    /* The definition changed: an apply of another field compares against the new one. */
    ((THealth *)de_entity_get(f->engine.world, a, p.health))->current = 70;
    EXPECT_TRUE(overrides_of(f, a, a, "THealth.current").found);
    /* A reload that changes nothing leaves the revision alone. */
    reload_and_notify(f, changed, 1);
    EXPECT_EQ(overrides_of(f, a, a, "THealth.current").revision, rev + 1);
}

TEST_F(editor_prefab_reload, a_base_reload_refreshes_the_loaded_variant_and_nesting_prefab,
       safety_fx) {
    const pf_world p = prefab_world_ex(f, true);
    const de_entity v = spawn_prefab(f, "props/crate_heavy");
    const de_entity s = spawn_prefab(f, "props/stack");
    const u64 rev = overrides_of(f, v, v, "THealth.max").revision;
    /* A variant file lists its own values: an edit of the base and the variant together, as a
     * checkout would make it. */
    const text crate = read_text(CRATE), heavy = read_text(HEAVY), stack = read_text(STACK);
    text e1 = replaced(crate, "current = 50.0", "current = 65.0");
    text e2 = replaced(heavy, "current = 50.0", "current = 65.0");
    text e3 = replaced(stack, "speed = 2.0", "speed = 3.0");
    e3 = replaced(e3, "current = 50.0", "current = 65.0");
    ASSERT_EQ(de_file_write_all_atomic(CRATE, e1.s, e1.n), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic(HEAVY, e2.s, e2.n), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic(STACK, e3.s, e3.n), DE_OK);
    /* The editor hears about the base only: the variant and the nesting prefab are reloaded
     * by the asset system, and their definitions are read again as dependents of it. */
    const char *const all[] = {"props/crate", "props/crate_heavy", "props/stack"};
    for (u32 i = 0; i < 3; ++i)
        ASSERT_TRUE(de__asset_reload_queue(f->engine.assets, de_asset_id_from(all[i])));
    reload_and_notify(f, all, 0);
    de__ed_prefab_reloaded(&f->ed, de_asset_id_from("props/crate"));
    EXPECT_FLOAT_EQ(health_current(f, &p, v), 65, 0);
    EXPECT_FLOAT_EQ(health_current(f, &p, first_child(f, s)), 65, 0);
    const de_entity leaf = first_child(f, first_child(f, s));
    EXPECT_FLOAT_EQ(((TTarget *)de_entity_get(f->engine.world, leaf, p.target))->speed, 3, 0);
    EXPECT_FALSE(overrides_of(f, v, v, "THealth.current").found);
    EXPECT_EQ(overrides_of(f, v, v, "THealth.max").revision, rev + 1); /* once, not per prefab */
}

TEST_F(editor_prefab_reload, the_editors_own_write_does_not_bump_again_or_fight_undo, safety_fx) {
    const pf_world p = prefab_world_ex(f, true);
    const de_entity a = spawn_prefab(f, "props/crate");
    const de_entity v = spawn_prefab(f, "props/crate_heavy");
    const text crate = read_text(CRATE);
    ((THealth *)de_entity_get(f->engine.world, a, p.health))->current = 70;
    const u32 tx =
        prefab_ok(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, a, (pf_call){.path = "THealth.current"});
    const u64 rev = overrides_of(f, a, a, "THealth.current").revision;
    const char *const written[] = {"props/crate", "props/crate_heavy", "props/stack"};
    reload_and_notify(f, written, 3); /* the recook of what apply wrote */
    EXPECT_EQ(overrides_of(f, a, a, "THealth.current").revision, rev);
    EXPECT_FLOAT_EQ(health_current(f, &p, a), 70, 0);
    EXPECT_FLOAT_EQ(health_current(f, &p, v), 70, 0);
    /* Undo restores the old bytes and value, and its own recook changes nothing more. */
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    const u64 rev2 = overrides_of(f, a, a, "THealth.current").revision;
    EXPECT_EQ(rev2, rev + 1);
    reload_and_notify(f, written, 3);
    EXPECT_EQ(overrides_of(f, a, a, "THealth.current").revision, rev2);
    /* The instance keeps the value it was given: back at the source's value, it is an override
     * again, and nothing was reset behind the user's back. */
    EXPECT_FLOAT_EQ(health_current(f, &p, a), 70, 0);
    EXPECT_TRUE(overrides_of(f, a, a, "THealth.current").found);
    EXPECT_TRUE(text_eq(read_text(CRATE), crate));
}

/* ---------- DENG-799: prefab instance links in scene text ---------- */

typedef struct pf_big {
    mpack_tree_t tree;
    mpack_node_t root;
    char buffer[65536];
} pf_big;

static pf_big g_big;
static char g_params799[65536];

static void big_call(safety_fx *f, de_ep_method method, const char *params, size_t size) {
    const de_ep_request req = {.id = 799,
                               .method = de_ep_cstr(de_ep_method_name(method)),
                               .params = (de_ep_raw){params, size}};
    de_ep_arena arena = {f->arena, sizeof f->arena, 0};
    mpack_writer_t w;
    mpack_writer_init(&w, g_big.buffer, sizeof g_big.buffer);
    ASSERT_TRUE(f->ed.handlers[method] != NULL);
    f->ed.handlers[method](&f->ed, &req, &arena, &w);
    const size_t used = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_init_data(&g_big.tree, g_big.buffer, used);
    mpack_tree_parse(&g_big.tree);
    ASSERT_EQ(mpack_tree_error(&g_big.tree), mpack_ok);
    g_big.root = mpack_tree_root(&g_big.tree);
}

static bool big_failed(void) {
    return mpack_node_map_contains_cstr(g_big.root, "error");
}

static char g_saved799[32768];

/* scene.serialize of `scene` into g_saved799. */
static const char *scene_text(safety_fx *f, u64 scene) {
    mpack_writer_t w;
    mpack_writer_init(&w, g_params799, sizeof g_params799);
    const de_ep_scene_params q = {scene};
    de_ep_write_scene_params(&w, &q);
    const size_t n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    big_call(f, DE_EP_METHOD_SCENE_SERIALIZE, g_params799, n);
    ASSERT_FALSE(big_failed());
    const mpack_node_t t = mpack_node_map_cstr(mpack_node_map_cstr(g_big.root, "result"), "text");
    const size_t len = mpack_node_strlen(t);
    ASSERT_TRUE(len < sizeof g_saved799);
    memcpy(g_saved799, mpack_node_str(t), len);
    g_saved799[len] = '\0';
    EXPECT_EQ(mpack_tree_destroy(&g_big.tree), mpack_ok);
    return g_saved799;
}

/* scene.reloadFromText; true on success. */
static bool scene_reload(safety_fx *f, u64 scene, const char *text) {
    mpack_writer_t w;
    mpack_writer_init(&w, g_params799, sizeof g_params799);
    const de_ep_scene_reload_params q = {scene, de_ep_cstr(text)};
    de_ep_write_scene_reload_params(&w, &q);
    const size_t n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    big_call(f, DE_EP_METHOD_SCENE_RELOAD_FROM_TEXT, g_params799, n);
    const bool ok = !big_failed();
    EXPECT_EQ(mpack_tree_destroy(&g_big.tree), mpack_ok);
    return ok;
}

/* scene.load (additive) of `path`; the new scene id, 0 on failure. */
static u64 scene_load(safety_fx *f, const char *path) {
    mpack_writer_t w;
    mpack_writer_init(&w, g_params799, sizeof g_params799);
    const de_ep_scene_load_params q = {de_ep_cstr(path), true};
    de_ep_write_scene_load_params(&w, &q);
    const size_t n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    big_call(f, DE_EP_METHOD_SCENE_LOAD, g_params799, n);
    const u64 id = big_failed()
                       ? 0
                       : mpack_node_u64(mpack_node_map_cstr(
                             mpack_node_map_cstr(g_big.root, "result"), "scene"));
    EXPECT_EQ(mpack_tree_destroy(&g_big.tree), mpack_ok);
    return id;
}

static u64 sid_of(de_entity e) {
    u64 scene = 0, sid = 0;
    EXPECT_TRUE(de__ed_scene_member(e.bits, &scene, &sid));
    return sid;
}

/* The live entity of `scene` with scene ID `sid`, or DE_ENTITY_NULL. */
static de_entity entity_of(safety_fx *f, u64 scene, u64 sid) {
    de_world *w = f->engine.world;
    for (u32 ai = 0; ai < w->archetypes.count; ++ai) {
        const ecs_archetype *a = ((ecs_archetype *const *)w->archetypes.data)[ai];
        for (u32 c = 0; c < a->chunks.count; ++c) {
            const ecs_chunk *ch = ecs_table_chunk(a, c);
            for (u32 r = 0; r < ch->count; ++r) {
                const de_entity e = ecs_chunk_entities(ch)[r];
                u64 sc = 0, s = 0;
                if (de__ed_scene_member(e.bits, &sc, &s) && sc == scene && s == sid)
                    return e;
            }
        }
    }
    return DE_ENTITY_NULL;
}

/* One override, by scene ID so it compares across a reload. */
typedef struct pf_ov {
    u64 sid;
    char path[64], source[64];
    char value[128], base[128];
} pf_ov;

/* A printable form of a MessagePack value, to compare values across a reload. */
static void text_put(char *out, size_t cap, size_t *at, const char *one) {
    const size_t k = strlen(one);
    if (*at + k < cap) {
        memcpy(out + *at, one, k + 1);
        *at += k;
    }
}

static void node_text(mpack_node_t n, char *out, size_t cap, size_t *at) {
    char one[80] = {0};
    switch (mpack_node_type(n)) {
    case mpack_type_uint:
        snprintf(one, sizeof one, "u%llu,", (unsigned long long)mpack_node_u64(n));
        break;
    case mpack_type_int:
        snprintf(one, sizeof one, "i%lld,", (long long)mpack_node_i64(n));
        break;
    case mpack_type_float:
        snprintf(one, sizeof one, "f%.9g,", (double)mpack_node_float(n));
        break;
    case mpack_type_double:
        snprintf(one, sizeof one, "d%.17g,", mpack_node_double(n));
        break;
    case mpack_type_bool:
        snprintf(one, sizeof one, "b%d,", mpack_node_bool(n) ? 1 : 0);
        break;
    case mpack_type_str:
        snprintf(one, sizeof one, "s%.*s,", (int)mpack_node_strlen(n), mpack_node_str(n));
        break;
    case mpack_type_array:
        text_put(out, cap, at, "[");
        for (size_t i = 0; i < mpack_node_array_length(n); ++i)
            node_text(mpack_node_array_at(n, i), out, cap, at);
        snprintf(one, sizeof one, "],");
        break;
    case mpack_type_map:
        text_put(out, cap, at, "{");
        for (size_t i = 0; i < mpack_node_map_count(n); ++i) {
            node_text(mpack_node_map_key_at(n, i), out, cap, at);
            node_text(mpack_node_map_value_at(n, i), out, cap, at);
        }
        snprintf(one, sizeof one, "},");
        break;
    default:
        snprintf(one, sizeof one, "t%d,", (int)mpack_node_type(n));
        break;
    }
    text_put(out, cap, at, one);
}

static int cmp_ov(const void *x, const void *y) {
    const pf_ov *a = x, *b = y;
    if (a->sid != b->sid)
        return a->sid < b->sid ? -1 : 1;
    return strcmp(a->path, b->path);
}

/* The overrides of the instance at `root`, sorted by (scene ID, path); -1 if not an instance. */
static int overrides_all(safety_fx *f, de_entity root, pf_ov out[16]) {
    char response[2048];
    mpack_tree_t tree;
    mpack_node_t r = prefab_call(f, DE_EP_METHOD_PREFAB_OVERRIDES, root, (pf_call){0}, &tree,
                                 response);
    if (!mpack_node_map_contains_cstr(r, "result")) {
        EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
        return -1;
    }
    mpack_node_t list = mpack_node_map_cstr(mpack_node_map_cstr(r, "result"), "overrides");
    u32 n = (u32)mpack_node_array_length(list);
    if (n > 16)
        n = 16;
    for (u32 i = 0; i < n; ++i) {
        mpack_node_t o = mpack_node_array_at(list, i);
        pf_ov *v = &out[i];
        memset(v, 0, sizeof *v);
        v->sid = sid_of((de_entity){mpack_node_u64(mpack_node_map_cstr(o, "entity"))});
        mpack_node_t pth = mpack_node_map_cstr(o, "path");
        snprintf(v->path, sizeof v->path, "%.*s", (int)mpack_node_strlen(pth),
                 mpack_node_str(pth));
        mpack_node_t src = mpack_node_map_cstr(o, "source");
        snprintf(v->source, sizeof v->source, "%.*s", (int)mpack_node_strlen(src),
                 mpack_node_str(src));
        size_t at = 0;
        node_text(mpack_node_map_cstr(o, "value"), v->value, sizeof v->value, &at);
        at = 0;
        node_text(mpack_node_map_cstr(o, "base"), v->base, sizeof v->base, &at);
    }
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    qsort(out, n, sizeof(pf_ov), cmp_ov);
    return (int)n;
}

static void expect_same_overrides(const pf_ov *a, int na, const pf_ov *b, int nb) {
    ASSERT_EQ(na, nb);
    for (int i = 0; i < na; ++i) {
        EXPECT_EQ(a[i].sid, b[i].sid);
        EXPECT_STREQ(a[i].path, b[i].path);
        EXPECT_STREQ(a[i].source, b[i].source);
        EXPECT_STREQ(a[i].value, b[i].value);
        EXPECT_STREQ(a[i].base, b[i].base);
    }
}

static size_t count_of(const char *text, const char *what) {
    size_t n = 0;
    for (const char *at = strstr(text, what); at != NULL; at = strstr(at + 1, what))
        ++n;
    return n;
}

TEST_F(editor_prefab_links, save_and_reload_keep_the_links_and_overrides, safety_fx) {
    const pf_world p = prefab_world(f);
    const de_entity s = spawn_prefab(f, "props/stack");
    const de_entity inner = first_child(f, s);
    const de_entity leaf = first_child(f, inner);
    ASSERT_TRUE(inner.bits != 0 && leaf.bits != 0);
    ((TTarget *)de_entity_get(f->engine.world, leaf, p.target))->speed = 3;
    ((THealth *)de_entity_get(f->engine.world, inner, p.health))->max = 61;
    const de_entity v = spawn_prefab(f, "props/crate_heavy");
    ((THealth *)de_entity_get(f->engine.world, v, p.health))->max = 1;
    pf_ov os[16], ov[16], got[16];
    const int ns = overrides_all(f, s, os), nv = overrides_all(f, v, ov);
    ASSERT_EQ(ns, 2);
    ASSERT_EQ(nv, 1);
    const u64 s_sid = sid_of(s), v_sid = sid_of(v);
    static char saved[32768];
    snprintf(saved, sizeof saved, "%s", scene_text(f, 1));
    EXPECT_EQ(count_of(saved, "[[prefab.nested]]\n"), (size_t)2);
    EXPECT_TRUE(strstr(saved, "asset = \"props/stack\"\n") != NULL);
    EXPECT_TRUE(strstr(saved, "asset = \"props/crate_heavy\"\n") != NULL);

    /* scene.reloadFromText: new entities, the same links and overrides, the same bytes. */
    ASSERT_TRUE(scene_reload(f, 1, saved));
    const de_entity s2 = entity_of(f, 1, s_sid), v2 = entity_of(f, 1, v_sid);
    ASSERT_TRUE(s2.bits != 0 && v2.bits != 0);
    EXPECT_TRUE(s2.bits != s.bits);
    expect_same_overrides(got, overrides_all(f, s2, got), os, ns);
    expect_same_overrides(got, overrides_all(f, v2, got), ov, nv);
    EXPECT_STREQ(scene_text(f, 1), saved);
    /* The nested crate is no instance root of its own after the reload either. */
    prefab_fails(f, DE_EP_METHOD_PREFAB_UNPACK, first_child(f, s2), (pf_call){0},
                 DE_EP_ERR_NOT_FOUND, "not_instance");

    /* From a file: scene.load reads the links too. */
    char path[256];
    snprintf(path, sizeof path, "%s/level799.scene.toml", f->copy);
    ASSERT_EQ(de_file_write_all_atomic(path, saved, strlen(saved)), DE_OK);
    const u64 sc = scene_load(f, path);
    ASSERT_TRUE(sc != 0);
    const de_entity s3 = entity_of(f, sc, s_sid), v3 = entity_of(f, sc, v_sid);
    ASSERT_TRUE(s3.bits != 0 && v3.bits != 0);
    expect_same_overrides(got, overrides_all(f, s3, got), os, ns);
    expect_same_overrides(got, overrides_all(f, v3, got), ov, nv);
    EXPECT_STREQ(scene_text(f, sc), saved);

    /* A loaded link works like a live one: revert puts back the prefab's value. */
    const u32 tx = prefab_ok(f, DE_EP_METHOD_PREFAB_REVERT_OVERRIDE, v3,
                             (pf_call){.path = "THealth.max"});
    EXPECT_EQ(overrides_all(f, v3, got), 0);
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    EXPECT_EQ(overrides_all(f, v3, got), 1);
}

TEST_F(editor_prefab_links, scenes_without_links_still_load, safety_fx) {
    (void)prefab_world(f);
    const de_entity a = spawn_prefab(f, "props/crate");
    const u64 a_sid = sid_of(a);
    static char saved[32768];
    snprintf(saved, sizeof saved, "%s", scene_text(f, 1));
    char *link = strstr(saved, "\n[[prefab.nested]]\n");
    ASSERT_TRUE(link != NULL);
    *link = '\0'; /* the text as it was before DENG-799 */
    ASSERT_TRUE(scene_reload(f, 1, saved));
    const de_entity a2 = entity_of(f, 1, a_sid);
    ASSERT_TRUE(a2.bits != 0);
    pf_ov got[16];
    EXPECT_EQ(overrides_all(f, a2, got), -1);
    EXPECT_STREQ(scene_text(f, 1), saved);
    /* A link to an entity that is not in the text, or a malformed one, is skipped. */
    static char bad[32768];
    snprintf(bad, sizeof bad,
             "%s\n[[prefab.nested]]\nasset = \"props/crate\"\nroot = \"0x99\"\nids = []\n"
             "\n[[prefab.nested]]\nasset = 7\n",
             saved);
    ASSERT_TRUE(scene_reload(f, 1, bad));
    EXPECT_STREQ(scene_text(f, 1), saved);
    /* An authored scene of the old layout. */
    EXPECT_TRUE(scene_load(f, "safe676:/assets/level_keyed.scene.toml") != 0);
}

TEST_F(editor_prefab_links, an_unpacked_instance_saves_no_link, safety_fx) {
    (void)prefab_world(f);
    const de_entity a = spawn_prefab(f, "props/crate");
    const de_entity b = spawn_prefab(f, "props/crate");
    const u64 a_sid = sid_of(a), b_sid = sid_of(b);
    (void)prefab_ok(f, DE_EP_METHOD_PREFAB_UNPACK, a, (pf_call){0});
    static char saved[32768];
    snprintf(saved, sizeof saved, "%s", scene_text(f, 1));
    EXPECT_EQ(count_of(saved, "[[prefab.nested]]\n"), (size_t)1);
    char root_line[64];
    snprintf(root_line, sizeof root_line, "root = \"0x%016llx\"\n", (unsigned long long)b_sid);
    EXPECT_TRUE(strstr(saved, root_line) != NULL);
    ASSERT_TRUE(scene_reload(f, 1, saved));
    pf_ov got[16];
    EXPECT_EQ(overrides_all(f, entity_of(f, 1, a_sid), got), -1);
    EXPECT_EQ(overrides_all(f, entity_of(f, 1, b_sid), got), 0);
    EXPECT_STREQ(scene_text(f, 1), saved);
}
#endif

#endif

/* ---------- DENG-810: structural overrides of components (SPEC 9.2, DIDE-R37) ---------- */

static bool has_comp(safety_fx *f, de_entity e, de_component c) {
    return de_entity_has(f->engine.world, e, c);
}

/* Applies a structural override and expects success; returns the tx and the files written. */
static u32 apply_files(safety_fx *f, de_entity e, pf_call c, const char *const *names, u32 n) {
    char response[2048];
    mpack_tree_t tree;
    mpack_node_t root = prefab_call(f, DE_EP_METHOD_PREFAB_APPLY_OVERRIDE, e, c, &tree, response);
    if (mpack_node_map_contains_cstr(root, "error")) {
        mpack_node_t m = mpack_node_map_cstr(mpack_node_map_cstr(root, "error"), "message");
        printf("prefab error: %.*s\n", (int)mpack_node_strlen(m), mpack_node_str(m));
    }
    ASSERT_TRUE(mpack_node_map_contains_cstr(root, "result"));
    mpack_node_t res = mpack_node_map_cstr(root, "result");
    const u32 tx = mpack_node_u32(mpack_node_map_cstr(res, "tx"));
    mpack_node_t files = mpack_node_map_cstr(res, "files");
    EXPECT_EQ(mpack_node_array_length(files), (size_t)n);
    for (u32 i = 0; i < n; ++i)
        EXPECT_TRUE(files_has(files, names[i]));
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    return tx;
}

/* `t` is `base` with `tail` appended. */
static bool appended(text t, text base, const char *tail) {
    const size_t k = strlen(tail);
    return t.n == base.n + k && memcmp(t.s, base.s, base.n) == 0 &&
           memcmp(t.s + base.n, tail, k) == 0;
}

#define CRATE_TARGET "[entity.TTarget]\ntarget = \"0x1\"\ntexture = \"ui/heart\"\nspeed = 1.0\n"
#define ADDED_HEALTH "[entity.THealth]\ncurrent = 7.0\nmax = 9.0\n"

TEST_F(editor_prefab_structural, a_removed_component_is_listed_reverted_and_applied, safety_fx) {
    const pf_world p = prefab_world(f);
    const text crate = read_text(CRATE), heavy = read_text(HEAVY), stack = read_text(STACK);
    const de_entity v = spawn_prefab(f, "props/crate_heavy");
    const de_entity s = spawn_prefab(f, "props/stack");
    const de_entity a = spawn_prefab(f, "props/crate"), b = spawn_prefab(f, "props/crate");
    const de_entity ca = first_child(f, a), cb = first_child(f, b), cv = first_child(f, v);
    const de_entity leaf = first_child(f, first_child(f, s));
    ASSERT_TRUE(ca.bits != 0 && cb.bits != 0 && cv.bits != 0 && leaf.bits != 0);
    EXPECT_EQ(overrides_of(f, a, ca, "TTarget").count, 0u);

    de_entity_remove(f->engine.world, ca, p.target);
    pf_found o = overrides_of(f, a, ca, "TTarget");
    EXPECT_EQ(o.count, 1u);
    ASSERT_TRUE(o.found);
    EXPECT_STREQ(o.kind, "component_removed");
    EXPECT_STREQ(o.source, "props/crate");
    EXPECT_EQ(o.levels, 1u);
    EXPECT_EQ(o.ids[0], 0x2u);
    EXPECT_EQ((int)o.value_type, (int)mpack_type_nil);
    EXPECT_EQ((int)o.base_type, (int)mpack_type_map);
    EXPECT_EQ(o.base_count, 3u);

    /* Revert puts the prefab's component back, remapped, without a new prefab revision. */
    const u64 rev = o.revision;
    const pf_call call = {.path = "TTarget", .kind = "component_removed"};
    const u32 rt = prefab_ok(f, DE_EP_METHOD_PREFAB_REVERT_OVERRIDE, ca, call);
    ASSERT_TRUE(has_comp(f, ca, p.target));
    const TTarget *t = de_entity_get(f->engine.world, ca, p.target);
    EXPECT_EQ(t->target.bits, a.bits);
    EXPECT_FLOAT_EQ(t->speed, 1.0f, 1e-6f);
    o = overrides_of(f, a, ca, "TTarget");
    EXPECT_FALSE(o.found);
    EXPECT_EQ(o.revision, rev);
    transaction(f, DE_EP_METHOD_EDIT_UNDO, rt);
    EXPECT_FALSE(has_comp(f, ca, p.target));
    EXPECT_TRUE(overrides_of(f, a, ca, "TTarget").found);

    /* Apply removes the table from the prefab and from the variant, which inherits it; the
     * nesting prefab sets its own TTarget (speed 2.0) and keeps it. Every instance follows. */
    static const char *const names[] = {"crate.prefab.toml", "crate_heavy.prefab.toml"};
    const u32 tx = apply_files(f, ca, call, names, 2);
    const text crate1 = read_text(CRATE), heavy1 = read_text(HEAVY);
    EXPECT_TRUE(text_eq(crate1, replaced(crate, CRATE_TARGET, "")));
    EXPECT_TRUE(text_eq(heavy1, replaced(heavy, CRATE_TARGET, "")));
    EXPECT_TRUE(text_eq(read_text(STACK), stack));
    EXPECT_FALSE(has_comp(f, cb, p.target));
    EXPECT_FALSE(has_comp(f, cv, p.target));
    EXPECT_TRUE(has_comp(f, leaf, p.target));
    EXPECT_EQ(overrides_of(f, a, ca, "TTarget").count, 0u);
    EXPECT_EQ(overrides_of(f, b, cb, "TTarget").count, 0u);
    const u64 after = checksum(f);

    /* Undo restores the bytes and the instances; redo writes the same bytes again. */
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    EXPECT_TRUE(text_eq(read_text(CRATE), crate));
    EXPECT_TRUE(text_eq(read_text(HEAVY), heavy));
    EXPECT_TRUE(text_eq(read_text(STACK), stack));
    EXPECT_TRUE(has_comp(f, cb, p.target));
    EXPECT_TRUE(has_comp(f, cv, p.target));
    EXPECT_FALSE(has_comp(f, ca, p.target));
    EXPECT_TRUE(overrides_of(f, a, ca, "TTarget").found);
    EXPECT_EQ(overrides_of(f, b, cb, "TTarget").count, 0u);
    transaction(f, DE_EP_METHOD_EDIT_REDO, tx);
    EXPECT_TRUE(text_eq(read_text(CRATE), crate1));
    EXPECT_TRUE(text_eq(read_text(HEAVY), heavy1));
    EXPECT_TRUE(text_eq(read_text(STACK), stack));
    EXPECT_EQ(checksum(f), after);
}

TEST_F(editor_prefab_structural, an_added_component_is_listed_reverted_and_applied, safety_fx) {
    const pf_world p = prefab_world(f);
    const text crate = read_text(CRATE), heavy = read_text(HEAVY), stack = read_text(STACK);
    const de_entity a = spawn_prefab(f, "props/crate"), b = spawn_prefab(f, "props/crate");
    const de_entity ca = first_child(f, a), cb = first_child(f, b);
    ASSERT_TRUE(ca.bits != 0 && cb.bits != 0);
    THealth *h = de_entity_add(f->engine.world, ca, p.health);
    h->current = 7;
    h->max = 9;
    pf_found o = overrides_of(f, a, ca, "THealth");
    EXPECT_EQ(o.count, 1u);
    ASSERT_TRUE(o.found);
    EXPECT_STREQ(o.kind, "component_added");
    EXPECT_STREQ(o.source, "props/crate");
    EXPECT_EQ((int)o.value_type, (int)mpack_type_map);
    EXPECT_EQ(o.value_count, 5u);
    EXPECT_EQ((int)o.base_type, (int)mpack_type_nil);
    /* No field rows for the added component's fields. */
    EXPECT_FALSE(overrides_of(f, a, ca, "THealth.current").found);

    const pf_call call = {.path = "THealth", .kind = "component_added"};
    const u32 rt = prefab_ok(f, DE_EP_METHOD_PREFAB_REVERT_OVERRIDE, ca, call);
    EXPECT_FALSE(has_comp(f, ca, p.health));
    EXPECT_EQ(overrides_of(f, a, ca, "THealth").count, 0u);
    transaction(f, DE_EP_METHOD_EDIT_UNDO, rt);
    ASSERT_TRUE(has_comp(f, ca, p.health));
    EXPECT_FLOAT_EQ(((THealth *)de_entity_get(f->engine.world, ca, p.health))->current, 7.0f,
                    1e-6f);

    /* Apply adds the table to the prefab; the unloaded variant and the nesting prefab lack the
     * component too, so they follow. The other instance gains it. */
    static const char *const names[] = {"crate.prefab.toml", "crate_heavy.prefab.toml",
                                        "stack.prefab.toml"};
    const u32 tx = apply_files(f, ca, call, names, 3);
    const text crate1 = read_text(CRATE), heavy1 = read_text(HEAVY), stack1 = read_text(STACK);
    EXPECT_TRUE(strncmp(crate1.s + crate.n, ADDED_HEALTH, strlen(ADDED_HEALTH)) == 0);
    EXPECT_TRUE(memcmp(crate1.s, crate.s, crate.n) == 0);
    EXPECT_TRUE(appended(heavy1, heavy, crate1.s + crate.n));
    EXPECT_TRUE(appended(stack1, stack, crate1.s + crate.n));
    ASSERT_TRUE(has_comp(f, cb, p.health));
    EXPECT_FLOAT_EQ(((THealth *)de_entity_get(f->engine.world, cb, p.health))->max, 9.0f, 1e-6f);
    EXPECT_EQ(overrides_of(f, a, ca, "THealth").count, 0u);
    const u64 after = checksum(f);

    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    EXPECT_TRUE(text_eq(read_text(CRATE), crate));
    EXPECT_TRUE(text_eq(read_text(HEAVY), heavy));
    EXPECT_TRUE(text_eq(read_text(STACK), stack));
    EXPECT_FALSE(has_comp(f, cb, p.health));
    EXPECT_TRUE(overrides_of(f, a, ca, "THealth").found);
    transaction(f, DE_EP_METHOD_EDIT_REDO, tx);
    EXPECT_TRUE(text_eq(read_text(CRATE), crate1));
    EXPECT_TRUE(text_eq(read_text(HEAVY), heavy1));
    EXPECT_TRUE(text_eq(read_text(STACK), stack1));
    EXPECT_EQ(checksum(f), after);
}

TEST_F(editor_prefab_structural, a_variant_applies_to_its_base, safety_fx) {
    const pf_world p = prefab_world(f);
    const text crate = read_text(CRATE), heavy = read_text(HEAVY), stack = read_text(STACK);
    const de_entity v = spawn_prefab(f, "props/crate_heavy");
    const de_entity cv = first_child(f, v);
    ASSERT_TRUE(cv.bits != 0);
    de_entity_remove(f->engine.world, cv, p.target);
    const pf_found o = overrides_of(f, v, cv, "TTarget");
    ASSERT_TRUE(o.found);
    EXPECT_STREQ(o.kind, "component_removed");
    EXPECT_STREQ(o.source, "props/crate_heavy");
    ASSERT_EQ(o.levels, 2u);
    EXPECT_STREQ(o.chain[0], "props/crate_heavy");
    EXPECT_STREQ(o.chain[1], "props/crate");
    static const char *const names[] = {"crate.prefab.toml", "crate_heavy.prefab.toml"};
    const u32 tx = apply_files(
        f, cv, (pf_call){.path = "TTarget", .kind = "component_removed", .target = "props/crate"},
        names, 2);
    EXPECT_TRUE(text_eq(read_text(CRATE), replaced(crate, CRATE_TARGET, "")));
    EXPECT_TRUE(text_eq(read_text(HEAVY), replaced(heavy, CRATE_TARGET, "")));
    EXPECT_TRUE(text_eq(read_text(STACK), stack));
    EXPECT_EQ(overrides_of(f, v, cv, "TTarget").count, 0u);
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    EXPECT_TRUE(text_eq(read_text(CRATE), crate));
    EXPECT_TRUE(text_eq(read_text(HEAVY), heavy));
    EXPECT_TRUE(overrides_of(f, v, cv, "TTarget").found);
}

TEST_F(editor_prefab_structural, a_nested_instance_applies_to_the_nested_prefab, safety_fx) {
    const pf_world p = prefab_world(f);
    const text crate = read_text(CRATE), heavy = read_text(HEAVY), stack = read_text(STACK);
    const de_entity s = spawn_prefab(f, "props/stack");
    const de_entity leaf = first_child(f, first_child(f, s));
    ASSERT_TRUE(leaf.bits != 0);
    THealth *h = de_entity_add(f->engine.world, leaf, p.health);
    h->current = 7;
    h->max = 9;
    const pf_found o = overrides_of(f, s, leaf, "THealth");
    ASSERT_TRUE(o.found);
    EXPECT_STREQ(o.kind, "component_added");
    EXPECT_STREQ(o.source, "props/stack");
    ASSERT_EQ(o.levels, 2u);
    EXPECT_STREQ(o.chain[0], "props/stack");
    EXPECT_STREQ(o.chain[1], "props/crate");
    EXPECT_EQ(o.ids[0], 0x12u);
    EXPECT_EQ(o.ids[1], 0x2u);
    static const char *const names[] = {"crate.prefab.toml", "crate_heavy.prefab.toml",
                                        "stack.prefab.toml"};
    const u32 tx = apply_files(
        f, leaf, (pf_call){.path = "THealth", .kind = "component_added", .target = "props/crate"},
        names, 3);
    const text crate1 = read_text(CRATE);
    EXPECT_TRUE(memcmp(crate1.s, crate.s, crate.n) == 0);
    EXPECT_TRUE(strncmp(crate1.s + crate.n, ADDED_HEALTH, strlen(ADDED_HEALTH)) == 0);
    EXPECT_TRUE(appended(read_text(HEAVY), heavy, crate1.s + crate.n));
    EXPECT_TRUE(appended(read_text(STACK), stack, crate1.s + crate.n));
    EXPECT_EQ(overrides_of(f, s, leaf, "THealth").count, 0u);
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    EXPECT_TRUE(text_eq(read_text(CRATE), crate));
    EXPECT_TRUE(text_eq(read_text(HEAVY), heavy));
    EXPECT_TRUE(text_eq(read_text(STACK), stack));
    EXPECT_TRUE(overrides_of(f, s, leaf, "THealth").found);
}

/* Entity ids change on a reload: the expected bases name entity `to` where they named `from`. */
static void rebase(pf_ov *o, int n, u64 from, u64 to) {
    char was[32], now[32];
    snprintf(was, sizeof was, "u%llu,", (unsigned long long)from);
    snprintf(now, sizeof now, "u%llu,", (unsigned long long)to);
    for (int i = 0; i < n; ++i) {
        char *at = strstr(o[i].base, was);
        if (at == NULL)
            continue;
        char rest[128];
        snprintf(rest, sizeof rest, "%s", at + strlen(was));
        snprintf(at, sizeof o[i].base - (size_t)(at - o[i].base), "%s%s", now, rest);
    }
}

TEST_F(editor_prefab_structural, save_and_reload_keep_the_structural_overrides, safety_fx) {
    const pf_world p = prefab_world(f);
    const de_entity s = spawn_prefab(f, "props/stack");
    const de_entity leaf = first_child(f, first_child(f, s));
    const de_entity a = spawn_prefab(f, "props/crate");
    const de_entity ca = first_child(f, a);
    ASSERT_TRUE(leaf.bits != 0 && ca.bits != 0);
    de_entity_remove(f->engine.world, leaf, p.target);
    THealth *h = de_entity_add(f->engine.world, ca, p.health);
    h->current = 7;
    h->max = 9;
    pf_ov os[16], oa[16], got[16];
    const int ns = overrides_all(f, s, os), na = overrides_all(f, a, oa);
    ASSERT_EQ(ns, 1);
    ASSERT_EQ(na, 1);
    EXPECT_STREQ(os[0].path, "TTarget");
    EXPECT_STREQ(oa[0].path, "THealth");
    const u64 s_sid = sid_of(s), a_sid = sid_of(a), ca_sid = sid_of(ca);
    const u64 inner = first_child(f, s).bits;
    static char saved[32768];
    snprintf(saved, sizeof saved, "%s", scene_text(f, 1));

    ASSERT_TRUE(scene_reload(f, 1, saved));
    const de_entity s2 = entity_of(f, 1, s_sid), a2 = entity_of(f, 1, a_sid);
    ASSERT_TRUE(s2.bits != 0 && a2.bits != 0);
    /* The removed TTarget's base targets the nested crate root, which is a new entity now. */
    rebase(os, ns, inner, first_child(f, s2).bits);
    expect_same_overrides(got, overrides_all(f, s2, got), os, ns);
    expect_same_overrides(got, overrides_all(f, a2, got), oa, na);
    EXPECT_STREQ(scene_text(f, 1), saved);

    char path[256];
    snprintf(path, sizeof path, "%s/level810.scene.toml", f->copy);
    ASSERT_EQ(de_file_write_all_atomic(path, saved, strlen(saved)), DE_OK);
    const u64 sc = scene_load(f, path);
    ASSERT_TRUE(sc != 0);
    const de_entity s3 = entity_of(f, sc, s_sid), a3 = entity_of(f, sc, a_sid);
    const de_entity ca3 = entity_of(f, sc, ca_sid);
    ASSERT_TRUE(s3.bits != 0 && a3.bits != 0 && ca3.bits != 0);
    rebase(os, ns, first_child(f, s2).bits, first_child(f, s3).bits);
    expect_same_overrides(got, overrides_all(f, s3, got), os, ns);
    expect_same_overrides(got, overrides_all(f, a3, got), oa, na);
    EXPECT_STREQ(scene_text(f, sc), saved);

    /* A loaded structural override reverts like a live one. */
    const u32 tx = prefab_ok(f, DE_EP_METHOD_PREFAB_REVERT_OVERRIDE, ca3,
                             (pf_call){.path = "THealth", .kind = "component_added"});
    EXPECT_FALSE(has_comp(f, ca3, p.health));
    EXPECT_EQ(overrides_all(f, a3, got), 0);
    transaction(f, DE_EP_METHOD_EDIT_UNDO, tx);
    EXPECT_EQ(overrides_all(f, a3, got), 1);
}
