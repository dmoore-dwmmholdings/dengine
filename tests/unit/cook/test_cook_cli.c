/*
 * test_cook_cli.c: `de-cook cook` (DENG-213, SPEC §11.5, §11.7). CTest names cook_cli.<test>.
 *
 * The tests run de_cook_cook_run in-process on the fixture project tests/data/cook_project/,
 * with the fixture's types registered through de_cook_hooks (cook_project_util.h). Outputs and
 * caches go to a scratch directory of the build tree (DE_COOK_CLI_TMP_DIR), one subdirectory per
 * test, since CTest runs the tests of one executable as parallel processes.
 *
 * Tool importers are available in every build type; all cases exercise the real
 * cooker even when the runtime has no in-process cooking support.
 */
#include "cook_project_util.h"

#include <dengine/de_task.h>

#include "asset/dpak/dpak_format.h"
#include "asset/dpak/dpak_reader.h"
#include "cook/lib/de_cooker.h"
#include "host/host_packs.h"

/* The asset IDs of the full project: 7 assets and 2 sub-assets. */
static const char *const k_project_ids[] = {
    "data/hello",    "data/big",      "levels/demo",         "props/barrel",        "props/crate",
    "text/greeting", "text/farewell", "text/greeting#lower", "text/farewell#lower",
};
#define PROJECT_ID_COUNT (sizeof k_project_ids / sizeof k_project_ids[0])

static u8 *read_pack(const char *sub, const char *name, size_t *size) {
    char rel[256], path[DE_FS_PATH_MAX];
    snprintf(rel, sizeof rel, "%s/%s.dpak", sub, name);
    return cct_read(cct_path(path, sizeof path, rel), size);
}

/* ---- test_cook_cli_identical_runs ------------------------------------------------------------ */

/* A cold cache, a warm cache, --jobs 1 with another cold cache, and no cache at all: four
 * byte-identical packs. The pack holds every asset and sub-asset and no host path. */
