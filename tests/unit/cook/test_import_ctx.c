/*
 * test_import_ctx.c: the importer-facing API of de_asset.h (de_asset_import_*, DENG-209): extra
 * source files and the asset-root boundary, settings, the scratch allocator, failures. CTest
 * names cooker.import_ctx.<test>.
 */
#include "cook_test_util.h"
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "asset/manifest_internal.h"
#include "cook/lib/de_cooker.h"

#include <stdio.h>
#include <string.h>

typedef struct ictx_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_cooker *c;
} ictx_fx;

static void ictx_fx_setup(ictx_fx *f) {
    ckt_mount();
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    de_cooker_desc d = {.allocator = &f->alloc,
                        .platform = DE_COOK_PLATFORM_LINUX,
                        .verify_determinism = DE_COOK_VERIFY_ON};
    ASSERT_EQ(de_cooker_create(&d, &f->c), DE_OK);
    de_asset_type_desc t = ckt_upper_type(NULL);
    ASSERT_EQ(de_cooker_register_type(f->c, &t), DE_OK);
}

static void ictx_fx_teardown(ictx_fx *f) {
    de_cooker_destroy(f->c);
    f->c = NULL;
    /* Outputs, scratch blocks left by importers, the read files: all gone. */
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
}

static de_result cook_in(ictx_fx *f, const char *root, const de__manifest_asset *a,
                         de_cook_result *r) {
    de_cook_input in = {.asset = a, .asset_root = root};
    return de_cooker_cook(f->c, &in, r);
}

/* ---- cooker.import_ctx.read_file_recorded --------------------------------------------------- */

TEST_F(import_ctx, read_file_recorded, ictx_fx) {
    ckt_put("ic_read_file_recorded/assets/text/main.txt", "main;");
    ckt_put("ic_read_file_recorded/assets/text/inc/a.txt", "a;");
    ckt_put("ic_read_file_recorded/assets/shared/b.txt", "b;");
    /* Relative to the source's directory; `..` is fine while it stays below the root; a file
     * read twice is listed once, in the order of the first read. */
    const char *const reads[] = {"inc/a.txt", "../shared/b.txt", "./inc/../inc/a.txt"};
    const de__manifest_kv kv[] = {
        {"read", {.type = DE__MANIFEST_STRING_ARRAY, .u.arr = {reads, 3}}},
        {"sub", {.type = DE__MANIFEST_BOOL, .u.b = false}},
    };
    de__manifest_asset a = ckt_asset("text/main", "upper", "text/main.txt", kv, 2);
    de_cook_result r;
    ASSERT_EQ(cook_in(f, "ckt:/ic_read_file_recorded/assets", &a, &r), DE_OK);
    EXPECT_EQ(r.size, 11u);
    EXPECT_MEMEQ(r.data, "MAIN;A;B;A;", 11);
    ASSERT_EQ(r.extra_source_count, 2u);
    EXPECT_STREQ(r.extra_sources[0], "text/inc/a.txt");
    EXPECT_STREQ(r.extra_sources[1], "shared/b.txt");
    EXPECT_STREQ(r.source, "text/main.txt");
    u64 h1 = r.output_hash;
    de_cook_result_free(&r);

    /* The extra sources are part of the output hash, so a different list is a different cook. */
    const de__manifest_kv kv1[] = {
        {"read", {.type = DE__MANIFEST_STRING_ARRAY, .u.arr = {reads, 1}}},
        {"sub", {.type = DE__MANIFEST_BOOL, .u.b = false}},
    };
    a = ckt_asset("text/main", "upper", "text/main.txt", kv1, 2);
    ASSERT_EQ(cook_in(f, "ckt:/ic_read_file_recorded/assets", &a, &r), DE_OK);
    EXPECT_EQ(r.extra_source_count, 1u);
    EXPECT_NE(r.output_hash, h1);
    de_cook_result_free(&r);

    /* A source at the top of the root reads its neighbours. */
    ckt_put("ic_read_file_recorded/assets/top.txt", "top;");
    const char *const top_reads[] = {"shared/b.txt"};
    const de__manifest_kv kv2[] = {
        {"read", {.type = DE__MANIFEST_STRING_ARRAY, .u.arr = {top_reads, 1}}},
        {"sub", {.type = DE__MANIFEST_BOOL, .u.b = false}},
    };
    a = ckt_asset("top", "upper", "top.txt", kv2, 2);
    ASSERT_EQ(cook_in(f, "ckt:/ic_read_file_recorded/assets", &a, &r), DE_OK);
    EXPECT_MEMEQ(r.data, "TOP;B;", 6);
    ASSERT_EQ(r.extra_source_count, 1u);
    EXPECT_STREQ(r.extra_sources[0], "shared/b.txt");
    de_cook_result_free(&r);

    /* A missing file fails the import (the importer passes the error on). */
    const char *const missing[] = {"inc/nope.txt"};
    const de__manifest_kv kv3[] = {
        {"read", {.type = DE__MANIFEST_STRING_ARRAY, .u.arr = {missing, 1}}}};
    a = ckt_asset("text/main", "upper", "text/main.txt", kv3, 1);
    EXPECT_EQ(cook_in(f, "ckt:/ic_read_file_recorded/assets", &a, &r), DE_ERR_NOT_FOUND);
    EXPECT_TRUE(strstr(de_error_last()->message, "text/inc/nope.txt") != NULL);
    /* The message names the asset once. */
    const char *msg = de_error_last()->message;
    const char *first = strstr(msg, "asset \"text/main\"");
    EXPECT_TRUE(first != NULL && strstr(first + 1, "asset \"text/main\"") == NULL);
}

