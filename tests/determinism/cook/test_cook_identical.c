/*
 * test_cook_identical.c: cooking is deterministic (SPEC §11.5; DENG-216).
 *
 * Cooks the fixture project tests/data/cook_project/ with `de-cook cook --platform linux`
 * (de_cook_cook_run in-process, with the fixture's types from cook_project_util.h) in several
 * ways, and checks that every pack and every `.decache` entry (`<2 hex>/<32 hex>.dc`) is the same
 * byte for byte:
 *
 *   cook.identical_cold_warm   a cold cache, then a warm one (all hits); the cache entries do not
 *                              change. Writes the pack to <build>/determinism/ for the CI
 *                              cross-host diff (determinism.yml), with a list of the cache entries.
 *   cook.identical_jobs        --jobs 1 and --jobs 8, each with a cold cache of its own, then a
 *                              warm --jobs 8 cook on the --jobs 1 cache.
 *   cook.identical_relocated   a copy of the project at another absolute path, cooked with its
 *                              default cache (.decache/ next to the manifest), against a cook of
 *                              the original. An absolute path that leaks into a pack, a cache key
 *                              or an entry shows here.
 *   cook.report_first_difference  the reports below, on planted differences.
 *
 * On a mismatch the test prints the first difference: for packs, the asset (name and ID) and the
 * byte offset in the file, in the asset's stored data and in its raw data; for caches, the entry
 * file and the offset. Packs are compared after the 256-byte header first, because the build ID
 * and the content hash in the header are hashes of the rest and differ whenever anything does.
 *
 * The cache's index.bin is not compared: it holds file modification times (the warm-path hint,
 * decache.h), which a copy or a checkout changes. Every cache key and entry is computed from
 * content.
 *
 * Cooking needs the runtime importers (DE_HAS_COOKER: Debug and Dev builds); elsewhere the tests
 * skip.
 */
#include "cook_project_util.h"

#include "asset/dpak/dpak_format.h"
#include "asset/dpak/dpak_reader.h"

#include <xxhash.h>

#define CD_PLATFORM "linux"

#if DE_HAS_COOKER

/* ---- Cooking ------------------------------------------------------------------------------- */

/* `a`/`b` into `dst`; fails the test instead of truncating. (Checking the result also keeps GCC's
 * -Wformat-truncation quiet about two path buffers of the same size.) */
static void cd_join(char *dst, size_t cap, const char *a, const char *b) {
    const int n = snprintf(dst, cap, "%s/%s", a, b);
    ASSERT_TRUE(n > 0 && (size_t)n < cap);
}

/* Cooks `manifest` into CCT_TMP/<out_rel>/<name>.dpak. `cache_rel` is the cache directory below
 * CCT_TMP, or NULL for the default (.decache next to the manifest). `jobs` may be NULL. */
static void cd_cook(const char *manifest, const char *out_rel, const char *name,
                    const char *cache_rel, const char *jobs) {
    char out[DE_FS_PATH_MAX], cache[DE_FS_PATH_MAX];
    cct_path(out, sizeof out, out_rel);
    const char *args[16];
    int n = 0;
    args[n++] = manifest;
    args[n++] = "--platform";
    args[n++] = CD_PLATFORM;
    args[n++] = "--out";
    args[n++] = out;
    args[n++] = "--pack-name";
    args[n++] = name;
    args[n++] = "--quiet";
    if (cache_rel != NULL) {
        args[n++] = "--cache";
        args[n++] = cct_path(cache, sizeof cache, cache_rel);
    }
    if (jobs != NULL) {
        args[n++] = "--jobs";
        args[n++] = jobs;
    }
    args[n] = NULL;
    const int rc = cct_cook(args);
    if (rc != DE_COOK_EXIT_OK)
        printf("  de-cook cook failed (%d):\n%s%s", rc, g_cct.out.p, g_cct.err.p);
    ASSERT_EQ(rc, DE_COOK_EXIT_OK);
}

static u8 *cd_read_pack(const char *out_rel, const char *name, size_t *size) {
    char rel[256], path[DE_FS_PATH_MAX];
    snprintf(rel, sizeof rel, "%s/%s.dpak", out_rel, name);
    u8 *p = cct_read(cct_path(path, sizeof path, rel), size);
    ASSERT_TRUE(p != NULL);
    return p;
}