TEST(cook_cli, identical_runs) {
    cct_fresh("identical");
    ASSERT_EQ(cct_cook_project("identical", "cold", "linux", NULL), DE_COOK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, "assets cooked: 7 (+2 sub-assets), bundles: 2") != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, "cache hits: 0, misses: 7") != NULL);
    ASSERT_EQ(
        cct_cook_project("identical", "warm", "linux", (const char *const[]){"--jobs", "3", NULL}),
        DE_COOK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, "cache hits: 7, misses: 0") != NULL);

    /* --jobs 1 with a cold cache of its own. */
    char cache2[DE_FS_PATH_MAX];
    cct_path(cache2, sizeof cache2, "identical/cache_jobs1");
    char out[DE_FS_PATH_MAX];
    cct_path(out, sizeof out, "identical");
    const char *jobs1[] = {CCT_PROJECT "/assets.toml",
                           "--platform=linux",
                           "--out",
                           out,
                           "--cache",
                           cache2,
                           "--pack-name",
                           "jobs1",
                           "--jobs",
                           "1",
                           NULL};
    ASSERT_EQ(cct_cook(jobs1), DE_COOK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.err.p, "[7/7]") != NULL); /* progress counts manifest assets */
    EXPECT_TRUE(strstr(g_cct.out.p, "cache hits: 0, misses: 7") != NULL);
    /* No cache at all (--cache and --no-cache together are a usage error). */
    ASSERT_EQ(cct_cook_project("identical", "nocache", "linux",
                               (const char *const[]){"--no-cache", NULL}),
              DE_COOK_EXIT_USAGE);
    const char *nocache[] = {CCT_PROJECT "/assets.toml",
                             "--platform",
                             "linux",
                             "--out",
                             out,
                             "--no-cache",
                             "--pack-name",
                             "nocache",
                             "--quiet",
                             NULL};
    ASSERT_EQ(cct_cook(nocache), DE_COOK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, "cache: off") != NULL);

    size_t n0 = 0, n1 = 0, n2 = 0, n3 = 0;
    u8 *p0 = read_pack("identical", "cold", &n0);
    u8 *p1 = read_pack("identical", "warm", &n1);
    u8 *p2 = read_pack("identical", "jobs1", &n2);
    u8 *p3 = read_pack("identical", "nocache", &n3);
    ASSERT_TRUE(p0 != NULL && p1 != NULL && p2 != NULL && p3 != NULL);
    ASSERT_EQ(n0, n1);
    ASSERT_EQ(n0, n2);
    ASSERT_EQ(n0, n3);
    EXPECT_MEMEQ(p0, p1, n0);
    EXPECT_MEMEQ(p0, p2, n0);
    EXPECT_MEMEQ(p0, p3, n0);

    /* No host path, user name or project directory in the pack. */
    EXPECT_FALSE(cct_contains(p0, n0, CCT_TMP));
    EXPECT_FALSE(cct_contains(p0, n0, CCT_PROJECT));
    EXPECT_FALSE(cct_contains(p0, n0, "cook_project"));
    EXPECT_FALSE(cct_contains(p0, n0, "cook:/"));
    const char *user = getenv("USER");
    if (user != NULL && strlen(user) >= 4)
        EXPECT_FALSE(cct_contains(p0, n0, user));

    /* Contents: every asset and sub-asset, with types, dependencies, bundles and the platform. */
    dpak_reader *r = NULL;
    ASSERT_EQ(dpak_open_memory(p0, n0, &(dpak_open_desc){.verify = true}, &r), DE_OK);
    const dpak_header *h = dpak_get_header(r);
    EXPECT_EQ(h->platform, (u32)DPAK_PLATFORM_LINUX);
    EXPECT_EQ(dpak_entry_count(r), (u32)PROJECT_ID_COUNT);
    for (size_t i = 0; i < PROJECT_ID_COUNT; ++i) {
        u32 k = dpak_find(r, de_asset_id_from(k_project_ids[i]).hash);
        ASSERT_NE(k, DPAK_NOT_FOUND);
        EXPECT_STREQ(dpak_name(r, k, NULL), k_project_ids[i]);
    }
    dpak_toc_entry e;
    dpak_entry(r, dpak_find(r, de_asset_id_from("text/greeting").hash), &e);
    EXPECT_EQ(e.type_hash, de_hash64_str("cooktext"));
    ASSERT_EQ(e.dep_count, 1u);
    EXPECT_EQ(dpak_dep(r, e.dep_first), de_asset_id_from("data/hello").hash);
    dpak_entry(r, dpak_find(r, de_asset_id_from("props/crate").hash), &e);
    EXPECT_EQ(e.type_hash, de_hash64_str("prefab"));
    ASSERT_EQ(e.dep_count, 1u); /* CookSpin.icon names data/hello */
    EXPECT_EQ(dpak_dep(r, e.dep_first), de_asset_id_from("data/hello").hash);
    dpak_entry(r, dpak_find(r, de_asset_id_from("text/greeting#lower").hash), &e);
    EXPECT_TRUE((e.flags & DPAK_ENTRY_SUB_ASSET) != 0);
    dpak_entry(r, dpak_find(r, de_asset_id_from("data/big").hash), &e);
    EXPECT_EQ(e.chunk_count, 2u); /* 88 KB: two 64 KiB chunks */
    u32 b = dpak_bundle_find(r, de_hash64_str("core"));
    ASSERT_NE(b, DPAK_NOT_FOUND);
    dpak_bundle_entry be;
    dpak_bundle(r, b, &be);
    EXPECT_EQ(be.member_count, 2u);
    EXPECT_TRUE((be.flags & DPAK_BUNDLE_RESIDENT) != 0);
    EXPECT_EQ(be.priority, (u32)DPAK_PRIORITY_HIGH);
    b = dpak_bundle_find(r, de_hash64_str("level_01"));
    ASSERT_NE(b, DPAK_NOT_FOUND);
    dpak_bundle(r, b, &be);
    EXPECT_EQ(be.member_count, 5u);
    ASSERT_EQ(be.depend_count, 1u);
    EXPECT_EQ(dpak_bundle_id(r, be.depend_first), de_hash64_str("core"));

    /* The derived build ID: XXH3-128 of the pack with a zero build ID field, not all zero. */
    u8 zero[16] = {0};
    EXPECT_TRUE(memcmp(h->build_id, zero, 16) != 0);
    dpak_close(r);

    /* Another platform: another pack (header and build ID), same entries. */
    ASSERT_EQ(cct_cook_project("identical", "mac", "macos", NULL), DE_COOK_EXIT_OK);
    size_t nm = 0;
    u8 *pm = read_pack("identical", "mac", &nm);
    ASSERT_TRUE(pm != NULL);
    EXPECT_TRUE(nm != n0 || memcmp(pm, p0, n0) != 0);
    free(pm);
    free(p0);
    free(p1);
    free(p2);
    free(p3);
}

/* ---- test_cook_cli_validation_abort ---------------------------------------------------------- */

