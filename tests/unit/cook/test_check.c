/*
 * test_check.c: `de-cook --check` (DENG-145, SPEC §11.2, §11.5). CTest names cook_check.<test>.
 *
 * The tests run de_cook_check_main in-process and collect what it writes. Fixtures are in
 * tests/unit/cook/data/ (DE_COOK_TEST_DATA_DIR); the shipped examples/assets.toml is read from
 * DE_COOK_EXAMPLES_DIR. The output names files by the manifest argument's directory, so the tests
 * remove the data directory prefix before comparing; the golden file then names the fixture
 * directory only ("four_issues/assets.toml:8: ...").
 */
#define _CRT_SECURE_NO_WARNINGS /* fopen */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dengine/de_platform.h>

#include "check.h"
#include "de_test.h"

#define DATA DE_COOK_TEST_DATA_DIR

/* ---- Capturing the output ----------------------------------------------------------------- */

typedef struct capture {
    char out[1 << 16];
    size_t out_len;
    char err[1 << 16];
    size_t err_len;
} capture;

static void cap_write(void *user, de_cook_stream stream, const char *text, size_t len) {
    capture *c = (capture *)user;
    char *buf = stream == DE_COOK_STDERR ? c->err : c->out;
    size_t *n = stream == DE_COOK_STDERR ? &c->err_len : &c->out_len;
    size_t room = sizeof c->out - 1 - *n;
    size_t k = len < room ? len : room;
    memcpy(buf + *n, text, k);
    *n += k;
    buf[*n] = '\0';
}

static capture g_cap;

/* Runs `de-cook --check` with a NULL-terminated argument list; returns the exit code. */
static int run_check(const char *const *args) {
    int argc = 0;
    while (args[argc] != NULL)
        ++argc;
    memset(&g_cap, 0, sizeof g_cap);
    de_cook_io io = {cap_write, &g_cap};
    return de_cook_check_main(argc, args, &io);
}

/* `s` with every occurrence of `from` removed (in place). */
static void remove_all(char *s, const char *from) {
    size_t fl = strlen(from);
    char *p;
    while ((p = strstr(s, from)) != NULL)
        memmove(p, p + fl, strlen(p + fl) + 1);
}

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    char *buf = (char *)calloc(1, 1 << 16);
    size_t n = buf != NULL ? fread(buf, 1, (1 << 16) - 1, f) : 0;
    fclose(f);
    if (buf != NULL) {
        buf[n] = '\0';
        remove_all(buf, "\r"); /* a CRLF checkout must still compare equal */
    }
    return buf;
}

static int count_lines(const char *s) {
    int n = 0;
    for (; *s != '\0'; ++s)
        n += *s == '\n';
    return n;
}

/* ---- Tests -------------------------------------------------------------------------------- */

/* examples/assets.toml with the stub includes on the search path: no issues, exit 0. */
TEST(cook_check, clean_example) {
    const char *const args[] = {DE_COOK_EXAMPLES_DIR "/assets.toml", "--no-sources", "-I",
                                DATA "/example_includes", NULL};
    EXPECT_EQ(run_check(args), DE_COOK_EXIT_OK);
    EXPECT_STREQ(g_cap.out, "de-cook --check: 0 errors, 0 warnings, 0 notes\n");
    EXPECT_STREQ(g_cap.err, "");

    /* Without the stubs, the two includes are errors that name the missing files. */
    const char *const no_stubs[] = {DE_COOK_EXAMPLES_DIR "/assets.toml", "--no-sources", NULL};
    EXPECT_EQ(run_check(no_stubs), DE_COOK_EXIT_ERRORS);
    remove_all(g_cap.out, DE_COOK_EXAMPLES_DIR "/");
    EXPECT_TRUE(strstr(g_cap.out, "assets.toml:8: error: cannot read characters.toml") != NULL);
    EXPECT_TRUE(strstr(g_cap.out, "[project.include[1]]") != NULL);
    EXPECT_TRUE(strstr(g_cap.out, "de-cook --check: 2 errors, 0 warnings, 0 notes\n") != NULL);
}

/* Unknown type, missing source, bundle cycle, unmatched glob: exactly these four, in the format
 * of expected.txt, exit 1. */