/* ---- Pack comparison ------------------------------------------------------------------------ */

typedef struct cd_report {
    char text[1024];
    u64 file_offset; /* first differing byte of the file */
    u64 asset_id;    /* 0 when the difference is not in an asset's records or data */
} cd_report;

/* Where `off` lies in pack `r`: writes a description to `where` and the TOC index of the asset it
 * belongs to (or DPAK_NOT_FOUND) to `*entry`, and the offset in its stored data (data only). */
static void cd_locate(const dpak_reader *r, u64 off, char *where, size_t cap, u32 *entry,
                      u64 *in_data) {
    static const char *const k_sections[DPAK_SECTION_COUNT] = {
        "TOC", "CHUNKS", "DEPS", "BUNDLES", "BUNDLE_IDS", "DICTS", "NAMES", "STRINGS"};
    const dpak_header *h = dpak_get_header(r);
    *entry = DPAK_NOT_FOUND;
    *in_data = 0;
    if (off < DPAK_HEADER_SIZE) {
        snprintf(where, cap, "the header");
        return;
    }
    if (off >= h->file_size) {
        snprintf(where, cap, "the bytes after the end of the first pack");
        return;
    }
    for (u32 s = 0; s < (u32)DPAK_SECTION_COUNT; ++s) {
        const dpak_section_desc *d = &h->sections[s];
        if (d->count == 0 || off < d->offset || off >= d->offset + d->size)
            continue;
        const u64 rec = (off - d->offset) / dpak_section_record_size((dpak_section)s);
        snprintf(where, cap, "section %s, record %llu", k_sections[s], (unsigned long long)rec);
        if (s == DPAK_SECTION_TOC || s == DPAK_SECTION_NAMES)
            *entry = (u32)rec;
        for (u32 i = 0; s == DPAK_SECTION_CHUNKS && i < dpak_entry_count(r); ++i) {
            dpak_toc_entry e;
            dpak_entry(r, i, &e);
            if (rec >= e.first_chunk && rec < (u64)e.first_chunk + e.chunk_count)
                *entry = i;
        }
        return;
    }
    for (u32 i = 0; i < dpak_entry_count(r); ++i) {
        dpak_toc_entry e;
        dpak_entry(r, i, &e);
        if (e.chunk_count != 0 && off >= e.data_offset && off < e.data_offset + e.stored_size) {
            *entry = i;
            *in_data = off - e.data_offset;
            snprintf(where, cap, "the data");
            return;
        }
    }
    snprintf(where, cap, "a dictionary or padding");
}

/* First raw byte where entry `ia` of `ra` and entry `ib` of `rb` differ, or U64_MAX when they
 * cannot both be read or are equal. */
static u64 cd_raw_diff(const dpak_reader *ra, u32 ia, const dpak_reader *rb, u32 ib) {
    dpak_toc_entry ea, eb;
    dpak_entry(ra, ia, &ea);
    dpak_entry(rb, ib, &eb);
    u8 *a = (u8 *)malloc(ea.raw_size + 1u);
    u8 *b = (u8 *)malloc(eb.raw_size + 1u);
    u64 at = UINT64_MAX;
    if (a != NULL && b != NULL && dpak_read_entry(ra, ia, NULL, a, ea.raw_size) == DE_OK &&
        dpak_read_entry(rb, ib, NULL, b, eb.raw_size) == DE_OK) {
        const u64 n = ea.raw_size < eb.raw_size ? ea.raw_size : eb.raw_size;
        for (u64 i = 0; i < n && at == UINT64_MAX; ++i)
            if (a[i] != b[i])
                at = i;
        if (at == UINT64_MAX && ea.raw_size != eb.raw_size)
            at = n;
    }
    free(a);
    free(b);
    return at;
}

/* Compares two packs. Returns true when they are equal; otherwise fills `rep` with the first
 * difference and prints it. `ref` names the reference (pack a) in the text. */
