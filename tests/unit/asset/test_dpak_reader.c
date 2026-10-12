/*
 * test_dpak_reader.c: the runtime .dpak reader (src/asset/dpak/dpak_reader.c, DENG-212) on packs
 * built in-process with the DENG-211 writer: byte-exact roundtrips (memory and file), dictionary
 * chunks, platform and version checks, truncation at every length class, mutated metadata, and
 * the optional content hash. Run under ASan: no input may make the reader read outside the pack.
 *
 * Files go to a scratch directory of the build tree, mounted as dpr:/.
 */
#include "de_test.h"
#include "pack_test_util.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "asset/dpak/dpak_format.h"
#include "asset/dpak/dpak_reader.h"
#include "dpak_writer.h"

#include <zstd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void mount_once(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("dpr", DE_DPAK_READER_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    mounted = true;
}

/* A fast level: the tests check the reader, not the compression ratio. */
static dpak_writer_desc fast_desc(void) {
    dpak_writer_desc d = {0};
    d.zstd_level = 3;
    d.use_dictionaries = true;
    d.include_names = true;
    d.platform = DPAK_PLATFORM_ANY;
    return d;
}

static void heap_free(void *data, u64 size) {
    DE_FREE(de_heap_allocator(), data, (size_t)size);
}

/* Every asset of `a` reads back byte for byte from `r`. */
static void expect_roundtrip(const dpak_reader *r, const pt_asset *a, u32 n) {
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    ASSERT_TRUE(dctx != NULL);
    ASSERT_EQ(dpak_entry_count(r), n);
    for (u32 i = 0; i < n; ++i) {
        u32 k = dpak_find(r, de_asset_id_from(a[i].path).hash);
        ASSERT_NE(k, DPAK_NOT_FOUND);
        dpak_toc_entry e;
        dpak_entry(r, k, &e);
        ASSERT_EQ(e.raw_size, (u64)a[i].size);
        EXPECT_EQ(e.type_hash, de_hash64_str(a[i].type));
        u8 *out = (u8 *)malloc(a[i].size + 1u);
        ASSERT_TRUE(out != NULL);
        /* Alternate the caller's decoder and the reader's temporary one. */
        ASSERT_EQ(dpak_read_entry(r, k, (i & 1u) ? NULL : dctx, out, a[i].size), DE_OK);
        if (a[i].size != 0)
            EXPECT_MEMEQ(out, a[i].data, a[i].size);
        free(out);
        u32 len = 0;
        const char *name = dpak_name(r, k, &len);
        ASSERT_TRUE(name != NULL);
        EXPECT_STREQ(name, a[i].path);
        EXPECT_EQ(len, (u32)strlen(a[i].path));
        EXPECT_EQ(e.dep_count, a[i].dep_count);
        for (u32 d = 0; d < e.dep_count; ++d) {
            u64 dep = dpak_dep(r, e.dep_first + d);
            bool known = false;
            for (u32 q = 0; q < a[i].dep_count; ++q)
                known = known || a[i].deps[q] == dep;
            EXPECT_TRUE(known);
        }
    }
    EXPECT_EQ(dpak_find(r, de_asset_id_from("not/in/the/pack").hash), DPAK_NOT_FOUND);
    ZSTD_freeDCtx(dctx);
}

/* ---- dpak_reader.roundtrip_100 -------------------------------------------------------------- */

