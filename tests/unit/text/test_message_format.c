#include "de_test.h"
#include "text/loc/loc_internal.h"
#include <string.h>
static void message(const char *pattern, const char *locale, const de_loc_arg *args, u32 count,
                    const char *expected) {
    char out[1024];
    u32 needed = 0;
    ASSERT_EQ(
        de__loc_message(pattern, strlen(pattern), locale, args, count, out, sizeof out, &needed),
        DE_OK);
    EXPECT_STREQ(out, expected);
    EXPECT_EQ(needed, strlen(expected));
}
TEST(loc_message, placeholders_numbers_plurals_and_exact_branch) {
    de_loc_arg args[] = {{"name", DE_LOC_ARG_STR, "Ada", 0, 0},
                         {"count", DE_LOC_ARG_INT, NULL, 1, 0}};
    message("Hello {name}: {count, plural, =0{none} one{# item} other{# items}}", "en", args, 2,
            "Hello Ada: 1 item");
    args[1].i = 2;
    message("{count, plural, one{# item} other{# items}}", "en", args, 2, "2 items");
    args[1].i = 0;
    message("{count, plural, =0{none} one{# item} other{# items}}", "en", args, 2, "none");
    const char *ru = "{count, plural, one{one} few{few} many{many} other{other}}";
    args[1].i = 21;
    message(ru, "ru", args, 2, "one");
    args[1].i = 22;
    message(ru, "ru", args, 2, "few");
    args[1].i = 25;
    message(ru, "ru", args, 2, "many");
    args[1].i = 1234;
    message("{count, number, integer}", "de", args, 2, "1.234");
}
TEST(loc_message, nested_select_plural_and_apostrophe_quoting) {
    de_loc_arg args[] = {{"gender", DE_LOC_ARG_STR, "she", 0, 0},
                         {"n", DE_LOC_ARG_INT, NULL, 2, 0}};
    message("{gender, select, she{{n, plural, one{She has # coin} other{She has # coins}}} "
            "other{Nobody}}",
            "en", args, 2, "She has 2 coins");
    message("'{name}' isn't ''quoted''", "en", NULL, 0, "{name} isn't 'quoted'");
    message("{n, plural, other{'#' and #}}", "en", args, 2, "# and 2");
}
TEST(loc_message, truncation_reports_full_bytes_and_keeps_utf8_valid) {
    const char *pattern = "é漢字";
    char out[6];
    u32 needed = 0;
    ASSERT_EQ(de__loc_message(pattern, strlen(pattern), "en", NULL, 0, out, sizeof out, &needed),
              DE_OK);
    EXPECT_EQ(needed, 8u);
    EXPECT_STREQ(out, "é漢");
    char tiny[4];
    ASSERT_EQ(de__loc_message(pattern, strlen(pattern), "en", NULL, 0, tiny, sizeof tiny, &needed),
              DE_OK);
    EXPECT_STREQ(tiny, "é");
    ASSERT_EQ(de__loc_message(pattern, strlen(pattern), "en", NULL, 0, NULL, 0, &needed), DE_OK);
    EXPECT_EQ(needed, 8u);
}
TEST(loc_message, invalid_patterns_and_depth_are_rejected) {
    const char *bad[] = {"{",
                         "}",
                         "{n, unknown}",
                         "{n, plural, one{x}}",
                         "{n, select, a{x}}",
                         "{n, number, currency}",
                         "{n, plural, banana{x} other{y}}"};
    for (u32 i = 0; i < sizeof bad / sizeof bad[0]; ++i)
        EXPECT_EQ(de__loc_validate(bad[i], strlen(bad[i]), NULL), DE_ERR_PARSE);
    char deep[4096];
    size_t at = 0;
    for (u32 i = 0; i < 40; ++i) {
        memcpy(deep + at, "{n, select, other{", 18);
        at += 18;
    }
    deep[at++] = 'x';
    for (u32 i = 0; i < 40; ++i) {
        deep[at++] = '}';
        deep[at++] = '}';
    }
    EXPECT_EQ(de__loc_validate(deep, at, NULL), DE_ERR_PARSE);
}

/* DENG-321 prerequisite: callers can format authored patterns without inventing a table ID. */
#include "text/text_internal.h"
#include "de_alloc_guard.h"
TEST(loc_message, public_inline_pattern_uses_published_locale_and_no_heap) {
    de_text_system text = {0};
    ASSERT_EQ(de__loc_create(NULL, NULL, &text.loc), DE_OK);
    de_loc_arg value = {"value", DE_LOC_ARG_INT, NULL, 42, 0};
    char out[128];
    EXPECT_EQ(de_loc_format_pattern(&text, de_str_from_cstr("{value, number, integer} / 100"),
                                    &value, 1, out, sizeof out),
              8u);
    EXPECT_STREQ(out, "42 / 100");
    ASSERT_EQ(de_loc_set_locale(&text, "ru"), DE_OK);
    de__loc_update(text.loc);
    value.i = 22;
    de_str pattern = de_str_from_cstr("{value, plural, one{one} few{few} many{many} other{other}}");
    EXPECT_EQ(de_loc_format_pattern(&text, pattern, &value, 1, out, sizeof out), 3u);
    EXPECT_STREQ(out, "few");
    de_alloc_guard guard = {0};
    guard.this_thread_only = true;
    de_alloc_guard_begin(&guard);
    for (u32 i = 0; i < 1000; ++i)
        (void)de_loc_format_pattern(&text, pattern, &value, 1, out, sizeof out);
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    ASSERT_EQ(de_loc_set_locale(&text, "ja"), DE_OK);
    de__loc_update(text.loc);
    de_loc_format_pattern(&text, pattern, &value, 1, out, sizeof out);
    EXPECT_STREQ(out, "other");
    de__loc_destroy(text.loc);
}
TEST(loc_message, public_inline_pattern_preserves_spans_truncation_and_errors) {
    const char span[] = {'{', 'n', '}', 'x'};
    de_loc_arg value = {"n", DE_LOC_ARG_INT, NULL, 42, 0};
    char out[16];
    EXPECT_EQ(de_loc_format_pattern(NULL, (de_str){span, 3}, &value, 1, out, sizeof out), 2u);
    EXPECT_STREQ(out, "42");
    de_str utf8 = de_str_from_cstr("é漢字");
    EXPECT_EQ(de_loc_format_pattern(NULL, utf8, NULL, 0, out, 6), 8u);
    EXPECT_STREQ(out, "é漢");
    EXPECT_EQ(de_loc_format_pattern(NULL, utf8, NULL, 0, NULL, 0), 8u);
    de_error_clear();
    EXPECT_EQ(de_loc_format_pattern(NULL, de_str_from_cstr("{n, plural, one{x}}"), &value, 1, out,
                                    sizeof out),
              0u);
    EXPECT_STREQ(out, "");
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_PARSE);
    de_error_clear();
    EXPECT_EQ(de_loc_format_pattern(NULL, (de_str){NULL, 3}, NULL, 0, out, sizeof out), 0u);
    EXPECT_STREQ(out, "");
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
}
