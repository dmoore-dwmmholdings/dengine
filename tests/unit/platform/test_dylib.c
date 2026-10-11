/* Tests for dynamic library loading (DENG-249). The fixture library is built from
 * dylib_fixture/fixture.c; CMake passes its path (DE_DYLIB_FIXTURE_PATH) and directory
 * (DE_DYLIB_FIXTURE_DIR). */
#define _CRT_SECURE_NO_WARNINGS /* fopen */

#include "de_test.h"

#include <dengine/de_platform.h>

#include <stdio.h>
#include <string.h>

typedef int (*fixture_add_fn)(int, int);
typedef int (*fixture_counter_fn)(void);

static fixture_add_fn get_add(de_dylib *lib) {
    return (fixture_add_fn)de_dylib_function(lib, "fixture_add");
}

static fixture_counter_fn get_counter(de_dylib *lib) {
    return (fixture_counter_fn)de_dylib_function(lib, "fixture_counter");
}

/* The platform file name of `stem` in the fixture directory. */
static void fixture_dir_name(const char *stem, char *buf, size_t cap) {
    char base[1024];
    int n = snprintf(base, sizeof base, "%s/%s", DE_DYLIB_FIXTURE_DIR, stem);
    ASSERT_TRUE(n > 0 && (size_t)n < sizeof base);
    ASSERT_EQ(de_dylib_platform_name(base, buf, cap), DE_OK);
}

static bool copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (in == NULL)
        return false;
    FILE *out = fopen(to, "wb");
    if (out == NULL) {
        fclose(in);
        return false;
    }
    char buf[16384];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    ok = ok && !ferror(in);
    fclose(in);
    ok = fclose(out) == 0 && ok;
    return ok;
}

static bool error_mentions(const char *needle) {
    const de_error *e = de_error_last();
    return e != NULL && strstr(e->message, needle) != NULL;
}

TEST(dylib, open_symbol_close) {
    de_dylib *lib = NULL;
    ASSERT_EQ(de_dylib_open(DE_DYLIB_FIXTURE_PATH, &lib), DE_OK);
    ASSERT_TRUE(lib != NULL);

    fixture_add_fn add = get_add(lib);
    ASSERT_TRUE(add != NULL);
    EXPECT_EQ(add(2, 3), 5);
    EXPECT_EQ(add(-7, 7), 0);

    const int *value = (const int *)de_dylib_symbol(lib, "fixture_value");
    ASSERT_TRUE(value != NULL);
    EXPECT_EQ(*value, 42);

    fixture_counter_fn counter = get_counter(lib);
    ASSERT_TRUE(counter != NULL);
    EXPECT_EQ(counter(), 1);
    EXPECT_EQ(counter(), 2);

    /* A second open of the same path is the same image (OS reference count). */
    de_dylib *again = NULL;
    ASSERT_EQ(de_dylib_open(DE_DYLIB_FIXTURE_PATH, &again), DE_OK);
    EXPECT_EQ(get_counter(again)(), 3);
    de_dylib_close(again);
    EXPECT_EQ(counter(), 4); /* still loaded: one open remains */

    /* Closing the last reference unloads it: a fresh load starts with fresh globals. */
    de_dylib_close(lib);
    lib = NULL;
    ASSERT_EQ(de_dylib_open(DE_DYLIB_FIXTURE_PATH, &lib), DE_OK);
    EXPECT_EQ(get_counter(lib)(), 1);
    de_dylib_close(lib);

    de_dylib_close(NULL); /* ignored */
}

