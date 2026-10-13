/*
 * make_dpak_corpus.c: the seed corpus of fuzz_dpak_reader (DENG-216), corpus/dpak_reader/.
 *
 * Each seed is a small pack written by the DENG-211 writer (tools/pack/lib), so the fuzzer starts
 * from packs that pass every reader check and mutates from there. The seeds cover what the reader
 * has distinct code for: raw chunks, zstd chunks, dictionary chunks, empty assets, sub-asset names,
 * dependencies, bundles (depends, resident, every priority, a member from another pack), stripped
 * names, an empty pack, and the default 4096-byte alignment with full 64 KiB chunks. The other
 * seeds use 1 KiB chunks and small alignments, so they stay small and still have several chunks
 * per asset. Every seed is format version 2 (chunk and dictionary hashes, DENG-392), except the
 * v1_* seeds: version 1 packs, which readers still accept. The writer writes version 1 byte for
 * byte as the version 1 writer did, so v1_zstd_chunks.dpak and v1_dictionary.dpak are the
 * zstd_chunks.dpak and dictionary.dpak seeds of DENG-216.
 *
 * CTest dpak_corpus.seeds checks that every seed file exists, opens with the content-hash check
 * and decodes, and has the feature it is there for, so a format change that turns the seeds into
 * garbage the reader stops at the header fails here and not silently in the fuzzer. To write the
 * seeds again (after a format change): DE_DPAK_CORPUS_UPDATE=1 ctest -R dpak_corpus.seeds.
 */
#define _CRT_SECURE_NO_WARNINGS /* fopen */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "asset/dpak/dpak_format.h"
#include "asset/dpak/dpak_reader.h"
#include "de_test.h"
#include "dpak_writer.h"