static bool cd_pack_diff(const char *what, const u8 *a, size_t na, const u8 *b, size_t nb,
                         cd_report *rep) {
    memset(rep, 0, sizeof *rep);
    if (na == nb && memcmp(a, b, na) == 0)
        return true;
    /* After the header first: the header's build ID and content hash hash the rest. */
    const size_t n = na < nb ? na : nb;
    u64 off = UINT64_MAX;
    for (size_t i = DPAK_HEADER_SIZE; i < n && off == UINT64_MAX; ++i)
        if (a[i] != b[i])
            off = i;
    if (off == UINT64_MAX && na != nb && n >= DPAK_HEADER_SIZE)
        off = n;
    for (size_t i = 0; i < DPAK_HEADER_SIZE && i < n && off == UINT64_MAX; ++i)
        if (a[i] != b[i])
            off = i;
    if (off == UINT64_MAX)
        off = n;
    rep->file_offset = off;

    dpak_reader *ra = NULL, *rb = NULL;
    (void)dpak_open_memory(a, na, NULL, &ra);
    (void)dpak_open_memory(b, nb, NULL, &rb);
    char where[256] = "an unreadable pack";
    u32 ia = DPAK_NOT_FOUND;
    u64 in_data = 0;
    if (ra != NULL)
        cd_locate(ra, off, where, sizeof where, &ia, &in_data);
    int len = snprintf(rep->text, sizeof rep->text,
                       "%s: packs differ (%zu vs %zu bytes); first difference at file offset %llu "
                       "(0x%llx), in %s",
                       what, na, nb, (unsigned long long)off, (unsigned long long)off, where);
    if (ra != NULL && ia != DPAK_NOT_FOUND && ia < dpak_entry_count(ra)) {
        dpak_toc_entry e;
        dpak_entry(ra, ia, &e);
        rep->asset_id = e.id;
        const char *name = dpak_name(ra, ia, NULL);
        len += snprintf(rep->text + len, sizeof rep->text - (size_t)len,
                        " of asset \"%s\" (id 0x%016llx)", name != NULL ? name : "?",
                        (unsigned long long)e.id);
        if (strcmp(where, "the data") == 0)
            len += snprintf(rep->text + len, sizeof rep->text - (size_t)len, ", stored byte %llu",
                            (unsigned long long)in_data);
        const u32 ib = rb != NULL ? dpak_find(rb, e.id) : DPAK_NOT_FOUND;
        if (ib == DPAK_NOT_FOUND) {
            len += snprintf(rep->text + len, sizeof rep->text - (size_t)len,
                            "; the other pack does not have it");
        } else {
            const u64 raw = cd_raw_diff(ra, ia, rb, ib);
            if (raw != UINT64_MAX)
                len += snprintf(rep->text + len, sizeof rep->text - (size_t)len,
                                "; the cooked bytes differ first at raw byte %llu",
                                (unsigned long long)raw);
            else
                len += snprintf(rep->text + len, sizeof rep->text - (size_t)len,
                                "; the cooked bytes are equal (layout or metadata differs)");
        }
    }
    (void)len;
    dpak_close(ra);
    dpak_close(rb);
    printf("  %s\n", rep->text);
    return false;
}

/* ---- Cache comparison ----------------------------------------------------------------------- */

typedef struct cd_list {
    char **items;
    size_t count, cap;
    char dir[DE_FS_PATH_MAX]; /* while listing: the subdirectory being read */
} cd_list;

static void cd_list_free(cd_list *l) {
    for (size_t i = 0; i < l->count; ++i)
        free(l->items[i]);
    free(l->items);
    memset(l, 0, sizeof *l);
}

/* Collects the `.dc` files of one subdirectory (l->dir) of a cache. */
static bool cd_list_entry(const char *name, bool is_dir, void *user) {
    cd_list *l = (cd_list *)user;
    if (is_dir)
        return true;
    const size_t n = strlen(name);
    if (n < 3 || strcmp(name + n - 3, ".dc") != 0)
        return true;
    if (l->count == l->cap) {
        l->cap = l->cap != 0 ? l->cap * 2 : 32;
        l->items = (char **)realloc(l->items, l->cap * sizeof *l->items);
        ASSERT_TRUE(l->items != NULL);
    }
    const size_t len = strlen(l->dir) + n + 2;
    char *p = (char *)malloc(len);
    ASSERT_TRUE(p != NULL);
    snprintf(p, len, "%s/%s", l->dir, name);
    l->items[l->count++] = p;
    return true;
}

