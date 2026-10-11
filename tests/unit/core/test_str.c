/* Unit tests: string views (de_base.h) and the string builder (core/de_strbuf.h). DENG-153. */
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "core/de_strbuf.h"

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------- A counting malloc allocator with an optional failure budget ---------- */

typedef struct test_heap {
    size_t live_bytes;
    size_t allocs;
    int fail_after; /* allocations (and reallocations) left before failing; -1 = never fail */
} test_heap;

static void *th_alloc(void *user, size_t size, size_t align, u32 tag) {
    test_heap *h = (test_heap *)user;
    (void)align; /* malloc's alignment is enough for chars */
    (void)tag;
    if (h->fail_after == 0)
        return NULL;
    if (h->fail_after > 0)
        --h->fail_after;
    void *p = malloc(size);
    if (p != NULL) {
        h->live_bytes += size;
        ++h->allocs;
    }
    return p;
}

static void *th_realloc(void *user, void *ptr, size_t old_size, size_t new_size, size_t align,
                        u32 tag) {
    test_heap *h = (test_heap *)user;
    (void)align;
    (void)tag;
    if (h->fail_after == 0)
        return NULL;
    if (h->fail_after > 0)
        --h->fail_after;
    void *p = realloc(ptr, new_size);
    if (p != NULL) {
        h->live_bytes += new_size - old_size;
        ++h->allocs;
    }
    return p;
}

static void th_free(void *user, void *ptr, size_t size) {
    test_heap *h = (test_heap *)user;
    h->live_bytes -= size;
    free(ptr);
}

static de_allocator test_allocator(test_heap *h) {
    h->live_bytes = 0;
    h->allocs = 0;
    h->fail_after = -1;
    de_allocator a = {th_alloc, th_realloc, th_free, h};
    return a;
}

static de_str S(const char *s) {
    return de_str_from_cstr(s);
}

/* ---------- Views ---------- */

TEST(str, views) {
    const de_str empty = {NULL, 0};
    EXPECT_TRUE(de_str_from_cstr(NULL).ptr == NULL);
    EXPECT_EQ(de_str_from_cstr(NULL).len, 0u);
    EXPECT_EQ(de_str_from_cstr("abc").len, 3u);

    EXPECT_TRUE(de_str_eq(S("abc"), DE_STR("abc")));
    EXPECT_FALSE(de_str_eq(S("abc"), S("abd")));
    EXPECT_FALSE(de_str_eq(S("abc"), S("ab")));
    EXPECT_TRUE(de_str_eq(empty, S("")));

    EXPECT_TRUE(de_str_starts_with(S("player/mesh"), S("player/")));
    EXPECT_TRUE(de_str_starts_with(S("x"), empty));
    EXPECT_TRUE(de_str_starts_with(empty, empty));
    EXPECT_FALSE(de_str_starts_with(S("pl"), S("player")));
    EXPECT_FALSE(de_str_starts_with(S("player"), S("plx")));

    EXPECT_EQ(de_str_find(S("hello world"), S("world")), 6u);
    EXPECT_EQ(de_str_find(S("hello world"), S("o")), 4u);
    EXPECT_EQ(de_str_find(S("hello world"), S("")), 0u);
    EXPECT_EQ(de_str_find(S("hello"), S("hello!")), DE_STR_NPOS);
    EXPECT_EQ(de_str_find(S("aaab"), S("aab")), 1u);
    EXPECT_EQ(de_str_find(S("abcabd"), S("abd")), 3u);
    EXPECT_EQ(de_str_find(S("abc"), S("c")), 2u);
    EXPECT_EQ(de_str_find(S("abc"), S("d")), DE_STR_NPOS);
    EXPECT_EQ(de_str_find(empty, S("a")), DE_STR_NPOS);
    /* Never reads past len: the match after the view's end must not count. */
    const de_str cut = {"abcXYZ", 4};
    EXPECT_EQ(de_str_find(cut, S("XY")), DE_STR_NPOS);
    EXPECT_EQ(de_str_find(cut, S("cX")), 2u);
}

static int split_all(const char *text, char sep, de_str *toks, int max) {
    de_str rest = de_str_from_cstr(text), tok;
    int n = 0;
    while (de_str_split_next(&rest, sep, &tok)) {
        if (n < max)
            toks[n] = tok;
        ++n;
    }
    return n;
}