/* ---- cooker.import_ctx.rejects_escape ------------------------------------------------------- */

static const char *g_escape_path;
static de_result g_escape_result;

static de_result escape_import(de_asset_import_ctx *ctx) {
    const void *d = NULL;
    u64 n = 0;
    g_escape_result = de_asset_import_read_file(ctx, g_escape_path, &d, &n);
    if (d != NULL || n != 0)
        return de_asset_import_fail(ctx, DE_ERR_INTERNAL, "a refused read returned data");
    return DE_OK; /* the refusal is checked by the test */
}

TEST_F(import_ctx, rejects_escape, ictx_fx) {
    ckt_put("ic_rejects_escape/assets/text/main.txt", "main");
    ckt_put("ic_rejects_escape/outside.txt", "secret");
    de_asset_type_desc t = {.name = "escape", .import = escape_import};
    ASSERT_EQ(de_cooker_register_type(f->c, &t), DE_OK);
    de__manifest_asset a = ckt_asset("text/main", "escape", "text/main.txt", NULL, 0);
    const char *bad[] = {
        "../../outside.txt",                  /* leaves the asset root */
        "../../../../etc/passwd",             /* leaves the mount root */
        "/etc/passwd",                        /* absolute */
        "ckt:/ic_rejects_escape/outside.txt", /* a virtual path */
        "..\\..\\outside.txt",                /* backslashes */
        "..",                                 /* the root's parent */
        "",                                   /* nothing */
    };
    for (u32 i = 0; i < (u32)(sizeof bad / sizeof bad[0]); ++i) {
        g_escape_path = bad[i];
        g_escape_result = DE_OK;
        de_cook_result r;
        ASSERT_EQ(cook_in(f, "ckt:/ic_rejects_escape/assets", &a, &r), DE_OK);
        EXPECT_EQ(g_escape_result, DE_ERR_INVALID_ARGUMENT);
        if (g_escape_result != DE_ERR_INVALID_ARGUMENT)
            printf("  accepted: \"%s\"\n", bad[i]);
        EXPECT_EQ(r.extra_source_count, 0u); /* refused paths are not recorded */
        de_cook_result_free(&r);
    }
    /* "../.." from text/ is the root itself: refused too (it is a directory, not a file). */
    g_escape_path = "../";
    de_cook_result r;
    ASSERT_EQ(cook_in(f, "ckt:/ic_rejects_escape/assets", &a, &r), DE_OK);
    EXPECT_EQ(g_escape_result, DE_ERR_INVALID_ARGUMENT);
    de_cook_result_free(&r);

    /* The source itself cannot leave the root either. */
    a = ckt_asset("text/out", "escape", "../outside.txt", NULL, 0);
    EXPECT_EQ(cook_in(f, "ckt:/ic_rejects_escape/assets", &a, &r), DE_ERR_INVALID_ARGUMENT);
    a = ckt_asset("text/out", "escape", "text/../../outside.txt", NULL, 0);
    EXPECT_EQ(cook_in(f, "ckt:/ic_rejects_escape/assets", &a, &r), DE_ERR_INVALID_ARGUMENT);
}

