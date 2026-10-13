/*
 * test_pack_cli.c: `de-pack list`, `extract` and `verify` (DENG-213, SPEC §11.7). CTest names
 * pack_cli.<test>.
 *
 * Each test cooks the fixture project tests/data/cook_project/ in-process with `de-cook cook`
 * (cook_project_util.h) into its own scratch subdirectory of the build tree, then runs de_pack_main
 * in-process on the pack. The tests need the runtime importers (DE_HAS_COOKER: Debug and Dev) to
 * cook; elsewhere they skip, except `pack_cli.usage`.
 */
#include "../cook/cook_project_util.h"

#include <dengine/de_ecs.h>

#include "asset/asset_internal.h"
#include "asset/dpak/dpak_format.h"
#include "asset/dpak/dpak_reader.h"
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"
#include "cook/lib/de_cooker.h"
#include "dpak_writer.h"
#include "pack_cmds.h"

#define NO_COOKER_MSG "this build has no runtime importers (DE_HAS_COOKER: Debug and Dev)"

/* ---- Running de-pack ------------------------------------------------------------------------ */

static void pk_test_write(void *user, de_pack_stream stream, const char *text, size_t len) {
    (void)user;
    cct_buf_add(stream == DE_PACK_STDERR ? &g_cct.err : &g_cct.out, text, len);
}

static int run_pack(const char *const *args) {
    cct_reset();
    de_pack_io io = {pk_test_write, NULL};
    return de_pack_main(cct_argc(args), args, &io);
}

/* Writes a copy of `pak` with byte `off` XOR `mask` as CCT_TMP/<rel>; its native path to `out`. */
static void damaged_copy(const char *pak, u64 off, u8 mask, const char *rel, char *out,
                         size_t cap) {
    size_t n = 0;
    u8 *bytes = cct_read(pak, &n);
    ASSERT_TRUE(bytes != NULL && off < n);
    bytes[off] ^= mask;
    cct_put(rel, bytes, n);
    free(bytes);
    cct_path(out, cap, rel);
}

static u32 count_of(const char *hay, const char *needle) {
    u32 n = 0;
    for (const char *p = strstr(hay, needle); p != NULL; p = strstr(p + 1, needle))
        ++n;
    return n;
}

#if DE_HAS_COOKER
/* Cooks the fixture into CCT_TMP/<sub>/game.dpak; the native path goes to `pak`. */
static void cook_fixture(const char *sub, const char *const *extra, char *pak, size_t cap) {
    cct_fresh(sub);
    ASSERT_EQ(cct_cook_project(sub, "game", "linux", extra), DE_COOK_EXIT_OK);
    char rel[256];
    snprintf(rel, sizeof rel, "%s/game.dpak", sub);
    cct_path(pak, cap, rel);
}

/* The data offset of `id`'s first chunk, and its first chunk's flags, from the pack file. */
static u64 chunk_offset(const char *pak, const char *id, u16 *flags) {
    size_t n = 0;
    u8 *bytes = cct_read(pak, &n);
    ASSERT_TRUE(bytes != NULL);
    dpak_reader *r = NULL;
    ASSERT_EQ(dpak_open_memory(bytes, n, NULL, &r), DE_OK);
    dpak_toc_entry e;
    u32 k = dpak_find(r, de_asset_id_from(id).hash);
    ASSERT_NE(k, DPAK_NOT_FOUND);
    dpak_entry(r, k, &e);
    dpak_chunk_entry c;
    dpak_chunk(r, e.first_chunk, &c);
    *flags = c.flags;
    dpak_close(r);
    free(bytes);
    return c.file_offset;
}

/* ---- A minimal JSON parser (syntax only) ---------------------------------------------------- */

typedef struct json {
    const char *p;
    bool ok;
} json;

