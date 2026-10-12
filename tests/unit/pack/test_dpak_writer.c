/*
 * Unit tests of the .dpak writer (tools/pack/lib/dpak_writer.c, DENG-211).
 *
 * The tests read the packs back with the shared format header (src/asset/dpak/dpak_format.h) and
 * a small reader below, and decompress every chunk with zstd to check that the bytes round-trip.
 * The runtime reader is DENG-212; this one only exists to check the writer's output.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#include <dengine/de_platform.h>

#include "asset/dpak/dpak_format.h"
#include "dpak_writer.h"
#include "platform/vfs_internal.h"

#include "de_test.h"

#ifndef DE_PACK_TEST_TMP_DIR
#error "DE_PACK_TEST_TMP_DIR must be defined by tests/unit/pack/CMakeLists.txt"
#endif

/* ---- Test data ---------------------------------------------------------------------------- */

static u64 rng_next(u64 *s) {
    /* xorshift64*: deterministic test data on every platform. */
    u64 x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 2685821657736338717ull;
}

static void fill_random(u8 *p, size_t n, u64 seed) {
    u64 s = seed ? seed : 1u;
    for (size_t i = 0; i < n; ++i)
        p[i] = (u8)(rng_next(&s) >> 56);
}

/* Text-like data that compresses well: words from a small vocabulary. */
static void fill_text(u8 *p, size_t n, u64 seed) {
    static const char *const words[] = {"mesh ",   "vertex ", "index ",     "material ",
                                        "albedo ", "normal ", "roughness ", "metallic ",
                                        "lod ",    "bone "};
    u64 s = seed ? seed : 1u;
    size_t at = 0;
    while (at < n) {
        const char *wd = words[rng_next(&s) % (sizeof words / sizeof words[0])];
        for (size_t k = 0; wd[k] != '\0' && at < n; ++k)
            p[at++] = (u8)wd[k];
    }
}

typedef struct test_entry {
    const char *name;
    const char *type;
    size_t size;
    int kind; /* 0 text, 1 random, 2 zeros */
    const char *deps[3];
} test_entry;

/* A mixed set: empty, tiny, exactly one chunk, multi-chunk with a partial tail, sub-assets,
 * incompressible data, and dependencies given out of order with a duplicate. */
static const test_entry k_set[] = {
    {"player/model", "model", 3000, 0, {"player/albedo", "player/model#Body", "player/albedo"}},
    {"player/model#Body", "mesh", 200000, 0, {"player/albedo", NULL, NULL}},
    {"player/albedo", "texture", 65536, 1, {NULL, NULL, NULL}},
    {"ui/font", "font", 70000, 0, {NULL, NULL, NULL}},
    {"ui/empty", "blob", 0, 0, {NULL, NULL, NULL}},
    {"ui/tiny", "blob", 7, 1, {NULL, NULL, NULL}},
    {"level01/terrain", "texture", 300000, 2, {"ui/font", NULL, NULL}},
    {"music/title", "sound", 131072, 1, {NULL, NULL, NULL}},
};
enum { k_set_count = sizeof k_set / sizeof k_set[0] };

static u8 *make_data(const test_entry *e, u64 seed) {
    u8 *p = malloc(e->size ? e->size : 1u);
    if (p == NULL)
        return NULL;
    if (e->kind == 0)
        fill_text(p, e->size, seed);
    else if (e->kind == 1)
        fill_random(p, e->size, seed);
    else
        memset(p, 0, e->size);
    return p;
}

static u32 dep_ids(const test_entry *e, u64 *out, int reverse) {
    u32 n = 0;
    for (u32 k = 0; k < 3; ++k)
        if (e->deps[k] != NULL)
            out[n++] = de_hash64_str(e->deps[k]);
    if (reverse)
        for (u32 k = 0; k < n / 2; ++k) {
            const u64 t = out[k];
            out[k] = out[n - 1 - k];
            out[n - 1 - k] = t;
        }
    return n;
}

/* Adds k_set (forwards or backwards) and two bundles. Each entry's data depends only on its
 * index, so both orders describe the same set. */
static void add_set(dpak_writer *w, int reverse) {
    for (u32 j = 0; j < k_set_count; ++j) {
        const u32 i = reverse ? (u32)k_set_count - 1u - j : j;
        const test_entry *e = &k_set[i];
        u8 *data = make_data(e, 1000u + i);
        ASSERT_TRUE(data != NULL);
        u64 deps[3];
        const u32 nd = dep_ids(e, deps, reverse);
        const de_result r =
            dpak_writer_add(w, de_hash64_str(e->name), e->type, data, e->size, deps, nd, e->name);
        free(data);
        ASSERT_EQ(r, DE_OK);
    }
    u64 common[] = {de_hash64_str("ui/font"), de_hash64_str("ui/tiny"), de_hash64_str("ui/empty")};
    u64 level[] = {de_hash64_str("player/model"), de_hash64_str("level01/terrain"),
                   de_hash64_str("player/model")};
    if (reverse) {
        const u64 t = common[0];
        common[0] = common[2];
        common[2] = t;
    }
    const char *const level_deps[] = {"common", "music"};
    const char *const level_deps_rev[] = {"music", "common"};
    if (!reverse) {
        ASSERT_EQ(
            dpak_writer_add_bundle(w, "common", common, 3, NULL, 0, true, DPAK_PRIORITY_NORMAL),
            DE_OK);
        ASSERT_EQ(dpak_writer_add_bundle(w, "level_01", level, 3, level_deps, 2, false,
                                         DPAK_PRIORITY_HIGH),
                  DE_OK);
    } else {
        ASSERT_EQ(dpak_writer_add_bundle(w, "level_01", level, 3, level_deps_rev, 2, false,
                                         DPAK_PRIORITY_HIGH),
                  DE_OK);
        ASSERT_EQ(
            dpak_writer_add_bundle(w, "common", common, 3, NULL, 0, true, DPAK_PRIORITY_NORMAL),
            DE_OK);
    }
}