static int cd_cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The two-hex-digit subdirectories of a cache directory. */
typedef struct cd_dirs {
    char names[256][3];
    u32 count;
} cd_dirs;

static bool cd_dirs_entry(const char *name, bool is_dir, void *user) {
    cd_dirs *d = (cd_dirs *)user;
    if (is_dir && strlen(name) == 2 && d->count < 256)
        snprintf(d->names[d->count++], 3, "%s", name);
    return true;
}

/* The sorted relative paths ("ab/<32 hex>.dc") of every entry of the cache at `vdir`. */
static cd_list cd_cache_entries(const char *vdir) {
    cd_list l;
    memset(&l, 0, sizeof l);
    cd_dirs dirs;
    memset(&dirs, 0, sizeof dirs);
    ASSERT_EQ(de_dir_list(vdir, cd_dirs_entry, &dirs), DE_OK);
    for (u32 i = 0; i < dirs.count; ++i) {
        char sub[DE_FS_PATH_MAX];
        cd_join(sub, sizeof sub, vdir, dirs.names[i]);
        snprintf(l.dir, sizeof l.dir, "%s", dirs.names[i]);
        ASSERT_EQ(de_dir_list(sub, cd_list_entry, &l), DE_OK);
    }
    if (l.count > 1)
        qsort(l.items, l.count, sizeof *l.items, cd_cmp_str);
    return l;
}

static u8 *cd_read_v(const char *vdir, const char *rel, u64 *size) {
    char v[DE_FS_PATH_MAX];
    cd_join(v, sizeof v, vdir, rel);
    void *data = NULL;
    ASSERT_EQ(de_file_read_all(v, NULL, &data, size), DE_OK);
    return (u8 *)data;
}

static void cd_free_v(u8 *data, u64 size) {
    DE_FREE(de_heap_allocator(), data, (size_t)size + 1u);
}

/* Compares the entries of two caches. Returns true when both hold the same entry files with the
 * same bytes; otherwise prints the first difference into `rep` and stdout. */
static bool cd_cache_diff(const char *what, const char *va, const char *vb, cd_report *rep) {
    memset(rep, 0, sizeof *rep);
    cd_list a = cd_cache_entries(va);
    cd_list b = cd_cache_entries(vb);
    bool same = true;
    size_t i = 0, k = 0;
    while (same && (i < a.count || k < b.count)) {
        const int c = i == a.count ? 1 : k == b.count ? -1 : strcmp(a.items[i], b.items[k]);
        if (c != 0) {
            snprintf(rep->text, sizeof rep->text,
                     "%s: cache entry %s is only in %s (a cache key input differs)", what,
                     c < 0 ? a.items[i] : b.items[k], c < 0 ? va : vb);
            same = false;
            break;
        }
        u64 na = 0, nb = 0;
        u8 *pa = cd_read_v(va, a.items[i], &na);
        u8 *pb = cd_read_v(vb, b.items[k], &nb);
        if (na != nb || memcmp(pa, pb, (size_t)na) != 0) {
            u64 off = 0;
            while (off < na && off < nb && pa[off] == pb[off])
                ++off;
            rep->file_offset = off;
            snprintf(rep->text, sizeof rep->text,
                     "%s: cache entry %s differs (%llu vs %llu bytes); first difference at offset "
                     "%llu",
                     what, a.items[i], (unsigned long long)na, (unsigned long long)nb,
                     (unsigned long long)off);
            same = false;
        }
        cd_free_v(pa, na);
        cd_free_v(pb, nb);
        ++i;
        ++k;
    }
    cd_list_free(&a);
    cd_list_free(&b);
    if (!same)
        printf("  %s\n", rep->text);
    return same;
}

static size_t cd_cache_count(const char *vdir) {
    cd_list l = cd_cache_entries(vdir);
    const size_t n = l.count;
    cd_list_free(&l);
    return n;
}

/* ---- Outputs for the CI cross-host diff ------------------------------------------------------ */