static void js_ws(json *j) {
    while (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')
        ++j->p;
}

static void js_value(json *j, int depth);

static void js_string(json *j) {
    if (*j->p != '"') {
        j->ok = false;
        return;
    }
    for (++j->p; *j->p != '"'; ++j->p) {
        if ((unsigned char)*j->p < 0x20) {
            j->ok = false;
            return;
        }
        if (*j->p == '\\') {
            ++j->p;
            if (*j->p == 'u') {
                for (int i = 0; i < 4; ++i)
                    if (strchr("0123456789abcdefABCDEF", *++j->p) == NULL || *j->p == '\0') {
                        j->ok = false;
                        return;
                    }
            } else if (strchr("\"\\/bfnrt", *j->p) == NULL || *j->p == '\0') {
                j->ok = false;
                return;
            }
        }
    }
    ++j->p;
}

static void js_value(json *j, int depth) {
    js_ws(j);
    if (!j->ok || depth > 32) {
        j->ok = false;
        return;
    }
    char c = *j->p;
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        ++j->p;
        js_ws(j);
        if (*j->p == close) {
            ++j->p;
            return;
        }
        for (;;) {
            if (c == '{') {
                js_ws(j);
                js_string(j);
                js_ws(j);
                if (!j->ok || *j->p != ':') {
                    j->ok = false;
                    return;
                }
                ++j->p;
            }
            js_value(j, depth + 1);
            js_ws(j);
            if (!j->ok)
                return;
            if (*j->p == ',') {
                ++j->p;
                continue;
            }
            if (*j->p == close) {
                ++j->p;
                return;
            }
            j->ok = false;
            return;
        }
    } else if (c == '"') {
        js_string(j);
    } else if (strncmp(j->p, "true", 4) == 0 || strncmp(j->p, "null", 4) == 0) {
        j->p += 4;
    } else if (strncmp(j->p, "false", 5) == 0) {
        j->p += 5;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        if (c == '-')
            ++j->p;
        if (!(*j->p >= '0' && *j->p <= '9'))
            j->ok = false;
        while ((*j->p >= '0' && *j->p <= '9') || *j->p == '.' || *j->p == 'e' || *j->p == 'E' ||
               *j->p == '+' || *j->p == '-')
            ++j->p;
    } else {
        j->ok = false;
    }
}

static bool json_valid(const char *text) {
    json j = {text, true};
    js_value(&j, 0);
    js_ws(&j);
    return j.ok && *j.p == '\0';
}

#endif /* DE_HAS_COOKER */

/* ---- test_pack_cli_verify_ok ------------------------------------------------------------------
 */

TEST(pack_cli, verify_ok) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    char pak[DE_FS_PATH_MAX];
    cook_fixture("verify_ok", NULL, pak, sizeof pak);
    const char *args[] = {"verify", pak, NULL};
    ASSERT_EQ(run_pack(args), DE_PACK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, ": OK (platform linux, 9 entries") != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, "0 external references") != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, "error") == NULL);

    /* A stripped pack verifies too. A pack that names an asset of another pack reports it as
     * external, which is not a problem. */
    char stripped[DE_FS_PATH_MAX];
    cook_fixture("verify_ok_stripped", (const char *const[]){"--strip-names", NULL}, stripped,
                 sizeof stripped);
    const char *sargs[] = {"verify", stripped, NULL};
    ASSERT_EQ(run_pack(sargs), DE_PACK_EXIT_OK);
    static const char manifest[] =
        "[project]\nasset_root = \"assets\"\n[assets]\n"
        "\"t\" = { type = \"cooktext\", source = \"t.txt\", dep = \"elsewhere/asset\" }\n";
    cct_put("verify_ok_ext/assets.toml", manifest, sizeof manifest - 1);
    cct_put("verify_ok_ext/assets/t.txt", "text", 4);
    char mpath[DE_FS_PATH_MAX], out[DE_FS_PATH_MAX], ext[DE_FS_PATH_MAX];
    cct_path(mpath, sizeof mpath, "verify_ok_ext/assets.toml");
    cct_path(out, sizeof out, "verify_ok_ext");
    const char *cargs[] = {mpath, "--platform", "windows", "--out", out, "--no-cache", NULL};
    ASSERT_EQ(cct_cook(cargs), DE_COOK_EXIT_OK);
    cct_path(ext, sizeof ext, "verify_ok_ext/game.dpak");
    const char *eargs[] = {"verify", ext, NULL};
    ASSERT_EQ(run_pack(eargs), DE_PACK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, "note: asset \"t\"") != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, "(external)") != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, "1 external reference") != NULL);
#endif
}

/* ---- test_pack_cli_verify_corrupt -------------------------------------------------------------
 */

/* One flipped byte fails verify with exit 1 (format v2, DENG-392). A byte in any chunk, raw or
 * compressed, is named with the asset that owns the chunk (its chunk hash), and so is the content
 * hash; a byte in the metadata only fails the content hash. A truncated pack does not open. */
