/*
 * test_decache.c: the cook cache `.decache` (DENG-210, SPEC §11.5): hits skip the importer, every
 * key input gives a miss, the warm path reads no source byte, a touched file is hashed once,
 * damaged entries are misses, and two cookers storing the same key at once leave one valid entry.
 * CTest names decache.<test>.
 *
 * Each test works in its own directory of the scratch tree (ckt:/dc_<test>/), which it empties
 * first: CTest runs the tests as parallel processes, and the tree survives between runs.
 */
#include "cook_test_util.h"
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "asset/manifest_internal.h"
#include "cook/lib/de_cooker.h"
#include "cook/lib/decache.h"
#include "platform/de_atomic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS ((u64)1000000)

/* ---- Scratch directories --------------------------------------------------------------------- */

typedef struct name_list {
    char (*names)[256];
    bool *dirs;
    u32 count, cap;
} name_list;

static bool collect(const char *name, bool is_dir, void *user) {
    name_list *l = (name_list *)user;
    if (l->count == l->cap) {
        u32 ncap = l->cap != 0 ? l->cap * 2u : 64u;
        l->names = (char (*)[256])realloc(l->names, sizeof(*l->names) * ncap);
        l->dirs = (bool *)realloc(l->dirs, sizeof(bool) * ncap);
        l->cap = ncap;
    }
    snprintf(l->names[l->count], sizeof l->names[0], "%s", name);
    l->dirs[l->count++] = is_dir;
    return true;
}

static void list_free(name_list *l) {
    free(l->names);
    free(l->dirs);
    memset(l, 0, sizeof *l);
}

/* `a/b` into `buf`; false when it does not fit (the return value is checked, so GCC's
 * -Wformat-truncation knows a long path is handled). */
static bool path_join(char *buf, size_t cap, const char *a, const char *b) {
    int n = snprintf(buf, cap, "%s/%s", a, b);
    return n > 0 && (size_t)n < cap;
}

/* Deletes `vpath` and everything below it. */
static void rm_tree(const char *vpath) {
    if (!de_path_exists(vpath))
        return;
    name_list l = {0};
    if (de_dir_list(vpath, collect, &l) == DE_OK) {
        for (u32 i = 0; i < l.count; ++i) {
            char child[DE_FS_PATH_MAX];
            if (!path_join(child, sizeof child, vpath, l.names[i]))
                continue;
            if (l.dirs[i])
                rm_tree(child);
            else
                (void)de_file_delete(child);
        }
    }
    list_free(&l);
    (void)de_file_delete(vpath);
    de_error_clear();
}

/* The paths of every file below `cache` whose name contains `part` (".dc", ".tmp-"). */
static u32 find_files(const char *cache, const char *part, char out[][DE_FS_PATH_MAX], u32 max) {
    u32 n = 0;
    name_list top = {0};
    if (de_dir_list(cache, collect, &top) != DE_OK)
        return 0;
    for (u32 i = 0; i < top.count; ++i) {
        if (!top.dirs[i])
            continue;
        char shard[DE_FS_PATH_MAX];
        if (!path_join(shard, sizeof shard, cache, top.names[i]))
            continue;
        name_list l = {0};
        if (de_dir_list(shard, collect, &l) == DE_OK)
            for (u32 k = 0; k < l.count; ++k)
                if (strstr(l.names[k], part) != NULL) {
                    if (n < max && !path_join(out[n], DE_FS_PATH_MAX, shard, l.names[k]))
                        continue;
                    ++n;
                }
        list_free(&l);
    }
    list_free(&top);
    return n;
}

static void put_bytes(const char *vpath, const void *data, u64 n) {
    ASSERT_EQ(de_file_write_all_atomic(vpath, data, n), DE_OK);
}

/* ---- Fixture: cookers on one cache directory --------------------------------------------------
 */

typedef struct dc_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_atomic_u32 imports;
    char dir[128];   /* ckt:/dc_<test> */
    char root[160];  /* <dir>/assets */
    char cache[160]; /* <dir>/cache */
} dc_fx;

static de_result counting_import(de_asset_import_ctx *ctx) {
    de_atomic_u32 *n = (de_atomic_u32 *)de_asset_import_user(ctx);
    (void)de_atomic_fetch_add_u32(n, 1, DE_ATOMIC_ACQ_REL);
    return ckt_upper_import(ctx);
}

static void dc_setup(dc_fx *f, const char *test) {
    ckt_mount();
    memset(f, 0, sizeof *f);
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    de_atomic_init_u32(&f->imports, 0);
    snprintf(f->dir, sizeof f->dir, "ckt:/dc_%s", test);
    snprintf(f->root, sizeof f->root, "%s/assets", f->dir);
    snprintf(f->cache, sizeof f->cache, "%s/cache", f->dir);
    rm_tree(f->dir);
    ASSERT_EQ(de_dir_create(f->root), DE_OK);
}

static void dc_teardown(dc_fx *f) {
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
    rm_tree(f->dir);
}

