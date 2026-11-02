#include "de_editor_protocol.h"
#include "de_test.h"

_Static_assert(MPACK_READ_TRACKING &&MPACK_WRITE_TRACKING,
               "raw protocol regressions must execute with compound tracking enabled");

static const char nested[] = "\x81\xa1"
                             "x"
                             "\x93\xc3\xa1"
                             "y"
                             "\x81\xa1"
                             "n"
                             "\x2a";

TEST(editor_protocol_raw, request_params_preserve_complete_values_and_reject_every_truncation) {
    char bytes[512];
    mpack_writer_t w;
    mpack_writer_init(&w, bytes, sizeof bytes);
    de_ep_write_request_begin(&w, 42, "edit.apply");
    mpack_write_object_bytes(&w, nested, sizeof nested - 1);
    de_ep_write_request_end(&w);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    de_ep_request req;
    ASSERT_TRUE(de_ep_read_request(bytes, size, &req));
    EXPECT_EQ(req.id, 42u);
    EXPECT_EQ(req.method.len, sizeof "edit.apply" - 1);
    EXPECT_MEMEQ(req.method.ptr, "edit.apply", req.method.len);
    ASSERT_EQ(req.params.len, sizeof nested - 1);
    EXPECT_MEMEQ(req.params.ptr, nested, sizeof nested - 1);
    EXPECT_TRUE(req.params.ptr >= bytes && req.params.ptr + req.params.len <= bytes + size);
    for (size_t n = 0; n < size; ++n) {
        EXPECT_FALSE(de_ep_read_request(bytes, n, &req));
        EXPECT_TRUE(req.params.ptr == NULL);
        EXPECT_EQ(req.params.len, 0u);
    }
}

static size_t edit_bytes(char bytes[512]) {
    de_ep_kv_raw fields[] = {{de_ep_cstr("Health"), {nested, sizeof nested - 1}},
                             {de_ep_cstr("Enabled"), {"\xc3", 1}}};
    const de_ep_edit_op op = {.op = de_ep_cstr("spawn"),
                              .has_entity = true,
                              .entity = 42,
                              .has_components = true,
                              .components = {fields, 2},
                              .has_value = true,
                              .value = {nested, sizeof nested - 1}};
    mpack_writer_t w;
    mpack_writer_init(&w, bytes, 512);
    de_ep_write_edit_op(&w, &op);
    const size_t size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
    return size;
}

TEST(editor_protocol_raw, generated_edit_values_preserve_complete_nested_component_maps) {
    char bytes[512];
    _Alignas(8) uint8_t storage[1024];
    const size_t size = edit_bytes(bytes);
    de_ep_arena arena = {storage, sizeof storage, 0};
    mpack_reader_t r;
    mpack_reader_init_data(&r, bytes, size);
    de_ep_edit_op op;
    de_ep_read_edit_op(&r, &arena, &op);
    ASSERT_EQ(mpack_reader_error(&r), mpack_ok);
    EXPECT_EQ(mpack_reader_remaining(&r, NULL), 0u);
    EXPECT_EQ(mpack_reader_destroy(&r), mpack_ok);
    EXPECT_TRUE(op.has_entity && op.has_components && op.has_value);
    EXPECT_EQ(op.entity, 42u);
    ASSERT_EQ(op.components.count, 2u);
    ASSERT_EQ(op.components.items[0].value.len, sizeof nested - 1);
    EXPECT_MEMEQ(op.components.items[0].value.ptr, nested, sizeof nested - 1);
    ASSERT_EQ(op.components.items[1].value.len, 1u);
    EXPECT_MEMEQ(op.components.items[1].value.ptr, "\xc3", 1);
    ASSERT_EQ(op.value.len, sizeof nested - 1);
    EXPECT_MEMEQ(op.value.ptr, nested, sizeof nested - 1);
}

TEST(editor_protocol_raw, buffered_readers_reject_borrowed_values_and_clear_outputs) {
    char bytes[512];
    _Alignas(8) uint8_t storage[1024];
    const size_t size = edit_bytes(bytes);
    de_ep_arena arena = {storage, sizeof storage, 0};
    mpack_reader_t r;
    mpack_reader_init(&r, bytes, sizeof bytes, size);
    de_ep_edit_op op;
    de_ep_read_edit_op(&r, &arena, &op);
    EXPECT_EQ(mpack_reader_destroy(&r), mpack_error_unsupported);
    ASSERT_TRUE(op.components.items != NULL);
    EXPECT_TRUE(op.components.items[0].value.ptr == NULL);
    EXPECT_EQ(op.components.items[0].value.len, 0u);
    EXPECT_TRUE(op.value.ptr == NULL);
    EXPECT_EQ(op.value.len, 0u);
}