TEST(pack_cli, verify_corrupt) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    char pak[DE_FS_PATH_MAX], bad[DE_FS_PATH_MAX];
    cook_fixture("verify_corrupt", NULL, pak, sizeof pak);

    /* data/big: zstd chunks. Its first byte is the frame's magic number. */
    u16 flags = 0;
    u64 off = chunk_offset(pak, "data/big", &flags);
    EXPECT_EQ(flags & DPAK_CHUNK_RAW, 0u);
    damaged_copy(pak, off, 0x01, "verify_corrupt/big.dpak", bad, sizeof bad);
    const char *args[] = {"verify", bad, NULL};
    ASSERT_EQ(run_pack(args), DE_PACK_EXIT_PROBLEMS);
    EXPECT_TRUE(strstr(g_cct.out.p, "error: asset \"data/big\" (0x") != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, "): chunk 0 of ") != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, "is damaged") != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, "content hash mismatch") != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, ": FAILED (2 problems, 1 damaged asset)") != NULL);

    /* The same in a stripped pack: the asset is named by its ID. */
    char stripped[DE_FS_PATH_MAX];
    cook_fixture("verify_corrupt_stripped", (const char *const[]){"--strip-names", NULL}, stripped,
                 sizeof stripped);
    off = chunk_offset(stripped, "data/big", &flags);
    damaged_copy(stripped, off, 0x80, "verify_corrupt_stripped/big.dpak", bad, sizeof bad);
    char want[64];
    snprintf(want, sizeof want, "error: asset 0x%016llx: chunk 0 of",
             (unsigned long long)de_asset_id_from("data/big").hash);
    ASSERT_EQ(run_pack(args), DE_PACK_EXIT_PROBLEMS);
    EXPECT_TRUE(strstr(g_cct.out.p, want) != NULL);

    /* data/hello: 13 bytes, stored raw. Format v1 could not name it (it decodes); v2 can. */
    off = chunk_offset(pak, "data/hello", &flags);
    EXPECT_NE(flags & DPAK_CHUNK_RAW, 0u);
    damaged_copy(pak, off + 3, 0x20, "verify_corrupt/hello.dpak", bad, sizeof bad);
    ASSERT_EQ(run_pack(args), DE_PACK_EXIT_PROBLEMS);
    EXPECT_TRUE(strstr(g_cct.out.p, "content hash mismatch") != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, "error: asset \"data/hello\" (0x") != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, ": FAILED (2 problems, 1 damaged asset)") != NULL);

    /* A byte in the TOC (the type hash of the first entry): the content hash only, and the note
     * that no asset data is damaged. */
    damaged_copy(pak, DPAK_HEADER_SIZE + 8, 0x01, "verify_corrupt/toc.dpak", bad, sizeof bad);
    ASSERT_EQ(run_pack(args), DE_PACK_EXIT_PROBLEMS);
    EXPECT_TRUE(strstr(g_cct.out.p, "content hash mismatch") != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, "every chunk and dictionary matches its hash") != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, ": FAILED (1 problem, 0 damaged assets)") != NULL);

    /* Truncated: the reader refuses it at open. */
    size_t n = 0;
    u8 *bytes = cct_read(pak, &n);
    ASSERT_TRUE(bytes != NULL);
    cct_put("verify_corrupt/short.dpak", bytes, n / 2);
    free(bytes);
    cct_path(bad, sizeof bad, "verify_corrupt/short.dpak");
    ASSERT_EQ(run_pack(args), DE_PACK_EXIT_PROBLEMS);
    EXPECT_TRUE(strstr(g_cct.out.p, "FAILED (the pack does not open)") != NULL);
    EXPECT_TRUE(strstr(g_cct.err.p, "not a valid pack") != NULL);

    /* No such file: an I/O error. */
    cct_path(bad, sizeof bad, "verify_corrupt/none.dpak");
    EXPECT_EQ(run_pack(args), DE_PACK_EXIT_USAGE);
#endif
}

/* ---- Packs from the writer (DENG-392): every chunk, dictionaries, format v1 -------------------
 * These packs come from the writer directly (tools/pack/lib), so they need no cooker and run in
 * every build that has the tools. */

