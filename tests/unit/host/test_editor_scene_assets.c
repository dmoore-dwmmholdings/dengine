#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* strdup, setenv and unsetenv under strict C11. */
#endif
#include "de_test.h"
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include "asset/slots_internal.h"
#include "cook/importers/model/model_import.h"
#include "render/formats/de_cooked_mesh.h"
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <dengine/de_render.h>
#include "ecs/ecs_internal.h"
#if defined(_WIN32)
#include <process.h>
#define scene_getpid() ((unsigned long)_getpid())
#else
#include <unistd.h>
#define scene_getpid() ((unsigned long)getpid())
#endif

static u32 scene_run_counter;

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
typedef struct scene_fx {
    de_tracking_allocator tracking;
    de_allocator allocator;
    de_engine engine;
    de__host host;
    de__editor editor;
    de_load_handle load;
    bool shift, cold;
    char native_dir[DE_FS_PATH_MAX];
    char note[32768];
    u64 arena[32768]; /* Match the production allocator-backed arena alignment. */
} scene_fx;
static void scene_fx_setup(scene_fx *f) {
    f->allocator = de_tracking_allocator_init(&f->tracking, NULL, DE_MEM_GENERAL);
    f->engine.allocator = &f->allocator;
    ASSERT_EQ(de_type_registry_create(NULL, &f->engine.types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->engine.types}, &f->engine.world), DE_OK);
    (void)de_builtin_components(f->engine.world);
    ASSERT_EQ(de_render_register_components(f->engine.world), DE_OK);
    ASSERT_EQ(de_log_init(&(de_log_desc){.no_console_sink = true}), DE_OK);
    ASSERT_EQ(de_fs_mount("scenedata", DE_EDITOR_SCENE_DATA, DE_MOUNT_READ_ONLY, 0), DE_OK);
    /* CTest executes cases in separate concurrent processes. Neither source files nor
     * cooked caches may be shared, including across cold_reset within one process. */
    ASSERT_EQ(de_fs_mount("scenebase", DE_EDITOR_SCENE_TMP, DE_MOUNT_WRITABLE, 0), DE_OK);
    char run[128];
    snprintf(run, sizeof run, "scenebase:/run-%lu-%u-%llu", scene_getpid(), scene_run_counter++,
             (unsigned long long)de_time_now_ns());
    ASSERT_FALSE(de_path_exists(run));
    ASSERT_EQ(de_dir_create(run), DE_OK);
    ASSERT_EQ(de_fs_resolve(run, f->native_dir, sizeof f->native_dir), DE_OK);
    ASSERT_EQ(de_fs_unmount("scenebase", DE_EDITOR_SCENE_TMP), DE_OK);
    ASSERT_EQ(de_fs_mount("scenetmp", f->native_dir, DE_MOUNT_WRITABLE, 0), DE_OK);
    void *source = NULL;
    u64 size = 0;
    ASSERT_EQ(
        de_file_read_all("scenedata:/output_selection.glb", de_heap_allocator(), &source, &size),
        DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("scenetmp:/model.glb", source, size), DE_OK);
    DE_FREE(de_heap_allocator(), source, (size_t)size);
    const char *manifest = "[project]\nasset_root=\".\"\nversion=1\n[assets]\nhero={type=\"model\","
                           "source=\"model.glb\"}\n"
                           "padding={type=\"blob\",source=\"model.glb\"}\n";
    ASSERT_EQ(de_file_write_all_atomic("scenetmp:/assets.toml", manifest, strlen(manifest)), DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.cache_dir = "scenetmp:/cache",
                                                             .types = f->engine.types,
                                                             .staging_ring_bytes = 1048576},
                                     &f->engine.assets),
              DE_OK);
    const char *types[] = {"model", "mesh", "material", "skeleton", "anim_clip"};
    for (u32 i = 0; i < 5; ++i) {
        de_asset_type_desc type = {.name = types[i], .load = bytes_load, .unload = bytes_unload};
        if (!i) {
            type.import = de_model_import;
            type.importer_version = DE_MODEL_IMPORTER_VERSION;
        }
        if (i == 1)
            type.user = f; /* Mesh loader derives dependencies from the actual cooked bytes. */
        ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(f->engine.assets, &type)));
    }
    ASSERT_EQ(de_asset_manifest_load(f->engine.assets, "scenetmp:/assets.toml"), DE_OK);
    if (f->shift)
        ASSERT_TRUE(DE_HANDLE_VALID(de_asset_blob(f->engine.assets, de_asset_id_from("padding"))));
    if (!f->cold) {
        f->load = de_asset_load(f->engine.assets, de_asset_id_from("hero"), 0);
        de_load_state state = de_asset_wait(f->engine.assets, f->load, 10000000000ULL);
        if (state != DE_LOAD_READY) {
            de_load_info info;
            de_asset_status(f->engine.assets, f->load, &info);
            fprintf(stderr, "model load: %s\n", info.first_error_msg);
        }
        ASSERT_EQ(state, DE_LOAD_READY);
    }
    f->editor.e = &f->engine;
    f->editor.h = &f->host;
    f->editor.mode = DE_EP_WORLD_MODE_EDIT;
    f->editor.note = f->note;
    f->editor.note_cap = sizeof f->note;
    ASSERT_EQ(de__ed_scene_register(&f->editor), DE_OK);
}
static void scene_fx_teardown(scene_fx *f) {
    de__ed_scene_engine_shutdown(&f->engine);
    de_asset_release(f->engine.assets, f->load);
    de_world_destroy(f->engine.world);
    de_asset_system_destroy(f->engine.assets);
    de_type_registry_destroy(f->engine.types);
    /* The real host-start test can already have cleared the process mount table. */
    (void)de_fs_unmount("scenetmp", f->native_dir);
    (void)de_fs_unmount("scenedata", DE_EDITOR_SCENE_DATA);
    de_log_shutdown();
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracking).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracking).blocks_live, 0u);
}
/* Invoke the registered production protocol handlers, retaining their actual response. */
static mpack_node_t raw_request(scene_fx *f, de_ep_method method, const char *params, size_t size,
                                mpack_tree_t *tree, char response[65536]) {
    de_ep_request req = {.id = 7, .params = {params, size}};
    de_ep_arena arena = {(u8 *)f->arena, sizeof f->arena, 0};
    mpack_writer_t w;
    mpack_writer_init(&w, response, 65536);
    f->editor.deferred = false;
    f->editor.handlers[method](&f->editor, &req, &arena, &w);
    if (f->editor.deferred) {
        const u64 deadline = de_time_now_ns() + 10000000000ULL;
        bool complete = false;
        do {
            de_asset_system_update(f->engine.assets, NULL);
            complete = de__ed_scene_poll(&f->editor, &w);
            if (!complete)
                de_sleep_ns(1000000);
        } while (!complete && de_time_now_ns() < deadline);
        ASSERT_TRUE(complete);
    }
    size_t written = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_init_data(tree, response, written);
    mpack_tree_parse(tree);
    mpack_node_t root = mpack_tree_root(tree);
    return root;
}
static mpack_node_t request(scene_fx *f, de_ep_method method, const char *params, size_t size,
                            mpack_tree_t *tree, char response[65536]) {
    mpack_node_t root = raw_request(f, method, params, size, tree, response);
    ASSERT_FALSE(mpack_node_map_contains_cstr(root, "error"));
    return mpack_node_map_cstr(root, "result");
}