TEST(str, split) {
    de_str t[8];
    ASSERT_EQ(split_all("a,,b", ',', t, 8), 3);
    EXPECT_TRUE(de_str_eq(t[0], S("a")));
    EXPECT_TRUE(de_str_eq(t[1], S("")));
    EXPECT_TRUE(de_str_eq(t[2], S("b")));

    ASSERT_EQ(split_all("key=value", '=', t, 8), 2);
    EXPECT_TRUE(de_str_eq(t[0], S("key")));
    EXPECT_TRUE(de_str_eq(t[1], S("value")));

    ASSERT_EQ(split_all("", ',', t, 8), 1);
    EXPECT_EQ(t[0].len, 0u);
    ASSERT_EQ(split_all("a,", ',', t, 8), 2);
    EXPECT_TRUE(de_str_eq(t[1], S("")));
    ASSERT_EQ(split_all(",", ',', t, 8), 2);
    ASSERT_EQ(split_all("no separator", ',', t, 8), 1);
    EXPECT_TRUE(de_str_eq(t[0], S("no separator")));
    ASSERT_EQ(split_all("a/b/c/d", '/', t, 8), 4);
    EXPECT_TRUE(de_str_eq(t[3], S("d")));

    /* A NULL view yields nothing, and so does a finished iterator. */
    de_str rest = {NULL, 0}, tok = S("untouched");
    EXPECT_FALSE(de_str_split_next(&rest, ',', &tok));
    EXPECT_TRUE(de_str_eq(tok, S("untouched")));

    /* Splitting stops at the view's end, not at a NUL. */
    de_str cut = {"x,y,z", 3};
    ASSERT_TRUE(de_str_split_next(&cut, ',', &tok));
    ASSERT_TRUE(de_str_split_next(&cut, ',', &tok));
    EXPECT_TRUE(de_str_eq(tok, S("y")));
    EXPECT_FALSE(de_str_split_next(&cut, ',', &tok));
}

TEST(str, trim) {
    EXPECT_TRUE(de_str_eq(de_str_trim(S("  a b \t\r\n")), S("a b")));
    EXPECT_TRUE(de_str_eq(de_str_trim(S("abc")), S("abc")));
    EXPECT_TRUE(de_str_eq(de_str_trim(S("\v\f x")), S("x")));
    EXPECT_EQ(de_str_trim(S("   ")).len, 0u);
    EXPECT_EQ(de_str_trim(S("")).len, 0u);
    const de_str empty = {NULL, 0};
    EXPECT_EQ(de_str_trim(empty).len, 0u);
    /* Not whitespace: NUL and non-ASCII bytes. */
    const de_str nul = {"\0a\0", 3};
    EXPECT_EQ(de_str_trim(nul).len, 3u);
    EXPECT_TRUE(de_str_eq(de_str_trim(S(" \xC3\xA9 ")), S("\xC3\xA9")));
    /* The result points into the input. */
    const char *text = "  mid  ";
    EXPECT_TRUE(de_str_trim(S(text)).ptr == text + 2);
}

static i64 parse_i64_ok(const char *s) {
    i64 v = 12345;
    de_error_clear();
    ASSERT_EQ(de_str_to_i64(S(s), &v), DE_OK);
    return v;
}

static void parse_i64_bad(const char *s) {
    i64 v = 12345;
    de_error_clear();
    EXPECT_EQ(de_str_to_i64(S(s), &v), DE_ERR_PARSE);
    EXPECT_EQ(v, 12345); /* untouched on failure */
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_PARSE);
}

static f64 parse_f64_ok(const char *s) {
    f64 v = 0;
    de_error_clear();
    ASSERT_EQ(de_str_to_f64(S(s), &v), DE_OK);
    return v;
}

static void parse_f64_bad(const char *s) {
    f64 v = 42.0;
    de_error_clear();
    EXPECT_EQ(de_str_to_f64(S(s), &v), DE_ERR_PARSE);
    EXPECT_FLOAT_EQ(v, 42.0, 0.0);
    EXPECT_TRUE(de_error_last() != NULL);
}