/* xorshift64*: the same bytes on every host. */
static u64 mc_rng(u64 *s) {
    u64 x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

typedef enum mc_kind { MC_TEXT, MC_NOISE, MC_RECORD } mc_kind;

/* `size` bytes of `kind` from `seed` into `out` (at least `size` bytes). */
static void mc_bytes(mc_kind kind, u64 seed, u8 *out, size_t size) {
    u64 st = seed * 0x9E3779B97F4A7C15ull + 1u;
    size_t at = 0;
    if (kind == MC_NOISE) {
        for (; at < size; ++at)
            out[at] = (u8)(mc_rng(&st) >> 56);
        return;
    }
    static const char *const k_words[] = {"alpha ", "bravo ", "charlie ", "delta "};
    static const char *const k_factions[] = {"bandits", "guards", "wildlife", "undead"};
    while (at < size) {
        char buf[512];
        int w;
        if (kind == MC_TEXT)
            w = snprintf(buf, sizeof buf, "%s%u\n", k_words[mc_rng(&st) % 4u],
                         (unsigned)(mc_rng(&st) % 97u));
        else
            w = snprintf(buf, sizeof buf,
                         "{\"name\":\"enemy_%04u\",\"health\":%u,\"faction\":\"%s\","
                         "\"ai\":{\"behaviour\":\"patrol\",\"aggro_radius\":%u},"
                         "\"model\":\"characters/enemy_%u/model\",\"spawn\":{\"weight\":%u}}\n",
                         (unsigned)(mc_rng(&st) % 10000u), (unsigned)(mc_rng(&st) % 500u),
                         k_factions[mc_rng(&st) % 4u], (unsigned)(mc_rng(&st) % 40u),
                         (unsigned)(mc_rng(&st) % 30u), (unsigned)(mc_rng(&st) % 20u));
        for (int k = 0; k < w && at < size; ++k)
            out[at++] = (u8)buf[k];
    }
}

/* Adds asset `name` of `type` with `size` bytes of `kind` and the given dependencies. */
static void mc_add(dpak_writer *w, const char *name, const char *type, mc_kind kind, u64 seed,
                   size_t size, const char *const *deps, u32 dep_count) {
    u8 *data = (u8 *)malloc(size != 0 ? size : 1u);
    ASSERT_TRUE(data != NULL);
    mc_bytes(kind, seed, data, size);
    u64 ids[8];
    ASSERT_LE(dep_count, 8u);
    for (u32 i = 0; i < dep_count; ++i)
        ids[i] = de_hash64_str(deps[i]);
    ASSERT_EQ(dpak_writer_add(w, de_hash64_str(name), type, data, size, ids, dep_count, name),
              DE_OK);
    free(data);
}

/* ---- The seeds ----------------------------------------------------------------------------- */

typedef enum mc_feature {
    MC_HAS_RAW = 1u << 0,    /* a DPAK_CHUNK_RAW chunk */
    MC_HAS_ZSTD = 1u << 1,   /* a zstd chunk without a dictionary */
    MC_HAS_DICT = 1u << 2,   /* a chunk compressed with a dictionary */
    MC_HAS_NAMES = 1u << 3,  /* the NAMES section */
    MC_NO_NAMES = 1u << 4,   /* stripped */
    MC_HAS_BUNDLE = 1u << 5, /* a bundle with a depend */
    MC_HAS_DEPS = 1u << 6,   /* a dependency */
    MC_EMPTY = 1u << 7,      /* no entries and no bundles */
    MC_MULTI = 1u << 8,      /* an asset of several chunks */
    MC_V2 = 1u << 9,         /* format version 2: chunk and dictionary hashes */
    MC_V1 = 1u << 10,        /* format version 1: no hashes */
} mc_feature;

typedef struct mc_seed {
    const char *file;
    dpak_writer_desc desc;
    void (*fill)(dpak_writer *w);
    u32 features;
} mc_seed;

static void fill_raw(dpak_writer *w) {
    mc_add(w, "textures/noise", "texture", MC_NOISE, 1, 3000, NULL, 0);
    mc_add(w, "textures/tiny", "texture", MC_NOISE, 2, 40, NULL, 0);
}

static void fill_zstd(dpak_writer *w) {
    static const char *const deps[] = {"data/strings", "models/crate", "other/pack/asset"};
    mc_add(w, "data/strings", "blob", MC_TEXT, 3, 5000, NULL, 0);
    mc_add(w, "data/empty", "blob", MC_TEXT, 4, 0, NULL, 0);
    mc_add(w, "models/crate", "model", MC_TEXT, 5, 1500, deps, 1);
    mc_add(w, "models/crate#Body", "mesh", MC_TEXT, 6, 300, deps, 2);
    mc_add(w, "levels/one", "scene", MC_TEXT, 7, 2100, deps, 3);
}

static void fill_dict(dpak_writer *w) {
    /* Enough similar records of one type for a dictionary that pays for itself. */
    for (u32 i = 0; i < 64u; ++i) {
        char name[64];
        snprintf(name, sizeof name, "enemies/%02u", i);
        mc_add(w, name, "config", MC_RECORD, 100u + i, 600, NULL, 0);
    }
    mc_add(w, "enemies/readme", "blob", MC_TEXT, 8, 200, NULL, 0);
}

static void fill_bundles(dpak_writer *w) {
    static const char *const core_dep[] = {"core"};
    static const char *const lvl_deps[] = {"core", "ui"};
    mc_add(w, "core/font", "font", MC_TEXT, 9, 700, NULL, 0);
    mc_add(w, "core/cursor", "texture", MC_NOISE, 10, 120, NULL, 0);
    mc_add(w, "ui/menu", "scene", MC_TEXT, 11, 400, NULL, 0);
    mc_add(w, "levels/two", "scene", MC_TEXT, 12, 1200, NULL, 0);
    const u64 core[] = {de_hash64_str("core/font"), de_hash64_str("core/cursor")};
    const u64 ui[] = {de_hash64_str("ui/menu")};
    /* The last member lives in another pack (a patch pack's bundle may name it). */
    const u64 lvl[] = {de_hash64_str("levels/two"), de_hash64_str("levels/two/extra")};
    ASSERT_EQ(dpak_writer_add_bundle(w, "core", core, 2, NULL, 0, true, DPAK_PRIORITY_CRITICAL),
              DE_OK);
    ASSERT_EQ(dpak_writer_add_bundle(w, "ui", ui, 1, core_dep, 1, false, DPAK_PRIORITY_HIGH),
              DE_OK);
    ASSERT_EQ(
        dpak_writer_add_bundle(w, "level_02", lvl, 2, lvl_deps, 2, false, DPAK_PRIORITY_NORMAL),
        DE_OK);
    ASSERT_EQ(dpak_writer_add_bundle(w, "music", NULL, 0, NULL, 0, false, DPAK_PRIORITY_LOW),
              DE_OK);
    ASSERT_EQ(
        dpak_writer_add_bundle(w, "prefetch", ui, 1, NULL, 0, false, DPAK_PRIORITY_BACKGROUND),
        DE_OK);
}

static void fill_stripped(dpak_writer *w) {
    static const char *const deps[] = {"a/one"};
    mc_add(w, "a/one", "blob", MC_TEXT, 13, 2500, NULL, 0);
    mc_add(w, "a/two", "blob", MC_NOISE, 14, 1100, deps, 1);
    mc_add(w, "a/three#Part", "mesh", MC_TEXT, 15, 90, deps, 1);
    const u64 ids[] = {de_hash64_str("a/one"), de_hash64_str("a/two")};
    static const char *const base[] = {"base"};
    ASSERT_EQ(dpak_writer_add_bundle(w, "base", ids, 1, NULL, 0, false, DPAK_PRIORITY_NORMAL),
              DE_OK);
    ASSERT_EQ(dpak_writer_add_bundle(w, "extra", ids + 1, 1, base, 1, true, DPAK_PRIORITY_LOW),
              DE_OK);
}

static void fill_empty(dpak_writer *w) {
    (void)w;
}

static void fill_aligned(dpak_writer *w) {
    static const char *const deps[] = {"big/text"};
    mc_add(w, "big/text", "blob", MC_TEXT, 16, 70000, NULL, 0);
    mc_add(w, "big/small", "blob", MC_TEXT, 17, 64, deps, 1);
}

#define MC_SMALL(...) {.chunk_size = DPAK_MIN_CHUNK_SIZE, __VA_ARGS__}

static const mc_seed k_seeds[] = {
    {"raw_chunks.dpak",
     MC_SMALL(.platform = DPAK_PLATFORM_LINUX, .alignment = 8, .zstd_level = 3,
              .include_names = true),
     fill_raw, MC_HAS_RAW | MC_HAS_NAMES | MC_MULTI | MC_V2},
    {"zstd_chunks.dpak",
     MC_SMALL(.platform = DPAK_PLATFORM_ANY, .alignment = 16, .zstd_level = 3,
              .include_names = true),
     fill_zstd, MC_HAS_ZSTD | MC_HAS_NAMES | MC_HAS_DEPS | MC_MULTI | MC_V2},
    {"dictionary.dpak",
     MC_SMALL(.platform = DPAK_PLATFORM_MACOS, .alignment = 8, .include_names = true,
              .use_dictionaries = true, .zstd_level = 19),
     fill_dict, MC_HAS_DICT | MC_HAS_ZSTD | MC_HAS_NAMES | MC_V2},
    {"bundles.dpak",
     MC_SMALL(.platform = DPAK_PLATFORM_WINDOWS, .alignment = 64, .zstd_level = 3,
              .include_names = true),
     fill_bundles, MC_HAS_BUNDLE | MC_HAS_RAW | MC_HAS_ZSTD | MC_HAS_NAMES | MC_V2},
    {"stripped.dpak", MC_SMALL(.platform = DPAK_PLATFORM_LINUX, .alignment = 8, .zstd_level = 3),
     fill_stripped, MC_NO_NAMES | MC_HAS_BUNDLE | MC_HAS_DEPS | MC_HAS_RAW | MC_MULTI | MC_V2},
    {"empty.dpak", MC_SMALL(.platform = DPAK_PLATFORM_ANY, .zstd_level = 3), fill_empty,
     MC_EMPTY | MC_NO_NAMES | MC_V2},
    {"aligned_4096.dpak",
     {.platform = DPAK_PLATFORM_ANY, .zstd_level = 3, .include_names = true},
     fill_aligned,
     MC_HAS_ZSTD | MC_HAS_NAMES | MC_HAS_DEPS | MC_MULTI | MC_V2},
    /* Format version 1 (DENG-392): the version 1 layout must keep loading. */
    {"v1_zstd_chunks.dpak",
     MC_SMALL(.platform = DPAK_PLATFORM_ANY, .alignment = 16, .zstd_level = 3,
              .include_names = true, .format_version = 1),
     fill_zstd, MC_HAS_ZSTD | MC_HAS_NAMES | MC_HAS_DEPS | MC_MULTI | MC_V1},
    {"v1_dictionary.dpak",
     MC_SMALL(.platform = DPAK_PLATFORM_MACOS, .alignment = 8, .include_names = true,
              .use_dictionaries = true, .zstd_level = 19, .format_version = 1),
     fill_dict, MC_HAS_DICT | MC_HAS_ZSTD | MC_HAS_NAMES | MC_V1},
};

static void mc_build(const mc_seed *s, void **data, u64 *size) {
    dpak_writer *w = NULL;
    ASSERT_EQ(dpak_writer_create(&s->desc, &w), DE_OK);
    s->fill(w);
    de_result r = dpak_writer_finish_memory(w, NULL, data, size);
    dpak_writer_destroy(w);
    ASSERT_EQ(r, DE_OK);
}

static u8 *mc_read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *p = (u8 *)malloc(n > 0 ? (size_t)n : 1u);
    if (p != NULL && n > 0 && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        p = NULL;
    }
    fclose(f);
    *size = n > 0 ? (size_t)n : 0u;
    return p;
}

