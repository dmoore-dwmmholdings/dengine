/*
 * test_asset_types.c: the asset type registry, de_asset_type_register and its lookups (DENG-140,
 * SPEC §11.2).
 */
#include "de_test.h"
#include "test_bytes_type.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "asset/asset_internal.h"
#include "asset/slots_internal.h"

#include <stdio.h>
#include <string.h>

typedef struct types_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_asset_system *s;
} types_fx;

static void types_fx_setup(types_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &f->alloc}, &f->s),
              DE_OK);
}

static void types_fx_teardown(types_fx *f) {
    de_asset_system_destroy(f->s);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
}

static de_result last_result(void) {
    return de_error_last() != NULL ? de_error_last()->result : DE_OK;
}

TEST_F(asset_types, register_duplicate, types_fx) {
    de_asset_system *s = f->s;
    de_asset_type a = test_bytes_register(s, NULL);
    ASSERT_TRUE(DE_HANDLE_VALID(a));
    EXPECT_EQ(de__asset_type_find(s, TEST_BYTES_TYPE).id, a.id);

    de_error_clear();
    de_asset_type b = test_bytes_register(s, NULL);
    EXPECT_FALSE(DE_HANDLE_VALID(b));
    EXPECT_EQ(last_result(), DE_ERR_ALREADY_EXISTS);
    EXPECT_TRUE(strstr(de_error_last()->message, TEST_BYTES_TYPE) != NULL);

    u32 n = 0;
    const char *const *names = de__asset_type_names(s, &n);
    ASSERT_EQ(n, 1u);
    EXPECT_STREQ(names[0], TEST_BYTES_TYPE);

    /* Another name is fine; the order of registration is kept. */
    ASSERT_TRUE(
        DE_HANDLE_VALID(de_asset_type_register(s, &(de_asset_type_desc){.name = "test_other"})));
    names = de__asset_type_names(s, &n);
    ASSERT_EQ(n, 2u);
    EXPECT_STREQ(names[0], TEST_BYTES_TYPE);
    EXPECT_STREQ(names[1], "test_other");
}

TEST_F(asset_types, extension_lookup, types_fx) {
    de_asset_system *s = f->s;
    static const char *const exts[] = {".png", "JPG", ".Bytes"};
    de_asset_type t = de_asset_type_register(
        s, &(de_asset_type_desc){.name = "test_image", .extensions = exts, .extension_count = 3});
    ASSERT_TRUE(DE_HANDLE_VALID(t));

    EXPECT_EQ(de__asset_type_find_ext(s, ".png").id, t.id);
    EXPECT_EQ(de__asset_type_find_ext(s, "png").id, t.id);
    EXPECT_EQ(de__asset_type_find_ext(s, ".PNG").id, t.id);
    EXPECT_EQ(de__asset_type_find_ext(s, ".jpg").id, t.id);
    EXPECT_EQ(de__asset_type_find_ext(s, "bytes").id, t.id);
    EXPECT_FALSE(DE_HANDLE_VALID(de__asset_type_find_ext(s, ".exr")));
    EXPECT_FALSE(DE_HANDLE_VALID(de__asset_type_find_ext(s, ".pn")));
    EXPECT_FALSE(DE_HANDLE_VALID(de__asset_type_find_ext(s, "")));
    EXPECT_FALSE(DE_HANDLE_VALID(de__asset_type_find_ext(s, NULL)));

    /* Stored lowercase without the dot. */
    const de_asset_type_desc *d = de__asset_type_get(s, t);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ(d->extension_count, 3u);
    EXPECT_STREQ(d->extensions[0], "png");
    EXPECT_STREQ(d->extensions[1], "jpg");
    EXPECT_STREQ(d->extensions[2], "bytes");

    /* A second type that claims `.png` (and `.bytes`) registers, but the first keeps them. */
    de_asset_type b = test_bytes_register(s, NULL);
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    static const char *const png[] = {"png", "tga"};
    de_asset_type c = de_asset_type_register(
        s, &(de_asset_type_desc){.name = "test_sprite", .extensions = png, .extension_count = 2});
    ASSERT_TRUE(DE_HANDLE_VALID(c));
    EXPECT_EQ(de__asset_type_find_ext(s, ".png").id, t.id);
    EXPECT_EQ(de__asset_type_find_ext(s, ".bytes").id, t.id);
    EXPECT_EQ(de__asset_type_find_ext(s, ".tga").id, c.id);
}

static de_result dummy_load(de_asset_load_ctx *ctx) {
    (void)ctx;
    return DE_OK;
}