/* The racy window of the test cookers: files written by a test count as settled once they are
 * older than this (settle()). The default (2 s) would make every test wait. */
#define TEST_RACY_NS ((i64)(20 * MS))

/* Waits past TEST_RACY_NS, so the files just written are recorded in the index and an extra
 * source no longer defers the store. */
static void settle(void) {
    de_sleep_ns(30 * MS);
}

/* A cooker on the fixture's cache with the counting `upper` importer. */
static de_cooker *dc_cooker(dc_fx *f, de_cook_platform platform, u32 importer_version) {
    de_cooker *c = NULL;
    de_cooker_desc d = {.allocator = &f->alloc,
                        .platform = platform,
                        .cache_dir = f->cache,
                        .verify_determinism = DE_COOK_VERIFY_OFF};
    if (de_cooker_create(&d, &c) != DE_OK)
        return NULL;
    de_asset_type_desc t = ckt_upper_type(&f->imports);
    t.import = counting_import;
    t.importer_version = importer_version;
    if (de_cooker_register_type(c, &t) != DE_OK) {
        de_cooker_destroy(c);
        return NULL;
    }
    de__decache_set_racy_window(c, TEST_RACY_NS);
    return c;
}

static void dc_put(dc_fx *f, const char *rel, const char *text) {
    char vpath[DE_FS_PATH_MAX];
    snprintf(vpath, sizeof vpath, "%s/%s", f->root, rel);
    char dir[DE_FS_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", vpath);
    *strrchr(dir, '/') = '\0';
    ASSERT_EQ(de_dir_create(dir), DE_OK);
    put_bytes(vpath, text, strlen(text));
}

static u32 imports(dc_fx *f) {
    return de_atomic_load_u32(&f->imports, DE_ATOMIC_ACQUIRE);
}

static de_cooker_cache_stats stats(const de_cooker *c) {
    de_cooker_cache_stats s;
    de_cooker_get_cache_stats(c, &s);
    return s;
}

static de_result dc_cook(dc_fx *f, de_cooker *c, const de__manifest_asset *a, de_cook_result *r) {
    de_cook_input in = {.asset = a, .asset_root = f->root};
    return de_cooker_cook(c, &in, r);
}

/* Every output of two results is the same. */
static void expect_same_result(const de_cook_result *x, const de_cook_result *y) {
    ASSERT_EQ(x->size, y->size);
    EXPECT_MEMEQ(x->data, y->data, (size_t)x->size);
    ASSERT_EQ(x->dependency_count, y->dependency_count);
    for (u32 i = 0; i < x->dependency_count; ++i)
        EXPECT_EQ(x->dependencies[i].hash, y->dependencies[i].hash);
    ASSERT_EQ(x->subasset_count, y->subasset_count);
    for (u32 i = 0; i < x->subasset_count; ++i) {
        EXPECT_STREQ(x->subassets[i].name, y->subassets[i].name);
        EXPECT_STREQ(x->subassets[i].type, y->subassets[i].type);
        EXPECT_EQ(x->subassets[i].id.hash, y->subassets[i].id.hash);
        ASSERT_EQ(x->subassets[i].size, y->subassets[i].size);
        EXPECT_MEMEQ(x->subassets[i].data, y->subassets[i].data, (size_t)x->subassets[i].size);
    }
    ASSERT_EQ(x->extra_source_count, y->extra_source_count);
    for (u32 i = 0; i < x->extra_source_count; ++i)
        EXPECT_STREQ(x->extra_sources[i], y->extra_sources[i]);
    EXPECT_STREQ(x->source, y->source);
    EXPECT_EQ(x->settings_hash, y->settings_hash);
    EXPECT_EQ(x->importer_version, y->importer_version);
    EXPECT_EQ((int)x->platform, (int)y->platform);
    EXPECT_EQ(x->output_hash, y->output_hash);
}

/* Reads a whole file (test allocator: the default heap). */
static u8 *slurp(const char *vpath, u64 *n) {
    void *d = NULL;
    *n = 0;
    if (de_file_read_all(vpath, NULL, &d, n) != DE_OK)
        return NULL;
    return (u8 *)d;
}
static void slurp_free(u8 *d, u64 n) {
    if (d != NULL)
        DE_FREE(de_heap_allocator(), d, (size_t)n + 1u);
}

/* ---- decache.hit_skips_import ------------------------------------------------------------------
 */

TEST(decache, hit_skips_import) {
    dc_fx f;
    dc_setup(&f, "hit");
    dc_put(&f, "text/a.txt", "hello cache");
    dc_put(&f, "text/inc.txt", "included");
    settle();
    static const char *const reads[] = {"inc.txt"};
    const de__manifest_kv kv[] = {
        {"dep", {.type = DE__MANIFEST_STRING, .u.s = "text/b"}},
    };
    const de__manifest_kv kv_extra[] = {
        {"read", {.type = DE__MANIFEST_STRING_ARRAY, .u.arr = {reads, 1}}},
    };
    de__manifest_asset plain = ckt_asset("text/a", "upper", "text/a.txt", kv, 1);
    de__manifest_asset extra = ckt_asset("text/x", "upper", "text/a.txt", kv_extra, 1);

    de_cooker *c = dc_cooker(&f, DE_COOK_PLATFORM_LINUX, 7);
    ASSERT_TRUE(c != NULL);
    de_cook_result r1, r2;
    /* Cold: a miss, an import, a store. */
    ASSERT_EQ(dc_cook(&f, c, &plain, &r1), DE_OK);
    EXPECT_EQ(imports(&f), 1u);
    de_cooker_cache_stats s = stats(c);
    EXPECT_EQ(s.misses, 1u);
    EXPECT_EQ(s.hits, 0u);
    EXPECT_EQ(s.stores, 1u);
    EXPECT_GT(s.bytes_written, 0u);
    /* Warm, same cooker: a hit, no import, the same result. */
    ASSERT_EQ(dc_cook(&f, c, &plain, &r2), DE_OK);
    EXPECT_EQ(imports(&f), 1u);
    s = stats(c);
    EXPECT_EQ(s.hits, 1u);
    EXPECT_GT(s.bytes_read, 0u);
    ASSERT_EQ(r2.size, 11u);
    EXPECT_MEMEQ(r2.data, "HELLO CACHE", 11);
    ASSERT_EQ(r2.subasset_count, 1u);
    EXPECT_EQ(r2.subassets[0].id.hash, de_asset_id_from("text/a#lower").hash);
    expect_same_result(&r1, &r2);
    de_cook_result_free(&r2);

    /* An asset whose importer reads another file: the entry sits behind a redirect. */
    de_cook_result x1, x2;
    ASSERT_EQ(dc_cook(&f, c, &extra, &x1), DE_OK);
    EXPECT_EQ(imports(&f), 2u);
    ASSERT_EQ(x1.extra_source_count, 1u);
    EXPECT_EQ(stats(c).stores, 3u); /* the entry and the redirect */
    ASSERT_EQ(dc_cook(&f, c, &extra, &x2), DE_OK);
    EXPECT_EQ(imports(&f), 2u);
    expect_same_result(&x1, &x2);
    de_cook_result_free(&x2);
    de_cooker_destroy(c);

    /* Another cooker (another process) on the same directory hits as well. */
    c = dc_cooker(&f, DE_COOK_PLATFORM_LINUX, 7);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(dc_cook(&f, c, &plain, &r2), DE_OK);
    ASSERT_EQ(dc_cook(&f, c, &extra, &x2), DE_OK);
    EXPECT_EQ(imports(&f), 2u);
    EXPECT_EQ(stats(c).hits, 2u);
    EXPECT_EQ(stats(c).misses, 0u);
    expect_same_result(&r1, &r2);
    expect_same_result(&x1, &x2);
    de_cook_result_free(&r2);
    de_cook_result_free(&x2);
    de_cooker_destroy(c);

    /* Identical inputs give byte-identical entries: a second cache directory gets the same
     * files (no time, process ID or host path in them). */
    char first[4][DE_FS_PATH_MAX], second[4][DE_FS_PATH_MAX];
    ASSERT_EQ(find_files(f.cache, ".dc", first, 4), 3u);
    snprintf(f.cache, sizeof f.cache, "%s/cache2", f.dir);
    c = dc_cooker(&f, DE_COOK_PLATFORM_LINUX, 7);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(dc_cook(&f, c, &plain, &r2), DE_OK);
    ASSERT_EQ(dc_cook(&f, c, &extra, &x2), DE_OK);
    de_cook_result_free(&r2);
    de_cook_result_free(&x2);
    de_cooker_destroy(c);
    ASSERT_EQ(find_files(f.cache, ".dc", second, 4), 3u);
    for (u32 i = 0; i < 3; ++i) {
        const char *name = strrchr(first[i], '/');
        bool found = false;
        for (u32 k = 0; k < 3 && !found; ++k) {
            if (strcmp(name, strrchr(second[k], '/')) != 0)
                continue;
            found = true;
            u64 na = 0, nb = 0;
            u8 *a = slurp(first[i], &na);
            u8 *b = slurp(second[k], &nb);
            ASSERT_TRUE(a != NULL && b != NULL);
            ASSERT_EQ(na, nb);
            EXPECT_MEMEQ(a, b, (size_t)na);
            slurp_free(a, na);
            slurp_free(b, nb);
        }
        EXPECT_TRUE(found);
    }

    de_cook_result_free(&r1);
    de_cook_result_free(&x1);

    /* An extra source that changed less than the racy window before the cook began may have
     * changed during the import: the store is left out, and the next cook imports again. */
    c = dc_cooker(&f, DE_COOK_PLATFORM_LINUX, 7);
    ASSERT_TRUE(c != NULL);
    de__decache_set_racy_window(c, (i64)(3600 * 1000 * MS)); /* one hour */
    dc_put(&f, "text/inc.txt", "changed!");
    u32 before = imports(&f);
    ASSERT_EQ(dc_cook(&f, c, &extra, &x2), DE_OK);
    de_cook_result_free(&x2);
    EXPECT_EQ(stats(c).store_deferred, 1u);
    EXPECT_EQ(stats(c).stores, 0u);
    ASSERT_EQ(dc_cook(&f, c, &extra, &x2), DE_OK);
    de_cook_result_free(&x2);
    EXPECT_EQ(imports(&f), before + 2u);
    EXPECT_EQ(stats(c).store_deferred, 2u);
    de__decache_set_racy_window(c, TEST_RACY_NS); /* settled: stored, then a hit */
    settle();
    ASSERT_EQ(dc_cook(&f, c, &extra, &x2), DE_OK);
    de_cook_result_free(&x2);
    EXPECT_EQ(stats(c).stores, 2u);
    ASSERT_EQ(dc_cook(&f, c, &extra, &x2), DE_OK);
    de_cook_result_free(&x2);
    EXPECT_EQ(imports(&f), before + 3u);
    EXPECT_EQ(stats(c).hits, 1u);
    de_cooker_destroy(c);

    /* Without a cache directory nothing is cached and the stats stay zero. */
    de_cooker *nc = NULL;
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.allocator = &f.alloc}, &nc), DE_OK);
    EXPECT_EQ(stats(nc).misses, 0u);
    EXPECT_EQ(de_cooker_flush_cache(nc), DE_OK);
    de_cooker_destroy(nc);
    dc_teardown(&f);
}