TEST(cook_check, four_issues_golden) {
    const char *const args[] = {DATA "/four_issues/assets.toml", NULL};
    EXPECT_EQ(run_check(args), DE_COOK_EXIT_ERRORS);
    EXPECT_STREQ(g_cap.err, "");
    remove_all(g_cap.out, DATA "/");
    char *golden = read_text(DATA "/four_issues/expected.txt");
    ASSERT_TRUE(golden != NULL);
    EXPECT_STREQ(g_cap.out, golden);
    EXPECT_EQ(count_lines(golden), 5); /* four issues and the summary */
    free(golden);

    /* Byte-stable: a second run prints the same bytes. */
    char first[sizeof g_cap.out];
    memcpy(first, g_cap.out, sizeof first);
    EXPECT_EQ(run_check(args), DE_COOK_EXIT_ERRORS);
    remove_all(g_cap.out, DATA "/");
    EXPECT_STREQ(g_cap.out, first);
}

/* Bad arguments exit 2 with the usage text on stderr; --help exits 0 with it on stdout. */
TEST(cook_check, usage_exit_code) {
    const char *const *const bad[] = {
        (const char *const[]){NULL},
        (const char *const[]){"--bogus", NULL},
        (const char *const[]){DATA "/four_issues/assets.toml", "--nope", NULL},
        (const char *const[]){"a.toml", "b.toml", NULL},
        (const char *const[]){"a.toml", "--types", NULL},
        (const char *const[]){"a.toml", "--types", "x,,y", NULL},
        (const char *const[]){"a.toml", "-I", NULL},
        (const char *const[]){"a.toml", "--include-dir=", NULL},
        (const char *const[]){"", NULL},
        (const char *const[]){"dir/", NULL},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        EXPECT_EQ(run_check(bad[i]), DE_COOK_EXIT_USAGE);
        EXPECT_TRUE(strstr(g_cap.err, "usage: de-cook --check <assets.toml>") != NULL);
        EXPECT_STREQ(g_cap.out, "");
    }

    const char *const help[] = {"--help", NULL};
    EXPECT_EQ(run_check(help), DE_COOK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cap.out, "usage: de-cook --check <assets.toml>") != NULL);
    EXPECT_TRUE(strstr(g_cap.out, "exit codes: 0 no errors") != NULL);
    EXPECT_STREQ(g_cap.err, "");

    /* I/O errors: a missing manifest, a directory, a missing -I directory. Exit 2, no usage. */
    const char *const missing[] = {DATA "/no_such_manifest.toml", NULL};
    EXPECT_EQ(run_check(missing), DE_COOK_EXIT_USAGE);
    EXPECT_TRUE(strstr(g_cap.err, "cannot read the manifest") != NULL);
    EXPECT_TRUE(strstr(g_cap.err, "usage:") == NULL);
    const char *const dir[] = {DATA "/four_issues", NULL};
    EXPECT_EQ(run_check(dir), DE_COOK_EXIT_USAGE);
    EXPECT_TRUE(strstr(g_cap.err, "cannot read the manifest") != NULL);
    const char *const bad_inc[] = {DATA "/four_issues/assets.toml", "-I", DATA "/no_such_dir",
                                   NULL};
    EXPECT_EQ(run_check(bad_inc), DE_COOK_EXIT_USAGE);
    EXPECT_TRUE(strstr(g_cap.err, "cannot open the include directory") != NULL);
    const char *const bad_dir[] = {DATA "/no_such_dir/assets.toml", NULL};
    EXPECT_EQ(run_check(bad_dir), DE_COOK_EXIT_USAGE);
    EXPECT_TRUE(strstr(g_cap.err, "cannot open the manifest directory") != NULL);
}

/* --types adds names to the built-in table, in every spelling. */
TEST(cook_check, extra_types) {
    const char *const plain[] = {DATA "/extra_types/assets.toml", "--no-sources", NULL};
    EXPECT_EQ(run_check(plain), DE_COOK_EXIT_ERRORS);
    EXPECT_TRUE(strstr(g_cap.out, "unknown asset type \"gizmo\"") != NULL);
    EXPECT_TRUE(strstr(g_cap.out, "\"font\"") == NULL); /* built in */

    const char *const *const ok[] = {
        (const char *const[]){DATA "/extra_types/assets.toml", "--no-sources", "--types", "gizmo",
                              NULL},
        (const char *const[]){DATA "/extra_types/assets.toml", "--no-sources", "--types=a,gizmo",
                              NULL},
        (const char *const[]){DATA "/extra_types/assets.toml", "--no-sources", "--types",
                              "a, gizmo ", NULL},
        (const char *const[]){"--types", "x", DATA "/extra_types/assets.toml", "--types", "gizmo",
                              "--no-sources", NULL},
    };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; ++i) {
        EXPECT_EQ(run_check(ok[i]), DE_COOK_EXIT_OK);
        EXPECT_STREQ(g_cap.out, "de-cook --check: 0 errors, 0 warnings, 0 notes\n");
    }

    /* Independent list: catch missing, duplicate or reordered built-ins as well as drift. */
    const char *const expected[] = {"texture", "model",      "material",     "shader",
                                    "sound",   "font",       "aseprite",     "tilemap",
                                    "strings", "stylesheet", "video",        "scene",
                                    "prefab",  "blob",       "string_table", "sprite_clip"};
    ASSERT_EQ(de_cook_known_type_count, (u32)(sizeof expected / sizeof expected[0]));
    for (u32 i = 0; i < de_cook_known_type_count; ++i) {
        EXPECT_STREQ(de_cook_known_types[i], expected[i]);
        for (u32 j = 0; j < i; ++j)
            EXPECT_TRUE(strcmp(de_cook_known_types[i], de_cook_known_types[j]) != 0);
    }
}