typedef struct packbuf {
    u8 *data;
    u64 size;
} packbuf;

static packbuf build(const dpak_writer_desc *d, int reverse) {
    dpak_writer *w = NULL;
    packbuf out = {0};
    ASSERT_EQ(dpak_writer_create(d, &w), DE_OK);
    add_set(w, reverse);
    void *data = NULL;
    const de_result r = dpak_writer_finish_memory(w, NULL, &data, &out.size);
    dpak_writer_destroy(w);
    ASSERT_EQ(r, DE_OK);
    out.data = data;
    return out;
}

static void packbuf_free(packbuf *p) {
    if (p->data != NULL)
        DE_FREE(de_heap_allocator(), p->data, (size_t)p->size);
    *p = (packbuf){0};
}

/* ---- A minimal checking reader ----------------------------------------------------------------
 */

typedef struct pak {
    const u8 *p;
    u64 size;
    dpak_header h;
} pak;

static void pak_open(pak *k, const packbuf *b) {
    k->p = b->data;
    k->size = b->size;
    ASSERT_TRUE(b->size >= DPAK_HEADER_SIZE);
    ASSERT_TRUE(dpak_magic_ok(b->data));
    dpak_header_load(b->data, &k->h);
    ASSERT_EQ(k->h.version, DPAK_VERSION);
    ASSERT_EQ(k->h.header_size, DPAK_HEADER_SIZE);
    ASSERT_EQ(k->h.file_size, b->size);
    ASSERT_EQ(k->h.reserved, 0u);
    for (u32 s = 0; s < (u32)DPAK_SECTION_COUNT; ++s) {
        const dpak_section_desc *d = &k->h.sections[s];
        ASSERT_EQ(d->reserved, 0u);
        ASSERT_EQ(d->size, (u64)d->count * dpak_section_record_size((dpak_section)s));
        if (d->size == 0) {
            EXPECT_EQ(d->offset, 0u);
            continue;
        }
        ASSERT_EQ(d->offset % DPAK_SECTION_ALIGNMENT, 0u);
        ASSERT_TRUE(d->offset >= DPAK_HEADER_SIZE);
        ASSERT_TRUE(d->offset + d->size <= b->size);
    }
}

static const u8 *pak_rec(const pak *k, dpak_section s, u32 i) {
    DE_ASSERT(i < k->h.sections[s].count);
    return k->p + k->h.sections[s].offset + (u64)i * dpak_section_record_size(s);
}

static dpak_toc_entry pak_toc(const pak *k, u32 i) {
    dpak_toc_entry e;
    dpak_toc_entry_load(pak_rec(k, DPAK_SECTION_TOC, i), &e);
    return e;
}

static dpak_chunk_entry pak_chunk(const pak *k, u32 i) {
    dpak_chunk_entry c;
    dpak_chunk_entry_load(pak_rec(k, DPAK_SECTION_CHUNKS, i), &c);
    return c;
}

static int pak_find(const pak *k, u64 id) {
    for (u32 i = 0; i < k->h.sections[DPAK_SECTION_TOC].count; ++i)
        if (pak_toc(k, i).id == id)
            return (int)i;
    return -1;
}

/* Decompresses TOC entry `i` into a malloc'd block. */
static u8 *pak_extract(const pak *k, u32 i) {
    const dpak_toc_entry e = pak_toc(k, i);
    u8 *out = malloc(e.raw_size ? (size_t)e.raw_size : 1u);
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    if (out == NULL || dctx == NULL) {
        free(out);
        ZSTD_freeDCtx(dctx);
        return NULL;
    }
    u64 at = 0;
    bool ok = true;
    for (u32 c = 0; c < e.chunk_count && ok; ++c) {
        const dpak_chunk_entry ch = pak_chunk(k, e.first_chunk + c);
        const u8 *src = k->p + ch.file_offset;
        if (ch.flags & DPAK_CHUNK_RAW) {
            ok = ch.stored_size == ch.raw_size;
            memcpy(out + at, src, ch.raw_size);
        } else if (ch.dict_index != DPAK_DICT_NONE) {
            dpak_dict_entry d;
            dpak_dict_entry_load(pak_rec(k, DPAK_SECTION_DICTS, ch.dict_index), &d);
            ZSTD_DDict *dd = ZSTD_createDDict(k->p + d.file_offset, d.size);
            const size_t r =
                ZSTD_decompress_usingDDict(dctx, out + at, ch.raw_size, src, ch.stored_size, dd);
            ZSTD_freeDDict(dd);
            ok = !ZSTD_isError(r) && r == ch.raw_size;
        } else {
            const size_t r = ZSTD_decompressDCtx(dctx, out + at, ch.raw_size, src, ch.stored_size);
            ok = !ZSTD_isError(r) && r == ch.raw_size;
        }
        at += ch.raw_size;
    }
    ZSTD_freeDCtx(dctx);
    if (!ok || at != e.raw_size) {
        free(out);
        return NULL;
    }
    return out;
}