/* ---- cooker.import_ctx.inputs_and_scratch --------------------------------------------------- */

typedef struct seen {
    de_asset_id id;
    char path[64];
    de_cook_platform platform;
    void *user;
    bool has_level, has_nope, flag;
    i64 level;
    f64 ratio, level_as_float;
    char name[16];
    u32 tag_count;
    char tag1[8];
} seen;

static de_result inputs_import(de_asset_import_ctx *ctx) {
    seen *s = (seen *)de_asset_import_user(ctx);
    s->id = de_asset_import_id(ctx);
    snprintf(s->path, sizeof s->path, "%s", de_asset_import_source_path(ctx));
    s->platform = de_asset_import_platform(ctx);
    s->user = de_asset_import_user(ctx);
    s->has_level = de_asset_import_setting_has(ctx, "level");
    s->has_nope = de_asset_import_setting_has(ctx, "nope");
    s->level = de_asset_import_setting_int(ctx, "level", -1);
    s->level_as_float = de_asset_import_setting_float(ctx, "level", -1.0);
    s->ratio = de_asset_import_setting_float(ctx, "ratio", -1.0);
    s->flag = de_asset_import_setting_bool(ctx, "flag", false);
    snprintf(s->name, sizeof s->name, "%s", de_asset_import_setting_string(ctx, "name", "-"));
    const char *const *items = NULL;
    s->tag_count = de_asset_import_setting_string_array(ctx, "tags", &items);
    snprintf(s->tag1, sizeof s->tag1, "%s", s->tag_count > 1 ? items[1] : "-");
    /* Wrong types fall back. */
    if (de_asset_import_setting_int(ctx, "name", 5) != 5 ||
        de_asset_import_setting_string(ctx, "level", NULL) != NULL ||
        de_asset_import_setting_string_array(ctx, "name", &items) != 0 || items != NULL)
        return de_asset_import_fail(ctx, DE_ERR_INTERNAL, "a wrong-type getter did not fall back");

    /* Scratch: grow one block, allocate a few more, free one; the cooker frees the rest. */
    const de_allocator *a = de_asset_import_allocator(ctx);
    u8 *p = (u8 *)DE_ALLOC(a, 16, 64, DE_MEM_SCRATCH);
    if (p == NULL || ((uintptr_t)p & 63u) != 0)
        return de_asset_import_fail(ctx, DE_ERR_INTERNAL, "scratch alignment");
    memset(p, 0xAB, 16);
    p = (u8 *)DE_REALLOC(a, p, 16, 4096, 64, DE_MEM_SCRATCH);
    if (p == NULL || p[15] != 0xAB)
        return de_asset_import_fail(ctx, DE_ERR_INTERNAL, "scratch realloc");
    void *q = DE_ALLOC(a, 100, 8, DE_MEM_SCRATCH);
    void *w = DE_ALLOC(a, 200, 8, DE_MEM_SCRATCH);
    DE_FREE(a, q, 100);
    (void)w;

    u64 n = 0;
    const void *src = de_asset_import_source(ctx, &n);
    DE_TRY(de_asset_import_write(ctx, src, n));
    DE_TRY(de_asset_import_write(ctx, NULL, 0));
    if (de_asset_import_write(ctx, NULL, 3) != DE_ERR_INVALID_ARGUMENT)
        return de_asset_import_fail(ctx, DE_ERR_INTERNAL, "NULL write accepted");
    return DE_OK;
}