/* Manifest errors abort with exit 1, print what --check prints (byte for byte, on stdout), and
 * write no pack. A failing import also exits 1 and names the asset; so does a type without an
 * importer. */
TEST(cook_cli, validation_abort) {
    cct_fresh("abort");
    char out[DE_FS_PATH_MAX];
    cct_path(out, sizeof out, "abort/out");
    const char *args[] = {DE_COOK_TEST_DATA_DIR "/four_issues/assets.toml",
                          "--platform",
                          "windows",
                          "--out",
                          out,
                          "--no-cache",
                          NULL};
    ASSERT_EQ(cct_cook(args), DE_COOK_EXIT_ERRORS);
    char *cook_out = g_cct.out.p;
    g_cct.out.p = NULL;
    EXPECT_TRUE(strstr(g_cct.err.p, "nothing was cooked") != NULL);
    const char *check_args[] = {DE_COOK_TEST_DATA_DIR "/four_issues/assets.toml", NULL};
    cct_reset();
    de_cook_io io = {cct_write, NULL};
    ASSERT_EQ(de_cook_check_main(1, check_args, &io), DE_COOK_EXIT_ERRORS);
    EXPECT_STREQ(cook_out, g_cct.out.p);
    EXPECT_TRUE(strstr(cook_out, "four_issues/assets.toml:8: error: asset \"fx/spark\": unknown "
                                 "asset type \"gizmo\"") != NULL);
    free(cook_out);
    EXPECT_FALSE(de_path_exists("cct:/abort/out/game.dpak"));

    /* A failing import: exit 1, the asset is named, no pack. */
    static const char manifest[] =
        "[project]\nasset_root = \"assets\"\n[assets]\n"
        "\"ok\" = { type = \"cooktext\", source = \"ok.txt\" }\n"
        "\"bad\" = { type = \"cooktext\", source = \"bad.txt\", fail = true }\n";
    cct_put("abort/proj/assets.toml", manifest, sizeof manifest - 1);
    cct_put("abort/proj/assets/ok.txt", "fine", 4);
    cct_put("abort/proj/assets/bad.txt", "broken", 6);
    char mpath[DE_FS_PATH_MAX];
    cct_path(mpath, sizeof mpath, "abort/proj/assets.toml");
    const char *fail_args[] = {mpath, "--platform", "linux", "--out", out, "--no-cache", NULL};
    ASSERT_EQ(cct_cook(fail_args), DE_COOK_EXIT_ERRORS);
    EXPECT_TRUE(strstr(g_cct.err.p, "asset \"bad\"") != NULL);
    EXPECT_TRUE(strstr(g_cct.err.p, "asked to fail") != NULL);
    EXPECT_TRUE(strstr(g_cct.err.p, "1 of 2 assets failed to cook; no pack written") != NULL);
    EXPECT_FALSE(de_path_exists("cct:/abort/out/game.dpak"));

    /* Video remains a known engine type without a built-in importer. */
    static const char tex[] = "[project]\nasset_root = \"assets\"\n[assets]\n"
                              "\"t\" = { type = \"video\", source = \"ok.txt\" }\n";
    cct_put("abort/proj/tex.toml", tex, sizeof tex - 1);
    cct_path(mpath, sizeof mpath, "abort/proj/tex.toml");
    const char *tex_args[] = {mpath, "--platform", "linux", "--out", out, "--no-cache", NULL};
    ASSERT_EQ(cct_cook(tex_args), DE_COOK_EXIT_ERRORS);
    EXPECT_TRUE(strstr(g_cct.err.p, "no importer for type \"video\"") != NULL);
    EXPECT_FALSE(de_path_exists("cct:/abort/out/game.dpak"));

    /* Without the hooks, the project's own type is unknown to the check: exit 1. */
    cct_reset();
    const char *proj_args[] = {
        CCT_PROJECT "/assets.toml", "--platform", "linux", "--out", out, "--no-cache", NULL};
    ASSERT_EQ(de_cook_cook_main(cct_argc(proj_args), proj_args, &io), DE_COOK_EXIT_ERRORS);
    EXPECT_TRUE(strstr(g_cct.out.p, "unknown asset type \"cooktext\"") != NULL);
}

/* ---- test_cook_cli_strip_names --------------------------------------------------------------- */

/* --strip-names leaves every asset path out (bundle names stay); IDs, types and data are the
 * same as in the named pack. --build-id is stored as given. */