/* Sum of stored chunk bytes plus dictionary bytes: the compressed payload of a pack. */
static u64 pak_payload(const pak *k) {
    u64 total = 0;
    for (u32 i = 0; i < k->h.sections[DPAK_SECTION_CHUNKS].count; ++i)
        total += pak_chunk(k, i).stored_size;
    for (u32 i = 0; i < k->h.sections[DPAK_SECTION_DICTS].count; ++i) {
        dpak_dict_entry d;
        dpak_dict_entry_load(pak_rec(k, DPAK_SECTION_DICTS, i), &d);
        total += d.size;
    }
    return total;
}

/* ---- Tests ---------------------------------------------------------------------------------- */

TEST(dpak_writer, format_records) {
    /* The encoders write little-endian at fixed offsets, and decode(encode(x)) == x. */
    u8 buf[DPAK_HEADER_SIZE];
    dpak_store_u64(buf, 0x0102030405060708ull);
    const u8 le[8] = {8, 7, 6, 5, 4, 3, 2, 1};
    EXPECT_MEMEQ(buf, le, 8);
    EXPECT_EQ(dpak_load_u64(buf), 0x0102030405060708ull);
    dpak_store_u16(buf, 0xBEEF);
    EXPECT_EQ(buf[0], 0xEF);
    EXPECT_EQ(dpak_load_u16(buf), 0xBEEF);

    const dpak_toc_entry t = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0};
    dpak_toc_entry t2;
    dpak_toc_entry_store(buf, &t);
    dpak_toc_entry_load(buf, &t2);
    EXPECT_MEMEQ(&t, &t2, sizeof t);
    EXPECT_EQ(dpak_load_u32(buf + 56), 10u); /* flags at offset 56 */

    const dpak_chunk_entry c = {0x1122334455667788ull, 99, 65536, DPAK_DICT_NONE,
                                DPAK_CHUNK_RAW,        0};
    dpak_chunk_entry c2;
    dpak_chunk_entry_store(buf, &c);
    dpak_chunk_entry_load(buf, &c2);
    EXPECT_MEMEQ(&c, &c2, sizeof c);

    dpak_header h = {.magic = {'D', 'P', 'A', 'K'},
                     .version = 1,
                     .header_size = 256,
                     .content_hash = 42,
                     .file_size = 4096,
                     .chunk_size = 65536};
    h.sections[DPAK_SECTION_STRINGS] = (dpak_section_desc){256, 10, 10, 0};
    dpak_header h2;
    dpak_header_store(buf, &h);
    dpak_header_load(buf, &h2);
    EXPECT_MEMEQ(&h, &h2, sizeof h);
    EXPECT_TRUE(dpak_magic_ok(buf));
    EXPECT_EQ(dpak_load_u64(buf + 64 + 7 * DPAK_SECTION_DESC_SIZE), 256u);
}