/* ---- decache.key_inputs -------------------------------------------------------------------------
 */

/* One key input changed: the source bytes, an extra source, a setting, the importer version or
 * the platform. Each gives a miss; putting it back gives a hit again. */
typedef enum key_input {
    KEY_SOURCE,
    KEY_EXTRA_SOURCE,
    KEY_SETTING,
    KEY_IMPORTER_VERSION,
    KEY_PLATFORM,
    KEY_COUNT
} key_input;

static const char *const key_input_names[KEY_COUNT] = {"source bytes", "extra source", "setting",
                                                       "importer version", "platform"};

TEST(decache, key_inputs) {
    dc_fx f;
    dc_setup(&f, "keys");
    /* The same sizes for the changed bytes, so only the content tells them apart. */
    dc_put(&f, "k/main.txt", "main source");
    dc_put(&f, "k/inc.txt", "include one");
    settle();
    static const char *const reads[] = {"inc.txt"};
    de__manifest_kv kv[] = {
        {"q", {.type = DE__MANIFEST_INT, .u.i = 1}},
        {"read", {.type = DE__MANIFEST_STRING_ARRAY, .u.arr = {reads, 1}}},
    };
    de__manifest_asset a = ckt_asset("k/main", "upper", "k/main.txt", kv, 2);

    de_cooker *base = dc_cooker(&f, DE_COOK_PLATFORM_LINUX, 7);
    ASSERT_TRUE(base != NULL);
    de_cook_result r;
    ASSERT_EQ(dc_cook(&f, base, &a, &r), DE_OK);
    de_cook_result_free(&r);
    ASSERT_EQ(dc_cook(&f, base, &a, &r), DE_OK);
    de_cook_result_free(&r);
    ASSERT_EQ(imports(&f), 1u);

    for (int k = 0; k < KEY_COUNT; ++k) {
        printf("  input: %s\n", key_input_names[k]);
        de_cooker *c = base;
        if (k == KEY_SOURCE)
            dc_put(&f, "k/main.txt", "MAIN source");
        else if (k == KEY_EXTRA_SOURCE)
            dc_put(&f, "k/inc.txt", "include two");
        else if (k == KEY_SETTING)
            kv[0].value.u.i = 2;
        else if (k == KEY_IMPORTER_VERSION)
            c = dc_cooker(&f, DE_COOK_PLATFORM_LINUX, 8);
        else
            c = dc_cooker(&f, DE_COOK_PLATFORM_WINDOWS, 7);
        settle();
        ASSERT_TRUE(c != NULL);
        u32 before = imports(&f);
        u64 hits = stats(c).hits;
        ASSERT_EQ(dc_cook(&f, c, &a, &r), DE_OK);
        de_cook_result_free(&r);
        EXPECT_EQ(imports(&f), before + 1u); /* a miss: imported */
        EXPECT_EQ(stats(c).hits, hits);
        /* The changed input is part of the key: cooking again hits. */
        ASSERT_EQ(dc_cook(&f, c, &a, &r), DE_OK);
        de_cook_result_free(&r);
        EXPECT_EQ(imports(&f), before + 1u);

        /* Put the input back: the base cooker hits its first entry again. */
        if (k == KEY_SOURCE)
            dc_put(&f, "k/main.txt", "main source");
        else if (k == KEY_EXTRA_SOURCE)
            dc_put(&f, "k/inc.txt", "include one");
        else if (k == KEY_SETTING)
            kv[0].value.u.i = 1;
        else
            de_cooker_destroy(c);
        settle();
        hits = stats(base).hits;
        ASSERT_EQ(dc_cook(&f, base, &a, &r), DE_OK);
        de_cook_result_free(&r);
        EXPECT_EQ(imports(&f), before + 1u);
        EXPECT_EQ(stats(base).hits, hits + 1u);
    }
    /* A failed import stores nothing: once its file exists the asset cooks (a miss). */
    static const char *const late_reads[] = {"late.txt"};
    const de__manifest_kv kv_late[] = {
        {"read", {.type = DE__MANIFEST_STRING_ARRAY, .u.arr = {late_reads, 1}}},
    };
    de__manifest_asset late = ckt_asset("k/late", "upper", "k/main.txt", kv_late, 1);
    EXPECT_NE(dc_cook(&f, base, &late, &r), DE_OK); /* the importer needs the file */
    de_error_clear();
    dc_put(&f, "k/late.txt", "now here");
    u32 before = imports(&f);
    ASSERT_EQ(dc_cook(&f, base, &late, &r), DE_OK);
    de_cook_result_free(&r);
    EXPECT_EQ(imports(&f), before + 1u);
    de_cooker_destroy(base);
    dc_teardown(&f);
}