/* --quiet keeps the error lines only; --no-sources drops the source check. */
TEST(cook_check, quiet_and_no_sources) {
    const char *const quiet[] = {DATA "/four_issues/assets.toml", "--quiet", NULL};
    EXPECT_EQ(run_check(quiet), DE_COOK_EXIT_ERRORS);
    EXPECT_EQ(count_lines(g_cap.out), 3);
    EXPECT_TRUE(strstr(g_cap.out, "warning") == NULL);
    EXPECT_TRUE(strstr(g_cap.out, "de-cook --check:") == NULL);

    const char *const no_src[] = {DATA "/four_issues/assets.toml", "--no-sources", NULL};
    EXPECT_EQ(run_check(no_src), DE_COOK_EXIT_ERRORS); /* the cycle and the type remain */
    EXPECT_TRUE(strstr(g_cap.out, "does not exist") == NULL);
    EXPECT_TRUE(strstr(g_cap.out, "2 errors, 1 warning, 0 notes") != NULL);
}

/* Issues are sorted by file, then line: the include's warning (found while parsing) and the
 * validator's error (found last) land by position, not by discovery order. */
TEST(cook_check, sorted_by_file_then_line) {
    const char *const args[] = {DATA "/sorted/assets.toml", "--no-sources", NULL};
    EXPECT_EQ(run_check(args), DE_COOK_EXIT_ERRORS);
    remove_all(g_cap.out, DATA "/");
    const char *a = strstr(g_cap.out, "sorted/a_inc.toml:2: warning: ");
    const char *b = strstr(g_cap.out, "sorted/assets.toml:6: warning: ");
    const char *c = strstr(g_cap.out, "sorted/assets.toml:10: error: ");
    ASSERT_TRUE(a != NULL && b != NULL && c != NULL);
    EXPECT_TRUE(a < b && b < c);
    EXPECT_TRUE(strstr(g_cap.out, " [colour]\n") != NULL);
}

/* An include missing next to its manifest is read from -I, and its issues name that file. The
 * mounts are gone afterwards. */
TEST(cook_check, include_dir) {
    const char *const args[] = {DATA "/include_dir/assets.toml", "--no-sources", "-I",
                                DATA "/include_dir/stubs", NULL};
    EXPECT_EQ(run_check(args), DE_COOK_EXIT_OK);
    remove_all(g_cap.out, DATA "/");
    EXPECT_STREQ(g_cap.out, "include_dir/stubs/stub.toml:3: warning: unknown top-level key "
                            "(expected project, defaults, assets, bundles); ignored [colour]\n"
                            "de-cook --check: 0 errors, 1 warning, 0 notes\n");

    /* -I<dir> and --include-dir=<dir>; a first -I without the file is skipped. */
    const char *const forms[] = {DATA "/include_dir/assets.toml", "--no-sources",
                                 "-I" DATA "/four_issues",
                                 "--include-dir=" DATA "/include_dir/stubs", NULL};
    EXPECT_EQ(run_check(forms), DE_COOK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cap.out, "0 errors, 1 warning") != NULL);

    const char *const without[] = {DATA "/include_dir/assets.toml", "--no-sources", NULL};
    EXPECT_EQ(run_check(without), DE_COOK_EXIT_ERRORS);
    remove_all(g_cap.out, DATA "/");
    EXPECT_TRUE(strstr(g_cap.out, "include_dir/assets.toml:4: error: cannot read "
                                  "include_dir/stub.toml") != NULL);

    EXPECT_FALSE(de_path_exists("cook:/assets.toml"));
    EXPECT_FALSE(de_path_exists("cookinc:/stub.toml"));
}