TEST(dpak_writer, desc_validation) {
    dpak_writer *w = NULL;
    EXPECT_EQ(dpak_writer_create(NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dpak_writer_create(&(dpak_writer_desc){.chunk_size = 65537}, &w),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dpak_writer_create(&(dpak_writer_desc){.chunk_size = 512}, &w),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dpak_writer_create(&(dpak_writer_desc){.alignment = 3000}, &w),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dpak_writer_create(&(dpak_writer_desc){.alignment = 4}, &w), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dpak_writer_create(&(dpak_writer_desc){.zstd_level = 99}, &w),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dpak_writer_create(&(dpak_writer_desc){.platform = 77}, &w), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(w == NULL);

    /* A NULL desc is a zeroed desc; an empty pack is a valid pack. */
    ASSERT_EQ(dpak_writer_create(NULL, &w), DE_OK);
    const u8 x = 1;
    EXPECT_EQ(dpak_writer_add(w, 1, NULL, &x, 1, NULL, 0, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dpak_writer_add(w, 1, "", &x, 1, NULL, 0, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dpak_writer_add(w, 1, "blob", NULL, 1, NULL, 0, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dpak_writer_add(w, 1, "blob", &x, 1, NULL, 2, NULL), DE_ERR_INVALID_ARGUMENT);
    void *data = NULL;
    u64 size = 0;
    ASSERT_EQ(dpak_writer_finish_memory(w, NULL, &data, &size), DE_OK);
    dpak_writer_destroy(w);
    packbuf b = {data, size};
    pak k;
    pak_open(&k, &b);
    EXPECT_EQ(size, (u64)DPAK_HEADER_SIZE);
    EXPECT_EQ(k.h.chunk_size, DPAK_MAX_CHUNK_SIZE);
    EXPECT_EQ(k.h.alignment, DPAK_DEFAULT_ALIGNMENT);
    EXPECT_EQ(k.h.flags, 0u);
    packbuf_free(&b);
}

TEST(dpak_writer, deterministic) {
    /* Same entry set, opposite add order (and dependency, member and depend order): identical
     * bytes, with and without dictionaries and names. */
    const dpak_writer_desc descs[] = {
        {.platform = DPAK_PLATFORM_MACOS, .include_names = true, .build_id = {1, 2, 3}},
        {.platform = DPAK_PLATFORM_LINUX, .use_dictionaries = true, .zstd_level = 3},
        {.chunk_size = 16384, .alignment = 512, .include_names = true},
    };
    for (u32 d = 0; d < sizeof descs / sizeof descs[0]; ++d) {
        packbuf a = build(&descs[d], 0);
        packbuf b = build(&descs[d], 1);
        packbuf c = build(&descs[d], 0);
        ASSERT_EQ(a.size, b.size);
        EXPECT_MEMEQ(a.data, b.data, (size_t)a.size);
        ASSERT_EQ(a.size, c.size);
        EXPECT_MEMEQ(a.data, c.data, (size_t)a.size);
        packbuf_free(&a);
        packbuf_free(&b);
        packbuf_free(&c);
    }
}

TEST(dpak_writer, sorted_toc) {
    packbuf b = build(&(dpak_writer_desc){.include_names = true}, 1);
    pak k;
    pak_open(&k, &b);
    ASSERT_EQ(k.h.flags & DPAK_HEADER_HAS_NAMES, (u32)DPAK_HEADER_HAS_NAMES);
    const u32 n = k.h.sections[DPAK_SECTION_TOC].count;
    ASSERT_EQ(n, (u32)k_set_count);
    ASSERT_EQ(k.h.sections[DPAK_SECTION_NAMES].count, n);
    const char *strings = (const char *)k.p + k.h.sections[DPAK_SECTION_STRINGS].offset;
    for (u32 i = 0; i < n; ++i) {
        const dpak_toc_entry e = pak_toc(&k, i);
        if (i > 0)
            EXPECT_LT(pak_toc(&k, i - 1).id, e.id);
        EXPECT_EQ(e.reserved, 0u);
        /* The names section is parallel to the TOC and hashes back to the ID. */
        dpak_name_entry ne;
        dpak_name_entry_load(pak_rec(&k, DPAK_SECTION_NAMES, i), &ne);
        ASSERT_TRUE((u64)ne.offset + ne.len < k.h.sections[DPAK_SECTION_STRINGS].size);
        EXPECT_EQ(strings[ne.offset + ne.len], '\0');
        const char *name = strings + ne.offset;
        EXPECT_EQ(de_hash64(name, ne.len, 0), e.id);
        /* Type hash, sub-asset flag and the original bytes. */
        const int src = (int)(strchr(name, '#') != NULL);
        EXPECT_EQ((e.flags & DPAK_ENTRY_SUB_ASSET) != 0, src != 0);
        u32 s = 0;
        while (s < (u32)k_set_count && strcmp(k_set[s].name, name) != 0)
            ++s;
        ASSERT_LT(s, (u32)k_set_count);
        EXPECT_EQ(e.type_hash, de_hash64_str(k_set[s].type));
        EXPECT_EQ(e.raw_size, (u64)k_set[s].size);
        u8 *want = make_data(&k_set[s], 1000u + s);
        u8 *got = pak_extract(&k, i);
        ASSERT_TRUE(want != NULL && got != NULL);
        EXPECT_MEMEQ(got, want, k_set[s].size);
        free(want);
        free(got);
        /* Dependencies: sorted, unique, the right IDs. */
        u64 deps[3];
        const u32 nd_in = dep_ids(&k_set[s], deps, 0);
        EXPECT_LE(e.dep_count, nd_in);
        for (u32 d = 0; d < e.dep_count; ++d) {
            const u64 v = dpak_load_u64(pak_rec(&k, DPAK_SECTION_DEPS, e.dep_first + d));
            if (d > 0)
                EXPECT_LT(dpak_load_u64(pak_rec(&k, DPAK_SECTION_DEPS, e.dep_first + d - 1)), v);
            bool found = false;
            for (u32 q = 0; q < nd_in; ++q)
                found = found || deps[q] == v;
            EXPECT_TRUE(found);
        }
    }
    /* player/model lists player/albedo twice: stored once. */
    const int pm = pak_find(&k, de_hash64_str("player/model"));
    ASSERT_GE(pm, 0);
    EXPECT_EQ(pak_toc(&k, (u32)pm).dep_count, 2u);
    packbuf_free(&b);

    /* Without include_names there is no NAMES section and no asset path in the file. */
    packbuf s = build(NULL, 0);
    pak ks;
    pak_open(&ks, &s);
    EXPECT_EQ(ks.h.flags & DPAK_HEADER_HAS_NAMES, 0u);
    EXPECT_EQ(ks.h.sections[DPAK_SECTION_NAMES].count, 0u);
    const char *needle = "player/model";
    bool leaked = false;
    for (u64 i = 0; i + strlen(needle) <= s.size && !leaked; ++i)
        leaked = memcmp(s.data + i, needle, strlen(needle)) == 0;
    EXPECT_FALSE(leaked);
    packbuf_free(&s);
}

static void check_layout(const packbuf *b, u32 alignment, u32 chunk_size) {
    pak k;
    pak_open(&k, b);
    EXPECT_EQ(k.h.alignment, alignment);
    EXPECT_EQ(k.h.chunk_size, chunk_size);
    const u32 n = k.h.sections[DPAK_SECTION_TOC].count;
    u64 prev_end = 0;
    u32 next_chunk = 0;
    for (u32 i = 0; i < n; ++i) {
        const dpak_toc_entry e = pak_toc(&k, i);
        EXPECT_EQ(e.chunk_count, (u32)((e.raw_size + chunk_size - 1) / chunk_size));
        EXPECT_EQ(e.data_offset % alignment, 0u);
        if (e.chunk_count == 0) {
            EXPECT_EQ(e.data_offset, 0u);
            EXPECT_EQ(e.stored_size, 0u);
            continue;
        }
        EXPECT_EQ(e.first_chunk, next_chunk); /* chunks in TOC order */
        next_chunk += e.chunk_count;
        /* Padding before the asset is zero. */
        ASSERT_TRUE(prev_end == 0 || prev_end <= e.data_offset);
        for (u64 p = prev_end ? prev_end : e.data_offset; p < e.data_offset; ++p)
            ASSERT_EQ(k.p[p], 0u);
        u64 at = e.data_offset, raw = 0, stored = 0;
        for (u32 c = 0; c < e.chunk_count; ++c) {
            const dpak_chunk_entry ch = pak_chunk(&k, e.first_chunk + c);
            EXPECT_EQ(ch.file_offset, at); /* back to back */
            EXPECT_LE(ch.raw_size, chunk_size);
            EXPECT_LE(ch.raw_size, DPAK_MAX_CHUNK_SIZE);
            if (c + 1 < e.chunk_count)
                EXPECT_EQ(ch.raw_size, chunk_size); /* only the last chunk is partial */
            EXPECT_EQ(ch.reserved, 0u);
            at += ch.stored_size;
            raw += ch.raw_size;
            stored += ch.stored_size;
        }
        EXPECT_EQ(raw, e.raw_size);
        EXPECT_EQ(stored, e.stored_size);
        prev_end = at;
    }
    EXPECT_EQ(prev_end, k.size); /* the file ends with the last asset's data */
}

TEST(dpak_writer, alignment) {
    packbuf a = build(NULL, 0);
    check_layout(&a, DPAK_DEFAULT_ALIGNMENT, DPAK_MAX_CHUNK_SIZE);
    packbuf_free(&a);
    packbuf b = build(&(dpak_writer_desc){.alignment = 65536, .chunk_size = 4096}, 1);
    check_layout(&b, 65536, 4096);
    packbuf_free(&b);
    packbuf c = build(&(dpak_writer_desc){.alignment = 8, .use_dictionaries = true}, 0);
    check_layout(&c, 8, DPAK_MAX_CHUNK_SIZE);
    packbuf_free(&c);
}

TEST(dpak_writer, raw_fallback) {
    enum { k_random = 300000, k_zero = 1u << 20 };
    u8 *rnd = malloc(k_random);
    u8 *zero = calloc(1, k_zero);
    ASSERT_TRUE(rnd != NULL && zero != NULL);
    fill_random(rnd, k_random, 7);
    dpak_writer *w = NULL;
    ASSERT_EQ(dpak_writer_create(NULL, &w), DE_OK);
    ASSERT_EQ(dpak_writer_add(w, 10, "texture", rnd, k_random, NULL, 0, NULL), DE_OK);
    ASSERT_EQ(dpak_writer_add(w, 20, "blob", zero, k_zero, NULL, 0, NULL), DE_OK);
    void *data = NULL;
    u64 size = 0;
    ASSERT_EQ(dpak_writer_finish_memory(w, NULL, &data, &size), DE_OK);
    dpak_writer_destroy(w);
    packbuf b = {data, size};
    pak k;
    pak_open(&k, &b);

    /* Incompressible: every chunk raw, the entry marked uncompressed, the bytes as given. */
    const dpak_toc_entry r = pak_toc(&k, 0);
    ASSERT_EQ(r.id, 10u);
    EXPECT_EQ(r.flags & DPAK_ENTRY_UNCOMPRESSED, (u32)DPAK_ENTRY_UNCOMPRESSED);
    EXPECT_EQ(r.stored_size, (u64)k_random);
    for (u32 c = 0; c < r.chunk_count; ++c) {
        const dpak_chunk_entry ch = pak_chunk(&k, r.first_chunk + c);
        EXPECT_EQ(ch.flags & DPAK_CHUNK_RAW, (u32)DPAK_CHUNK_RAW);
        EXPECT_EQ(ch.stored_size, ch.raw_size);
        EXPECT_EQ(ch.dict_index, DPAK_DICT_NONE);
    }
    EXPECT_MEMEQ(k.p + r.data_offset, rnd, k_random);

    /* 1 MiB of zeros: compressed, under 1% of the raw size, and round-trips. */
    const dpak_toc_entry z = pak_toc(&k, 1);
    ASSERT_EQ(z.id, 20u);
    EXPECT_EQ(z.flags & DPAK_ENTRY_UNCOMPRESSED, 0u);
    EXPECT_LT(z.stored_size * 100u, (u64)k_zero);
    for (u32 c = 0; c < z.chunk_count; ++c)
        EXPECT_EQ(pak_chunk(&k, z.first_chunk + c).flags & DPAK_CHUNK_RAW, 0u);
    u8 *back = pak_extract(&k, 1);
    ASSERT_TRUE(back != NULL);
    EXPECT_MEMEQ(back, zero, k_zero);
    free(back);

    packbuf_free(&b);
    free(rnd);
    free(zero);
}

/* Many small, similar records of one type: what per-type dictionaries are for. */
static packbuf build_records(bool dicts, u32 records, u32 rare, bool reverse) {
    dpak_writer *w = NULL;
    packbuf out = {0};
    ASSERT_EQ(
        dpak_writer_create(&(dpak_writer_desc){.use_dictionaries = dicts, .zstd_level = 19}, &w),
        DE_OK);
    static const char *const factions[] = {"bandits", "guards", "wildlife", "undead"};
    char buf[1024];
    for (u32 j = 0; j < records + rare; ++j) {
        /* Each record depends only on its index, so both add orders give the same set. */
        const u32 i = reverse ? records + rare - 1u - j : j;
        u64 s = 99u + i;
        const bool is_rare = i >= records;
        const int n = snprintf(
            buf, sizeof buf,
            "{\"name\":\"enemy_%04u\",\"health\":%u,\"speed\":%u.%02u,\"faction\":\"%s\","
            "\"loot_table\":\"loot/common_%u\",\"ai\":{\"behaviour\":\"patrol\","
            "\"aggro_radius\":%u,\"leash_radius\":%u,\"flee_below_health\":0.%02u},"
            "\"model\":\"characters/enemy_%u/model\",\"material_overrides\":[],"
            "\"spawn\":{\"weight\":%u,\"min_level\":%u,\"max_level\":%u},"
            "\"audio\":{\"aggro\":\"sfx/enemy/aggro_%u\",\"death\":\"sfx/enemy/death_%u\"}}",
            i, (u32)(rng_next(&s) % 500u), (u32)(rng_next(&s) % 10u), (u32)(rng_next(&s) % 100u),
            factions[rng_next(&s) % 4u], (u32)(rng_next(&s) % 9u), (u32)(rng_next(&s) % 40u),
            (u32)(rng_next(&s) % 80u), (u32)(rng_next(&s) % 50u), (u32)(rng_next(&s) % 30u),
            (u32)(rng_next(&s) % 20u), (u32)(rng_next(&s) % 10u), (u32)(rng_next(&s) % 60u),
            (u32)(rng_next(&s) % 5u), (u32)(rng_next(&s) % 5u));
        ASSERT_TRUE(n > 0 && (size_t)n < sizeof buf);
        char name[64];
        snprintf(name, sizeof name, "%s/%04u", is_rare ? "rare" : "enemies", i);
        ASSERT_EQ(dpak_writer_add(w, de_hash64_str(name), is_rare ? "rare_config" : "config", buf,
                                  (u64)n, NULL, 0, name),
                  DE_OK);
    }
    void *data = NULL;
    const de_result r = dpak_writer_finish_memory(w, NULL, &data, &out.size);
    dpak_writer_destroy(w);
    ASSERT_EQ(r, DE_OK);
    out.data = data;
    return out;
}

TEST(dpak_writer, dictionaries) {
    /* 400 records of type "config" and 7 of "rare_config" (below DPAK_DICT_MIN_SAMPLES). */
    packbuf off = build_records(false, 400, DPAK_DICT_MIN_SAMPLES - 1u, false);
    packbuf on = build_records(true, 400, DPAK_DICT_MIN_SAMPLES - 1u, false);
    /* Training sees the samples in TOC order, so the add order does not change the bytes. */
    packbuf on_rev = build_records(true, 400, DPAK_DICT_MIN_SAMPLES - 1u, true);
    ASSERT_EQ(on.size, on_rev.size);
    EXPECT_MEMEQ(on.data, on_rev.data, (size_t)on.size);
    packbuf_free(&on_rev);
    pak koff, kon;
    pak_open(&koff, &off);
    pak_open(&kon, &on);
    EXPECT_EQ(koff.h.sections[DPAK_SECTION_DICTS].count, 0u);
    ASSERT_EQ(kon.h.sections[DPAK_SECTION_DICTS].count, 1u);
    dpak_dict_entry d;
    dpak_dict_entry_load(pak_rec(&kon, DPAK_SECTION_DICTS, 0), &d);
    EXPECT_EQ(d.type_hash, de_hash64_str("config"));
    EXPECT_GT(d.size, 0u);
    EXPECT_LE(d.size, DPAK_DICT_MAX_SIZE);
    EXPECT_EQ(d.file_offset % DPAK_SECTION_ALIGNMENT, 0u);
    EXPECT_EQ(d.zstd_id, (u32)ZSTD_getDictID_fromDict(kon.p + d.file_offset, d.size));

    /* Smaller with the dictionary, counting the dictionary itself. */
    const u64 payload_off = pak_payload(&koff), payload_on = pak_payload(&kon);
    EXPECT_LT(payload_on, payload_off);
    printf("  dictionaries: %llu -> %llu payload bytes (dictionary %u bytes)\n",
           (unsigned long long)payload_off, (unsigned long long)payload_on, d.size);

    /* Only "config" chunks use it, and every entry round-trips. */
    u32 with_dict = 0;
    const u32 n = kon.h.sections[DPAK_SECTION_TOC].count;
    for (u32 i = 0; i < n; ++i) {
        const dpak_toc_entry e = pak_toc(&kon, i);
        for (u32 c = 0; c < e.chunk_count; ++c) {
            const dpak_chunk_entry ch = pak_chunk(&kon, e.first_chunk + c);
            if (ch.dict_index != DPAK_DICT_NONE) {
                EXPECT_EQ(ch.dict_index, 0u);
                EXPECT_EQ(e.type_hash, de_hash64_str("config"));
                ++with_dict;
            }
        }
        u8 *a = pak_extract(&kon, i);
        u8 *b = pak_extract(&koff, i);
        ASSERT_TRUE(a != NULL && b != NULL);
        ASSERT_EQ(e.raw_size, pak_toc(&koff, i).raw_size);
        EXPECT_MEMEQ(a, b, (size_t)e.raw_size);
        free(a);
        free(b);
    }
    EXPECT_GT(with_dict, 0u);
    packbuf_free(&off);
    packbuf_free(&on);

    /* A type with too few samples never gets a dictionary. */
    packbuf few = build_records(true, DPAK_DICT_MIN_SAMPLES - 1u, 0, false);
    pak kf;
    pak_open(&kf, &few);
    EXPECT_EQ(kf.h.sections[DPAK_SECTION_DICTS].count, 0u);
    packbuf_free(&few);
}

TEST(dpak_writer, bundles) {
    /* Bundle names are stored even in a stripped pack. */
    packbuf b = build(NULL, 1);
    pak k;
    pak_open(&k, &b);
    ASSERT_EQ(k.h.sections[DPAK_SECTION_BUNDLES].count, 2u);
    const char *strings = (const char *)k.p + k.h.sections[DPAK_SECTION_STRINGS].offset;
    dpak_bundle_entry e[2];
    for (u32 i = 0; i < 2; ++i)
        dpak_bundle_entry_load(pak_rec(&k, DPAK_SECTION_BUNDLES, i), &e[i]);
    EXPECT_LT(e[0].name_hash, e[1].name_hash);
    for (u32 i = 0; i < 2; ++i) {
        const char *name = strings + e[i].name_offset;
        EXPECT_EQ(name[e[i].name_len], '\0');
        EXPECT_EQ(de_hash64(name, e[i].name_len, 0), e[i].name_hash);
        const bool common = strcmp(name, "common") == 0;
        EXPECT_TRUE(common || strcmp(name, "level_01") == 0);
        EXPECT_EQ(e[i].flags, common ? (u32)DPAK_BUNDLE_RESIDENT : 0u);
        EXPECT_EQ(e[i].priority, common ? (u32)DPAK_PRIORITY_NORMAL : (u32)DPAK_PRIORITY_HIGH);
        /* level_01 lists player/model twice: stored once. */
        EXPECT_EQ(e[i].member_count, common ? 3u : 2u);
        EXPECT_EQ(e[i].depend_count, common ? 0u : 2u);
        EXPECT_EQ(e[i].depend_first, e[i].member_first + e[i].member_count);
        for (u32 m = 1; m < e[i].member_count; ++m)
            EXPECT_LT(
                dpak_load_u64(pak_rec(&k, DPAK_SECTION_BUNDLE_IDS, e[i].member_first + m - 1)),
                dpak_load_u64(pak_rec(&k, DPAK_SECTION_BUNDLE_IDS, e[i].member_first + m)));
        for (u32 m = 0; m < e[i].member_count; ++m)
            EXPECT_GE(pak_find(&k, dpak_load_u64(pak_rec(&k, DPAK_SECTION_BUNDLE_IDS,
                                                         e[i].member_first + m))),
                      0);
        if (!common) {
            const u64 d0 = dpak_load_u64(pak_rec(&k, DPAK_SECTION_BUNDLE_IDS, e[i].depend_first));
            const u64 d1 =
                dpak_load_u64(pak_rec(&k, DPAK_SECTION_BUNDLE_IDS, e[i].depend_first + 1));
            const u64 hc = de_hash64_str("common"), hm = de_hash64_str("music");
            EXPECT_EQ(d0, hc < hm ? hc : hm);
            EXPECT_EQ(d1, hc < hm ? hm : hc);
        }
    }
    packbuf_free(&b);

    /* Errors. */
    dpak_writer *w = NULL;
    ASSERT_EQ(dpak_writer_create(NULL, &w), DE_OK);
    const u64 ids[] = {1, 2};
    const char *const self[] = {"a"};
    EXPECT_EQ(dpak_writer_add_bundle(w, "a", ids, 2, self, 1, false, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dpak_writer_add_bundle(w, "", ids, 2, NULL, 0, false, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dpak_writer_add_bundle(w, "a", NULL, 2, NULL, 0, false, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dpak_writer_add_bundle(w, "a", ids, 2, NULL, 0, false, DPAK_PRIORITY_COUNT_),
              DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(dpak_writer_add_bundle(w, "a", ids, 2, NULL, 0, false, 0), DE_OK);
    ASSERT_EQ(dpak_writer_add_bundle(w, "a", NULL, 0, NULL, 0, true, 0), DE_OK);
    void *data = NULL;
    u64 size = 0;
    EXPECT_EQ(dpak_writer_finish_memory(w, NULL, &data, &size), DE_ERR_ALREADY_EXISTS);
    EXPECT_TRUE(data == NULL);
    dpak_writer_destroy(w);
}

TEST(dpak_writer, duplicate_id) {
    dpak_writer *w = NULL;
    ASSERT_EQ(dpak_writer_create(NULL, &w), DE_OK);
    const u8 x[4] = {1, 2, 3, 4};
    const u64 id = de_hash64_str("a/thing");
    ASSERT_EQ(dpak_writer_add(w, id, "blob", x, 4, NULL, 0, "a/thing"), DE_OK);
    ASSERT_EQ(dpak_writer_add(w, 5, "blob", x, 4, NULL, 0, NULL), DE_OK);
    /* A self-dependency is refused at add time and leaves the writer unchanged. */
    EXPECT_EQ(dpak_writer_add(w, 6, "blob", x, 4, (const u64[]){1, 6}, 2, NULL),
              DE_ERR_INVALID_ARGUMENT);
    /* Same ID under a different name (a hash collision looks exactly like this). */
    ASSERT_EQ(dpak_writer_add(w, id, "mesh", x, 2, NULL, 0, "b/other"), DE_OK);
    void *data = NULL;
    u64 size = 0;
    EXPECT_EQ(dpak_writer_finish_memory(w, NULL, &data, &size), DE_ERR_ALREADY_EXISTS);
    EXPECT_TRUE(data == NULL);
    const de_error *err = de_error_last();
    ASSERT_TRUE(err != NULL);
    EXPECT_TRUE(strstr(err->message, "a/thing") != NULL);
    EXPECT_TRUE(strstr(err->message, "b/other") != NULL);
    dpak_writer_destroy(w);
}

TEST(dpak_writer, content_hash) {
    packbuf b = build(&(dpak_writer_desc){.include_names = true, .use_dictionaries = true}, 0);
    pak k;
    pak_open(&k, &b);
    EXPECT_EQ(k.h.content_hash,
              de_hash64(b.data + DPAK_HEADER_SIZE, (size_t)(b.size - DPAK_HEADER_SIZE), 0));
    /* Any change after the header changes the hash. */
    b.data[b.size - 1] ^= 0x5Au;
    EXPECT_NE(k.h.content_hash,
              de_hash64(b.data + DPAK_HEADER_SIZE, (size_t)(b.size - DPAK_HEADER_SIZE), 0));
    b.data[b.size - 1] ^= 0x5Au;

    /* dpak_writer_finish writes the same bytes to a file through the VFS. */
    char dir[DE_FS_PATH_MAX];
    snprintf(dir, sizeof dir, "%s/content_hash", DE_PACK_TEST_TMP_DIR);
    ASSERT_EQ(de__native_mkdirs(dir), DE_OK);
    ASSERT_EQ(de_fs_mount("packtest", dir, DE_MOUNT_WRITABLE, 0), DE_OK);
    dpak_writer *w = NULL;
    ASSERT_EQ(dpak_writer_create(
                  &(dpak_writer_desc){.include_names = true, .use_dictionaries = true}, &w),
              DE_OK);
    add_set(w, 1);
    const de_result r = dpak_writer_finish(w, "packtest:/out.dpak");
    dpak_writer_destroy(w);
    ASSERT_EQ(r, DE_OK);
    void *file = NULL;
    u64 file_size = 0;
    ASSERT_EQ(de_file_read_all("packtest:/out.dpak", NULL, &file, &file_size), DE_OK);
    ASSERT_EQ(file_size, b.size);
    EXPECT_MEMEQ(file, b.data, (size_t)b.size);
    DE_FREE(de_heap_allocator(), file, (size_t)file_size + 1u);
    EXPECT_EQ(de_file_delete("packtest:/out.dpak"), DE_OK);
    EXPECT_EQ(de_fs_unmount("packtest", dir), DE_OK);
    packbuf_free(&b);
}
