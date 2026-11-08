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
    de_engine engine;
    de__host host;
    de__editor editor;
    de_load_handle load;
    bool shift;
    char note[32768];
    u64 arena[32768]; /* Match the production allocator-backed arena alignment. */
} scene_fx;
static void scene_fx_setup(scene_fx *f) {
    ASSERT_EQ(de_type_registry_create(NULL, &f->engine.types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->engine.types}, &f->engine.world), DE_OK);
    (void)de_builtin_components(f->engine.world);
    ASSERT_EQ(de_render_register_components(f->engine.world), DE_OK);
    ASSERT_EQ(de_log_init(&(de_log_desc){.no_console_sink = true}), DE_OK);
    ASSERT_EQ(de_fs_mount("scenedata", DE_EDITOR_SCENE_DATA, DE_MOUNT_READ_ONLY, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("scenetmp", DE_EDITOR_SCENE_TMP, DE_MOUNT_WRITABLE, 0), DE_OK);
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
    f->editor.mode = DE_EP_WORLD_MODE_EDIT;
    f->editor.note = f->note;
    f->editor.note_cap = sizeof f->note;
    de__ed_scene_register(&f->editor);
}
static void scene_fx_teardown(scene_fx *f) {
    de_asset_release(f->engine.assets, f->load);
    de_world_destroy(f->engine.world);
    de_asset_system_destroy(f->engine.assets);
    de_type_registry_destroy(f->engine.types);
    de_log_shutdown();
}
/* Invoke the registered production protocol handlers, retaining their actual response. */
static mpack_node_t request(scene_fx *f, de_ep_method method, const char *params, size_t size,
                            mpack_tree_t *tree, char response[65536]) {
    de_ep_request req = {.id = 7, .params = {params, size}};
    de_ep_arena arena = {(u8 *)f->arena, sizeof f->arena, 0};
    mpack_writer_t w;
    mpack_writer_init(&w, response, 65536);
    f->editor.handlers[method](&f->editor, &req, &arena, &w);
    size_t written = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_init_data(tree, response, written);
    mpack_tree_parse(tree);
    mpack_node_t root = mpack_tree_root(tree);
    if (mpack_node_map_contains_cstr(root, "error")) {
        mpack_node_t message = mpack_node_map_cstr(mpack_node_map_cstr(root, "error"), "message");
        fprintf(stderr, "handler: %.*s\n", (int)mpack_node_strlen(message),
                mpack_node_str(message));
    }
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