static void serialize(scene_fx *f, char text[32768]) {
    char params[64], response[65536];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    de_ep_write_scene_params(&w, &(de_ep_scene_params){1});
    size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_t tree;
    mpack_node_t result = request(f, DE_EP_METHOD_SCENE_SERIALIZE, params, size, &tree, response);
    mpack_node_copy_cstr(mpack_node_map_cstr(result, "text"), text, 32768);
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
}
static void validate_renderers(scene_fx *f, u32 handles[3]) {
    static const char *mesh_names[] = {"hero#Body", "hero#Sword", "hero#Shield"};
    const de_component component = de_component_find(f->engine.world, "DeMeshRenderer");
    u32 count = 0;
    for (u32 ai = 0; ai < f->engine.world->archetypes.count; ++ai) {
        const ecs_archetype *a = ((ecs_archetype **)f->engine.world->archetypes.data)[ai];
        for (u32 ci = 0; ci < a->chunks.count; ++ci) {
            const ecs_chunk *ch = ecs_table_chunk(a, ci);
            for (u32 row = 0; row < ch->count; ++row) {
                de_entity entity = ecs_chunk_entities(ch)[row];
                const DeMeshRenderer *mr = de_entity_get_const(f->engine.world, entity, component);
                if (!mr)
                    continue;
                ++count;
                const de__slot *mesh = de__slot_get(f->engine.assets, mr->mesh.id);
                ASSERT_TRUE(mesh != NULL);
                EXPECT_EQ(mesh->type_hash, de_hash64_str("mesh"));
                EXPECT_TRUE(de__slot_object(mesh) != NULL);
                bool found = false;
                for (u32 k = 0; k < 3; ++k)
                    if (mesh->id.hash == de_asset_id_from(mesh_names[k]).hash) {
                        handles[k] = mr->mesh.id;
                        found = true;
                        const de__slot *material =
                            de__slot_get(f->engine.assets, mr->materials[0].id);
                        ASSERT_TRUE(material != NULL);
                        EXPECT_EQ(material->type_hash, de_hash64_str("material"));
                        EXPECT_EQ(material->id.hash, de_asset_id_from(k == 1 ? "hero#material:Steel"
                                                                             : "hero#material:Skin")
                                                         .hash);
                        EXPECT_TRUE(de__slot_object(material) != NULL);
                    }
                EXPECT_TRUE(found);
            }
        }
    }
    EXPECT_EQ(count, 3u);
}
TEST_F(editor_scene_assets, generated_prefab_fresh_system_roundtrip, scene_fx) {
    char params[32768], response[65536], saved[32768], reopened[32768];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    de_ep_write_prefab_instantiate_params(
        &w, &(de_ep_prefab_instantiate_params){.asset = de_ep_cstr("hero#prefab"), .scene = 1});
    size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_t tree;
    mpack_node_t result =
        request(f, DE_EP_METHOD_PREFAB_INSTANTIATE, params, size, &tree, response);
    de_entity root = {mpack_node_u64(mpack_node_map_cstr(result, "entity"))};
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    ASSERT_TRUE(de_entity_alive(f->engine.world, root));
    DeTransform *transform =
        de_entity_get(f->engine.world, root, de_component_find(f->engine.world, "DeTransform"));
    ASSERT_TRUE(transform != NULL);
    transform->position.x = 12.5f;
    transform->position.y = -3.0f;
    u32 before[3] = {0}, after[3] = {0};
    validate_renderers(f, before);
    serialize(f, saved);
    const char *names[] = {"hero#Body", "hero#Sword", "hero#Shield", "hero#material:Skin",
                           "hero#material:Steel"};
    for (u32 i = 0; i < 5; ++i) {
        char canonical[32];
        snprintf(canonical, sizeof canonical, "0x%016llx",
                 (unsigned long long)de_asset_id_from(names[i]).hash);
        EXPECT_TRUE(strstr(saved, canonical) != NULL || strstr(saved, names[i]) != NULL);
    }
    /* Destroy the entire world and asset system. Different slot allocation must not change
     * persisted identities. The root import establishes the real generated registry again. */
    scene_fx_teardown(f);
    memset(f, 0, sizeof *f);
    f->shift = true;
    scene_fx_setup(f);
    mpack_writer_init(&w, params, sizeof params);
    de_ep_write_scene_reload_params(
        &w, &(de_ep_scene_reload_params){.scene = 1, .text = de_ep_cstr(saved)});
    size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    (void)request(f, DE_EP_METHOD_SCENE_RELOAD_FROM_TEXT, params, size, &tree, response);
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    validate_renderers(f, after);
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_NE(before[i], 0u);
        ASSERT_NE(after[i], 0u);
        EXPECT_NE(before[i], after[i]);
    }
    serialize(f, reopened);
    /* Includes stable entity IDs, parent links, every local transform and render field. */
    EXPECT_STREQ(saved, reopened);
}

