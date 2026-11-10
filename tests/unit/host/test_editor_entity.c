#include "de_test.h"
#if !defined(DE_SHIPPING)
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include <dengine/de_ecs.h>
#include <dengine/de_reflect.h>

/* Real registered handler, protocol decoder and fixed response arena; no host or GPU launch. */
typedef struct entity_fx {
    de_engine engine;
    de__editor editor;
    unsigned char storage[4096];
} entity_fx;

static void entity_fx_setup(entity_fx *f) {
    memset(f, 0, sizeof *f);
    ASSERT_EQ(de_type_registry_create(NULL, &f->engine.types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->engine.types}, &f->engine.world), DE_OK);
    f->editor.e = &f->engine;
    de__ed_world_register(&f->editor);
}
static void entity_fx_teardown(entity_fx *f) {
    for (u32 i = 0; i < f->editor.reset_count; ++i)
        f->editor.resets[i](&f->editor);
    de_world_destroy(f->engine.world);
    de_type_registry_destroy(f->engine.types);
}
static de_ep_request entity_request(char params[256], u64 *ids, u32 count) {
    de_ep_str missing = de_ep_cstr("ComponentThatDoesNotExist");
    const de_ep_entity_params q = {
        .ids = {ids, count}, .has_components = true, .components = {&missing, 1}};
    mpack_writer_t w;
    mpack_writer_init(&w, params, 256);
    de_ep_write_entity_params(&w, &q);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    return (de_ep_request){
        .id = 93, .method = de_ep_cstr("world.entity"), .params = {params, size}};
}
/* Exhaust the real arena after decoding and result storage, optionally allowing one empty
 * entity's component-map storage. The decoder's allocation layout is measured, not guessed. */
static size_t row_budget(entity_fx *f, const de_ep_request *req, u32 allowed_rows) {
    de_ep_arena a = {f->storage, sizeof f->storage, 0};
    de_ep_entity_params q;
    ASSERT_TRUE(de_ep_read_params_world_entity(req, &a, &q));
    ASSERT_TRUE(de__ed_alloc(&a, sizeof(de_ep_entity_row) * (q.ids.count + 1)) != NULL);
    for (u32 i = 0; i < allowed_rows; ++i)
        ASSERT_TRUE(de__ed_alloc(&a, sizeof(de_ep_kv_raw)) != NULL);
    return a.used;
}
static mpack_node_t entity_response(entity_fx *f, const de_ep_request *req, size_t capacity,
                                    char response[1024], mpack_tree_t *tree) {
    de_ep_arena a = {f->storage, capacity, 0};
    mpack_writer_t w;
    mpack_writer_init(&w, response, 1024);
    ASSERT_TRUE(f->editor.handlers[DE_EP_METHOD_WORLD_ENTITY] != NULL);
    f->editor.handlers[DE_EP_METHOD_WORLD_ENTITY](&f->editor, req, &a, &w);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    mpack_tree_init_data(tree, response, size);
    mpack_tree_parse(tree);
    mpack_node_t root = mpack_tree_root(tree);
    ASSERT_EQ(mpack_node_u64(mpack_node_map_cstr(root, "id")), req->id);
    return root;
}
static void expect_error(mpack_node_t root, de_ep_error code) {
    EXPECT_FALSE(mpack_node_map_contains_cstr(root, "result"));
    mpack_node_t error = mpack_node_map_cstr(root, "error");
    EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(error, "code")), (i32)code);
    EXPECT_GT(mpack_node_strlen(mpack_node_map_cstr(error, "message")), 0u);
}
TEST_F(editor_entity, row_arena_exhaustion_is_busy_without_partial_results, entity_fx) {
    const de_entity first = de_entity_spawn(f->engine.world);
    const de_entity second = de_entity_spawn(f->engine.world);
    ASSERT_TRUE(de_entity_alive(f->engine.world, first));
    ASSERT_TRUE(de_entity_alive(f->engine.world, second));
    u64 ids[] = {first.bits, second.bits};
    char params[256], response[1024];
    const de_ep_request req = entity_request(params, ids, 2);
    for (u32 allowed = 0; allowed < 2; ++allowed) {
        mpack_tree_t tree;
        mpack_node_t root = entity_response(f, &req, row_budget(f, &req, allowed), response, &tree);
        expect_error(root, DE_EP_ERR_BUSY);
        EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
        EXPECT_TRUE(de_entity_alive(f->engine.world, first));
        EXPECT_TRUE(de_entity_alive(f->engine.world, second));
    }
}
TEST_F(editor_entity, absent_entity_remains_not_found_when_no_row_storage_fits, entity_fx) {
    const de_entity dead = de_entity_spawn(f->engine.world);
    de_entity_despawn(f->engine.world, dead);
    u64 ids[] = {dead.bits};
    char params[256], response[1024];
    const de_ep_request req = entity_request(params, ids, 1);
    mpack_tree_t tree;
    mpack_node_t root = entity_response(f, &req, row_budget(f, &req, 0), response, &tree);
    expect_error(root, DE_EP_ERR_NOT_FOUND);
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
}
TEST_F(editor_entity, missing_requested_component_preserves_the_existing_entity, entity_fx) {
    const de_entity live = de_entity_spawn(f->engine.world);
    u64 ids[] = {live.bits};
    char params[256], response[1024];
    const de_ep_request req = entity_request(params, ids, 1);
    mpack_tree_t tree;
    mpack_node_t root = entity_response(f, &req, sizeof f->storage, response, &tree);
    EXPECT_FALSE(mpack_node_map_contains_cstr(root, "error"));
    mpack_node_t entities = mpack_node_map_cstr(mpack_node_map_cstr(root, "result"), "entities");
    ASSERT_EQ(mpack_node_array_length(entities), 1u);
    mpack_node_t entity = mpack_node_array_at(entities, 0);
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(entity, "entity")), live.bits);
    EXPECT_EQ(mpack_node_map_count(mpack_node_map_cstr(entity, "components")), 0u);
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
}
#endif
