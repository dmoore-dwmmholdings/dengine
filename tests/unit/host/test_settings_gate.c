#include "de_test.h"
#if !defined(DE_SHIPPING)
#include "host/editor/editor_internal.h"
#include "host/engine_internal.h"

/* Exercise the registered production callback and its real MessagePack response. */
static void expect_error(de__editor *ed, de_ep_raw params, de_ep_error expected,
                         const char *message) {
    const u64 id = UINT64_C(0x1122334455667788);
    const de_ep_request req = {
        .id = id,
        .method = de_ep_cstr(de_ep_method_name(DE_EP_METHOD_ASSET_PREVIEW_SETTINGS)),
        .params = params};
    unsigned char storage[8192];
    de_ep_arena arena = {storage, sizeof storage, 0};
    char response[2048];
    mpack_writer_t writer;
    mpack_writer_init(&writer, response, sizeof response);
    ASSERT_TRUE(ed->handlers[DE_EP_METHOD_ASSET_PREVIEW_SETTINGS] != NULL);
    ed->handlers[DE_EP_METHOD_ASSET_PREVIEW_SETTINGS](ed, &req, &arena, &writer);
    const size_t bytes = mpack_writer_buffer_used(&writer);
    ASSERT_EQ(mpack_writer_destroy(&writer), mpack_ok);
    EXPECT_FALSE(ed->deferred);

    mpack_tree_t tree;
    mpack_tree_init_data(&tree, response, bytes);
    mpack_tree_parse(&tree);
    mpack_node_t root = mpack_tree_root(&tree);
    EXPECT_EQ(mpack_node_map_count(root), 2u);
    EXPECT_EQ(mpack_node_u64(mpack_node_map_cstr(root, "id")), id);
    EXPECT_FALSE(mpack_node_map_contains_cstr(root, "result"));
    mpack_node_t error = mpack_node_map_cstr(root, "error");
    EXPECT_EQ(mpack_node_i32(mpack_node_map_cstr(error, "code")), (i32)expected);
    if (message) {
        mpack_node_t text = mpack_node_map_cstr(error, "message");
        ASSERT_EQ(mpack_node_strlen(text), strlen(message));
        ASSERT_MEMEQ(mpack_node_str(text), message, strlen(message));
    }
    EXPECT_EQ(mpack_tree_destroy(&tree), mpack_ok);
}
TEST(editor_settings_gate, malformed_requests_validate_before_capability_checks) {
    de__editor ed = {0}; /* No engine is needed to validate the protocol schema. */
    de__ed_settings_register(&ed);
    const u8 missing[] = {0x80}, wrong_type[] = {0xc3};
    expect_error(&ed, (de_ep_raw){(const char *)missing, sizeof missing}, DE_EP_ERR_INVALID_PARAMS,
                 NULL);
    expect_error(&ed, (de_ep_raw){(const char *)wrong_type, sizeof wrong_type},
                 DE_EP_ERR_INVALID_PARAMS, NULL);
    de__ed_settings_shutdown(&ed);
}
TEST(editor_settings_gate, cooker_capability_precedes_source_asset_state) {
    de__editor ed = {0};
#if DE_HAS_COOKER
    de_engine engine = {0}; /* Available cooker still requires an actual source manifest. */
    ed.e = &engine;
#endif
    de__ed_settings_register(&ed);
    char params[256];
    mpack_writer_t writer;
    mpack_writer_init(&writer, params, sizeof params);
    mpack_start_map(&writer, 2);
    mpack_write_cstr(&writer, "id");
    mpack_write_cstr(&writer, "texture/test");
    mpack_write_cstr(&writer, "settings");
    mpack_start_map(&writer, 0);
    mpack_finish_map(&writer);
    mpack_finish_map(&writer);
    const size_t bytes = mpack_writer_buffer_used(&writer);
    ASSERT_EQ(mpack_writer_destroy(&writer), mpack_ok);
#if DE_HAS_COOKER
    const char *message = "settings previews require source assets";
#else
    /* A disabled cooker must answer without even dereferencing ed.e. */
    const char *message = "settings previews require the Dev cooker";
#endif
    expect_error(&ed, (de_ep_raw){params, bytes}, DE_EP_ERR_UNSUPPORTED, message);
    de__ed_settings_shutdown(&ed);
}

#endif