TEST_F(editor_scene_assets, generated_prefab_cold_manifest_only_roundtrip, scene_fx) {
    char params[32768], response[65536], saved[32768], reopened[32768];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    de_ep_write_prefab_instantiate_params(
        &w, &(de_ep_prefab_instantiate_params){.asset = de_ep_cstr("hero#prefab"), .scene = 1});
    size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_t tree;
    mpack_node_t result =
        request(f, DE_EP_METHOD_PREFAB_INSTANTIATE, params, size, &tree, response);
    de_entity root = {mpack_node_u64(mpack_node_map_cstr(result, "entity"))};
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    ASSERT_TRUE(de_entity_alive(f->engine.world, root));
    DeTransform *transform =
        de_entity_get(f->engine.world, root, de_component_find(f->engine.world, "DeTransform"));
    ASSERT_TRUE(transform != NULL);
    transform->position.x = 12.5f;
    transform->position.y = -3.0f;
    u32 before[3] = {0}, after[3] = {0};
    validate_renderers(f, before);
    serialize(f, saved);
    const char *names[] = {"hero#Body", "hero#Sword", "hero#Shield", "hero#material:Skin",
                           "hero#material:Steel"};
    for (u32 i = 0; i < 5; ++i) {
        char canonical[32];
        snprintf(canonical, sizeof canonical, "0x%016llx",
                 (unsigned long long)de_asset_id_from(names[i]).hash);
        EXPECT_TRUE(strstr(saved, canonical) != NULL || strstr(saved, names[i]) != NULL);
    }
    /* Destroy the entire world and asset system. Different slot allocation must not change
     * persisted identities. This reopen starts from the manifest only, without root import. */
    scene_fx_teardown(f);
    memset(f, 0, sizeof *f);
    f->shift = true;
    f->cold = true;
    scene_fx_setup(f);
    mpack_writer_init(&w, params, sizeof params);
    de_ep_write_scene_reload_params(
        &w, &(de_ep_scene_reload_params){.scene = 1, .text = de_ep_cstr(saved)});
    size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    (void)request(f, DE_EP_METHOD_SCENE_RELOAD_FROM_TEXT, params, size, &tree, response);
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    validate_renderers(f, after);
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_NE(before[i], 0u);
        ASSERT_NE(after[i], 0u);
        EXPECT_NE(before[i], after[i]);
    }
    serialize(f, reopened);
    /* Includes stable entity IDs, parent links, every local transform and render field. */
    EXPECT_STREQ(saved, reopened);
}

#include "host/editor/editor_scene_load.h"
TEST_F(editor_scene_assets, cold_preflight_retains_exact_children_not_parent, scene_fx) {
    scene_fx_teardown(f);
    memset(f, 0, sizeof *f);
    f->cold = true;
    scene_fx_setup(f);
    const char *text = "[[entity]]\nid=\"0x1\"\n[entity.DeMeshRenderer]\nmesh=\"hero#Body\"\n";
    de__scene_assets p;
    ASSERT_EQ(de__scene_assets_begin(&f->editor, text, strlen(text), &p), DE_OK);
    const u64 deadline = de_time_now_ns() + 10000000000ULL;
    de_result result;
    do {
        de_asset_system_update(f->engine.assets, NULL);
        result = de__scene_assets_poll(&p);
        if (result == DE_ERR_BUSY)
            de_sleep_ns(1000000);
    } while (result == DE_ERR_BUSY && de_time_now_ns() < deadline);
    EXPECT_EQ(result, DE_OK);
    const u32 mesh = de__asset_handle_of(f->engine.assets, de_asset_id_from("hero#Body"), "mesh");
    EXPECT_NE(mesh, 0u);
    EXPECT_TRUE(de__asset_object(f->engine.assets, mesh) != NULL);
    EXPECT_EQ(p.discovery.id, 0u);
    de_asset_flush_unused(f->engine.assets);
    EXPECT_TRUE(de__asset_object(f->engine.assets, mesh) != NULL);
    const u32 root = de__asset_handle_of(f->engine.assets, de_asset_id_from("hero"), "model");
    EXPECT_TRUE(de__asset_object(f->engine.assets, root) == NULL);
    de__scene_assets_drop(&p);
    de_asset_flush_unused(f->engine.assets);
    EXPECT_TRUE(de__asset_object(f->engine.assets, mesh) == NULL);
}

