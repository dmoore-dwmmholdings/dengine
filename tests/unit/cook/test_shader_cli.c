#include "de_test.h"
#include "shader_cmd.h"
#include "importers/shader/slang_compile.h"
#include <string.h>

static de_result no_import(void *user, const char *path, const void **data, u64 *size) {
    (void)user;
    (void)path;
    (void)data;
    (void)size;
    return DE_ERR_NOT_FOUND;
}
TEST(shader_cli, reflection_ownership_and_budgets) {
    char why[256];
    if (!de__slang_available(why, sizeof why))
        SKIP(why);
    de_tracking_allocator tracking;
    de_allocator allocator = de_tracking_allocator_init(&tracking, NULL, DE_MEM_ASSET);
    const char *source = "[shader(\"compute\")][numthreads(8,4,1)] void cs_main() {}";
    de__slang_compile_desc d = {.target = DE__SLANG_SPIRV_ASM,
                                .path = "inspect.slang",
                                .source = source,
                                .source_size = strlen(source),
                                .read = no_import,
                                .allocator = &allocator,
                                .reflection_json = true,
                                .max_entries = 1,
                                .max_output_bytes = 1024u * 1024u};
    de__slang_compile_result result = {0};
    ASSERT_EQ(de__slang_compile(&d, &result), DE_OK);
    EXPECT_TRUE(strstr(result.entries[0].reflection_json, "entryPoints") != NULL);
    EXPECT_EQ(result.entries[0].threadgroup[0], 8u);
    de__slang_compile_result_free(&result);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).blocks_live, 0u);
    d.max_output_bytes = 8;
    EXPECT_EQ(de__slang_compile(&d, &result), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).blocks_live, 0u);
    d.max_output_bytes = 1024u * 1024u;
    for (i64 allowed = 2; allowed <= 4; ++allowed) {
        de_tracking_allocator_fail_after(&tracking, allowed);
        EXPECT_EQ(de__slang_compile(&d, &result), DE_ERR_OUT_OF_MEMORY);
        EXPECT_EQ(de_tracking_allocator_stats(&tracking).blocks_live, 0u);
    }
}

TEST(shader_cli, arguments_and_limits) {
    const char *args[] = {"source path/a.slang",
                          "--target",
                          "spirv",
                          "--code",
                          "a.txt",
                          "--reflection",
                          "a.json",
                          "--report",
                          "report.json",
                          "-Iinclude path",
                          "-D",
                          "VALUE=2",
                          "--entry",
                          "cs_main"};
    de_cook_shader_options o;
    char error[256];
    ASSERT_TRUE(de_cook_shader_parse(14, args, &o, error));
    EXPECT_EQ(o.include_count, 1u);
    EXPECT_EQ(o.define_count, 1u);
    EXPECT_EQ(o.entry_count, 1u);
    EXPECT_STREQ(o.includes[0], "include path");
    EXPECT_STREQ(o.defines[0], "VALUE=2");
    const char *bad[] = {"--target", "bogus"};
    EXPECT_FALSE(de_cook_shader_parse(2, bad, &o, error));
    const char *missing[] = {"--report"};
    EXPECT_FALSE(de_cook_shader_parse(1, missing, &o, error));
    const char *duplicates[] = {"x.slang", "--target", "metal", "--target", "spirv"};
    EXPECT_FALSE(de_cook_shader_parse(5, duplicates, &o, error));
    const char *many[34];
    for (u32 i = 0; i < 34; ++i)
        many[i] = "-DVALUE=1";
    EXPECT_FALSE(de_cook_shader_parse(34, many, &o, error));
    EXPECT_TRUE(strstr(error, "too many") != NULL);
}
TEST(shader_cli, located_diagnostics) {
    de_cook_shader_diagnostic d;
    ASSERT_TRUE(de_cook_shader_diagnostic_parse(
        "C:\\project space\\a.slang(12,9): error[E1]: bad identifier", &d));
    EXPECT_STREQ(d.source, "C:\\project space\\a.slang");
    EXPECT_EQ(d.line, 12u);
    EXPECT_EQ(d.column, 9u);
    EXPECT_STREQ(d.severity, "error");
    ASSERT_TRUE(de_cook_shader_diagnostic_parse("/tmp/a.slang:3:7: warning: conversion", &d));
    EXPECT_EQ(d.line, 3u);
    EXPECT_EQ(d.column, 7u);
    EXPECT_STREQ(d.severity, "warning");
    ASSERT_TRUE(de_cook_shader_diagnostic_parse("inc.slang(4): note: imported here", &d));
    EXPECT_EQ(d.line, 4u);
    EXPECT_EQ(d.column, 0u);
    EXPECT_STREQ(d.severity, "note");
}
TEST(shader_cli, rich_diagnostic_ranges) {
    de_cook_shader_diagnostic d;
    ASSERT_TRUE(de_cook_shader_diagnostic_parse(
        "error[E30015]: undefined identifier\n --> project space/a.slang:3:18\n  |\n3 | void "
        "main() { missing_identifier = 1; }\n  |                  ^^^^^^^^^^^^^^^^^^ undefined "
        "identifier\nerror[E39999]: import failed\n",
        &d));
    EXPECT_STREQ(d.source, "project space/a.slang");
    EXPECT_EQ(d.line, 3u);
    EXPECT_EQ(d.column, 18u);
    EXPECT_EQ(d.end_column, 36u);
    ASSERT_TRUE(de_cook_shader_diagnostic_parse(
        "error: no location\nwarning: different diagnostic\n --> other.slang:4:2\n", &d));
    EXPECT_EQ(d.line, 0u);
    ASSERT_TRUE(
        de_cook_shader_diagnostic_parse("error: overflow\n --> other.slang:4294967296:2\n", &d));
    EXPECT_EQ(d.line, 0u);
}
TEST(shader_cli, malformed_diagnostics) {
    de_cook_shader_diagnostic d;
    EXPECT_FALSE(de_cook_shader_diagnostic_parse("x.slang:4294967296:2: error: overflow", &d));
    EXPECT_FALSE(de_cook_shader_diagnostic_parse("x.slang:2:4294967296: error: overflow", &d));
    EXPECT_FALSE(de_cook_shader_diagnostic_parse("x.slang(0,1): error: invalid row", &d));
    EXPECT_FALSE(de_cook_shader_diagnostic_parse("x.slang(1,): error: missing column", &d));
    EXPECT_FALSE(de_cook_shader_diagnostic_parse("some source line with error inside it", &d));
    ASSERT_TRUE(de_cook_shader_diagnostic_parse("error: unsupported compiler", &d));
    EXPECT_EQ(d.line, 0u);
    EXPECT_STREQ(d.severity, "error");
}
