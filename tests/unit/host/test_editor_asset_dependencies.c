#include "de_test.h"
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include "asset/slots_internal.h"
#include "cook/importers/model/model_import.h"
#include "render/formats/de_cooked_mesh.h"
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <stdio.h>

/* Real model cook and asset pipeline; CPU loaders retain bytes and declare the actual mesh
 * material references. No renderer, invented catalog IDs or private IDE fixture required. */
static de_result bytes_load(de_asset_load_ctx *ctx) {
    u64 size = 0;
    const void *data = de_asset_load_ctx_data(ctx, &size);
    if (de_asset_load_ctx_user(ctx)) {
        de_cooked_mesh_view mesh;
        if (!de_cooked_mesh_parse(data, size, &mesh, NULL))
            return DE_ERR_PARSE;
        for (u32 i = 0; i < mesh.submesh_count; ++i) {
            de_asset_id material = {de_cooked_mesh_submesh(&mesh, i).material_id};
            if (material.hash)
                DE_TRY(de_asset_load_ctx_add_dependency(ctx, material));
        }
    }
    void *copy = DE_ALLOC(de_heap_allocator(), (size_t)size + sizeof(u64), 16, DE_MEM_ASSET);
    if (!copy)
        return DE_ERR_OUT_OF_MEMORY;
    *(u64 *)copy = size;
    memcpy((u8 *)copy + sizeof(u64), data, (size_t)size);
    de_asset_load_ctx_set_object(ctx, copy, size + sizeof(u64));
    return DE_OK;
}
static void bytes_unload(void *object, void *user) {
    (void)user;
    DE_FREE(de_heap_allocator(), object, (size_t)*(u64 *)object + sizeof(u64));
}
typedef struct dependency_fx {
    de_engine engine;
    de__host host;
    de__editor editor;
    de_load_handle load;
    u64 arena[16384]; /* Match the production allocator-backed arena alignment. */
} dependency_fx;
static void dependency_fx_setup(dependency_fx *f) {
    ASSERT_EQ(de_log_init(&(de_log_desc){.no_console_sink = true}), DE_OK);
    ASSERT_EQ(de_fs_mount("depdata", DE_EDITOR_DEP_DATA, DE_MOUNT_READ_ONLY, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("deptmp", DE_EDITOR_DEP_TMP, DE_MOUNT_WRITABLE, 0), DE_OK);
    void *source = NULL;
    u64 size = 0;
    ASSERT_EQ(
        de_file_read_all("depdata:/output_selection.glb", de_heap_allocator(), &source, &size),
        DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("deptmp:/model.glb", source, size), DE_OK);
    DE_FREE(de_heap_allocator(), source, (size_t)size);
    const char *manifest = "[project]\nasset_root=\".\"\nversion=1\n[assets]\nhero={type=\"model\","
                           "source=\"model.glb\"}\n";
    ASSERT_EQ(de_file_write_all_atomic("deptmp:/assets.toml", manifest, strlen(manifest)), DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.cache_dir = "deptmp:/cache",
                                                             .no_builtin_types = true,
                                                             .staging_ring_bytes = 1048576},
                                     &f->engine.assets),
              DE_OK);
    const char *types[] = {"model", "mesh", "material", "skeleton", "anim_clip", "prefab"};
    for (u32 i = 0; i < 6; ++i) {
        de_asset_type_desc type = {.name = types[i], .load = bytes_load, .unload = bytes_unload};
        if (!i) {
            type.import = de_model_import;
            type.importer_version = DE_MODEL_IMPORTER_VERSION;
        }
        if (i == 1)
            type.user = f; /* Mesh loader derives dependencies from the actual cooked bytes. */
        ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(f->engine.assets, &type)));
    }
    ASSERT_EQ(de_asset_manifest_load(f->engine.assets, "deptmp:/assets.toml"), DE_OK);
    f->load = de_asset_load(f->engine.assets, de_asset_id_from("hero"), 0);
    de_load_state state = de_asset_wait(f->engine.assets, f->load, 10000000000ULL);
    if (state != DE_LOAD_READY) {
        de_load_info info;
        de_asset_status(f->engine.assets, f->load, &info);
        fprintf(stderr, "model load: %s\n", info.first_error_msg);
    }
    ASSERT_EQ(state, DE_LOAD_READY);
    f->editor.e = &f->engine;
    f->editor.h = &f->host;
    de__ed_assets_register(&f->editor);
}
static void dependency_fx_teardown(dependency_fx *f) {
    de_asset_release(f->engine.assets, f->load);
    de_asset_system_destroy(f->engine.assets);
    de_log_shutdown();
}
static u32 references(dependency_fx *f, de_ep_method method, const char *id, char names[16][128]) {
    char params[256], response[4096];
    mpack_writer_t writer;
    mpack_writer_init(&writer, params, sizeof params);
    mpack_start_map(&writer, 1);
    mpack_write_cstr(&writer, "id");
    mpack_write_cstr(&writer, id);
    mpack_finish_map(&writer);
    size_t params_size = mpack_writer_buffer_used(&writer);
    ASSERT_EQ(mpack_writer_destroy(&writer), mpack_ok);
    de_ep_request request = {.id = 7, .params = {params, params_size}};
    de_ep_arena arena = {(u8 *)f->arena, sizeof f->arena, 0};
    mpack_writer_init(&writer, response, sizeof response);
    ASSERT_TRUE(f->editor.handlers[method] != NULL);
    f->editor.handlers[method](&f->editor, &request, &arena, &writer);
    size_t response_size = mpack_writer_buffer_used(&writer);
    ASSERT_EQ(mpack_writer_destroy(&writer), mpack_ok);
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, response, response_size);
    mpack_tree_parse(&tree);
    mpack_node_t root = mpack_tree_root(&tree);
    ASSERT_FALSE(mpack_node_map_contains_cstr(root, "error"));
    mpack_node_t ids = mpack_node_map_cstr(mpack_node_map_cstr(root, "result"), "ids");
    u32 count = (u32)mpack_node_array_length(ids);
    ASSERT_LE(count, 16u);
    for (u32 i = 0; i < count; ++i)
        mpack_node_copy_cstr(mpack_node_array_at(ids, i), names[i], 128);
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    return count;
}
static bool contains(char names[16][128], u32 count, const char *name) {
    for (u32 i = 0; i < count; ++i)
        if (!strcmp(names[i], name))
            return true;
    return false;
}
TEST_F(editor_dependencies, generated_model_names_navigate_and_reverse, dependency_fx) {
    static const char *children[] = {
        "hero#Body",           "hero#Sword",        "hero#Shield",    "hero#material:Skin",
        "hero#material:Steel", "hero#skeleton:Rig", "hero#anim:Idle", "hero#anim:Walk",
        "hero#anim:Run",       "hero#anim:Jump",    "hero#prefab"};
    char names[16][128];
    u32 count = references(f, DE_EP_METHOD_ASSET_DEPENDENCIES, "hero", names);
    ASSERT_EQ(count, 11u);
    for (u32 i = 0; i < 11; ++i)
        EXPECT_TRUE(contains(names, count, children[i]));
    for (u32 i = 0; i < count; ++i) {
        /* Use the actual response string for loading/navigation, not the expected fixture ID. */
        de_asset_id id = de_asset_id_from(names[i]);
        ASSERT_NE(de__slot_find(f->engine.assets, id), 0u);
        de_load_handle load = de_asset_load(f->engine.assets, id, 0);
        ASSERT_EQ(de_asset_wait(f->engine.assets, load, 10000000000ULL), DE_LOAD_READY);
        de_asset_release(f->engine.assets, load);
        char back[16][128];
        u32 n = references(f, DE_EP_METHOD_ASSET_REFERENCERS, names[i], back);
        EXPECT_TRUE(contains(back, n, "hero"));
        (void)references(f, DE_EP_METHOD_ASSET_DEPENDENCIES, names[i], back);
    }
    count = references(f, DE_EP_METHOD_ASSET_DEPENDENCIES, "hero#Body", names);
    ASSERT_EQ(count, 1u);
    EXPECT_STREQ(names[0], "hero#material:Skin");
    count = references(f, DE_EP_METHOD_ASSET_REFERENCERS, "hero#material:Skin", names);
    EXPECT_TRUE(contains(names, count, "hero"));
    EXPECT_TRUE(contains(names, count, "hero#Body"));
}

TEST_F(editor_dependencies, unknown_dependency_retains_hex_fallback, dependency_fx) {
    de__slot *root =
        de__slot_get(f->engine.assets, de__slot_find(f->engine.assets, de_asset_id_from("hero")));
    ASSERT_TRUE(root != NULL);
    ASSERT_GT(root->dep_count, 0u);
    /* Exercise serialization of an unresolved edge without inventing a registry name. */
    de_asset_id saved = root->deps[0];
    de_asset_id unknown = de_asset_id_from("not-registered#missing");
    root->deps[0] = unknown;
    char names[16][128];
    u32 count = references(f, DE_EP_METHOD_ASSET_DEPENDENCIES, "hero", names);
    root->deps[0] = saved;
    char expected[24];
    snprintf(expected, sizeof expected, "%016llx", (unsigned long long)unknown.hash);
    EXPECT_TRUE(contains(names, count, expected));
}