static void expect_reload_failure(scene_fx *f, const char *text) {
    char before[32768], after[32768], params[32768], response[65536];
    serialize(f, before);
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    de_ep_write_scene_reload_params(&w, &(de_ep_scene_reload_params){1, de_ep_cstr(text)});
    size_t n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_t tree;
    mpack_node_t root =
        raw_request(f, DE_EP_METHOD_SCENE_RELOAD_FROM_TEXT, params, n, &tree, response);
    EXPECT_TRUE(mpack_node_map_contains_cstr(root, "error"));
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    serialize(f, after);
    EXPECT_STREQ(before, after);
}
TEST_F(editor_scene_assets, invalid_replacement_preserves_old_scene, scene_fx) {
    de_entity old = de_entity_spawn(f->engine.world);
    ASSERT_TRUE(old.bits != 0);
    (void)de_entity_add(f->engine.world, old, de_component_find(f->engine.world, "DeTransform"));
    const char *bad[] = {
        "not toml [",
        "[[entity]]\nid=\"0x1\"\n[entity.DeMeshRenderer]\nmesh=\"unknown#Body\"\n",
        "[[entity]]\nid=\"0x1\"\n[entity.DeMeshRenderer]\nmesh=\"hero#absent\"\n",
        "[[entity]]\nid=\"0x1\"\n[entity.DeMeshRenderer]\nmesh=\"hero#material:Skin\"\n",
        "[[entity]]\nid=\"0x1\"\n[entity.DeMeshRenderer]\nmesh={id=\"0x0000000000100003\"}\n",
        "[[entity]]\nid=\"0x1\"\n[entity.DeTransform]\nposition=\"wrong type\"\n",
        "[[entity]]\nid=\"0x1\"\n[[entity]]\nid=\"0x1\"\n",
        "[[entity]]\nid=\"0x1\"\nparent=\"0x2\"\n[[entity]]\nid=\"0x2\"\nparent=\"0x1\"\n"};
    for (u32 i = 0; i < sizeof bad / sizeof *bad; ++i) {
        expect_reload_failure(f, bad[i]);
        EXPECT_TRUE(de_entity_alive(f->engine.world, old));
    }
}

static const char cold_text[] =
    "[[entity]]\nid=\"0xabc\"\n[entity.DeMeshRenderer]\nmesh=\"hero#Body\"\n";