/* xorshift64*: the same bytes on every host. */
static u64 vc_rng(u64 *s) {
    u64 x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

/* 48 JSON-like records of type "rec" (a dictionary pays off for them), a 3000-byte noise asset
 * (raw chunks) and a 5000-byte text asset (zstd chunks), in 1 KiB chunks. Written to
 * CCT_TMP/<rel>; the native path goes to `out`. */
static void vc_write_pack(u32 version, bool names, const char *rel, char *out, size_t cap) {
    dpak_writer *w = NULL;
    dpak_writer_desc wd = {.chunk_size = DPAK_MIN_CHUNK_SIZE,
                           .alignment = 16,
                           .zstd_level = 19,
                           .use_dictionaries = true,
                           .include_names = names,
                           .format_version = version};
    ASSERT_EQ(dpak_writer_create(&wd, &w), DE_OK);
    static const char *const k_factions[] = {"bandits", "guards", "wildlife", "undead"};
    char buf[2048], name[64];
    for (u32 i = 0; i < 48u; ++i) {
        u64 st = 7u + i;
        size_t at = 0;
        while (at < 900u) {
            int n = snprintf(buf + at, sizeof buf - at,
                             "{\"name\":\"enemy_%04u\",\"health\":%u,\"faction\":\"%s\","
                             "\"ai\":{\"behaviour\":\"patrol\",\"aggro_radius\":%u}}\n",
                             (unsigned)(vc_rng(&st) % 10000u), (unsigned)(vc_rng(&st) % 500u),
                             k_factions[vc_rng(&st) % 4u], (unsigned)(vc_rng(&st) % 40u));
            ASSERT_TRUE(n > 0);
            at += (size_t)n;
        }
        snprintf(name, sizeof name, "recs/%02u", i);
        ASSERT_EQ(dpak_writer_add(w, de_asset_id_from(name).hash, "rec", buf, at, NULL, 0, name),
                  DE_OK);
    }
    u8 noise[3000];
    u64 st = 99;
    for (size_t i = 0; i < sizeof noise; ++i)
        noise[i] = (u8)(vc_rng(&st) >> 56);
    ASSERT_EQ(dpak_writer_add(w, de_asset_id_from("misc/noise").hash, "blob", noise, sizeof noise,
                              NULL, 0, "misc/noise"),
              DE_OK);
    char text[5000];
    for (size_t i = 0; i < sizeof text; ++i)
        text[i] = "alpha bravo charlie delta "[(i * 7u + i / 26u) % 26u];
    ASSERT_EQ(dpak_writer_add(w, de_asset_id_from("misc/text").hash, "blob", text, sizeof text,
                              NULL, 0, "misc/text"),
              DE_OK);
    void *data = NULL;
    u64 size = 0;
    de_result r = dpak_writer_finish_memory(w, NULL, &data, &size);
    dpak_writer_destroy(w);
    ASSERT_EQ(r, DE_OK);
    cct_put(rel, data, (size_t)size);
    DE_FREE(de_heap_allocator(), data, (size_t)size);
    cct_path(out, cap, rel);
}

/* A flipped byte in any chunk of any asset (raw, zstd or dictionary; first, middle or last byte)
 * is reported with the asset ID that owns it, and only that asset. Several damaged assets are all
 * reported. A stripped pack names them by ID. */
TEST(pack_cli, verify_every_chunk) {
    char pak[DE_FS_PATH_MAX], bad[DE_FS_PATH_MAX];
    vc_write_pack(0, true, "verify_every/ok.dpak", pak, sizeof pak);
    const char *ok_args[] = {"verify", pak, NULL};
    ASSERT_EQ(run_pack(ok_args), DE_PACK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, ": OK (platform any, 50 entries") != NULL);

    size_t n = 0;
    u8 *bytes = cct_read(pak, &n);
    ASSERT_TRUE(bytes != NULL);
    dpak_reader *r = NULL;
    ASSERT_EQ(dpak_open_memory(bytes, n, NULL, &r), DE_OK);
    ASSERT_EQ(dpak_dict_count(r), 1u);
    u32 raw = 0, zstd = 0, dict = 0, runs = 0;
    const char *args[] = {"verify", bad, NULL};
    for (u32 i = 0; i < dpak_entry_count(r); ++i) {
        dpak_toc_entry e;
        dpak_entry(r, i, &e);
        const char *name = dpak_name(r, i, NULL);
        ASSERT_TRUE(name != NULL);
        char want[160];
        for (u32 k = 0; k < e.chunk_count; ++k) {
            dpak_chunk_entry c;
            dpak_chunk(r, e.first_chunk + k, &c);
            raw += (c.flags & DPAK_CHUNK_RAW) != 0;
            dict += (c.flags & DPAK_CHUNK_RAW) == 0 && c.dict_index != DPAK_DICT_NONE;
            zstd += (c.flags & DPAK_CHUNK_RAW) == 0 && c.dict_index == DPAK_DICT_NONE;
            const u64 at = c.file_offset + (u64)(runs % 3u == 0   ? 0u
                                                 : runs % 3u == 1 ? c.stored_size / 2u
                                                                  : c.stored_size - 1u);
            ++runs;
            damaged_copy(pak, at, 0x40, "verify_every/bad.dpak", bad, sizeof bad);
            ASSERT_EQ(run_pack(args), DE_PACK_EXIT_PROBLEMS);
            snprintf(want, sizeof want, "error: asset \"%s\" (0x%016llx): chunk %u of %u (", name,
                     (unsigned long long)e.id, (unsigned)k, (unsigned)e.chunk_count);
            EXPECT_TRUE(strstr(g_cct.out.p, want) != NULL);
            EXPECT_EQ(count_of(g_cct.out.p, "error: asset"), 1u);
            EXPECT_TRUE(strstr(g_cct.out.p, ": FAILED (2 problems, 1 damaged asset)") != NULL);
        }
    }
    EXPECT_GT(raw, 0u);
    EXPECT_GT(zstd, 0u);
    EXPECT_GT(dict, 0u);

    /* Two assets at once: both are named. */
    dpak_toc_entry e0, e1;
    dpak_entry(r, 0, &e0);
    dpak_entry(r, dpak_entry_count(r) - 1u, &e1);
    dpak_chunk_entry c0, c1;
    dpak_chunk(r, e0.first_chunk, &c0);
    dpak_chunk(r, e1.first_chunk + e1.chunk_count - 1u, &c1);
    bytes[c0.file_offset + 1u] ^= 0x02u;
    bytes[c1.file_offset + c1.stored_size - 1u] ^= 0x02u;
    cct_put("verify_every/two.dpak", bytes, n);
    cct_path(bad, sizeof bad, "verify_every/two.dpak");
    ASSERT_EQ(run_pack(args), DE_PACK_EXIT_PROBLEMS);
    EXPECT_EQ(count_of(g_cct.out.p, "error: asset"), 2u);
    char hex[24];
    snprintf(hex, sizeof hex, "(0x%016llx)", (unsigned long long)e0.id);
    EXPECT_TRUE(strstr(g_cct.out.p, hex) != NULL);
    snprintf(hex, sizeof hex, "(0x%016llx)", (unsigned long long)e1.id);
    EXPECT_TRUE(strstr(g_cct.out.p, hex) != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, ": FAILED (3 problems, 2 damaged assets)") != NULL);
    dpak_close(r);
    free(bytes);

    /* Stripped: the asset is named by its ID. */
    vc_write_pack(0, false, "verify_every/stripped.dpak", pak, sizeof pak);
    bytes = cct_read(pak, &n);
    ASSERT_TRUE(bytes != NULL);
    ASSERT_EQ(dpak_open_memory(bytes, n, NULL, &r), DE_OK);
    u32 k = dpak_find(r, de_asset_id_from("misc/noise").hash);
    ASSERT_NE(k, DPAK_NOT_FOUND);
    dpak_toc_entry e;
    dpak_entry(r, k, &e);
    dpak_chunk_entry c;
    dpak_chunk(r, e.first_chunk + 1u, &c);
    dpak_close(r);
    free(bytes);
    damaged_copy(pak, c.file_offset + 5u, 0x01, "verify_every/stripped_bad.dpak", bad, sizeof bad);
    ASSERT_EQ(run_pack(args), DE_PACK_EXIT_PROBLEMS);
    char want[96];
    snprintf(want, sizeof want, "error: asset 0x%016llx: chunk 1 of %u (", (unsigned long long)e.id,
             (unsigned)e.chunk_count);
    EXPECT_TRUE(strstr(g_cct.out.p, want) != NULL);
}