TEST(str, parse_numbers) {
    EXPECT_EQ(parse_i64_ok("0"), 0);
    EXPECT_EQ(parse_i64_ok("42"), 42);
    EXPECT_EQ(parse_i64_ok("-42"), -42);
    EXPECT_EQ(parse_i64_ok("+7"), 7);
    EXPECT_EQ(parse_i64_ok("007"), 7);
    EXPECT_EQ(parse_i64_ok("0x1F"), 31);
    EXPECT_EQ(parse_i64_ok("0XfF"), 255);
    EXPECT_EQ(parse_i64_ok("-0x10"), -16);
    EXPECT_EQ(parse_i64_ok("0b1011"), 11);
    EXPECT_EQ(parse_i64_ok("9223372036854775807"), INT64_MAX);
    EXPECT_EQ(parse_i64_ok("-9223372036854775808"), INT64_MIN);
    EXPECT_EQ(parse_i64_ok("0x7fffffffffffffff"), INT64_MAX);
    EXPECT_EQ(parse_i64_ok("-0x8000000000000000"), INT64_MIN);
    parse_i64_bad("");
    parse_i64_bad("-");
    parse_i64_bad("+");
    parse_i64_bad("0x");
    parse_i64_bad("0b");
    parse_i64_bad("0b102");
    parse_i64_bad("12a");
    parse_i64_bad(" 1");
    parse_i64_bad("1 ");
    parse_i64_bad("1.0");
    parse_i64_bad("--1");
    parse_i64_bad("0xg");
    parse_i64_bad("9223372036854775808");
    parse_i64_bad("-9223372036854775809");
    parse_i64_bad("0x8000000000000000");
    parse_i64_bad("99999999999999999999999");
    /* A view that stops before trailing garbage parses. */
    i64 v = 0;
    const de_str cut = {"123abc", 3};
    EXPECT_EQ(de_str_to_i64(cut, &v), DE_OK);
    EXPECT_EQ(v, 123);

    EXPECT_FLOAT_EQ(parse_f64_ok("0"), 0.0, 0.0);
    EXPECT_FLOAT_EQ(parse_f64_ok("1.5"), 1.5, 0.0);
    EXPECT_FLOAT_EQ(parse_f64_ok("-2.25"), -2.25, 0.0);
    EXPECT_FLOAT_EQ(parse_f64_ok("+.5"), 0.5, 0.0);
    EXPECT_FLOAT_EQ(parse_f64_ok("5."), 5.0, 0.0);
    EXPECT_FLOAT_EQ(parse_f64_ok("1e3"), 1000.0, 0.0);
    EXPECT_FLOAT_EQ(parse_f64_ok("2.5E-2"), 0.025, 1e-18);
    EXPECT_FLOAT_EQ(parse_f64_ok("0.1"), 0.1, 0.0); /* correctly rounded, like strtod */
    EXPECT_FLOAT_EQ(parse_f64_ok("1e-400"), 0.0, 0.0); /* underflow rounds to zero */
    EXPECT_TRUE(isinf(parse_f64_ok("inf")) && parse_f64_ok("inf") > 0);
    EXPECT_TRUE(isinf(parse_f64_ok("-Infinity")) && parse_f64_ok("-INF") < 0);
    EXPECT_TRUE(isnan(parse_f64_ok("NaN")));
    EXPECT_TRUE(isnan(parse_f64_ok("-nan")));
    parse_f64_bad("");
    parse_f64_bad(".");
    parse_f64_bad("-");
    parse_f64_bad("e5");
    parse_f64_bad("1e");
    parse_f64_bad("1e+");
    parse_f64_bad("1.2.3");
    parse_f64_bad(" 1.0");
    parse_f64_bad("1.0 ");
    parse_f64_bad("0x1p3");
    parse_f64_bad("1,5");
    parse_f64_bad("nan(1)");
    parse_f64_bad("infin");
    parse_f64_bad("1e400"); /* overflow */
    /* Length limit: 511 bytes parse, 512 do not. 300 integer digits keep the value finite. */
    char longest[600];
    memset(longest, '1', sizeof longest - 1);
    longest[300] = '.';
    longest[512] = '\0';
    parse_f64_bad(longest);
    longest[511] = '\0';
    EXPECT_TRUE(parse_f64_ok(longest) > 1e299);
    f64 d = 0;
    const de_str cut_f = {"2.5xyz", 3};
    EXPECT_EQ(de_str_to_f64(cut_f, &d), DE_OK);
    EXPECT_FLOAT_EQ(d, 2.5, 0.0);
}

/* '.' is the decimal point even when the C locale uses ','. Skips if no such locale exists. */
TEST(str, parse_f64_ignores_locale) {
    static const char *locales[] = {"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8", "de_DE.utf8",
                                    "German_Germany.1252"};
    const char *found = NULL;
    for (size_t i = 0; i < sizeof locales / sizeof locales[0] && found == NULL; ++i)
        found = setlocale(LC_NUMERIC, locales[i]);
    if (found == NULL || localeconv()->decimal_point[0] != ',') {
        setlocale(LC_NUMERIC, "C");
        SKIP("no installed locale with a ',' decimal point");
    }
    f64 v = 0;
    const de_result r = de_str_to_f64(S("1.25"), &v);
    const de_result r2 = de_str_to_f64(S("1,25"), &v);
    setlocale(LC_NUMERIC, "C");
    EXPECT_EQ(r, DE_OK);
    EXPECT_EQ(r2, DE_ERR_PARSE);
    EXPECT_FLOAT_EQ(v, 1.25, 0.0);
}