static void cold_reset(scene_fx *f) {
    scene_fx_teardown(f);
    memset(f, 0, sizeof *f);
    f->cold = true;
    scene_fx_setup(f);
}
static void begin_pending(scene_fx *f) {
    char params[4096], response[4096];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    de_ep_write_scene_reload_params(&w, &(de_ep_scene_reload_params){1, de_ep_cstr(cold_text)});
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    de_ep_request req = {.id = 42, .params = {params, size}};
    de_ep_arena arena = {(u8 *)f->arena, sizeof f->arena, 0};
    mpack_writer_init(&w, response, sizeof response);
    f->editor.deferred = false;
    f->editor.handlers[DE_EP_METHOD_SCENE_RELOAD_FROM_TEXT](&f->editor, &req, &arena, &w);
    EXPECT_EQ(mpack_writer_buffer_used(&w), 0u);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    ASSERT_TRUE(f->editor.deferred);
}
static void finish_pending(scene_fx *f, bool error) {
    char response[65536];
    mpack_writer_t w;
    mpack_writer_init(&w, response, sizeof response);
    const u64 deadline = de_time_now_ns() + 10000000000ULL;
    bool complete;
    do {
        de_asset_system_update(f->engine.assets, NULL);
        complete = de__ed_scene_poll(&f->editor, &w);
        if (!complete)
            de_sleep_ns(1000000);
    } while (!complete && de_time_now_ns() < deadline);
    ASSERT_TRUE(complete);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, response, size);
    mpack_tree_parse(&tree);
    EXPECT_EQ(mpack_node_map_contains_cstr(mpack_tree_root(&tree), "error"), error);
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
}
TEST_F(editor_scene_assets, pending_overlap_mode_and_stale_edits, scene_fx) {
    cold_reset(f);
    de_entity old = de_entity_spawn(f->engine.world);
    begin_pending(f);
    char params[64], response[65536];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    de_ep_write_scene_params(&w, &(de_ep_scene_params){1});
    size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_t tree;
    mpack_node_t root = raw_request(f, DE_EP_METHOD_SCENE_UNLOAD, params, size, &tree, response);
    EXPECT_TRUE(mpack_node_map_contains_cstr(root, "error"));
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    EXPECT_TRUE(de_entity_alive(f->engine.world, old));
    f->editor.mode = DE_EP_WORLD_MODE_PLAY;
    finish_pending(f, true);
    EXPECT_TRUE(de_entity_alive(f->engine.world, old));
    cold_reset(f);
    old = de_entity_spawn(f->engine.world);
    begin_pending(f);
    /* A same-tick editor mutation must invalidate the snapshot even when ECS revisions match. */
    de__ed_scene_mark_dirty(1);
    finish_pending(f, true);
    EXPECT_TRUE(de_entity_alive(f->engine.world, old));
}
TEST_F(editor_scene_assets, reset_cancels_pending_idle_ticks_do_not_invalidate, scene_fx) {
    cold_reset(f);
    de_entity old = de_entity_spawn(f->engine.world);
    begin_pending(f);
    for (u32 i = 0; i < f->editor.reset_count; ++i)
        f->editor.resets[i](&f->editor);
    char response[64];
    mpack_writer_t w;
    mpack_writer_init(&w, response, sizeof response);
    EXPECT_FALSE(de__ed_scene_poll(&f->editor, &w));
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    EXPECT_TRUE(de_entity_alive(f->engine.world, old));
    cold_reset(f);
    begin_pending(f);
    for (u32 i = 0; i < 20; ++i)
        de__ed_world_touch(&f->editor);
    finish_pending(f, false);
    const u32 mesh = de__asset_handle_of(f->engine.assets, de_asset_id_from("hero#Body"), "mesh");
    de__ed_scene_detach(&f->editor);
    de_asset_flush_unused(f->engine.assets);
    EXPECT_TRUE(de__asset_object(f->engine.assets, mesh) != NULL);
    f->editor.tick_count = f->editor.reset_count = 0;
    ASSERT_EQ(de__ed_scene_register(&f->editor), DE_OK);
    de_asset_flush_unused(f->engine.assets);
    EXPECT_TRUE(de__asset_object(f->engine.assets, mesh) != NULL);
    de__ed_scene_engine_shutdown(&f->engine);
    de_asset_flush_unused(f->engine.assets);
    EXPECT_TRUE(de__asset_object(f->engine.assets, mesh) == NULL);
}
TEST_F(editor_scene_assets, second_attachment_rejected_detached_owner_cleanup_isolated, scene_fx) {
    de_engine other = {0};
    de__editor second = {.e = &other, .h = &f->host};
    EXPECT_EQ(de__ed_scene_register(&second), DE_ERR_BUSY);
    EXPECT_TRUE(second.scene == NULL);
    de__ed_scene_detach(&f->editor);
    ASSERT_EQ(de__ed_scene_register(&second), DE_OK);
    void *state = second.scene;
    de__ed_scene_engine_shutdown(&f->engine);
    EXPECT_TRUE(second.scene == state);
    /* Dispatch against B after freeing detached A, exercising its still-valid active binding. */
    char bytes[4096];
    mpack_writer_t writer;
    mpack_writer_init(&writer, bytes, sizeof bytes);
    de_ep_request req = {.id = 1};
    de_ep_arena arena = {0};
    second.handlers[DE_EP_METHOD_SCENE_LIST](&second, &req, &arena, &writer);
    EXPECT_GT(mpack_writer_buffer_used(&writer), 0u);
    ASSERT_EQ(mpack_writer_destroy(&writer), mpack_ok);
    de__ed_scene_detach(&second);
    de__ed_scene_engine_shutdown(&other);
}
#if !defined(_WIN32)
TEST_F(editor_scene_assets, response_send_reset_keeps_committed_ownership, scene_fx) {
    cold_reset(f);
    begin_pending(f);
    char response[65536];
    f->editor.out = response;
    f->editor.out_cap = sizeof response;
    f->editor.connected = f->editor.authed = true;
    f->editor.conn.fd = -1; /* Real send failure on a deliberately invalid, unowned descriptor. */
    const u64 deadline = de_time_now_ns() + 10000000000ULL;
    do {
        de_asset_system_update(f->engine.assets, NULL);
        for (u32 i = 0; i < f->editor.tick_count; ++i)
            f->editor.ticks[i](&f->editor);
        if (f->editor.connected)
            de_sleep_ns(1000000);
    } while (f->editor.connected && de_time_now_ns() < deadline);
    EXPECT_FALSE(f->editor.connected);
    const u32 mesh = de__asset_handle_of(f->engine.assets, de_asset_id_from("hero#Body"), "mesh");
    de_asset_flush_unused(f->engine.assets);
    EXPECT_TRUE(de__asset_object(f->engine.assets, mesh) != NULL);
    f->editor.out = NULL;
    f->editor.out_cap = 0;
}
#endif

TEST_F(editor_scene_assets, allocation_failures_preserve_old_scene_and_retry, scene_fx) {
    /* Sweep bounded real transaction allocations, not giant files or a mock loader. */
    const char *text = "[[entity]]\nid=\"0x11\"\n[entity.DeTransform]\n"
                       "position=[1.0,2.0,3.0]\n[[entity]]\nid=\"0x12\"\nparent=\"0x11\"\n";
    de_entity old = de_entity_spawn(f->engine.world);
    ASSERT_TRUE(de_entity_add(f->engine.world, old,
                              de_component_find(f->engine.world, "DeTransform")) != NULL);
    char before[32768], after[32768];
    serialize(f, before);
    char params[4096], response[65536];
    mpack_writer_t writer;
    mpack_writer_init(&writer, params, sizeof params);
    de_ep_write_scene_reload_params(&writer, &(de_ep_scene_reload_params){1, de_ep_cstr(text)});
    const size_t size = mpack_writer_buffer_used(&writer);
    ASSERT_EQ(mpack_writer_destroy(&writer), mpack_ok);
    u32 failed = 0;
    bool succeeded = false;
    for (i64 allowed = 0; allowed < 64; ++allowed) {
        de_tracking_allocator_fail_after(&f->tracking, allowed);
        mpack_tree_t tree;
        mpack_node_t result =
            raw_request(f, DE_EP_METHOD_SCENE_RELOAD_FROM_TEXT, params, size, &tree, response);
        const bool error = mpack_node_map_contains_cstr(result, "error");
        de_tracking_allocator_fail_after(&f->tracking, -1);
        ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
        if (!error) {
            succeeded = true;
            EXPECT_FALSE(de_entity_alive(f->engine.world, old));
            break;
        }
        ++failed;
        EXPECT_TRUE(de_entity_alive(f->engine.world, old));
        serialize(f, after);
        EXPECT_STREQ(before, after);
    }
    EXPECT_GT(failed, 4u);
    EXPECT_TRUE(succeeded);
}