TEST_F(import_ctx, inputs_and_scratch, ictx_fx) {
    ckt_put("ic_inputs_and_scratch/assets/data/in.bin", "payload");
    seen s;
    memset(&s, 0, sizeof s);
    de_asset_type_desc t = {.name = "inputs", .import = inputs_import, .user = &s};
    ASSERT_EQ(de_cooker_register_type(f->c, &t), DE_OK);
    const char *const tags[] = {"a", "bee"};
    const de__manifest_kv kv[] = {
        {"flag", {.type = DE__MANIFEST_BOOL, .u.b = true}},
        {"level", {.type = DE__MANIFEST_INT, .u.i = 4}},
        {"name", {.type = DE__MANIFEST_STRING, .u.s = "hero"}},
        {"ratio", {.type = DE__MANIFEST_FLOAT, .u.f = 0.25}},
        {"tags", {.type = DE__MANIFEST_STRING_ARRAY, .u.arr = {tags, 2}}},
    };
    de__manifest_asset a = ckt_asset("data/in", "inputs", "data/./in.bin", kv, 5);
    de_cook_result r;
    ASSERT_EQ(cook_in(f, "ckt:/ic_inputs_and_scratch//assets/", &a, &r),
              DE_OK); /* the root is normalised */
    EXPECT_MEMEQ(r.data, "payload", 7);
    de_cook_result_free(&r);
    EXPECT_EQ(s.id.hash, de_asset_id_from("data/in").hash);
    EXPECT_STREQ(s.path, "data/in.bin");
    EXPECT_EQ((int)s.platform, (int)DE_COOK_PLATFORM_LINUX);
    EXPECT_TRUE(s.user == &s);
    EXPECT_TRUE(s.has_level);
    EXPECT_FALSE(s.has_nope);
    EXPECT_EQ(s.level, 4);
    EXPECT_FLOAT_EQ(s.level_as_float, 4.0, 0.0);
    EXPECT_FLOAT_EQ(s.ratio, 0.25, 0.0);
    EXPECT_TRUE(s.flag);
    EXPECT_STREQ(s.name, "hero");
    EXPECT_EQ(s.tag_count, 2u);
    EXPECT_STREQ(s.tag1, "bee");

    /* NULL contexts are harmless. */
    u64 n = 1;
    EXPECT_STREQ((const char *)de_asset_import_source(NULL, &n), "");
    EXPECT_EQ(n, 0u);
    EXPECT_STREQ(de_asset_import_source_path(NULL), "");
    EXPECT_EQ(de_asset_import_id(NULL).hash, 0u);
    EXPECT_FALSE(de_asset_import_setting_has(NULL, "x"));
    EXPECT_EQ(de_asset_import_fail(NULL, DE_OK, "x"), DE_ERR_INTERNAL);
}

/* ---- cooker.import_ctx.failures ------------------------------------------------------------- */

static de_result fail_then_ok_import(de_asset_import_ctx *ctx) {
    (void)de_asset_import_fail(ctx, DE_ERR_PARSE, "bad header at byte %d", 12);
    (void)de_asset_import_fail(ctx, DE_ERR_IO, "second failure"); /* the first one wins */
    return DE_OK;
}

static de_result plain_error_import(de_asset_import_ctx *ctx) {
    (void)ctx;
    return DE_ERR_UNSUPPORTED; /* no message: the cooker writes one */
}

static de_result context_error_import(de_asset_import_ctx *ctx) {
    (void)ctx;
    return DE_FAIL(DE_ERR_PARSE, "chunk %s is broken", "IHDR");
}

TEST_F(import_ctx, failures, ictx_fx) {
    ckt_put("ic_failures/assets/text/main.txt", "main");
    struct {
        const char *type;
        de_result (*fn)(de_asset_import_ctx *);
        de_result code;
        const char *msg;
    } cases[] = {
        {"fail_ok", fail_then_ok_import, DE_ERR_PARSE,
         "asset \"text/main\": bad header at byte 12"},
        {"plain", plain_error_import, DE_ERR_UNSUPPORTED, "the \"plain\" importer failed"},
        {"ctxerr", context_error_import, DE_ERR_PARSE, "chunk IHDR is broken"},
    };
    for (u32 i = 0; i < 3; ++i) {
        de_asset_type_desc t = {.name = cases[i].type, .import = cases[i].fn};
        ASSERT_EQ(de_cooker_register_type(f->c, &t), DE_OK);
        de__manifest_asset a = ckt_asset("text/main", cases[i].type, "text/main.txt", NULL, 0);
        de_cook_result r;
        EXPECT_EQ(cook_in(f, "ckt:/ic_failures/assets", &a, &r), cases[i].code);
        EXPECT_TRUE(strstr(de_error_last()->message, cases[i].msg) != NULL);
        if (strstr(de_error_last()->message, cases[i].msg) == NULL)
            printf("  %s: \"%s\"\n", cases[i].type, de_error_last()->message);
        EXPECT_TRUE(r.data == NULL);
    }
}