TEST(cook_cli, strip_names) {
    cct_fresh("strip");
    ASSERT_EQ(cct_cook_project("strip", "named", "linux", NULL), DE_COOK_EXIT_OK);
    ASSERT_EQ(cct_cook_project("strip", "stripped", "linux",
                               (const char *const[]){"--strip-names", "--build-id",
                                                     "00112233445566778899aabbccddeeff", NULL}),
              DE_COOK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, "build id: 00112233445566778899aabbccddeeff") != NULL);
    size_t nn = 0, ns = 0;
    u8 *pn = read_pack("strip", "named", &nn);
    u8 *ps = read_pack("strip", "stripped", &ns);
    ASSERT_TRUE(pn != NULL && ps != NULL);
    EXPECT_TRUE(ns <= nn); /* assets start on 4 KiB boundaries, so often equal */
    for (size_t i = 0; i < PROJECT_ID_COUNT; ++i)
        EXPECT_FALSE(cct_contains(ps, ns, k_project_ids[i]));
    EXPECT_TRUE(cct_contains(ps, ns, "level_01")); /* bundle names are always stored */

    dpak_reader *rn = NULL, *rs = NULL;
    ASSERT_EQ(dpak_open_memory(pn, nn, &(dpak_open_desc){.verify = true}, &rn), DE_OK);
    ASSERT_EQ(dpak_open_memory(ps, ns, &(dpak_open_desc){.verify = true}, &rs), DE_OK);
    const dpak_header *hs = dpak_get_header(rs);
    EXPECT_EQ(hs->flags & DPAK_HEADER_HAS_NAMES, 0u);
    static const u8 want_id[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                   0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    EXPECT_MEMEQ(hs->build_id, want_id, 16);
    ASSERT_EQ(dpak_entry_count(rs), dpak_entry_count(rn));
    for (u32 i = 0; i < dpak_entry_count(rs); ++i) {
        dpak_toc_entry a, b;
        dpak_entry(rn, i, &a);
        dpak_entry(rs, i, &b);
        EXPECT_EQ(a.id, b.id);
        EXPECT_EQ(a.type_hash, b.type_hash);
        EXPECT_EQ(a.raw_size, b.raw_size);
        EXPECT_EQ(a.dep_count, b.dep_count);
        EXPECT_TRUE(dpak_name(rs, i, NULL) == NULL);
        u8 *da = (u8 *)malloc((size_t)a.raw_size + 1), *db = (u8 *)malloc((size_t)b.raw_size + 1);
        ASSERT_TRUE(da != NULL && db != NULL);
        ASSERT_EQ(dpak_read_entry(rn, i, NULL, da, a.raw_size), DE_OK);
        ASSERT_EQ(dpak_read_entry(rs, i, NULL, db, b.raw_size), DE_OK);
        EXPECT_MEMEQ(da, db, (size_t)a.raw_size);
        free(da);
        free(db);
    }
    dpak_close(rn);
    dpak_close(rs);
    free(pn);
    free(ps);
}

/* ---- Arguments ------------------------------------------------------------------------------- */

TEST(cook_cli, usage) {
    cct_mount();
    char out[DE_FS_PATH_MAX];
    cct_path(out, sizeof out, "usage");
    const char *const cases[][12] = {
        {NULL},
        {CCT_PROJECT "/assets.toml", "--out", out, NULL},          /* no platform */
        {CCT_PROJECT "/assets.toml", "--platform", "linux", NULL}, /* no --out */
        {CCT_PROJECT "/assets.toml", "--platform", "amiga", "--out", out, NULL},
        {CCT_PROJECT "/assets.toml", "--platform", "linux", "--out", out, "--jobs", "0", NULL},
        {CCT_PROJECT "/assets.toml", "--platform", "linux", "--out", out, "--jobs", "2x", NULL},
        {CCT_PROJECT "/assets.toml", "--platform", "linux", "--out", out, "--build-id", "abc",
         NULL},
        {CCT_PROJECT "/assets.toml", "--platform", "linux", "--out", out, "--pack-name", "../x",
         NULL},
        {CCT_PROJECT "/assets.toml", "--platform", "linux", "--out", out, "--bogus", NULL},
        {CCT_PROJECT "/assets.toml", "a.toml", "--platform", "linux", "--out", out, NULL},
        {CCT_PROJECT "/assets.toml", "--platform", NULL},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        EXPECT_EQ(cct_cook(cases[i]), DE_COOK_EXIT_USAGE);
        EXPECT_TRUE(strstr(g_cct.err.p, "usage: de-cook cook") != NULL);
    }
    const char *help[] = {"--help", NULL};
    EXPECT_EQ(cct_cook(help), DE_COOK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, "--strip-names") != NULL);
    const char *missing[] = {"no/such/assets.toml", "--platform", "linux", "--out", out, NULL};
    EXPECT_EQ(cct_cook(missing), DE_COOK_EXIT_USAGE);
    EXPECT_TRUE(strstr(g_cct.err.p, "cannot") != NULL);
}