/* ---- decache.warm_no_source_reads --------------------------------------------------------------
 */

#define WARM_ASSETS 1000u

TEST(decache, warm_no_source_reads) {
    dc_fx f;
    dc_setup(&f, "warm");
    static char ids[WARM_ASSETS][32], srcs[WARM_ASSETS][32];
    static de__manifest_asset assets[WARM_ASSETS];
    for (u32 i = 0; i < WARM_ASSETS; ++i) {
        snprintf(ids[i], sizeof ids[i], "w/%u", i);
        snprintf(srcs[i], sizeof srcs[i], "w/%u/%u.txt", i / 100u, i);
        char text[64];
        snprintf(text, sizeof text, "warm asset number %u", i);
        dc_put(&f, srcs[i], text);
        assets[i] = ckt_asset(ids[i], "upper", srcs[i], NULL, 0);
    }
    /* The index records only files older than the racy window: wait past a short window. */
    settle();

    de_cooker *c = dc_cooker(&f, DE_COOK_PLATFORM_HOST, 7);
    ASSERT_TRUE(c != NULL);
    for (u32 i = 0; i < WARM_ASSETS; ++i) {
        de_cook_result r;
        ASSERT_EQ(dc_cook(&f, c, &assets[i], &r), DE_OK);
        de_cook_result_free(&r);
    }
    de_cooker_cache_stats s = stats(c);
    EXPECT_EQ(s.misses, (u64)WARM_ASSETS);
    EXPECT_EQ(s.files_hashed, (u64)WARM_ASSETS);
    EXPECT_GE(s.index_writes, 1u); /* every DE__DECACHE_INDEX_WRITE_EVERY stores */
    de_cooker_destroy(c);          /* writes the index */
    EXPECT_EQ(imports(&f), WARM_ASSETS);

    /* A new cooker (the next run): every asset hits, and no source byte is read. */
    c = dc_cooker(&f, DE_COOK_PLATFORM_HOST, 7);
    ASSERT_TRUE(c != NULL);
    u64 t0 = de_time_now_ns();
    for (u32 i = 0; i < WARM_ASSETS; ++i) {
        de_cook_result r;
        ASSERT_EQ(dc_cook(&f, c, &assets[i], &r), DE_OK);
        if (i == 123u) {
            ASSERT_EQ(r.size, 21u);
            EXPECT_MEMEQ(r.data, "WARM ASSET NUMBER 123", 21);
        }
        de_cook_result_free(&r);
    }
    u64 dt = de_time_now_ns() - t0;
    s = stats(c);
    EXPECT_EQ(s.hits, (u64)WARM_ASSETS);
    EXPECT_EQ(s.misses, 0u);
    EXPECT_EQ(s.source_bytes_read, 0u);
    EXPECT_EQ(s.files_hashed, 0u);
    EXPECT_EQ(imports(&f), WARM_ASSETS);
    printf("  warm: %u hits in %.1f ms, %llu cache bytes read\n", WARM_ASSETS, (double)dt / 1e6,
           (unsigned long long)s.bytes_read);
    de_cooker_destroy(c);
    dc_teardown(&f);
}