/* Opens the seed file with every check and decodes every entry; returns its features. */
static u32 mc_check(const char *path) {
    size_t n = 0;
    u8 *bytes = mc_read_file(path, &n);
    if (bytes == NULL) {
        FAIL("cannot read a seed (write the corpus with DE_DPAK_CORPUS_UPDATE=1)");
    }
    dpak_reader *r = NULL;
    de_result res =
        dpak_open_memory(bytes, n, &(dpak_open_desc){.verify = true, .check_chunks = true}, &r);
    if (res != DE_OK) {
        free(bytes);
        FAIL("a seed does not open (write the corpus again with DE_DPAK_CORPUS_UPDATE=1)");
    }
    const dpak_header *h = dpak_get_header(r);
    u32 got = 0;
    got |= (h->flags & DPAK_HEADER_HAS_NAMES) != 0 ? MC_HAS_NAMES : MC_NO_NAMES;
    got |= dpak_has_hashes(r) ? MC_V2 : MC_V1;
    for (u32 i = 0; i < dpak_dict_count(r); ++i)
        EXPECT_EQ(dpak_check_dict(r, i), DE_OK);
    if (dpak_entry_count(r) == 0 && dpak_bundle_count(r) == 0)
        got |= MC_EMPTY;
    for (u32 i = 0; i < dpak_bundle_count(r); ++i) {
        dpak_bundle_entry b;
        dpak_bundle(r, i, &b);
        if (b.depend_count != 0)
            got |= MC_HAS_BUNDLE;
    }
    for (u32 i = 0; i < dpak_entry_count(r); ++i) {
        dpak_toc_entry e;
        dpak_entry(r, i, &e);
        if (e.dep_count != 0)
            got |= MC_HAS_DEPS;
        if (e.chunk_count > 1)
            got |= MC_MULTI;
        for (u32 k = 0; k < e.chunk_count; ++k) {
            dpak_chunk_entry c;
            dpak_chunk(r, e.first_chunk + k, &c);
            EXPECT_EQ(dpak_check_chunk(r, &c, bytes + c.file_offset), DE_OK);
            if ((c.flags & DPAK_CHUNK_RAW) != 0)
                got |= MC_HAS_RAW;
            else if (c.dict_index != DPAK_DICT_NONE)
                got |= MC_HAS_DICT;
            else
                got |= MC_HAS_ZSTD;
        }
        u8 *raw = (u8 *)malloc(e.raw_size != 0 ? (size_t)e.raw_size : 1u);
        ASSERT_TRUE(raw != NULL);
        EXPECT_EQ(dpak_read_entry(r, i, NULL, raw, e.raw_size), DE_OK);
        free(raw);
    }
    dpak_close(r);
    free(bytes);
    return got;
}

