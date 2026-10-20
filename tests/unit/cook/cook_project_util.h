/*
 * cook_project_util.h: what the `de-cook cook` and `de-pack` tests share (DENG-213): the types of
 * the fixture project tests/data/cook_project/ (the `CookSpin` component and the `cooktext` asset
 * type, defined in cook_project_types.h and registered through de_cook_hooks), output capture, a
 * scratch directory of the build tree mounted as `cct:/`, and file helpers. Included by
 * tests/unit/cook/test_cook_cli.c, tests/unit/cook/test_cook_game.c and
 * tests/unit/pack/test_pack_cli.c; each defines DE_COOK_CLI_TMP_DIR (its scratch directory) and
 * DE_COOK_PROJECT_DIR (the fixture project).
 */
#ifndef DE_COOK_PROJECT_UTIL_H
#define DE_COOK_PROJECT_UTIL_H

#define _CRT_SECURE_NO_WARNINGS /* fopen */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dengine/de_asset.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>

#include "cook_cmd.h"
#define DE_COOK_TEST_IMPORTERS 1 /* This test executable links tool importers in every config. */
#include "cook_project_types.h"
#include "de_test.h"

/* ---- Fixture types (cook_project_types.h) --------------------------------------------------- */

static const de_cook_hooks g_cct_hooks = {cct_register_types, NULL};

/* ---- Output capture ---------------------------------------------------------------------- */

typedef struct cct_buf {
    char *p;
    size_t len, cap;
} cct_buf;

static inline void cct_buf_add(cct_buf *b, const char *text, size_t len) {
    if (b->len + len + 1 > b->cap) {
        size_t cap = b->cap != 0 ? b->cap : 4096;
        while (cap < b->len + len + 1)
            cap *= 2;
        char *q = (char *)realloc(b->p, cap);
        ASSERT_TRUE(q != NULL);
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->len, text, len);
    b->len += len;
    b->p[b->len] = '\0';
}

typedef struct cct_capture {
    cct_buf out, err;
} cct_capture;

static cct_capture g_cct;

static inline void cct_reset(void) {
    free(g_cct.out.p);
    free(g_cct.err.p);
    memset(&g_cct, 0, sizeof g_cct);
    cct_buf_add(&g_cct.out, "", 0);
    cct_buf_add(&g_cct.err, "", 0);
}

static inline void cct_write(void *user, de_cook_stream stream, const char *text, size_t len) {
    (void)user;
    cct_buf_add(stream == DE_COOK_STDERR ? &g_cct.err : &g_cct.out, text, len);
}

static inline int cct_argc(const char *const *args) {
    int n = 0;
    while (args[n] != NULL)
        ++n;
    return n;
}

/** Runs `de-cook cook` in-process with the fixture hooks (NULL-terminated arguments). */
static inline int cct_cook(const char *const *args) {
    cct_reset();
    de_cook_io io = {cct_write, NULL};
    return de_cook_cook_run(cct_argc(args), args, &io, &g_cct_hooks);
}

/* ---- Scratch directory and files ----------------------------------------------------------- */

#define CCT_TMP     DE_COOK_CLI_TMP_DIR
#define CCT_PROJECT DE_COOK_PROJECT_DIR

static inline void cct_mount(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("cct", CCT_TMP, DE_MOUNT_WRITABLE, 0), DE_OK);
    mounted = true;
}

static inline bool cct_rm_entry(const char *name, bool is_dir, void *user);

/** Deletes cct:/<vdir> and everything below it (if it exists). */
static inline void cct_rm_tree(const char *vdir) {
    if (!de_path_exists(vdir))
        return;
    ASSERT_EQ(de_dir_list(vdir, cct_rm_entry, (void *)vdir), DE_OK);
    ASSERT_EQ(de_file_delete(vdir), DE_OK);
}

static inline bool cct_rm_entry(const char *name, bool is_dir, void *user) {
    char path[DE_FS_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", (const char *)user, name);
    if (is_dir)
        cct_rm_tree(path);
    else
        EXPECT_EQ(de_file_delete(path), DE_OK);
    return true;
}

/** Empties (or creates) the scratch subdirectory `sub` of this test. */
static inline void cct_fresh(const char *sub) {
    cct_mount();
    char v[DE_FS_PATH_MAX];
    snprintf(v, sizeof v, "cct:/%s", sub);
    cct_rm_tree(v);
    ASSERT_EQ(de_dir_create(v), DE_OK);
}

/** The native path CCT_TMP/<rel> in `buf`. */
static inline const char *cct_path(char *buf, size_t cap, const char *rel) {
    snprintf(buf, cap, "%s/%s", CCT_TMP, rel);
    return buf;
}

/** Writes `text` to cct:/<rel>, creating its directories. */
static inline void cct_put(const char *rel, const void *data, size_t n) {
    cct_mount();
    char v[DE_FS_PATH_MAX];
    snprintf(v, sizeof v, "cct:/%s", rel);
    for (char *p = strchr(v + 5, '/'); p != NULL; p = strchr(p + 1, '/')) {
        *p = '\0';
        if (!de_path_exists(v))
            ASSERT_EQ(de_dir_create(v), DE_OK);
        *p = '/';
    }
    ASSERT_EQ(de_file_write_all_atomic(v, data, n), DE_OK);
}

/** Reads the whole native file `path` (malloc'd, NUL after the bytes), or NULL. */
static inline u8 *cct_read(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    size_t cap = 1 << 16, n = 0;
    u8 *buf = (u8 *)malloc(cap + 1);
    for (;;) {
        if (buf == NULL) {
            fclose(f);
            return NULL;
        }
        n += fread(buf + n, 1, cap - n, f);
        if (n < cap)
            break;
        cap *= 2;
        buf = (u8 *)realloc(buf, cap + 1);
    }
    fclose(f);
    buf[n] = 0;
    *size = n;
    return buf;
}

/** True if `hay` (n bytes) contains the string `needle`. */
static inline bool cct_contains(const u8 *hay, size_t n, const char *needle) {
    size_t k = strlen(needle);
    for (size_t i = 0; k <= n && i <= n - k; ++i)
        if (memcmp(hay + i, needle, k) == 0)
            return true;
    return false;
}

/**
 * Cooks the full fixture project into CCT_TMP/<sub>/<name>.dpak for `platform`, with a cache in
 * CCT_TMP/<sub>/cache and the extra arguments `extra` (NULL-terminated, may be NULL). Returns the
 * exit code.
 */
static inline int cct_cook_project(const char *sub, const char *name, const char *platform,
                                   const char *const *extra) {
    char out[DE_FS_PATH_MAX], cache[DE_FS_PATH_MAX], rel[256];
    snprintf(rel, sizeof rel, "%s", sub);
    cct_path(out, sizeof out, rel);
    snprintf(rel, sizeof rel, "%s/cache", sub);
    cct_path(cache, sizeof cache, rel);
    const char *args[32] = {CCT_PROJECT "/assets.toml",
                            "--platform",
                            platform,
                            "--out",
                            out,
                            "--cache",
                            cache,
                            "--pack-name",
                            name,
                            "--quiet"};
    int n = 10;
    for (int i = 0; extra != NULL && extra[i] != NULL && n < 31; ++i)
        args[n++] = extra[i];
    args[n] = NULL;
    return cct_cook(args);
}

#endif /* DE_COOK_PROJECT_UTIL_H */