TEST(dpak_reader, roundtrip_100) {
    mount_once();
    static pt_asset a[100];
    pt_make_100(a);
    dpak_writer_desc wd = fast_desc();
    void *data = NULL;
    u64 size = 0;
    pt_pack_memory(&wd, a, 100, &data, &size);

    /* The kinds of chunks the set is meant to cover. */
    dpak_reader *r = NULL;
    ASSERT_EQ(dpak_open_memory(data, size, &(dpak_open_desc){.verify = true}, &r), DE_OK);
    u32 multi = 0, raw = 0, dict = 0, empty = 0;
    for (u32 i = 0; i < dpak_entry_count(r); ++i) {
        dpak_toc_entry e;
        dpak_entry(r, i, &e);
        multi += e.chunk_count > 1u;
        empty += e.chunk_count == 0;
        for (u32 k = 0; k < e.chunk_count; ++k) {
            dpak_chunk_entry c;
            dpak_chunk(r, e.first_chunk + k, &c);
            raw += (c.flags & DPAK_CHUNK_RAW) != 0;
            dict += c.dict_index != DPAK_DICT_NONE;
        }
    }
    EXPECT_GE(multi, 10u);
    EXPECT_GE(raw, 10u);
    EXPECT_GT(dict, 0u);
    EXPECT_EQ(empty, 10u);
    expect_roundtrip(r, a, 100);
    dpak_close(r);

    /* The same pack as a file, through the VFS. */
    pt_pack_file(&wd, a, 100, "dpr:/r100.dpak");
    ASSERT_EQ(dpak_open("dpr:/r100.dpak",
                        &(dpak_open_desc){.platform = dpak_platform_host(), .verify = true}, &r),
              DE_OK);
    EXPECT_TRUE(dpak_file(r) != NULL);
    EXPECT_TRUE(dpak_memory(r) == NULL);
    EXPECT_EQ(dpak_get_header(r)->file_size, size);
    expect_roundtrip(r, a, 100);
    dpak_close(r);

    heap_free(data, size);
    pt_asset_free(a, 100);
}

/* ---- dpak_reader.dictionary_chunks --------------------------------------------------------- */

TEST(dpak_reader, dictionary_chunks) {
    /* Many short records of one type: the writer trains a dictionary and most chunks use it. */
    static pt_asset a[200];
    for (u32 i = 0; i < 200; ++i) {
        memset(&a[i], 0, sizeof a[i]);
        snprintf(a[i].path, sizeof a[i].path, "rec/%02u", i);
        a[i].type = "pk_rec";
        a[i].size = 300u + i;
        a[i].data = pt_make_bytes(PT_RECORD, 1000u + i, a[i].size);
    }
    dpak_writer_desc wd = fast_desc();
    void *data = NULL;
    u64 size = 0;
    pt_pack_memory(&wd, a, 200, &data, &size);
    dpak_reader *r = NULL;
    ASSERT_EQ(dpak_open_memory(data, size, NULL, &r), DE_OK);
    ASSERT_EQ(dpak_get_header(r)->sections[DPAK_SECTION_DICTS].count, 1u);
    u32 with_dict = 0;
    for (u32 i = 0; i < dpak_entry_count(r); ++i) {
        dpak_toc_entry e;
        dpak_entry(r, i, &e);
        ASSERT_EQ(e.chunk_count, 1u);
        dpak_chunk_entry c;
        dpak_chunk(r, e.first_chunk, &c);
        with_dict += c.dict_index == 0u;
    }
    EXPECT_GT(with_dict, 100u);
    expect_roundtrip(r, a, 200);
    dpak_close(r);

    /* A damaged dictionary is refused at open (its zstd ID no longer matches, or it is no
     * dictionary at all); a damaged dictionary chunk fails its decode, cleanly. */
    dpak_header h;
    dpak_header_load((const u8 *)data, &h);
    dpak_dict_entry d;
    dpak_dict_entry_load((const u8 *)data + h.sections[DPAK_SECTION_DICTS].offset, &d);
    u8 *copy = (u8 *)malloc((size_t)size);
    ASSERT_TRUE(copy != NULL);
    memcpy(copy, data, (size_t)size);
    copy[d.file_offset + 4] ^= 0x5Au; /* the dictionary ID field of the zstd dictionary header */
    EXPECT_EQ(dpak_open_memory(copy, size, NULL, &r), DE_ERR_PARSE);
    memcpy(copy, data, (size_t)size);
    ASSERT_EQ(dpak_open_memory(copy, size, NULL, &r), DE_OK);
    u32 damaged = 0;
    for (u32 i = 0; i < dpak_entry_count(r); ++i) {
        dpak_toc_entry e;
        dpak_entry(r, i, &e);
        dpak_chunk_entry c;
        dpak_chunk(r, e.first_chunk, &c);
        if (c.dict_index == DPAK_DICT_NONE || (c.flags & DPAK_CHUNK_RAW) != 0)
            continue;
        copy[c.file_offset + c.stored_size / 2u] ^= 0xFFu;
        u8 out[2048];
        de_result res = dpak_read_entry(r, i, NULL, out, sizeof out);
        /* zstd may not notice every flipped byte (no checksum in pack frames), but it never
         * writes past raw_size. */
        damaged += res != DE_OK;
        EXPECT_TRUE(res == DE_OK || res == DE_ERR_PARSE);
    }
    EXPECT_GT(damaged, 0u);
    dpak_close(r);
    free(copy);
    heap_free(data, size);
    pt_asset_free(a, 200);
}