TEST(dpak_corpus, seeds) {
    const char *update = getenv("DE_DPAK_CORPUS_UPDATE");
    const bool write = update != NULL && strcmp(update, "1") == 0;
    for (size_t i = 0; i < sizeof k_seeds / sizeof k_seeds[0]; ++i) {
        const mc_seed *s = &k_seeds[i];
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", DE_DPAK_CORPUS_DIR, s->file);
        if (write) {
            void *data = NULL;
            u64 size = 0;
            mc_build(s, &data, &size);
            FILE *f = fopen(path, "wb");
            ASSERT_TRUE(f != NULL);
            ASSERT_EQ(fwrite(data, 1, (size_t)size, f), (size_t)size);
            fclose(f);
            DE_FREE(de_heap_allocator(), data, (size_t)size);
            printf("  wrote %s (%llu bytes)\n", path, (unsigned long long)size);
        }
        const u32 got = mc_check(path);
        if ((got & s->features) != s->features) {
            printf("  %s: features 0x%x, expected 0x%x\n", s->file, got, s->features);
            FAIL("a seed lacks the feature it is there for");
        }
    }
}

/* ---- Regression inputs ------------------------------------------------------------------------
 * Inputs that fuzz_dpak_reader found, kept in the corpus as regress_*.dpak. The corpus replay
 * finds a crash only in a sanitizer build; this test also checks the result in every build. */