TEST(dylib, missing) {
    de_dylib *lib = (de_dylib *)&lib; /* must be reset to NULL on failure */

    /* No such file. */
    char missing[1024];
    fixture_dir_name("does_not_exist", missing, sizeof missing);
    de_error_clear();
    EXPECT_EQ(de_dylib_open(missing, &lib), DE_ERR_NOT_FOUND);
    EXPECT_TRUE(lib == NULL);
    EXPECT_TRUE(error_mentions("does_not_exist"));
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_GT(strlen(de_error_last()->message), strlen(missing)); /* plus the OS text */

    /* A file that is not a library: the OS refuses it, with its own message. */
    char garbage[1024];
    fixture_dir_name("not_a_library", garbage, sizeof garbage);
    FILE *f = fopen(garbage, "wb");
    ASSERT_TRUE(f != NULL);
    fputs("this is not a shared library\n", f);
    fclose(f);
    de_error_clear();
    EXPECT_EQ(de_dylib_open(garbage, &lib), DE_ERR_IO);
    EXPECT_TRUE(lib == NULL);
    EXPECT_TRUE(error_mentions("not_a_library"));
    remove(garbage);

    /* Bad arguments. */
    EXPECT_EQ(de_dylib_open(NULL, &lib), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_dylib_open("", &lib), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_dylib_open(DE_DYLIB_FIXTURE_PATH, NULL), DE_ERR_INVALID_ARGUMENT);

    /* Missing symbols in a library that loaded. */
    ASSERT_EQ(de_dylib_open(DE_DYLIB_FIXTURE_PATH, &lib), DE_OK);
    de_error_clear();
    EXPECT_TRUE(de_dylib_symbol(lib, "fixture_no_such_symbol") == NULL);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_NOT_FOUND);
    EXPECT_TRUE(error_mentions("fixture_no_such_symbol"));
    de_error_clear();
    EXPECT_TRUE(de_dylib_function(lib, "fixture_no_such_function") == NULL);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_NOT_FOUND);
    EXPECT_TRUE(error_mentions("fixture_no_such_function"));
    /* The hidden-visibility counter variable is not exported. */
    EXPECT_TRUE(de_dylib_symbol(lib, "fixture_count") == NULL);
    EXPECT_TRUE(de_dylib_symbol(lib, NULL) == NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(de_dylib_symbol(NULL, "fixture_add") == NULL);
    EXPECT_TRUE(de_dylib_function(NULL, "fixture_add") == NULL);
    de_dylib_close(lib);
}

TEST(dylib, load_copy) {
    /* Hot reload (SPEC §10.1) copies the rebuilt library to a new name and loads it while the
     * old one is still loaded. The copy must be a separate image with its own globals. */
    char copy[1024];
    fixture_dir_name("dylib_fixture_copy1", copy, sizeof copy);
    remove(copy);

    de_dylib *orig = NULL, *dup = NULL;
    ASSERT_EQ(de_dylib_open(DE_DYLIB_FIXTURE_PATH, &orig), DE_OK);
    fixture_counter_fn c_orig = get_counter(orig);
    ASSERT_TRUE(c_orig != NULL);
    EXPECT_EQ(c_orig(), 1);
    EXPECT_EQ(c_orig(), 2);

    ASSERT_TRUE(copy_file(DE_DYLIB_FIXTURE_PATH, copy));
    ASSERT_EQ(de_dylib_open(copy, &dup), DE_OK);
    EXPECT_TRUE(dup != orig);
    fixture_counter_fn c_dup = get_counter(dup);
    ASSERT_TRUE(c_dup != NULL);
    EXPECT_TRUE(c_dup != c_orig);
    EXPECT_EQ(c_dup(), 1); /* fresh globals in the copy */
    EXPECT_EQ(c_orig(), 3);
    EXPECT_EQ(get_add(dup)(20, 22), 42);
    EXPECT_TRUE(de_dylib_symbol(dup, "fixture_value") != de_dylib_symbol(orig, "fixture_value"));

    /* Unload the old image first, as the host does after the swap; the copy keeps working. */
    de_dylib_close(orig);
    EXPECT_EQ(c_dup(), 2);
    de_dylib_close(dup);
    EXPECT_EQ(remove(copy), 0);
}

TEST(dylib, platform_name) {
    char buf[256];
#if defined(_WIN32)
    const char *game = "game.dll", *nested = "bin/game.dll";
#elif defined(__APPLE__)
    const char *game = "libgame.dylib", *nested = "bin/libgame.dylib";
#else
    const char *game = "libgame.so", *nested = "bin/libgame.so";
#endif
    ASSERT_EQ(de_dylib_platform_name("game", buf, sizeof buf), DE_OK);
    EXPECT_STREQ(buf, game);
    ASSERT_EQ(de_dylib_platform_name("bin/game", buf, sizeof buf), DE_OK);
    EXPECT_STREQ(buf, nested);

    /* Exactly enough room, then one byte short. */
    size_t need = strlen(game) + 1;
    ASSERT_EQ(de_dylib_platform_name("game", buf, need), DE_OK);
    EXPECT_STREQ(buf, game);
    EXPECT_EQ(de_dylib_platform_name("game", buf, need - 1), DE_ERR_INVALID_ARGUMENT);
    EXPECT_STREQ(buf, "");

    EXPECT_EQ(de_dylib_platform_name("", buf, sizeof buf), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_dylib_platform_name("bin/", buf, sizeof buf), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_dylib_platform_name(NULL, buf, sizeof buf), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_dylib_platform_name("game", NULL, 16), DE_ERR_INVALID_ARGUMENT);

    /* CMake names a SHARED library the same way. */
    char fixture[1024];
    fixture_dir_name("dylib_fixture", fixture, sizeof fixture);
    EXPECT_STREQ(fixture, DE_DYLIB_FIXTURE_PATH);
}