/* ---- decache.mtime_touch ------------------------------------------------------------------------
 */

TEST(decache, mtime_touch) {
    dc_fx f;
    dc_setup(&f, "touch");
    const char *text = "touch me but keep the bytes";
    dc_put(&f, "t/a.txt", text);
    char vpath[DE_FS_PATH_MAX];
    snprintf(vpath, sizeof vpath, "%s/t/a.txt", f.root);
    de__manifest_asset a = ckt_asset("t/a", "upper", "t/a.txt", NULL, 0);
    settle();

    de_cooker *c = dc_cooker(&f, DE_COOK_PLATFORM_HOST, 7);
    ASSERT_TRUE(c != NULL);
    de_cook_result r;
    ASSERT_EQ(dc_cook(&f, c, &a, &r), DE_OK); /* cold: hashed once, recorded */
    de_cook_result_free(&r);
    ASSERT_EQ(dc_cook(&f, c, &a, &r), DE_OK); /* warm: the index answers */
    de_cook_result_free(&r);
    EXPECT_EQ(stats(c).files_hashed, 1u);
    EXPECT_EQ(stats(c).hits, 1u);

    /* Touch: write the same bytes again until the mtime moves (coarse file-system clocks). */
    u64 size0 = 0, size1 = 0;
    i64 m0 = 0, m1 = 0;
    ASSERT_EQ(de__decache_file_stat(vpath, &size0, &m0), DE_OK);
    m1 = m0;
    for (int i = 0; i < 400 && m1 == m0; ++i) {
        put_bytes(vpath, text, strlen(text));
        ASSERT_EQ(de__decache_file_stat(vpath, &size1, &m1), DE_OK);
        if (m1 == m0)
            de_sleep_ns(5 * MS);
    }
    ASSERT_NE(m1, m0);
    EXPECT_EQ(size1, size0);
    settle(); /* older than the racy window, so the new mtime is recorded */

    ASSERT_EQ(dc_cook(&f, c, &a, &r), DE_OK); /* rehashed once, still a hit */
    de_cook_result_free(&r);
    de_cooker_cache_stats s = stats(c);
    EXPECT_EQ(s.files_hashed, 2u);
    EXPECT_EQ(s.source_bytes_read, 2u * (u64)strlen(text));
    EXPECT_EQ(s.hits, 2u);
    EXPECT_EQ(imports(&f), 1u);
    ASSERT_EQ(dc_cook(&f, c, &a, &r), DE_OK); /* the index has the new mtime */
    de_cook_result_free(&r);
    EXPECT_EQ(stats(c).files_hashed, 2u);
    de_cooker_destroy(c);

    /* ... and it was written: the next cooker reads nothing either. */
    c = dc_cooker(&f, DE_COOK_PLATFORM_HOST, 7);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(dc_cook(&f, c, &a, &r), DE_OK);
    de_cook_result_free(&r);
    EXPECT_EQ(stats(c).hits, 1u);
    EXPECT_EQ(stats(c).files_hashed, 0u);
    EXPECT_EQ(imports(&f), 1u);
    de_cooker_destroy(c);

    /* A file younger than the racy window is not recorded: it is hashed again next time. */
    c = dc_cooker(&f, DE_COOK_PLATFORM_HOST, 7);
    ASSERT_TRUE(c != NULL);
    de__decache_set_racy_window(c, (i64)(3600 * 1000 * MS)); /* one hour */
    ASSERT_EQ(dc_cook(&f, c, &a, &r), DE_OK);                /* the old record still matches */
    de_cook_result_free(&r);
    put_bytes(vpath, text, strlen(text));
    ASSERT_EQ(dc_cook(&f, c, &a, &r), DE_OK);
    de_cook_result_free(&r);
    ASSERT_EQ(dc_cook(&f, c, &a, &r), DE_OK);
    de_cook_result_free(&r);
    EXPECT_EQ(stats(c).files_hashed, 2u); /* both cooks after the write hashed it */
    EXPECT_EQ(stats(c).hits, 3u);
    de_cooker_destroy(c);
    dc_teardown(&f);
}