/* A damaged dictionary is reported, and so is every asset with a chunk that uses it. */
TEST(pack_cli, verify_dictionary) {
    char pak[DE_FS_PATH_MAX], bad[DE_FS_PATH_MAX];
    vc_write_pack(0, true, "verify_dict/ok.dpak", pak, sizeof pak);
    size_t n = 0;
    u8 *bytes = cct_read(pak, &n);
    ASSERT_TRUE(bytes != NULL);
    dpak_reader *r = NULL;
    ASSERT_EQ(dpak_open_memory(bytes, n, NULL, &r), DE_OK);
    ASSERT_EQ(dpak_dict_count(r), 1u);
    dpak_dict_entry d;
    dpak_dict(r, 0, &d);
    u32 users = 0;
    for (u32 i = 0; i < dpak_entry_count(r); ++i) {
        dpak_toc_entry e;
        dpak_entry(r, i, &e);
        bool uses = false;
        for (u32 k = 0; k < e.chunk_count; ++k) {
            dpak_chunk_entry c;
            dpak_chunk(r, e.first_chunk + k, &c);
            uses = uses || c.dict_index == 0u;
        }
        users += uses;
    }
    dpak_close(r);
    free(bytes);
    ASSERT_GT(users, 0u);
    /* The last byte: zstd still parses the dictionary, so the pack opens. */
    damaged_copy(pak, d.file_offset + d.size - 1u, 0x08, "verify_dict/bad.dpak", bad, sizeof bad);
    const char *args[] = {"verify", bad, NULL};
    ASSERT_EQ(run_pack(args), DE_PACK_EXIT_PROBLEMS);
    EXPECT_TRUE(strstr(g_cct.out.p, "error: dictionary 0 (type 0x") != NULL);
    EXPECT_EQ(count_of(g_cct.out.p, "uses dictionary 0, which is damaged"), users);
    char want[64];
    snprintf(want, sizeof want, "%u damaged assets)", (unsigned)users);
    EXPECT_TRUE(strstr(g_cct.out.p, want) != NULL);
}

