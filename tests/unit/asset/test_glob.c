/*
 * test_glob.c: bundle globs on asset IDs (DENG-139, SPEC §11.2). CTest names glob.<test>.
 */
#include "de_test.h"

#include <dengine/de_base.h>

#include "asset/resolve_internal.h"

#include <stdio.h>
#include <string.h>

#define MATCH(g, s)    EXPECT_TRUE(de__glob_match((g), (s)))
#define NO_MATCH(g, s) EXPECT_FALSE(de__glob_match((g), (s)))

TEST(glob, single_star) {
    MATCH("ui/*", "ui/font");
    MATCH("ui/*", "ui/font_pixel");
    NO_MATCH("ui/*", "ui/a/b");
    NO_MATCH("ui/*", "uix/font");
    NO_MATCH("ui/*", "ui");
    MATCH("ui/*", "ui/"); /* `*` may be empty (not a valid ID, but the matcher allows it) */
    MATCH("*", "font");
    NO_MATCH("*", "ui/font");
    MATCH("*/*", "ui/font");
    MATCH("env/stone_*", "env/stone_albedo");
    MATCH("env/*_normal", "env/stone_normal");
    MATCH("env/*o*e*", "env/stone");
    NO_MATCH("env/*_normal", "env/stone_albedo");
    MATCH("a*b*c", "aXbYc");
    MATCH("*a*a*b", "aaaab");
    NO_MATCH("*a*a*b", "aaaa/b");
    MATCH("*/b", "aaaa/b");
}

TEST(glob, double_star) {
    MATCH("env/**", "env/a/b");
    MATCH("env/**", "env/a");
    MATCH("env/**", "env/castle");
    NO_MATCH("env/**", "env");
    NO_MATCH("env/**", "level01/env/a");
    MATCH("**", "a/b/c");
    MATCH("**/c", "a/b/c");
    MATCH("**c", "a/b/c");
    MATCH("a/**/c", "a/b/x/c");
    /* `**` is "any run, '/' included", so the pattern needs "a/", a run, then "/c". */
    NO_MATCH("a/**/c", "a/c");
    MATCH("a/**/*.png", "a/b/c/d.png");
    NO_MATCH("a/**/*.png", "a/b/c/d.png/e");
    MATCH("***", "x/y"); /* three or more stars act as `**` */
    MATCH("a**b", "a/x/b");
    MATCH("**/*_normal", "env/rocks/stone_normal");
    NO_MATCH("**/*_normal", "env/rocks_normal/stone");
}

TEST(glob, question_mark_and_literals) {
    MATCH("ui/fon?", "ui/font");
    NO_MATCH("ui/fon?", "ui/fonts");
    NO_MATCH("ui?font", "ui/font"); /* `?` never matches '/' */
    MATCH("?"
          "?/?",
          "ab/c");                      /* split: "??/" would be a trigraph */
    MATCH("caf?", "caf\xc3\xa9");       /* one UTF-8 character, two bytes */
    MATCH("?/x", "\xf0\x9f\x8e\xae/x"); /* four bytes */
    NO_MATCH("caf??", "caf\xc3\xa9");
    MATCH("sfx/click", "sfx/click");
    NO_MATCH("sfx/click", "sfx/Click"); /* case-sensitive */
    NO_MATCH("sfx/click", "sfx/click2");
    MATCH("", "");
    NO_MATCH("", "a");
    NO_MATCH(NULL, "a");
    NO_MATCH("a", NULL);
}

TEST(glob, no_subassets) {
    NO_MATCH("ui/*", "ui/model#Node");
    NO_MATCH("**", "ui/model#Node");
    NO_MATCH("ui/model#*", "ui/model#Node");
    NO_MATCH("ui/model#Node", "ui/model#Node");
    MATCH("ui/*", "ui/model");
}

TEST(glob, is_pattern) {
    EXPECT_TRUE(de__glob_is_pattern("ui/*"));
    EXPECT_TRUE(de__glob_is_pattern("ui/fon?"));
    EXPECT_FALSE(de__glob_is_pattern("ui/font"));
    EXPECT_FALSE(de__glob_is_pattern(""));
    EXPECT_FALSE(de__glob_is_pattern(NULL));
}

/* Reference matcher: plain recursion over every split (exponential, ASCII only). */
static bool ref_match(const char *g, const char *s) {
    if (*g == '\0')
        return *s == '\0';
    if (*g == '*') {
        size_t n = 0;
        while (g[n] == '*')
            ++n;
        for (const char *t = s;; ++t) {
            if (ref_match(g + n, t))
                return true;
            if (*t == '\0' || (n == 1 && *t == '/'))
                return false;
        }
    }
    if (*s == '\0')
        return false;
    if (*g == '?')
        return *s != '/' && ref_match(g + 1, s + 1);
    return *g == *s && ref_match(g + 1, s + 1);
}

static u64 g_rng = 0x9E3779B97F4A7C15ull;
static u32 rnd(u32 n) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (u32)(g_rng % n);
}

TEST(glob, matches_reference) {
    static const char k_pat[] = "ab/*?";
    static const char k_txt[] = "ab/";
    char g[12], s[12];
    u32 matches = 0;
    for (u32 iter = 0; iter < 300000; ++iter) {
        u32 gl = rnd(10), sl = rnd(10);
        for (u32 i = 0; i < gl; ++i)
            g[i] = k_pat[rnd(5)];
        g[gl] = '\0';
        for (u32 i = 0; i < sl; ++i)
            s[i] = k_txt[rnd(3)];
        s[sl] = '\0';
        bool want = ref_match(g, s), got = de__glob_match(g, s);
        if (want != got) {
            fprintf(stderr, "glob \"%s\" on \"%s\": expected %d, got %d\n", g, s, (int)want,
                    (int)got);
            FAIL("glob disagrees with the reference matcher");
        }
        matches += want ? 1u : 0u;
    }
    EXPECT_GT(matches, 1000u); /* the sample is not trivially all misses */
}

TEST(glob, no_exponential_backtracking) {
    /* A naive recursive matcher takes forever on these; ours is polynomial. The CTest timeout
     * catches a regression. */
    static char text[4097];
    memset(text, 'a', 4096);
    text[4096] = '\0';
    NO_MATCH("*a*a*a*a*a*a*a*a*a*a*a*a*b", text);
    NO_MATCH("**a**a**a**a**a**a**a**a**b", text);
    NO_MATCH("**a*a**a*a**a*a**b", text);
    for (u32 i = 64; i < 4096; i += 64)
        text[i] = '/';
    NO_MATCH("**/*a*a*a*a*a*a*a*b", text);
    MATCH("**/*a*a*a*a*a*a*a*a", text);
}
