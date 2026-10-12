/*
 * pack_test_util.h: shared helpers of the pack tests (test_dpak_reader.c, test_mount.c;
 * DENG-212): deterministic test bytes and a pack builder over the tools-only writer
 * (tools/pack/lib, linked into the tests only, never into de_asset).
 */
#ifndef DE_PACK_TEST_UTIL_H
#define DE_PACK_TEST_UTIL_H

#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "asset/dpak/dpak_format.h"
#include "dpak_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* xorshift64*: the same bytes on every host. */
static inline u64 pt_rng(u64 *state) {
    u64 x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

/** Kinds of test content. */
typedef enum pt_kind {
    PT_TEXT,  /**< compressible: repeated words with a counter */
    PT_NOISE, /**< incompressible: stored as raw chunks */
    PT_RECORD /**< short records that share most bytes (dictionary training) */
} pt_kind;

/** `size` bytes of `kind`, from `seed` (malloc; free with free). Never NULL. */
static inline u8 *pt_make_bytes(pt_kind kind, u64 seed, size_t size) {
    u8 *p = (u8 *)malloc(size != 0 ? size : 1u);
    if (p == NULL)
        abort();
    u64 st = seed * 0x9E3779B97F4A7C15ull + 1u;
    if (kind == PT_NOISE) {
        for (size_t i = 0; i < size; ++i)
            p[i] = (u8)(pt_rng(&st) >> 56);
    } else if (kind == PT_TEXT) {
        static const char *const k_words[] = {"alpha ", "bravo ",   "charlie ", "delta ",
                                              "echo ",  "foxtrot ", "golf ",    "hotel "};
        size_t at = 0;
        u32 n = 0;
        while (at < size) {
            char word[32];
            int w = snprintf(word, sizeof word, "%s%u\n", k_words[pt_rng(&st) % 8u], n++ % 97u);
            for (int k = 0; k < w && at < size; ++k)
                p[at++] = (u8)word[k];
        }
    } else {
        /* JSON-like enemy records: the same keys and shape, different values. Alone, one record
         * barely compresses; with a dictionary trained on its siblings it does. */
        static const char *const k_factions[] = {"bandits", "guards", "wildlife", "undead"};
        size_t at = 0;
        while (at < size) {
            char rec[640];
            int w = snprintf(
                rec, sizeof rec,
                "{\"name\":\"enemy_%04u\",\"health\":%u,\"speed\":%u.%02u,\"faction\":\"%s\","
                "\"loot_table\":\"loot/common_%u\",\"ai\":{\"behaviour\":\"patrol\","
                "\"aggro_radius\":%u,\"leash_radius\":%u,\"flee_below_health\":0.%02u},"
                "\"model\":\"characters/enemy_%u/model\",\"material_overrides\":[],"
                "\"spawn\":{\"weight\":%u,\"min_level\":%u,\"max_level\":%u}}",
                (unsigned)(pt_rng(&st) % 10000u), (unsigned)(pt_rng(&st) % 500u),
                (unsigned)(pt_rng(&st) % 10u), (unsigned)(pt_rng(&st) % 100u),
                k_factions[pt_rng(&st) % 4u], (unsigned)(pt_rng(&st) % 9u),
                (unsigned)(pt_rng(&st) % 40u), (unsigned)(pt_rng(&st) % 90u),
                (unsigned)(pt_rng(&st) % 100u), (unsigned)(pt_rng(&st) % 30u),
                (unsigned)(pt_rng(&st) % 20u), (unsigned)(pt_rng(&st) % 10u),
                (unsigned)(pt_rng(&st) % 60u));
            for (int k = 0; k < w && at < size; ++k)
                p[at++] = (u8)rec[k];
        }
    }
    return p;
}

/** One asset of a test pack. */
typedef struct pt_asset {
    char path[64];
    const char *type;
    u8 *data;
    size_t size;
    u64 deps[4];
    u32 dep_count;
} pt_asset;

static inline void pt_asset_free(pt_asset *a, u32 n) {
    for (u32 i = 0; i < n; ++i) {
        free(a[i].data);
        a[i].data = NULL;
    }
}

/** A writer with the assets added (bundles may be added before finish). */
static inline dpak_writer *pt_writer(const dpak_writer_desc *wd, const pt_asset *a, u32 n) {
    dpak_writer *w = NULL;
    if (dpak_writer_create(wd, &w) != DE_OK)
        abort();
    for (u32 i = 0; i < n; ++i)
        if (dpak_writer_add(w, de_asset_id_from(a[i].path).hash, a[i].type, a[i].data, a[i].size,
                            a[i].dep_count != 0 ? a[i].deps : NULL, a[i].dep_count,
                            a[i].path) != DE_OK)
            abort();
    return w;
}

/** The pack of `a` in memory (heap; free with DE_FREE(de_heap_allocator(), data, size)). */
static inline void pt_pack_memory(const dpak_writer_desc *wd, const pt_asset *a, u32 n, void **data,
                                  u64 *size) {
    dpak_writer *w = pt_writer(wd, a, n);
    if (dpak_writer_finish_memory(w, NULL, data, size) != DE_OK)
        abort();
    dpak_writer_destroy(w);
}

/** Writes the pack of `a` to `vpath` (a writable mount). */
static inline void pt_pack_file(const dpak_writer_desc *wd, const pt_asset *a, u32 n,
                                const char *vpath) {
    dpak_writer *w = pt_writer(wd, a, n);
    if (dpak_writer_finish(w, vpath) != DE_OK)
        abort();
    dpak_writer_destroy(w);
}

/**
 * The 100-asset set of the roundtrip tests: every 10th asset is multi-chunk text (130-330 KiB),
 * every 10th + 1 incompressible noise (70 KiB, raw chunks), every 10th + 2 empty, the rest short
 * `rec` records (300-500 bytes) that share most of their shape (a dictionary is trained for them).
 * Some assets depend on others. `a` has room for 100.
 */
static inline void pt_make_100(pt_asset *a) {
    for (u32 i = 0; i < 100; ++i) {
        pt_asset *x = &a[i];
        memset(x, 0, sizeof *x);
        switch (i % 10u) {
        case 0:
            snprintf(x->path, sizeof x->path, "big/text_%02u", i);
            x->type = "pk_big";
            x->size = (size_t)(130u + i * 2u) * 1024u + i;
            x->data = pt_make_bytes(PT_TEXT, i, x->size);
            break;
        case 1:
            snprintf(x->path, sizeof x->path, "noise/n_%02u", i);
            x->type = "pk_noise";
            x->size = 70u * 1024u + i;
            x->data = pt_make_bytes(PT_NOISE, i, x->size);
            break;
        case 2:
            snprintf(x->path, sizeof x->path, "empty/e_%02u", i);
            x->type = "pk_big";
            x->size = 0;
            x->data = pt_make_bytes(PT_TEXT, i, 0);
            break;
        default:
            snprintf(x->path, sizeof x->path, "rec/r_%02u#part", i);
            x->type = "pk_rec";
            x->size = 300u + i * 2u;
            x->data = pt_make_bytes(PT_RECORD, i, x->size);
            break;
        }
    }
    /* A few dependencies inside the pack (no cycles: always on a lower index). */
    for (u32 i = 20; i < 100; i += 7) {
        a[i].deps[a[i].dep_count++] = de_asset_id_from(a[i - 17].path).hash;
        a[i].deps[a[i].dep_count++] = de_asset_id_from(a[i - 5].path).hash;
    }
}

#endif /* DE_PACK_TEST_UTIL_H */