/* Format v1 packs still verify. Without hashes, a damaged raw chunk is caught only by the content
 * hash, as before v2. */
TEST(pack_cli, verify_v1) {
    char pak[DE_FS_PATH_MAX], bad[DE_FS_PATH_MAX];
    vc_write_pack(1, true, "verify_v1/ok.dpak", pak, sizeof pak);
    const char *ok_args[] = {"verify", pak, NULL};
    ASSERT_EQ(run_pack(ok_args), DE_PACK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, ": OK (platform any, 50 entries") != NULL);
    const char *list_args[] = {"list", pak, NULL};
    ASSERT_EQ(run_pack(list_args), DE_PACK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, ": version 1,") != NULL);

    size_t n = 0;
    u8 *bytes = cct_read(pak, &n);
    ASSERT_TRUE(bytes != NULL);
    dpak_reader *r = NULL;
    ASSERT_EQ(dpak_open_memory(bytes, n, NULL, &r), DE_OK);
    EXPECT_FALSE(dpak_has_hashes(r));
    dpak_toc_entry e;
    dpak_entry(r, dpak_find(r, de_asset_id_from("misc/noise").hash), &e);
    dpak_chunk_entry c;
    dpak_chunk(r, e.first_chunk, &c);
    dpak_close(r);
    free(bytes);
    ASSERT_NE(c.flags & DPAK_CHUNK_RAW, 0u);
    damaged_copy(pak, c.file_offset + 9u, 0x01, "verify_v1/bad.dpak", bad, sizeof bad);
    const char *args[] = {"verify", bad, NULL};
    ASSERT_EQ(run_pack(args), DE_PACK_EXIT_PROBLEMS);
    EXPECT_TRUE(strstr(g_cct.out.p, "content hash mismatch") != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, "format v1 has no per-asset hash") != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, ": FAILED (1 problem, 0 damaged assets)") != NULL);
}

/* ---- test_pack_cli_list_json ------------------------------------------------------------------
 */

#if DE_HAS_COOKER
static const char *const k_ids[] = {
    "data/hello",    "data/big",      "levels/demo",         "props/barrel",        "props/crate",
    "text/greeting", "text/farewell", "text/greeting#lower", "text/farewell#lower",
};
#endif

TEST(pack_cli, list_json) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    char pak[DE_FS_PATH_MAX];
    cook_fixture("list_json", NULL, pak, sizeof pak);
    const char *args[] = {"list", pak, "--json", NULL};
    ASSERT_EQ(run_pack(args), DE_PACK_EXIT_OK);
    const char *js = g_cct.out.p;
    EXPECT_TRUE(json_valid(js));
    EXPECT_EQ(count_of(js, "{\"id\": "), 9u);
    for (size_t i = 0; i < sizeof k_ids / sizeof k_ids[0]; ++i) {
        char want[128], id[64];
        snprintf(want, sizeof want, "\"name\": \"%s\"", k_ids[i]);
        EXPECT_TRUE(strstr(js, want) != NULL);
        snprintf(id, sizeof id, "{\"id\": \"0x%016llx\"",
                 (unsigned long long)de_asset_id_from(k_ids[i]).hash);
        EXPECT_TRUE(strstr(js, id) != NULL);
    }
    EXPECT_EQ(count_of(js, "\"sub_asset\": true"), 2u);
    EXPECT_EQ(count_of(js, "\"type\": \"scene\""), 1u);
    EXPECT_EQ(count_of(js, "\"type\": \"prefab\""), 2u);
    EXPECT_EQ(count_of(js, "\"type\": null"), 4u); /* cooktext is not an engine type */
    EXPECT_TRUE(strstr(js, "{\"name\": \"level_01\", \"priority\": \"normal\", \"resident\": "
                           "false") != NULL);
    EXPECT_TRUE(strstr(js, "\"depends\": [\"core\"]") != NULL);
    EXPECT_TRUE(strstr(js, "\"platform\": \"linux\"") != NULL);

    /* A stripped pack: valid JSON, names null. */
    char stripped[DE_FS_PATH_MAX];
    cook_fixture("list_json_stripped", (const char *const[]){"--strip-names", NULL}, stripped,
                 sizeof stripped);
    const char *sargs[] = {"list", "--json", stripped, NULL};
    ASSERT_EQ(run_pack(sargs), DE_PACK_EXIT_OK);
    EXPECT_TRUE(json_valid(g_cct.out.p));
    EXPECT_EQ(count_of(g_cct.out.p, "\"name\": null"), 9u);
    EXPECT_TRUE(strstr(g_cct.out.p, "\"has_names\": false") != NULL);

    /* The text listing: a header, one line per entry, the bundles. */
    const char *targs[] = {"list", pak, NULL};
    ASSERT_EQ(run_pack(targs), DE_PACK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, "platform linux, 9 entries, 2 bundles") != NULL);
    for (size_t i = 0; i < sizeof k_ids / sizeof k_ids[0]; ++i)
        EXPECT_TRUE(strstr(g_cct.out.p, k_ids[i]) != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, "bundle core: 2 members, priority high, resident") != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, "bundle level_01: 5 members, priority normal, depends: core") !=
                NULL);