/* ---- decache.corrupt_entry ----------------------------------------------------------------------
 */

typedef enum damage {
    DAMAGE_TRUNCATE_BODY,
    DAMAGE_TRUNCATE_HEADER,
    DAMAGE_EMPTY,
    DAMAGE_MAGIC,
    DAMAGE_BODY_BYTE,
    DAMAGE_EXTRA_BYTES,
    DAMAGE_COUNT
} damage;

static void damage_file(const char *path, damage how) {
    u64 n = 0;
    u8 *d = slurp(path, &n);
    ASSERT_TRUE(d != NULL);
    ASSERT_GT(n, 60u);
    switch (how) {
    case DAMAGE_TRUNCATE_BODY:
        put_bytes(path, d, n - 5u);
        break;
    case DAMAGE_TRUNCATE_HEADER:
        put_bytes(path, d, 10u);
        break;
    case DAMAGE_EMPTY:
        put_bytes(path, d, 0u);
        break;
    case DAMAGE_MAGIC:
        d[0] ^= 0xFFu;
        put_bytes(path, d, n);
        break;
    case DAMAGE_BODY_BYTE:
        d[n - 3u] ^= 0x5Au;
        put_bytes(path, d, n);
        break;
    case DAMAGE_EXTRA_BYTES: {
        u8 *e = (u8 *)malloc((size_t)n + 7u);
        memcpy(e, d, (size_t)n);
        memset(e + n, 0xAB, 7);
        put_bytes(path, e, n + 7u);
        free(e);
        break;
    }
    case DAMAGE_COUNT:
        break;
    }
    slurp_free(d, n);
}