#if !defined(_WIN32)
TEST_F(editor_scene_assets, real_start_rejects_second_service_before_side_effects, scene_fx) {
    de__ed_scene_detach(&f->editor);
    const char *prior = getenv("DE_EDITOR_TOKEN");
    char *saved = prior ? strdup(prior) : NULL;
    ASSERT_EQ(setenv("DE_EDITOR_TOKEN", "deng708-local-test", 1), 0);
    de__game_config config;
    de__game_config_defaults(&config);
    config.task_workers = 1;
    de_engine *engine = NULL;
    ASSERT_EQ(de__engine_create(&(de__engine_desc){.config = &config, .headless = true}, &engine),
              DE_OK);
    char endpoint[256];
    snprintf(endpoint, sizeof endpoint, "/tmp/deng708-start-%llu.sock",
             (unsigned long long)de_time_now_ns());
    char *argv[] = {"deng708", "--editor-service", endpoint};
    de__host first = {.engine = engine, .desc = {.argc = 3, .argv = argv}};
    ASSERT_EQ(de__editor_start(&first), DE_OK);
    ASSERT_TRUE(first.editor != NULL);
    de__editor *active = first.editor;
    void *scene = active->scene;
    const u32 hooks = first.loop_hooks.frame_begin_count;
    de__host second = {.engine = engine, .desc = {.argc = 3, .argv = argv}};
    EXPECT_EQ(de__editor_start(&second), DE_ERR_BUSY);
    EXPECT_TRUE(second.editor == NULL);
    EXPECT_TRUE(first.editor == active && active->scene == scene);
    EXPECT_EQ(first.loop_hooks.frame_begin_count, hooks);
    char response[4096];
    mpack_writer_t writer;
    mpack_writer_init(&writer, response, sizeof response);
    de_ep_request request = {.id = 7};
    de_ep_arena arena = {0};
    active->handlers[DE_EP_METHOD_SCENE_LIST](active, &request, &arena, &writer);
    EXPECT_GT(mpack_writer_buffer_used(&writer), 0u);
    ASSERT_EQ(mpack_writer_destroy(&writer), mpack_ok);
    de__editor_stop(&first);
    de__engine_destroy(engine);
    if (saved) {
        ASSERT_EQ(setenv("DE_EDITOR_TOKEN", saved, 1), 0);
        free(saved);
    } else
        ASSERT_EQ(unsetenv("DE_EDITOR_TOKEN"), 0);
}
#endif

typedef struct SceneMeshRef {
    u32 mesh;
} SceneMeshRef;
typedef struct SceneNestedRefs {
    SceneMeshRef single;
    SceneMeshRef items[2];
} SceneNestedRefs;
static const de_field_info mesh_ref_fields[] = {
    DE_FIELD_EX(SceneMeshRef, mesh, DE_FK_ASSET, DE_FIELD_DEFAULT | DE_FIELD_ASSET_REF,
                {0, 0, 0, 0, "mesh", 0}),
};
static const de_field_info nested_ref_fields[] = {
    DE_FIELD_EX(SceneNestedRefs, single, DE_FK_STRUCT, DE_FIELD_DEFAULT,
                {0, 0, 0, 0, "SceneMeshRef", 0}),
    DE_FIELD_EX(SceneNestedRefs, items, DE_FK_FIXED_ARRAY, DE_FIELD_DEFAULT,
                {0, 0, 0, 0, "SceneMeshRef", 2}),
};
TEST_F(editor_scene_assets, cold_nested_struct_and_fixed_array_references, scene_fx) {
    cold_reset(f);
    ASSERT_TRUE(DE_HANDLE_VALID(
        de_type_register(f->engine.types, &DE_TYPE_DESC(SceneMeshRef, 1, mesh_ref_fields))));
    de_type_id type =
        de_type_register(f->engine.types, &DE_TYPE_DESC(SceneNestedRefs, 1, nested_ref_fields));
    ASSERT_TRUE(DE_HANDLE_VALID(type));
    de_component component = de_component_register(f->engine.world, type, DE_COMP_SERIALIZE);
    ASSERT_TRUE(DE_HANDLE_VALID(component));
    const char *text = "[[entity]]\nid=\"0x1\"\n[entity.SceneNestedRefs]\n"
                       "single={mesh=\"hero#Body\"}\n"
                       "items=[{mesh=\"hero#Sword\"},{mesh=\"hero#Shield\"}]\n";
    char params[4096], response[65536];
    mpack_writer_t writer;
    mpack_writer_init(&writer, params, sizeof params);
    de_ep_write_scene_reload_params(&writer, &(de_ep_scene_reload_params){1, de_ep_cstr(text)});
    size_t size = mpack_writer_buffer_used(&writer);
    ASSERT_EQ(mpack_writer_destroy(&writer), mpack_ok);
    mpack_tree_t tree;
    (void)request(f, DE_EP_METHOD_SCENE_RELOAD_FROM_TEXT, params, size, &tree, response);
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    u32 found = 0;
    for (u32 ai = 0; ai < f->engine.world->archetypes.count; ++ai) {
        const ecs_archetype *a = ((ecs_archetype **)f->engine.world->archetypes.data)[ai];
        for (u32 ci = 0; ci < a->chunks.count; ++ci) {
            const ecs_chunk *ch = ecs_table_chunk(a, ci);
            for (u32 row = 0; row < ch->count; ++row) {
                const SceneNestedRefs *refs =
                    de_entity_get_const(f->engine.world, ecs_chunk_entities(ch)[row], component);
                if (!refs)
                    continue;
                ++found;
                const u32 handles[] = {refs->single.mesh, refs->items[0].mesh, refs->items[1].mesh};
                const char *names[] = {"hero#Body", "hero#Sword", "hero#Shield"};
                de_asset_flush_unused(f->engine.assets);
                for (u32 i = 0; i < 3; ++i) {
                    const de__slot *slot = de__slot_get(f->engine.assets, handles[i]);
                    ASSERT_TRUE(slot != NULL);
                    EXPECT_EQ(slot->id.hash, de_asset_id_from(names[i]).hash);
                    EXPECT_EQ(slot->type_hash, de_hash64_str("mesh"));
                    EXPECT_TRUE(de__slot_object(slot) != NULL);
                }
            }
        }
    }
    EXPECT_EQ(found, 1u);
}