#endif
}

/* ---- test_pack_cli_extract --------------------------------------------------------------------
 */

#if DE_HAS_COOKER
/* Cooks `id` (and its sub-asset `sub`, or NULL) straight through a cooker, as de-cook sets one
 * up: the fixture's types, the ECS built-ins, the asset system's type descs. `*out` gets the
 * bytes (malloc'd). */
static void cook_direct(const char *id, const char *sub, u8 **out, size_t *size) {
    ASSERT_EQ(de_fs_mount("ccp", CCT_PROJECT, DE_MOUNT_READ_ONLY, 0), DE_OK);
    de_type_registry *types = NULL;
    ASSERT_EQ(de_type_registry_create(NULL, &types), DE_OK);
    de_world *w = NULL;
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = types}, &w), DE_OK);
    ASSERT_TRUE(DE_HANDLE_VALID(de_builtin_components(w).transform));
    de_asset_system *s = NULL;
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.types = types, .io_threads = 1}, &s),
              DE_OK);
    ASSERT_EQ(cct_register_types(NULL, s, types), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(s, "ccp:/assets.toml"), DE_OK);
    const de__manifest_asset *a = de__resolved_asset(s->resolved, de_asset_id_from(id));
    ASSERT_TRUE(a != NULL);
    de_cooker *c = NULL;
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.platform = DE_COOK_PLATFORM_LINUX}, &c), DE_OK);
    const de_asset_type_desc *td = de__asset_type_get(s, de__asset_type_find(s, a->type));
    ASSERT_TRUE(td != NULL);
    ASSERT_EQ(de_cooker_register_type(c, td), DE_OK);
    de_cook_result r;
    ASSERT_EQ(de_cooker_cook(c, &(de_cook_input){.asset = a, .asset_root = "ccp:/assets"}, &r),
              DE_OK);
    const void *data = r.data;
    u64 n = r.size;
    if (sub != NULL) {
        const de_cook_subasset *sa = de_cook_result_subasset(&r, sub);
        ASSERT_TRUE(sa != NULL);
        data = sa->data;
        n = sa->size;
    }
    *out = (u8 *)malloc((size_t)n + 1);
    ASSERT_TRUE(*out != NULL);
    memcpy(*out, data, (size_t)n);
    *size = (size_t)n;
    de_cook_result_free(&r);
    de_cooker_destroy(c);
    de_asset_system_destroy(s);
    de_world_destroy(w);
    de_type_registry_destroy(types);
    ASSERT_EQ(de_fs_unmount("ccp", CCT_PROJECT), DE_OK);
}

static void expect_file_equals_cook(const char *rel, const char *id, const char *sub) {
    char path[DE_FS_PATH_MAX];
    size_t n = 0, m = 0;
    u8 *got = cct_read(cct_path(path, sizeof path, rel), &n);
    ASSERT_TRUE(got != NULL);
    u8 *want = NULL;
    cook_direct(id, sub, &want, &m);
    ASSERT_EQ(n, m);
    EXPECT_MEMEQ(got, want, n);
    free(got);
    free(want);
}
#endif

/* Extracted bytes equal what the cooker produces for the asset; selection by path, ID and glob;
 * stripped packs name files by path or ID. */