TEST(decache, corrupt_entry) {
    dc_fx f;
    dc_setup(&f, "corrupt");
    dc_put(&f, "c/a.txt",
           "a source long enough to make a body worth compressing, a source long "
           "enough to make a body worth compressing");
    de__manifest_asset a = ckt_asset("c/a", "upper", "c/a.txt", NULL, 0);
    de_cooker *c = dc_cooker(&f, DE_COOK_PLATFORM_HOST, 7);
    ASSERT_TRUE(c != NULL);
    de_cook_result good, r;
    ASSERT_EQ(dc_cook(&f, c, &a, &good), DE_OK);
    char entries[2][DE_FS_PATH_MAX];
    ASSERT_EQ(find_files(f.cache, ".dc", entries, 2), 1u);

    for (int k = 0; k < DAMAGE_COUNT; ++k) {
        printf("  damage %d\n", k);
        damage_file(entries[0], (damage)k);
        u32 before = imports(&f);
        u64 corrupt = stats(c).corrupt;
        ASSERT_EQ(dc_cook(&f, c, &a, &r), DE_OK); /* a miss and a new cook, no crash */
        expect_same_result(&good, &r);
        de_cook_result_free(&r);
        EXPECT_EQ(imports(&f), before + 1u);
        EXPECT_EQ(stats(c).corrupt, corrupt + 1u);
        /* The damaged file was replaced by a good one. */
        ASSERT_EQ(dc_cook(&f, c, &a, &r), DE_OK);
        de_cook_result_free(&r);
        EXPECT_EQ(imports(&f), before + 1u);
    }

    /* A damaged redirect and a damaged entry behind it. */
    dc_put(&f, "c/inc.txt", "include");
    settle();
    static const char *const reads[] = {"inc.txt"};
    const de__manifest_kv kv[] = {
        {"read", {.type = DE__MANIFEST_STRING_ARRAY, .u.arr = {reads, 1}}},
    };
    de__manifest_asset x = ckt_asset("c/x", "upper", "c/a.txt", kv, 1);
    ASSERT_EQ(dc_cook(&f, c, &x, &r), DE_OK);
    de_cook_result_free(&r);
    char all[4][DE_FS_PATH_MAX];
    ASSERT_EQ(find_files(f.cache, ".dc", all, 4), 3u);
    for (u32 i = 0; i < 3; ++i) {
        if (strcmp(all[i], entries[0]) == 0)
            continue;
        damage_file(all[i], DAMAGE_TRUNCATE_BODY);
        u32 before = imports(&f);
        ASSERT_EQ(dc_cook(&f, c, &x, &r), DE_OK);
        de_cook_result_free(&r);
        EXPECT_EQ(imports(&f), before + 1u);
        ASSERT_EQ(dc_cook(&f, c, &x, &r), DE_OK);
        de_cook_result_free(&r);
        EXPECT_EQ(imports(&f), before + 1u);
    }
    de_cooker_destroy(c);

    /* A damaged index is ignored (and rewritten): entries still hit. */
    char index[DE_FS_PATH_MAX];
    snprintf(index, sizeof index, "%s/index.bin", f.cache);
    put_bytes(index, "DEIXgarbage that is not an index", 32u);
    c = dc_cooker(&f, DE_COOK_PLATFORM_HOST, 7);
    ASSERT_TRUE(c != NULL);
    u32 before = imports(&f);
    ASSERT_EQ(dc_cook(&f, c, &a, &r), DE_OK);
    expect_same_result(&good, &r);
    de_cook_result_free(&r);
    EXPECT_EQ(imports(&f), before);
    EXPECT_EQ(stats(c).hits, 1u);
    de_cooker_destroy(c);
    u64 n = 0;
    u8 *idx = slurp(index, &n);
    ASSERT_TRUE(idx != NULL);
    ASSERT_GE(n, 24u); /* a good index again */
    EXPECT_MEMEQ(idx, "DEIX\x01\0\0\0", 8);
    slurp_free(idx, n);

    de_cook_result_free(&good);
    dc_teardown(&f);
}

/* ---- decache.concurrent_store -------------------------------------------------------------------
 */

/* Two cookers, each on its own thread (two processes as far as the cache can tell: separate
 * indexes, separate temporary names), store the same keys at the same moment. */
#define RACE_ROUNDS 40

typedef struct race_side {
    dc_fx *f;
    de_cooker *c;
    de__manifest_kv kv[2]; /* q (changed every round), read (second half) */
    de__manifest_asset a;
    de_atomic_u32 *go;    /* round number both sides start */
    de_atomic_u32 *ready; /* sides waiting for the round */
    u32 round_value[RACE_ROUNDS];
    de_result result[RACE_ROUNDS];
    u64 output_hash[RACE_ROUNDS];
} race_side;

static i32 race_thread(void *user) {
    race_side *s = (race_side *)user;
    for (u32 round = 0; round < RACE_ROUNDS; ++round) {
        (void)de_atomic_fetch_add_u32(s->ready, 1, DE_ATOMIC_ACQ_REL);
        while (de_atomic_load_u32(s->go, DE_ATOMIC_ACQUIRE) <= round)
            de_cpu_pause();
        s->kv[0].value.u.i = (i64)round; /* a new key every round */
        de_cook_result r;
        s->result[round] = dc_cook(s->f, s->c, &s->a, &r);
        s->output_hash[round] = r.output_hash;
        de_cook_result_free(&r);
    }
    return 0;
}