/* ---- The packed host: game:/ packs ------------------------------------------------------------
 */

/* de__host_mount_packs (what a PACKED engine does with game:/) mounts every .dpak of a directory
 * in name order: assets load from the packs with no manifest, and a later name overrides an
 * earlier one. */
TEST(cook_cli, host_mounts_packs) {
    cct_fresh("host");
    const char *host = de_cook_platform_name(de_cook_platform_host());
    ASSERT_EQ(cct_cook_project("host", "game", host, NULL), DE_COOK_EXIT_OK);
    /* A patch pack that replaces data/hello. */
    static const char patch[] = "[project]\nasset_root = \"assets\"\n[assets]\n"
                                "\"data/hello\" = { type = \"blob\", source = \"hello.txt\" }\n";
    cct_put("host/patch_src/assets.toml", patch, sizeof patch - 1);
    cct_put("host/patch_src/assets/hello.txt", "patched", 7);
    cct_put("host/game.txt", "not a pack", 10); /* ignored: not .dpak */
    char mpath[DE_FS_PATH_MAX], out[DE_FS_PATH_MAX];
    cct_path(mpath, sizeof mpath, "host/patch_src/assets.toml");
    cct_path(out, sizeof out, "host");
    const char *pargs[] = {mpath,        "--platform",  host,          "--out", out,
                           "--no-cache", "--pack-name", "game_patch1", NULL};
    ASSERT_EQ(cct_cook(pargs), DE_COOK_EXIT_OK);

    de_fp_env_init_thread();
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &ts), DE_OK);
    de_type_registry *types = NULL;
    ASSERT_EQ(de_type_registry_create(NULL, &types), DE_OK);
    de_asset_system *s = NULL;
    ASSERT_EQ(de_asset_system_create(
                  &(de_asset_system_desc){
                      .tasks = ts, .mode = DE_ASSET_MODE_PACKED, .io_threads = 1, .types = types},
                  &s),
              DE_OK);
    ASSERT_EQ(cct_register_types(NULL, s, types), DE_OK);
    u32 count = 0;
    ASSERT_EQ(de__host_mount_packs(s, "cct:/host", NULL, &count), DE_OK);
    EXPECT_EQ(count, 2u);

    /* data/hello comes from the patch (mounted later: "game_patch1" > "game"). */
    de_load_handle h = de_asset_load(s, de_asset_id_from("data/hello"), 0);
    ASSERT_EQ((int)de_asset_wait(s, h, (u64)60 * 1000000000u), (int)DE_LOAD_READY);
    u64 n = 0;
    const char *bytes =
        (const char *)de_blob_data(s, de_asset_blob(s, de_asset_id_from("data/hello")), &n);
    ASSERT_TRUE(bytes != NULL);
    EXPECT_EQ(n, 7u);
    EXPECT_MEMEQ(bytes, "patched", 7);
    de_asset_release(s, h);

    /* A bundle of the base pack, with its dependency bundle, and no manifest. */
    h = de_asset_load_bundle(s, "level_01", 0);
    ASSERT_EQ((int)de_asset_wait(s, h, (u64)60 * 1000000000u), (int)DE_LOAD_READY);
    EXPECT_TRUE(DE_HANDLE_VALID(de_asset_scene(s, de_asset_id_from("levels/demo"))));
    bytes = (const char *)de_blob_data(s, de_asset_blob(s, de_asset_id_from("data/big")), &n);
    ASSERT_TRUE(bytes != NULL);
    EXPECT_EQ(n, 88332u);
    de_asset_release(s, h);

    /* A directory without packs mounts none; a missing one is DE_ERR_NOT_FOUND. */
    cct_put("host/empty/readme.txt", "x", 1);
    EXPECT_EQ(de__host_mount_packs(s, "cct:/host/empty", NULL, &count), DE_OK);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(de__host_mount_packs(s, "cct:/host/none", NULL, &count), DE_ERR_NOT_FOUND);
    de_error_clear();

    de_asset_system_destroy(s);
    de_type_registry_destroy(types);
    de_task_system_destroy(ts);
}