/* Writes the pack and a list of the cache entries (path, size, XXH3-64) to DE_DET_OUT_DIR. */
static void cd_write_outputs(const u8 *pack, size_t n, const char *cache_vdir) {
    char path[DE_FS_PATH_MAX];
    snprintf(path, sizeof path, "%s/cook_project.%s.dpak", DE_DET_OUT_DIR, CD_PLATFORM);
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ(fwrite(pack, 1, n, f), n);
    fclose(f);

    snprintf(path, sizeof path, "%s/cook_project.%s.decache", DE_DET_OUT_DIR, CD_PLATFORM);
    f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    fprintf(f,
            "# de-cook cook --platform %s of tests/data/cook_project: cache entries "
            "(path, bytes, XXH3-64)\n",
            CD_PLATFORM);
    cd_list l = cd_cache_entries(cache_vdir);
    for (size_t i = 0; i < l.count; ++i) {
        u64 sz = 0;
        u8 *p = cd_read_v(cache_vdir, l.items[i], &sz);
        fprintf(f, "%s %llu %016llx\n", l.items[i], (unsigned long long)sz,
                (unsigned long long)XXH3_64bits(p, (size_t)sz));
        cd_free_v(p, sz);
    }
    cd_list_free(&l);
    fclose(f);
    printf("  wrote %s/cook_project.%s.{dpak,decache}\n", DE_DET_OUT_DIR, CD_PLATFORM);
}

/* ---- Copying the project -------------------------------------------------------------------- */

typedef struct cd_copy {
    char from[DE_FS_PATH_MAX]; /* virtual source directory */
    char to[512];              /* destination below CCT_TMP */
} cd_copy;

static void cd_copy_tree(const char *from, const char *to);

static bool cd_copy_entry(const char *name, bool is_dir, void *user) {
    const cd_copy *c = (const cd_copy *)user;
    if (name[0] == '.')
        return true; /* a .decache a hand-run cook may have left next to the fixture */
    char from[DE_FS_PATH_MAX], to[512]; /* `to` gets the "cct:/" prefix in cct_put */
    cd_join(from, sizeof from, c->from, name);
    cd_join(to, sizeof to, c->to, name);
    if (is_dir) {
        cd_copy_tree(from, to);
        return true;
    }
    void *data = NULL;
    u64 size = 0;
    ASSERT_EQ(de_file_read_all(from, NULL, &data, &size), DE_OK);
    cct_put(to, data, (size_t)size);
    DE_FREE(de_heap_allocator(), data, (size_t)size + 1u);
    return true;
}

static void cd_copy_tree(const char *from, const char *to) {
    cd_copy c;
    snprintf(c.from, sizeof c.from, "%s", from);
    snprintf(c.to, sizeof c.to, "%s", to);
    ASSERT_EQ(de_dir_list(from, cd_copy_entry, &c), DE_OK);
}

#endif /* DE_HAS_COOKER */

#define NO_COOKER_MSG "de-cook cook needs the runtime importers (DE_HAS_COOKER: Debug and Dev)"

/* ---- Tests --------------------------------------------------------------------------------- */

TEST(cook, identical_cold_warm) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    cct_fresh("cold_warm");
    cd_cook(CCT_PROJECT "/assets.toml", "cold_warm", "cold", "cold_warm/cache", NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, "cache hits: 0, misses: 7") != NULL);
    /* Keep a copy of the cold cache's entries to check that the warm cook leaves them alone. */
    cct_fresh("cold_warm_snapshot");
    cd_copy_tree("cct:/cold_warm/cache", "cold_warm_snapshot");
    ASSERT_GE(cd_cache_count("cct:/cold_warm_snapshot"), (size_t)7);

    cd_cook(CCT_PROJECT "/assets.toml", "cold_warm", "warm", "cold_warm/cache", NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, "cache hits: 7, misses: 0, stores: 0") != NULL);

    size_t n0 = 0, n1 = 0;
    u8 *cold = cd_read_pack("cold_warm", "cold", &n0);
    u8 *warm = cd_read_pack("cold_warm", "warm", &n1);
    cd_report rep;
    EXPECT_TRUE(cd_pack_diff("cold vs warm cache", cold, n0, warm, n1, &rep));
    EXPECT_TRUE(cd_cache_diff("cache before vs after the warm cook", "cct:/cold_warm_snapshot",
                              "cct:/cold_warm/cache", &rep));

    /* The pack is the one CI compares across hosts: it must verify and be for linux. */
    dpak_reader *r = NULL;
    ASSERT_EQ(dpak_open_memory(cold, n0, &(dpak_open_desc){.verify = true}, &r), DE_OK);
    EXPECT_EQ(dpak_get_header(r)->platform, (u32)DPAK_PLATFORM_LINUX);
    EXPECT_EQ(dpak_entry_count(r), 9u); /* 7 assets and 2 sub-assets */
    dpak_close(r);
    cd_write_outputs(cold, n0, "cct:/cold_warm/cache");
    free(cold);
    free(warm);