TEST(decache, concurrent_store) {
    dc_fx f;
    dc_setup(&f, "race");
    dc_put(&f, "r/a.txt", "two cookers race to store this");
    dc_put(&f, "r/inc.txt", "and this include");
    settle();
    static const char *const reads[] = {"inc.txt"};
    de_atomic_u32 go, ready;
    de_atomic_init_u32(&go, 0);
    de_atomic_init_u32(&ready, 0);
    static race_side side[2];
    for (int i = 0; i < 2; ++i) {
        memset(&side[i], 0, sizeof side[i]);
        side[i].f = &f;
        side[i].c = dc_cooker(&f, DE_COOK_PLATFORM_HOST, 7);
        ASSERT_TRUE(side[i].c != NULL);
        side[i].go = &go;
        side[i].ready = &ready;
        side[i].kv[0] = (de__manifest_kv){"q", {.type = DE__MANIFEST_INT, .u.i = 0}};
        side[i].kv[1] =
            (de__manifest_kv){"read", {.type = DE__MANIFEST_STRING_ARRAY, .u.arr = {reads, 1}}};
        side[i].a = ckt_asset("r/a", "upper", "r/a.txt", side[i].kv, 1);
    }
    de_thread *t[2];
    for (int i = 0; i < 2; ++i)
        ASSERT_EQ(
            de_thread_create(race_thread, &side[i], "decache-race", DE_THREAD_NORMAL, 0, &t[i]),
            DE_OK);
    for (u32 round = 0; round < RACE_ROUNDS; ++round) {
        while (de_atomic_load_u32(&ready, DE_ATOMIC_ACQUIRE) < 2u * (round + 1u))
            de_cpu_pause();
        if (round == RACE_ROUNDS / 2u) {
            /* Second half: the importer reads a file, so each store writes an entry and a
             * redirect. Both threads wait for `go` while this changes. */
            for (int i = 0; i < 2; ++i)
                side[i].a.setting_count = 2;
        }
        de_atomic_store_u32(&go, round + 1u, DE_ATOMIC_RELEASE);
    }
    for (int i = 0; i < 2; ++i)
        EXPECT_EQ(de_thread_join(t[i]), 0);

    for (u32 round = 0; round < RACE_ROUNDS; ++round) {
        EXPECT_EQ(side[0].result[round], DE_OK);
        EXPECT_EQ(side[1].result[round], DE_OK);
        EXPECT_EQ(side[0].output_hash[round], side[1].output_hash[round]);
    }
    de_cooker_cache_stats s0 = stats(side[0].c), s1 = stats(side[1].c);
    EXPECT_EQ(s0.store_failures + s1.store_failures, 0u);
    EXPECT_EQ(s0.corrupt + s1.corrupt, 0u);
    printf("  stores %llu + %llu, skipped %llu + %llu, hits %llu + %llu\n",
           (unsigned long long)s0.stores, (unsigned long long)s1.stores,
           (unsigned long long)s0.store_skipped, (unsigned long long)s1.store_skipped,
           (unsigned long long)s0.hits, (unsigned long long)s1.hits);
    de_cooker_destroy(side[0].c);
    de_cooker_destroy(side[1].c);

    /* One valid entry per key: a third cooker hits every round without importing, and no
     * temporary file is left. */
    static char found[2 * RACE_ROUNDS][DE_FS_PATH_MAX];
    /* First half: one entry per round; second half: an entry and a redirect per round. */
    EXPECT_EQ(find_files(f.cache, ".dc", found, 2 * RACE_ROUNDS),
              (u32)(RACE_ROUNDS / 2 + 2 * (RACE_ROUNDS - RACE_ROUNDS / 2)));
    EXPECT_EQ(find_files(f.cache, ".tmp-", found, 2 * RACE_ROUNDS), 0u);
    de_cooker *c = dc_cooker(&f, DE_COOK_PLATFORM_HOST, 7);
    ASSERT_TRUE(c != NULL);
    u32 before = imports(&f);
    de__manifest_kv kv[2] = {side[0].kv[0], side[0].kv[1]};
    de__manifest_asset a = ckt_asset("r/a", "upper", "r/a.txt", kv, 1);
    for (u32 round = 0; round < RACE_ROUNDS; ++round) {
        kv[0].value.u.i = (i64)round;
        a.setting_count = round < RACE_ROUNDS / 2u ? 1u : 2u;
        de_cook_result r;
        ASSERT_EQ(dc_cook(&f, c, &a, &r), DE_OK);
        EXPECT_EQ(r.output_hash, side[0].output_hash[round]);
        de_cook_result_free(&r);
    }
    EXPECT_EQ(imports(&f), before);
    EXPECT_EQ(stats(c).hits, (u64)RACE_ROUNDS);
    EXPECT_EQ(stats(c).corrupt, 0u);
    de_cooker_destroy(c);
    dc_teardown(&f);
}