/* ---------- String builder ---------- */

TEST(strbuf, appendf_growth) {
    test_heap h;
    const de_allocator a = test_allocator(&h);
    de_strbuf b;
    de_strbuf_init(&b, &a);
    EXPECT_STREQ(de_strbuf_cstr(&b), "");
    EXPECT_EQ(de_strbuf_view(&b).len, 0u);
    EXPECT_EQ(h.allocs, 0u);

    /* Many small appends, mixed kinds; check the result against a reference built by hand. */
    char ref[20000];
    size_t ref_len = 0;
    for (int i = 0; i < 1000; ++i) {
        ASSERT_EQ(de_strbuf_appendf(&b, "%d:%s;", i, i % 2 ? "odd" : "even"), DE_OK);
        ref_len += (size_t)snprintf(ref + ref_len, sizeof ref - ref_len, "%d:%s;", i,
                                    i % 2 ? "odd" : "even");
    }
    ASSERT_EQ(de_strbuf_append(&b, DE_STR("|tail")), DE_OK);
    ASSERT_EQ(de_strbuf_append_char(&b, '!'), DE_OK);
    ASSERT_EQ(de_strbuf_append_cstr(&b, NULL), DE_OK);
    ref_len += (size_t)snprintf(ref + ref_len, sizeof ref - ref_len, "|tail!");
    EXPECT_EQ(b.len, ref_len);
    EXPECT_STREQ(de_strbuf_cstr(&b), ref);
    EXPECT_TRUE(b.cap > b.len);
    /* Doubling: about log2(len / 64) allocations, not one per append. */
    EXPECT_LT(h.allocs, 20u);

    /* One append bigger than the whole buffer takes the second appendf pass. */
    const size_t before = b.len;
    ASSERT_EQ(de_strbuf_appendf(&b, "%5000s", "x"), DE_OK);
    EXPECT_EQ(b.len, before + 5000);
    EXPECT_EQ(b.ptr[b.len - 1], 'x');
    EXPECT_EQ(b.ptr[b.len], '\0');

    /* Appending a view of the builder itself (which the growth may move). */
    de_strbuf_clear(&b);
    EXPECT_STREQ(de_strbuf_cstr(&b), "");
    ASSERT_EQ(de_strbuf_append_cstr(&b, "abc"), DE_OK);
    for (int i = 0; i < 8; ++i)
        ASSERT_EQ(de_strbuf_append(&b, de_strbuf_view(&b)), DE_OK);
    EXPECT_EQ(b.len, 3u * 256u);
    EXPECT_TRUE(memcmp(b.ptr + 3 * 255, "abc", 3) == 0);

    de_strbuf_free(&b);
    EXPECT_EQ(h.live_bytes, 0u);
    EXPECT_TRUE(b.ptr == NULL);
    /* Reusable after free. */
    ASSERT_EQ(de_strbuf_appendf(&b, "%s", "again"), DE_OK);
    EXPECT_STREQ(de_strbuf_cstr(&b), "again");
    de_strbuf_free(&b);
    EXPECT_EQ(h.live_bytes, 0u);
}

TEST(strbuf, out_of_memory_keeps_contents) {
    test_heap h;
    const de_allocator a = test_allocator(&h);
    de_strbuf b;
    de_strbuf_init(&b, &a);
    ASSERT_EQ(de_strbuf_append_cstr(&b, "keep"), DE_OK);
    h.fail_after = 0;
    char big[200];
    memset(big, 'z', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    de_error_clear();
    EXPECT_EQ(de_strbuf_append_cstr(&b, big), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_strbuf_appendf(&b, "%s", big), DE_ERR_OUT_OF_MEMORY);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_STREQ(de_strbuf_cstr(&b), "keep");
    EXPECT_EQ(b.len, 4u);
    /* Small appends that fit need no allocation and still work. */
    EXPECT_EQ(de_strbuf_appendf(&b, "-%d", 1), DE_OK);
    EXPECT_STREQ(de_strbuf_cstr(&b), "keep-1");
    h.fail_after = -1;
    de_strbuf_free(&b);
    EXPECT_EQ(h.live_bytes, 0u);
}