#endif
}

TEST(cook, identical_jobs) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    cct_fresh("jobs");
    cd_cook(CCT_PROJECT "/assets.toml", "jobs", "jobs1", "jobs/cache1", "1");
    EXPECT_TRUE(strstr(g_cct.out.p, "cache hits: 0, misses: 7") != NULL);
    cd_cook(CCT_PROJECT "/assets.toml", "jobs", "jobs8", "jobs/cache8", "8");
    EXPECT_TRUE(strstr(g_cct.out.p, "cache hits: 0, misses: 7") != NULL);
    /* A warm cook with 8 jobs on the cache that 1 job wrote. */
    cd_cook(CCT_PROJECT "/assets.toml", "jobs", "warm8", "jobs/cache1", "8");
    EXPECT_TRUE(strstr(g_cct.out.p, "cache hits: 7, misses: 0") != NULL);

    size_t n1 = 0, n8 = 0, nw = 0;
    u8 *p1 = cd_read_pack("jobs", "jobs1", &n1);
    u8 *p8 = cd_read_pack("jobs", "jobs8", &n8);
    u8 *pw = cd_read_pack("jobs", "warm8", &nw);
    cd_report rep;
    EXPECT_TRUE(cd_pack_diff("--jobs 1 vs --jobs 8", p1, n1, p8, n8, &rep));
    EXPECT_TRUE(cd_pack_diff("--jobs 1 vs warm --jobs 8", p1, n1, pw, nw, &rep));
    EXPECT_TRUE(
        cd_cache_diff("--jobs 1 vs --jobs 8 cache", "cct:/jobs/cache1", "cct:/jobs/cache8", &rep));
    EXPECT_GE(cd_cache_count("cct:/jobs/cache8"), (size_t)7);
    free(p1);
    free(p8);
    free(pw);
#endif
}

TEST(cook, identical_relocated) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    cct_fresh("relocated");
    ASSERT_EQ(de_fs_mount("cdsrc", CCT_PROJECT, 0, 0), DE_OK);
    /* Deeper than the original and with another directory name, so a path that leaks changes the
     * bytes and the lengths. */
    cd_copy_tree("cdsrc:/", "relocated/moved/to/another/place/game_project");
    ASSERT_EQ(de_fs_unmount("cdsrc", CCT_PROJECT), DE_OK);

    cd_cook(CCT_PROJECT "/assets.toml", "relocated", "original", "relocated/cache", NULL);
    char manifest[DE_FS_PATH_MAX];
    cct_path(manifest, sizeof manifest,
             "relocated/moved/to/another/place/game_project/assets.toml");
    cd_cook(manifest, "relocated", "moved", NULL, NULL); /* default .decache/ in the copy */
    EXPECT_TRUE(strstr(g_cct.out.p, "cache hits: 0, misses: 7") != NULL);

    size_t na = 0, nb = 0;
    u8 *a = cd_read_pack("relocated", "original", &na);
    u8 *b = cd_read_pack("relocated", "moved", &nb);
    cd_report rep;
    EXPECT_TRUE(cd_pack_diff("original vs relocated project", a, na, b, nb, &rep));
    EXPECT_TRUE(cd_cache_diff("original vs relocated cache", "cct:/relocated/cache",
                              "cct:/relocated/moved/to/another/place/game_project/.decache", &rep));
    EXPECT_FALSE(cct_contains(b, nb, "game_project"));
    EXPECT_FALSE(cct_contains(b, nb, CCT_TMP));
    free(a);
    free(b);
#endif
}