TEST(pack_cli, extract) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    char pak[DE_FS_PATH_MAX], out[DE_FS_PATH_MAX];
    cook_fixture("extract", NULL, pak, sizeof pak);
    cct_path(out, sizeof out, "extract/out/a/b"); /* created with its parents */

    const char *by_path[] = {"extract", pak, "props/crate", "-o", out, NULL};
    ASSERT_EQ(run_pack(by_path), DE_PACK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, "1 entry written") != NULL);
    expect_file_equals_cook("extract/out/a/b/props/crate", "props/crate", NULL);

    const char *by_glob[] = {"extract", pak, "text/**", "-o", out, NULL};
    ASSERT_EQ(run_pack(by_glob), DE_PACK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, "4 entries written") != NULL);
    expect_file_equals_cook("extract/out/a/b/text/greeting", "text/greeting", NULL);
    expect_file_equals_cook("extract/out/a/b/text/greeting#lower", "text/greeting", "lower");

    char hex[32];
    snprintf(hex, sizeof hex, "0x%llx", (unsigned long long)de_asset_id_from("data/big").hash);
    const char *by_id[] = {"extract", pak, hex, "-o", out, NULL};
    ASSERT_EQ(run_pack(by_id), DE_PACK_EXIT_OK);
    expect_file_equals_cook("extract/out/a/b/data/big", "data/big", NULL);
    /* A blob is its source, byte for byte. */
    char path[DE_FS_PATH_MAX];
    size_t n = 0, m = 0;
    u8 *got = cct_read(cct_path(path, sizeof path, "extract/out/a/b/data/big"), &n);
    u8 *src = cct_read(CCT_PROJECT "/assets/data/big.txt", &m);
    ASSERT_TRUE(got != NULL && src != NULL);
    ASSERT_EQ(n, m);
    EXPECT_MEMEQ(got, src, n);
    free(got);
    free(src);

    /* `*` stops at `/`; nothing matched is exit 1. */
    const char *none[] = {"extract", pak, "*", "-o", out, NULL};
    EXPECT_EQ(run_pack(none), DE_PACK_EXIT_PROBLEMS);
    EXPECT_TRUE(strstr(g_cct.err.p, "matches") != NULL);
    const char *missing[] = {"extract", pak, "no/such/asset", "-o", out, NULL};
    EXPECT_EQ(run_pack(missing), DE_PACK_EXIT_PROBLEMS);

    /* A stripped pack: by path (the file is named by the path), by ID (named by the ID); a glob
     * needs names. */
    char stripped[DE_FS_PATH_MAX], sout[DE_FS_PATH_MAX];
    cook_fixture("extract_stripped", (const char *const[]){"--strip-names", NULL}, stripped,
                 sizeof stripped);
    cct_path(sout, sizeof sout, "extract_stripped/out");
    const char *s_path[] = {"extract", stripped, "levels/demo", "-o", sout, NULL};
    ASSERT_EQ(run_pack(s_path), DE_PACK_EXIT_OK);
    expect_file_equals_cook("extract_stripped/out/levels/demo", "levels/demo", NULL);
    snprintf(hex, sizeof hex, "0x%016llx", (unsigned long long)de_asset_id_from("data/hello").hash);
    const char *s_id[] = {"extract", stripped, hex, "-o", sout, NULL};
    ASSERT_EQ(run_pack(s_id), DE_PACK_EXIT_OK);
    char rel[128];
    snprintf(rel, sizeof rel, "extract_stripped/out/%016llx",
             (unsigned long long)de_asset_id_from("data/hello").hash);
    expect_file_equals_cook(rel, "data/hello", NULL);
    const char *s_glob[] = {"extract", stripped, "data/*", "-o", sout, NULL};
    EXPECT_EQ(run_pack(s_glob), DE_PACK_EXIT_PROBLEMS);
    EXPECT_TRUE(strstr(g_cct.err.p, "has no names") != NULL);
#endif
}

/* ---- Arguments --------------------------------------------------------------------------------
 */

TEST(pack_cli, usage) {
    const char *const cases[][6] = {
        {NULL},
        {"pack", "x.dpak", NULL},
        {"list", NULL},
        {"list", "a.dpak", "b.dpak", NULL},
        {"list", "a.dpak", "--yaml", NULL},
        {"extract", "a.dpak", NULL},
        {"extract", "a.dpak", "x", "-o", NULL},
        {"verify", NULL},
        {"verify", "a.dpak", "--fast", NULL},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        EXPECT_EQ(run_pack(cases[i]), DE_PACK_EXIT_USAGE);
        EXPECT_TRUE(strstr(g_cct.err.p, "usage: de-pack") != NULL);
    }
    const char *help[] = {"--help", NULL};
    EXPECT_EQ(run_pack(help), DE_PACK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, "verify <pak>") != NULL);
    const char *cmd_help[] = {"list", "-h", NULL};
    EXPECT_EQ(run_pack(cmd_help), DE_PACK_EXIT_OK);
    /* Not a pack at all: exit 1. */
    cct_put("usage/not_a_pack.dpak", "hello, this is text", 19);
    char path[DE_FS_PATH_MAX];
    const char *list[] = {"list", cct_path(path, sizeof path, "usage/not_a_pack.dpak"), NULL};
    EXPECT_EQ(run_pack(list), DE_PACK_EXIT_PROBLEMS);
    EXPECT_TRUE(strstr(g_cct.err.p, "not a valid pack") != NULL);
}
