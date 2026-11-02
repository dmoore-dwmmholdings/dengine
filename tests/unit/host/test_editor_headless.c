#include "de_test.h"
#include "host/editor/editor_internal.h"
#include "host/engine_internal.h"

/* Call the registered production handlers, without renderer/GPU stubs or a live connection. */
static void expect_error(de__editor *ed, de_ep_method method, de_ep_raw params,
                         de_ep_error expected) {
    const u64 id = UINT64_C(0x1020304050607080);
    const de_ep_request req = {
        .id = id, .method = de_ep_cstr(de_ep_method_name(method)), .params = params};
    unsigned char arena_bytes[8192];
    de_ep_arena arena = {arena_bytes, sizeof arena_bytes, 0};
    char response[2048];
    mpack_writer_t writer;
    mpack_writer_init(&writer, response, sizeof response);
    ASSERT_TRUE(ed->handlers[method] != NULL);
    ed->handlers[method](ed, &req, &arena, &writer);
    const size_t n = mpack_writer_buffer_used(&writer);
    ASSERT_EQ(mpack_writer_destroy(&writer), mpack_ok);
    EXPECT_FALSE(ed->deferred);

    mpack_tree_t tree;
    mpack_tree_init_data(&tree, response, n);
    mpack_tree_parse(&tree);
    mpack_node_t root = mpack_tree_root(&tree);
    EXPECT_EQ(mpack_node_map_count(root), 2u);
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(root, "id")), id);
    EXPECT_FALSE(mpack_node_map_contains_cstr(root, "result"));
    mpack_node_t error = mpack_node_map_cstr(root, "error");
    EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(error, "code")), (i32)expected);
    EXPECT_TRUE(mpack_node_strlen(mpack_node_map_cstr(error, "message")) > 0);
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
}

static void register_handlers(de__editor *ed, de_engine *engine) {
    memset(engine, 0, sizeof *engine);
    memset(ed, 0, sizeof *ed);
    ed->e = engine;
    de__ed_views_register(ed);
    de__ed_assets_register(ed);
    EXPECT_EQ(ed->cap_count, 0u);
}

static void reset_handlers(de__editor *ed) {
    for (u32 i = 0; i < ed->reset_count; ++i)
        ed->resets[i](ed);
}

TEST(editor_headless, valid_gpu_requests_report_unsupported_without_deferring) {
    de_engine engine;
    de__editor ed;
    register_handlers(&ed, &engine);
    char params[512];
    mpack_writer_t w;
    mpack_writer_init(&w, params, sizeof params);
    const de_ep_pick_params pick = {.view = 1, .x = 12, .y = 34};
    de_ep_write_pick_params(&w, &pick);
    size_t n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    expect_error(&ed, DE_EP_METHOD_PICK, (de_ep_raw){params, n}, DE_EP_ERR_UNSUPPORTED);

    mpack_writer_init(&w, params, sizeof params);
    float rect[] = {1, 2, 30, 40};
    const de_ep_pick_rect_params pick_rect = {.view = 1, .rect = {rect, 4}};
    de_ep_write_pick_rect_params(&w, &pick_rect);
    n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    expect_error(&ed, DE_EP_METHOD_PICK_RECT, (de_ep_raw){params, n}, DE_EP_ERR_UNSUPPORTED);

    mpack_writer_init(&w, params, sizeof params);
    const de_ep_model_inspect_params inspect = {.id = de_ep_cstr("model"), .mesh = 0};
    de_ep_write_model_inspect_params(&w, &inspect);
    n = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    expect_error(&ed, DE_EP_METHOD_ASSET_MODEL_INSPECT, (de_ep_raw){params, n},
                 DE_EP_ERR_UNSUPPORTED);
    reset_handlers(&ed);
}

TEST(editor_headless, malformed_gpu_requests_still_validate_the_protocol_schema) {
    de_engine engine;
    de__editor ed;
    register_handlers(&ed, &engine);
    const de_ep_method methods[] = {DE_EP_METHOD_PICK, DE_EP_METHOD_PICK_RECT,
                                    DE_EP_METHOD_ASSET_MODEL_INSPECT};
    const char missing_fields[] = {(char)0x80}; /* Empty MessagePack map. */
    const char wrong_type[] = {(char)0xc3};     /* Boolean instead of the parameter map. */
    for (u32 i = 0; i < sizeof methods / sizeof methods[0]; ++i) {
        expect_error(&ed, methods[i], (de_ep_raw){missing_fields, sizeof missing_fields},
                     DE_EP_ERR_INVALID_PARAMS);
        expect_error(&ed, methods[i], (de_ep_raw){wrong_type, sizeof wrong_type},
                     DE_EP_ERR_INVALID_PARAMS);
    }
    reset_handlers(&ed);
}