/* ---- dpak_reader.bad_platform, bad_version -------------------------------------------------- */

TEST(dpak_reader, bad_platform) {
    pt_asset a[1];
    memset(a, 0, sizeof a);
    snprintf(a[0].path, sizeof a[0].path, "x/y");
    a[0].type = "pk_big";
    a[0].size = 100;
    a[0].data = pt_make_bytes(PT_TEXT, 1, 100);
    u32 host = dpak_platform_host();
    u32 other = host == DPAK_PLATFORM_WINDOWS ? DPAK_PLATFORM_LINUX : DPAK_PLATFORM_WINDOWS;

    dpak_writer_desc wd = fast_desc();
    wd.platform = other;
    void *data = NULL;
    u64 size = 0;
    pt_pack_memory(&wd, a, 1, &data, &size);
    dpak_reader *r = NULL;
    EXPECT_EQ(dpak_open_memory(data, size, &(dpak_open_desc){.platform = host}, &r),
              DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(r == NULL);
    EXPECT_TRUE(strstr(de_error_last()->message, "cooked for") != NULL);
    /* No expected platform: any pack opens. */
    ASSERT_EQ(dpak_open_memory(data, size, NULL, &r), DE_OK);
    dpak_close(r);
    heap_free(data, size);

    /* A platform-independent pack opens everywhere; this host's own pack too. */
    wd.platform = DPAK_PLATFORM_ANY;
    pt_pack_memory(&wd, a, 1, &data, &size);
    ASSERT_EQ(dpak_open_memory(data, size, &(dpak_open_desc){.platform = host}, &r), DE_OK);
    dpak_close(r);
    heap_free(data, size);
    wd.platform = host;
    pt_pack_memory(&wd, a, 1, &data, &size);
    ASSERT_EQ(dpak_open_memory(data, size, &(dpak_open_desc){.platform = host}, &r), DE_OK);
    dpak_close(r);

    /* An unknown platform value is a damaged header. */
    u8 *copy = (u8 *)malloc((size_t)size);
    ASSERT_TRUE(copy != NULL);
    memcpy(copy, data, (size_t)size);
    dpak_store_u32(copy + 12, 77);
    EXPECT_EQ(dpak_open_memory(copy, size, NULL, &r), DE_ERR_PARSE);
    free(copy);
    heap_free(data, size);
    pt_asset_free(a, 1);
}

TEST(dpak_reader, bad_version) {
    pt_asset a[1];
    memset(a, 0, sizeof a);
    snprintf(a[0].path, sizeof a[0].path, "x/y");
    a[0].type = "pk_big";
    a[0].size = 10;
    a[0].data = pt_make_bytes(PT_TEXT, 2, 10);
    dpak_writer_desc wd = fast_desc();
    void *data = NULL;
    u64 size = 0;
    pt_pack_memory(&wd, a, 1, &data, &size);
    u8 *copy = (u8 *)malloc((size_t)size);
    ASSERT_TRUE(copy != NULL);
    dpak_reader *r = NULL;

    memcpy(copy, data, (size_t)size);
    dpak_store_u32(copy + 4, DPAK_VERSION + 1u);
    EXPECT_EQ(dpak_open_memory(copy, size, NULL, &r), DE_ERR_VERSION_MISMATCH);
    EXPECT_TRUE(strstr(de_error_last()->message, "format version") != NULL);
    memcpy(copy, data, (size_t)size);
    dpak_store_u32(copy + 8, DPAK_HEADER_SIZE + 8u);
    EXPECT_EQ(dpak_open_memory(copy, size, NULL, &r), DE_ERR_VERSION_MISMATCH);
    memcpy(copy, data, (size_t)size);
    copy[0] = 'X';
    EXPECT_EQ(dpak_open_memory(copy, size, NULL, &r), DE_ERR_PARSE);
    EXPECT_EQ(dpak_open_memory(copy, 0, NULL, &r), DE_ERR_PARSE);
    EXPECT_EQ(dpak_open_memory(NULL, 0, NULL, &r), DE_ERR_PARSE);
    EXPECT_EQ(dpak_open_memory(NULL, 8, NULL, &r), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dpak_open_memory(data, size, NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dpak_open(NULL, NULL, &r), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(dpak_open("dpr:/does_not_exist.dpak", NULL, &r), DE_ERR_NOT_FOUND);
    free(copy);
    heap_free(data, size);
    pt_asset_free(a, 1);
}

/* ---- dpak_reader.truncated ------------------------------------------------------------------ */

/* Opens a truncated copy (exactly `n` bytes in their own block, so ASan sees any read past it)
 * two ways: as cut (the header's file_size disagrees) and with file_size patched to `n` (so the
 * deeper checks run on the cut sections and data). Both must fail; neither may crash. */
static void expect_truncated_fails(const u8 *data, u64 n) {
    u8 *cut = (u8 *)malloc(n != 0 ? (size_t)n : 1u);
    ASSERT_TRUE(cut != NULL);
    memcpy(cut, data, (size_t)n);
    dpak_reader *r = NULL;
    de_result res = dpak_open_memory(cut, n, NULL, &r);
    EXPECT_NE(res, DE_OK);
    EXPECT_TRUE(r == NULL);
    if (n >= DPAK_HEADER_SIZE) {
        dpak_store_u64(cut + 40, n);
        res = dpak_open_memory(cut, n, NULL, &r);
        EXPECT_EQ(res, DE_ERR_PARSE);
        if (res == DE_OK)
            dpak_close(r);
    }
    free(cut);
}

TEST(dpak_reader, truncated) {
    mount_once();
    static pt_asset a[100];
    pt_make_100(a);
    dpak_writer_desc wd = fast_desc();
    void *data = NULL;
    u64 size = 0;
    pt_pack_memory(&wd, a, 100, &data, &size);
    const u8 *p = (const u8 *)data;

    /* Every length up to the end of the metadata (where the cut lands in the header, a
     * section or a dictionary), then a stride through the data, then the last bytes. */
    dpak_header h;
    dpak_header_load(p, &h);
    u64 meta_end = DPAK_HEADER_SIZE;
    for (u32 s = 0; s < (u32)DPAK_SECTION_COUNT; ++s)
        if (h.sections[s].offset + h.sections[s].size > meta_end)
            meta_end = h.sections[s].offset + h.sections[s].size;
    for (u64 n = 0; n < meta_end + 64u && n < size; n += n < 512u ? 1u : 7u)
        expect_truncated_fails(p, n);
    for (u64 n = meta_end; n < size; n += 4093u)
        expect_truncated_fails(p, n);
    for (u64 n = size - 16u; n < size; ++n)
        expect_truncated_fails(p, n);

    /* A truncated file fails at open too. */
    ASSERT_EQ(de_file_write_all_atomic("dpr:/cut.dpak", p, size - 1u), DE_OK);
    dpak_reader *r = NULL;
    EXPECT_EQ(dpak_open("dpr:/cut.dpak", NULL, &r), DE_ERR_PARSE);
    ASSERT_EQ(de_file_write_all_atomic("dpr:/cut.dpak", p, 100), DE_OK);
    EXPECT_EQ(dpak_open("dpr:/cut.dpak", NULL, &r), DE_ERR_PARSE);
    EXPECT_TRUE(r == NULL);

    heap_free(data, size);
    pt_asset_free(a, 100);
}

/* ---- dpak_reader.mutated_metadata ----------------------------------------------------------- */

/* Random byte changes in the header and the sections. Each mutant either fails to open, or
 * opens and then reads every entry without a crash (decode errors are allowed). A small fuzz
 * pass that runs on every build; DENG-216 adds the real fuzz target. */
TEST(dpak_reader, mutated_metadata) {
    static pt_asset a[100]; /* only the first 20 go into the pack */
    pt_make_100(a);
    dpak_writer_desc wd = fast_desc();
    void *data = NULL;
    u64 size = 0;
    pt_pack_memory(&wd, a, 20, &data, &size);
    dpak_header h;
    dpak_header_load((const u8 *)data, &h);
    u64 meta_end = DPAK_HEADER_SIZE;
    for (u32 s = 0; s < (u32)DPAK_SECTION_COUNT; ++s)
        if (h.sections[s].offset + h.sections[s].size > meta_end)
            meta_end = h.sections[s].offset + h.sections[s].size;

    u8 *copy = (u8 *)malloc((size_t)size);
    u8 *out = (u8 *)malloc((size_t)1 << 20);
    ASSERT_TRUE(copy != NULL && out != NULL);
    u64 st = 12345;
    u32 opened = 0;
    for (u32 iter = 0; iter < 3000; ++iter) {
        memcpy(copy, data, (size_t)size);
        u32 flips = 1u + (u32)(pt_rng(&st) % 4u);
        for (u32 k = 0; k < flips; ++k) {
            u64 at = pt_rng(&st) % meta_end;
            if (at >= 32 && at < 48)
                continue; /* the content hash and file_size are covered elsewhere */
            u64 v = pt_rng(&st);
            copy[at] = (v & 1u) ? (u8)(v >> 8) : (u8)(copy[at] ^ (1u << ((v >> 8) & 7u)));
        }
        dpak_reader *r = NULL;
        if (dpak_open_memory(copy, size, NULL, &r) != DE_OK)
            continue;
        opened++;
        for (u32 i = 0; i < dpak_entry_count(r); ++i) {
            dpak_toc_entry e;
            dpak_entry(r, i, &e);
            if (e.raw_size <= ((u64)1 << 20)) {
                de_result res = dpak_read_entry(r, i, NULL, out, (u64)1 << 20);
                EXPECT_TRUE(res == DE_OK || res == DE_ERR_PARSE);
            }
            (void)dpak_name(r, i, NULL);
        }
        for (u32 b = 0; b < dpak_bundle_count(r); ++b) {
            dpak_bundle_entry be;
            dpak_bundle(r, b, &be);
            (void)dpak_bundle_name(r, &be);
        }
        dpak_close(r);
    }
    EXPECT_GT(opened, 0u); /* some flips land on bytes that do not matter (names, reserved) */
    free(out);
    free(copy);
    heap_free(data, size);
    pt_asset_free(a, 100);
}

/* ---- dpak_reader.verify --------------------------------------------------------------------- */

TEST(dpak_reader, verify) {
    mount_once();
    static pt_asset a[100]; /* the first 10 go into the pack */
    pt_make_100(a);
    dpak_writer_desc wd = fast_desc();
    void *data = NULL;
    u64 size = 0;
    pt_pack_memory(&wd, a, 10, &data, &size);
    u8 *copy = (u8 *)malloc((size_t)size);
    ASSERT_TRUE(copy != NULL);
    memcpy(copy, data, (size_t)size);
    copy[size - 3u] ^= 0x01u; /* asset data: the structure stays valid */

    dpak_reader *r = NULL;
    ASSERT_EQ(dpak_open_memory(copy, size, NULL, &r), DE_OK); /* not checked without verify */
    dpak_close(r);
    EXPECT_EQ(dpak_open_memory(copy, size, &(dpak_open_desc){.verify = true}, &r), DE_ERR_PARSE);
    EXPECT_TRUE(strstr(de_error_last()->message, "content hash") != NULL);
    ASSERT_EQ(dpak_open_memory(data, size, &(dpak_open_desc){.verify = true}, &r), DE_OK);
    dpak_close(r);

    /* File readers stream the hash. */
    ASSERT_EQ(de_file_write_all_atomic("dpr:/v.dpak", data, size), DE_OK);
    ASSERT_EQ(dpak_open("dpr:/v.dpak", &(dpak_open_desc){.verify = true}, &r), DE_OK);
    dpak_close(r);
    ASSERT_EQ(de_file_write_all_atomic("dpr:/v.dpak", copy, size), DE_OK);
    EXPECT_EQ(dpak_open("dpr:/v.dpak", &(dpak_open_desc){.verify = true}, &r), DE_ERR_PARSE);
    ASSERT_EQ(dpak_open("dpr:/v.dpak", NULL, &r), DE_OK);
    dpak_close(r);

    free(copy);
    heap_free(data, size);
    pt_asset_free(a, 100);
}

/* ---- dpak_reader.bundles -------------------------------------------------------------------- */

TEST(dpak_reader, bundles) {
    static pt_asset a[100]; /* the first 3 go into the pack */
    pt_make_100(a);
    dpak_writer *w = pt_writer(&(dpak_writer_desc){.zstd_level = 3}, a, 3);
    const u64 m1[] = {de_asset_id_from(a[0].path).hash, de_asset_id_from(a[1].path).hash};
    const char *const deps[] = {"common"};
    ASSERT_EQ(dpak_writer_add_bundle(w, "level_01", m1, 2, deps, 1, false, DPAK_PRIORITY_HIGH),
              DE_OK);
    const u64 m2[] = {de_asset_id_from(a[2].path).hash};
    ASSERT_EQ(dpak_writer_add_bundle(w, "common", m2, 1, NULL, 0, true, DPAK_PRIORITY_NORMAL),
              DE_OK);
    void *data = NULL;
    u64 size = 0;
    ASSERT_EQ(dpak_writer_finish_memory(w, NULL, &data, &size), DE_OK);
    dpak_writer_destroy(w);

    dpak_reader *r = NULL;
    ASSERT_EQ(dpak_open_memory(data, size, NULL, &r), DE_OK);
    EXPECT_EQ(dpak_bundle_count(r), 2u);
    EXPECT_EQ(dpak_name(r, 0, NULL), NULL); /* (dpak_writer_desc){0}: no names */
    u32 k = dpak_bundle_find(r, de_hash64_str("level_01"));
    ASSERT_NE(k, DPAK_NOT_FOUND);
    dpak_bundle_entry b;
    dpak_bundle(r, k, &b);
    EXPECT_STREQ(dpak_bundle_name(r, &b), "level_01");
    EXPECT_EQ(b.member_count, 2u);
    EXPECT_EQ(b.depend_count, 1u);
    EXPECT_EQ(dpak_bundle_id(r, b.depend_first), de_hash64_str("common"));
    EXPECT_EQ(b.priority, (u32)DPAK_PRIORITY_HIGH);
    u32 c = dpak_bundle_find(r, de_hash64_str("common"));
    ASSERT_NE(c, DPAK_NOT_FOUND);
    dpak_bundle(r, c, &b);
    EXPECT_NE(b.flags & DPAK_BUNDLE_RESIDENT, 0u);
    EXPECT_EQ(dpak_bundle_find(r, de_hash64_str("nope")), DPAK_NOT_FOUND);
    dpak_close(r);
    heap_free(data, size);
    pt_asset_free(a, 100);
}