TEST(cook, report_first_difference) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    cct_fresh("report");
    cd_cook(CCT_PROJECT "/assets.toml", "report", "base", "report/cache", "1");
    size_t n = 0;
    u8 *a = cd_read_pack("report", "base", &n);
    u8 *b = (u8 *)malloc(n);
    ASSERT_TRUE(b != NULL);

    /* A byte in the stored data of one asset: named, with its offsets. */
    dpak_reader *r = NULL;
    ASSERT_EQ(dpak_open_memory(a, n, NULL, &r), DE_OK);
    const u64 id = de_asset_id_from("data/big").hash;
    const u32 i = dpak_find(r, id);
    ASSERT_NE(i, DPAK_NOT_FOUND);
    dpak_toc_entry e;
    dpak_entry(r, i, &e);
    ASSERT_GT(e.stored_size, 100u);
    dpak_close(r);
    memcpy(b, a, n);
    b[e.data_offset + 100u] ^= 0x5Au;
    cd_report rep;
    EXPECT_FALSE(cd_pack_diff("planted data byte", a, n, b, n, &rep));
    EXPECT_EQ(rep.asset_id, id);
    EXPECT_EQ(rep.file_offset, e.data_offset + 100u);
    EXPECT_TRUE(strstr(rep.text, "\"data/big\"") != NULL);
    EXPECT_TRUE(strstr(rep.text, "stored byte 100") != NULL);

    /* A changed TOC field of another asset, plus a header change (the build ID): the header is
     * skipped, the TOC record named. */
    memcpy(b, a, n);
    const u32 k = i == 0 ? 1u : 0u;
    b[20] ^= 1u; /* in the header's build ID (bytes 16 to 31) */
    r = NULL;
    ASSERT_EQ(dpak_open_memory(a, n, NULL, &r), DE_OK);
    const u64 toc = dpak_get_header(r)->sections[DPAK_SECTION_TOC].offset;
    dpak_toc_entry ek;
    dpak_entry(r, k, &ek);
    dpak_close(r);
    b[toc + (u64)k * DPAK_TOC_ENTRY_SIZE + 8u] ^= 1u; /* its type hash */
    EXPECT_FALSE(cd_pack_diff("planted TOC byte", a, n, b, n, &rep));
    EXPECT_EQ(rep.asset_id, ek.id);
    EXPECT_EQ(rep.file_offset, toc + (u64)k * DPAK_TOC_ENTRY_SIZE + 8u);
    EXPECT_TRUE(strstr(rep.text, "section TOC") != NULL);

    /* A pack one byte longer: the end of the shorter one. */
    u8 *c = (u8 *)malloc(n + 1u);
    ASSERT_TRUE(c != NULL);
    memcpy(c, a, n);
    c[n] = 0;
    EXPECT_FALSE(cd_pack_diff("planted extra byte", a, n, c, n + 1u, &rep));
    EXPECT_EQ(rep.file_offset, (u64)n);
    free(c);

    /* Caches: a changed byte in one entry, then an entry only one cache has. */
    cct_fresh("report_cache");
    cd_copy_tree("cct:/report/cache", "report_cache");
    cd_list l = cd_cache_entries("cct:/report_cache");
    ASSERT_GE(l.count, (size_t)7);
    u64 sz = 0;
    u8 *ent = cd_read_v("cct:/report_cache", l.items[2], &sz);
    ASSERT_GT(sz, 40u);
    ent[40] ^= 1u;
    char rel[256];
    snprintf(rel, sizeof rel, "report_cache/%s", l.items[2]);
    cct_put(rel, ent, (size_t)sz);
    cd_free_v(ent, sz);
    EXPECT_FALSE(
        cd_cache_diff("planted entry byte", "cct:/report/cache", "cct:/report_cache", &rep));
    EXPECT_EQ(rep.file_offset, 40u);
    EXPECT_TRUE(strstr(rep.text, l.items[2]) != NULL);
    snprintf(rel, sizeof rel, "cct:/report_cache/%s", l.items[2]);
    ASSERT_EQ(de_file_delete(rel), DE_OK);
    EXPECT_FALSE(cd_cache_diff("missing entry", "cct:/report/cache", "cct:/report_cache", &rep));
    EXPECT_TRUE(strstr(rep.text, "is only in cct:/report/cache") != NULL);
    cd_list_free(&l);
    free(a);
    free(b);
#endif
}