static u64 load_scene_file(scene_fx *f, const char *path, bool additive) {
    char params[1024], response[65536];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    de_ep_write_scene_load_params(&w, &(de_ep_scene_load_params){de_ep_cstr(path), additive});
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_t tree;
    mpack_node_t result = request(f, DE_EP_METHOD_SCENE_LOAD, params, size, &tree, response);
    u64 scene = mpack_node_u64(mpack_node_map_cstr(result, "scene"));
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    return scene;
}
TEST_F(editor_scene_assets, unload_and_replacement_release_only_owned_requests, scene_fx) {
    cold_reset(f);
    ASSERT_EQ(de_file_write_all_atomic("scenetmp:/owned.toml", cold_text, strlen(cold_text)),
              DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("scenetmp:/empty.toml", "", 0), DE_OK);
    de_entity game = de_entity_spawn(f->engine.world);
    for (u32 replacement = 0; replacement < 2; ++replacement) {
        const u64 scene = load_scene_file(f, "scenetmp:/owned.toml", false);
        EXPECT_NE(scene, 0u);
        const u32 mesh =
            de__asset_handle_of(f->engine.assets, de_asset_id_from("hero#Body"), "mesh");
        de_asset_flush_unused(f->engine.assets);
        EXPECT_TRUE(de__asset_object(f->engine.assets, mesh) != NULL);
        EXPECT_TRUE(de_entity_alive(f->engine.world, game));
        if (replacement) {
            EXPECT_NE(load_scene_file(f, "scenetmp:/empty.toml", false), scene);
        } else {
            char params[64], response[65536];
            mpack_writer_t writer;
            mpack_writer_init(&writer, params, sizeof params);
            de_ep_write_scene_params(&writer, &(de_ep_scene_params){scene});
            const size_t size = mpack_writer_buffer_used(&writer);
            ASSERT_EQ(mpack_writer_destroy(&writer), mpack_ok);
            mpack_tree_t tree;
            (void)request(f, DE_EP_METHOD_SCENE_UNLOAD, params, size, &tree, response);
            ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
        }
        de_asset_flush_unused(f->engine.assets);
        EXPECT_TRUE(de__asset_object(f->engine.assets, mesh) == NULL);
        EXPECT_TRUE(de_entity_alive(f->engine.world, game));
    }
}

TEST_F(editor_scene_assets, expired_transaction_preserves_scene_and_releases_requests, scene_fx) {
    cold_reset(f);
    de_entity old = de_entity_spawn(f->engine.world);
    begin_pending(f);
    /* Real monotonic deadline, no clock replacement or weakened cold-load oracle. */
    de_sleep_ns(3010000000ULL);
    finish_pending(f, true);
    EXPECT_TRUE(de_entity_alive(f->engine.world, old));
    de_asset_flush_unused(f->engine.assets);
    const u32 mesh = de__asset_handle_of(f->engine.assets, de_asset_id_from("hero#Body"), "mesh");
    EXPECT_TRUE(de__asset_object(f->engine.assets, mesh) == NULL);
    begin_pending(f);
    finish_pending(f, false);
    EXPECT_FALSE(de_entity_alive(f->engine.world, old));
}