TEST_F(asset_types, desc_is_copied, types_fx) {
    de_asset_system *s = f->s;
    char name[] = "test_copy";
    char ext0[] = ".dat";
    const char *exts[] = {ext0};
    int user = 0;
    de_asset_type_desc desc = {.name = name,
                               .extensions = exts,
                               .extension_count = 1,
                               .importer_version = 7,
                               .load = dummy_load,
                               .user = &user,
                               .main_thread_load = true};
    de_asset_type t = de_asset_type_register(s, &desc);
    ASSERT_TRUE(DE_HANDLE_VALID(t));
    /* The caller's strings and array may change or die after the call. */
    name[0] = 'X';
    ext0[1] = 'X';
    exts[0] = NULL;
    memset(&desc, 0, sizeof desc);

    const de_asset_type_desc *d = de__asset_type_get(s, t);
    ASSERT_TRUE(d != NULL);
    EXPECT_STREQ(d->name, "test_copy");
    EXPECT_STREQ(d->extensions[0], "dat");
    EXPECT_EQ(d->importer_version, 7u); /* kept for the S08 cook cache */
    EXPECT_TRUE(d->load == dummy_load);
    EXPECT_TRUE(d->user == &user);
    EXPECT_TRUE(d->main_thread_load);
    EXPECT_EQ(de__asset_type_find(s, "test_copy").id, t.id);
    EXPECT_EQ(de__asset_type_find_hash(s, de_hash64_str("test_copy")).id, t.id);
    EXPECT_FALSE(DE_HANDLE_VALID(de__asset_type_find(s, "Xest_copy")));

    /* Invalid handles. */
    EXPECT_TRUE(de__asset_type_get(s, (de_asset_type){0}) == NULL);
    EXPECT_TRUE(de__asset_type_get(s, (de_asset_type){t.id + 1}) == NULL); /* index 1: none */
    EXPECT_TRUE(de__asset_type_get(s, (de_asset_type){de__slot_handle_make(0, 2)}) == NULL);
    EXPECT_TRUE(de__asset_type_get(NULL, t) == NULL);
}

TEST_F(asset_types, invalid_descs, types_fx) {
    de_asset_system *s = f->s;
    static const char *const ok_ext[] = {"ok"};
    static const char *const null_ext[] = {NULL};
    static const char *const empty_ext[] = {""};
    static const char *const dot_ext[] = {"."};
    static const char *const path_ext[] = {"a/b"};
    static const char *const multi_ext[] = {".tar.gz"};
    struct {
        de_asset_type_desc d;
        const char *why;
    } bad[] = {
        {{.name = NULL}, "NULL name"},
        {{.name = ""}, "empty name"},
        {{.name = "has space"}, "space"},
        {{.name = "quote\""}, "quote"},
        {{.name = "tab\t"}, "control"},
        {{.name = "\xC3\xA9t\xC3\xA9"}, "non-ASCII"},
        {{.name = "a123456789012345678901234567890123456789012345678901234567890123"}, "64 bytes"},
        {{.name = "t", .extensions = NULL, .extension_count = 1}, "count without array"},
        {{.name = "t", .extensions = null_ext, .extension_count = 1}, "NULL extension"},
        {{.name = "t", .extensions = empty_ext, .extension_count = 1}, "empty extension"},
        {{.name = "t", .extensions = dot_ext, .extension_count = 1}, "only a dot"},
        {{.name = "t", .extensions = path_ext, .extension_count = 1}, "slash"},
        {{.name = "t", .extensions = multi_ext, .extension_count = 1}, "two dots"},
    };
    for (u32 i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        de_error_clear();
        de_asset_type t = de_asset_type_register(s, &bad[i].d);
        if (DE_HANDLE_VALID(t) || last_result() != DE_ERR_INVALID_ARGUMENT) {
            char msg[128];
            snprintf(msg, sizeof msg, "case %u (%s) was accepted", i, bad[i].why);
            FAIL(msg);
        }
    }
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_type_register(NULL, &bad[1].d)));
    EXPECT_EQ(last_result(), DE_ERR_INVALID_ARGUMENT);
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_type_register(s, NULL)));
    u32 n = 9;
    EXPECT_TRUE(de__asset_type_names(s, &n) == NULL);
    EXPECT_EQ(n, 0u);

    /* 63 bytes is the longest name; an extension list with a valid entry works. */
    EXPECT_TRUE(DE_HANDLE_VALID(de_asset_type_register(
        s, &(de_asset_type_desc){
               .name = "a12345678901234567890123456789012345678901234567890123456789012",
               .extensions = ok_ext,
               .extension_count = 1})));
}

TEST_F(asset_types, capacity_and_oom, types_fx) {
    de_asset_system *s = f->s;
    char name[32];
    for (u32 i = 0; i < DE__ASSET_MAX_TYPES; ++i) {
        snprintf(name, sizeof name, "test_t%u", i);
        if (!DE_HANDLE_VALID(de_asset_type_register(s, &(de_asset_type_desc){.name = name})))
            FAIL("register failed before the capacity");
    }
    de_error_clear();
    EXPECT_FALSE(
        DE_HANDLE_VALID(de_asset_type_register(s, &(de_asset_type_desc){.name = "one_more"})));
    EXPECT_EQ(last_result(), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de__asset_type_find(s, "test_t255").id, de__slot_handle_make(255, 1));

    /* An allocation failure leaves the registry as it was. */
    de_tracking_allocator t2;
    de_allocator a2 = de_tracking_allocator_init(&t2, NULL, DE_MEM_ASSET);
    de_asset_system *s2 = NULL;
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &a2}, &s2), DE_OK);
    for (i64 n = 0; n < 3; ++n) {
        de_tracking_allocator_fail_after(&t2, n);
        de_asset_type r = de_asset_type_register(s2, &(de_asset_type_desc){.name = "x"});
        de_tracking_allocator_fail_after(&t2, -1);
        if (DE_HANDLE_VALID(r))
            break;
        EXPECT_EQ(last_result(), DE_ERR_OUT_OF_MEMORY);
        EXPECT_FALSE(DE_HANDLE_VALID(de__asset_type_find(s2, "x")));
    }
    EXPECT_TRUE(DE_HANDLE_VALID(de__asset_type_find(s2, "x")));
    de_asset_system_destroy(s2);
    EXPECT_EQ(de_tracking_allocator_stats(&t2).blocks_live, 0u);
}