typedef struct mc_regression {
    const char *file;
    de_result expect; /* of dpak_open_memory, no verify, any platform */
    const char *what;
} mc_regression;

static const mc_regression k_regressions[] = {
    /* DENG-216: an empty DICTS section with offset 2^64 - 4. The reader checked only the size of
     * an empty section, and section_ptr() then formed a pointer far outside the buffer (UBSan
     * pointer-overflow in check_dicts). Empty sections must have offset 0. */
    {"regress_empty_section_offset.dpak", DE_ERR_PARSE, "empty section with an offset"},
};

TEST(dpak_corpus, regressions) {
    for (size_t i = 0; i < sizeof k_regressions / sizeof k_regressions[0]; ++i) {
        const mc_regression *g = &k_regressions[i];
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", DE_DPAK_CORPUS_DIR, g->file);
        size_t n = 0;
        u8 *bytes = mc_read_file(path, &n);
        ASSERT_TRUE(bytes != NULL);
        dpak_reader *r = NULL;
        const de_result got = dpak_open_memory(bytes, n, &(dpak_open_desc){0}, &r);
        if (got != g->expect)
            printf("  %s (%s): got %s\n", g->file, g->what, de_result_str(got));
        EXPECT_EQ(got, g->expect);
        dpak_close(r);
        free(bytes);
    }
}