TEST_F(editor_scene_assets, save_allocation_failure_never_returns_truncated_success, scene_fx) {
    char params[4096], response[65536];
    mpack_writer_t writer;
    mpack_writer_init(&writer, params, sizeof params);
    de_ep_write_scene_reload_params(&writer,
                                    &(de_ep_scene_reload_params){1, de_ep_cstr(cold_text)});
    size_t size = mpack_writer_buffer_used(&writer);
    ASSERT_EQ(mpack_writer_destroy(&writer), mpack_ok);
    mpack_tree_t tree;
    (void)request(f, DE_EP_METHOD_SCENE_RELOAD_FROM_TEXT, params, size, &tree, response);
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    char expected[32768];
    serialize(f, expected);
    mpack_writer_init(&writer, params, sizeof params);
    de_ep_write_scene_params(&writer, &(de_ep_scene_params){1});
    size = mpack_writer_buffer_used(&writer);
    ASSERT_EQ(mpack_writer_destroy(&writer), mpack_ok);
    u32 failures = 0;
    bool complete = false;
    for (i64 allowed = 0; allowed < 64; ++allowed) {
        de_tracking_allocator_fail_after(&f->tracking, allowed);
        mpack_node_t root =
            raw_request(f, DE_EP_METHOD_SCENE_SERIALIZE, params, size, &tree, response);
        de_tracking_allocator_fail_after(&f->tracking, -1);
        if (mpack_node_map_contains_cstr(root, "error")) {
            ++failures;
        } else {
            char actual[32768];
            mpack_node_copy_cstr(mpack_node_map_cstr(mpack_node_map_cstr(root, "result"), "text"),
                                 actual, sizeof actual);
            EXPECT_STREQ(expected, actual);
            complete = true;
        }
        ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
        if (complete)
            break;
    }
    EXPECT_GT(failures, 3u);
    EXPECT_TRUE(complete);
}

#if defined(__APPLE__)
void de__ed_views_shutdown(de__editor *ed);
TEST_F(editor_scene_assets, reload_preserves_real_editor_camera_and_other_scene_child, scene_fx) {
    ASSERT_EQ(de_gpu_device_create(
                  &(de_gpu_device_desc){.backend = DE_GPU_METAL, .pipeline_cache_dir = ""},
                  &f->engine.gpu),
              DE_OK);
    ASSERT_EQ(
        de_renderer_create(&(de_renderer_desc){.device = f->engine.gpu, .types = f->engine.types},
                           &f->engine.renderer),
        DE_OK);
    de_renderer_attach(f->engine.renderer, f->engine.world);
    de__ed_views_register(&f->editor);
    char params[4096], response[65536];
    mpack_writer_t writer;
    mpack_writer_init(&writer, params, sizeof params);
    double position[] = {0, 0, 5};
    float rotation[] = {0, 0, 0, 1};
    de_ep_view_create_params view = {.width = 16,
                                     .height = 16,
                                     .scale = 1,
                                     .render_mode = de_ep_cstr("lit"),
                                     .camera = {.position = {position, 3},
                                                .rotation = {rotation, 4},
                                                .fov_y = 1,
                                                .near = .1f,
                                                .far = 100}};
    de_ep_write_view_create_params(&writer, &view);
    size_t size = mpack_writer_buffer_used(&writer);
    ASSERT_EQ(mpack_writer_destroy(&writer), mpack_ok);
    mpack_tree_t tree;
    (void)request(f, DE_EP_METHOD_VIEW_CREATE, params, size, &tree, response);
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    de_entity camera = {0};
    for (u32 ai = 0; ai < f->engine.world->archetypes.count; ++ai) {
        const ecs_archetype *a = ((ecs_archetype **)f->engine.world->archetypes.data)[ai];
        for (u32 ci = 0; ci < a->chunks.count; ++ci) {
            const ecs_chunk *ch = ecs_table_chunk(a, ci);
            for (u32 row = 0; row < ch->count; ++row)
                if (de__ed_views_owns(ecs_chunk_entities(ch)[row].bits))
                    camera = ecs_chunk_entities(ch)[row];
        }
    }
    ASSERT_TRUE(camera.bits != 0);
    de_entity target = de_entity_spawn(f->engine.world);
    const char *other_text = "[[entity]]\nid=\"0xdead\"\n";
    ASSERT_EQ(de_file_write_all_atomic("scenetmp:/other.toml", other_text, strlen(other_text)),
              DE_OK);
    u64 other_scene = load_scene_file(f, "scenetmp:/other.toml", true);
    de_entity other = de_entity_spawn(f->engine.world);
    de__ed_scene_adopt(&f->editor, other, other_scene, (de_entity){0});
    ASSERT_EQ(de__ed_scene_of(&f->editor, other), other_scene);
    ASSERT_EQ(de_entity_set_parent(f->engine.world, other, target), DE_OK);
    mpack_writer_init(&writer, params, sizeof params);
    de_ep_write_scene_reload_params(&writer, &(de_ep_scene_reload_params){1, de_ep_cstr("")});
    size = mpack_writer_buffer_used(&writer);
    ASSERT_EQ(mpack_writer_destroy(&writer), mpack_ok);
    (void)request(f, DE_EP_METHOD_SCENE_RELOAD_FROM_TEXT, params, size, &tree, response);
    ASSERT_EQ(mpack_tree_destroy(&tree), mpack_ok);
    EXPECT_FALSE(de_entity_alive(f->engine.world, target));
    EXPECT_TRUE(de_entity_alive(f->engine.world, camera));
    EXPECT_TRUE(de__ed_views_owns(camera.bits));
    EXPECT_TRUE(de_entity_alive(f->engine.world, other));
    EXPECT_EQ(de__ed_scene_of(&f->editor, other), other_scene);
    EXPECT_EQ(de_entity_parent(f->engine.world, other).bits, 0u);
    de__ed_views_shutdown(&f->editor);
    de_renderer_destroy(f->engine.renderer);
    f->engine.renderer = NULL;
    de_gpu_device_destroy(f->engine.gpu);
    f->engine.gpu = NULL;
}
#endif
