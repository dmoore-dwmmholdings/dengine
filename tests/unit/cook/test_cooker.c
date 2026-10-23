/*
 * test_cooker.c: the cooker library (DENG-209, SPEC §11.5, §11.6): the type registry, cooking with
 * a test importer, sub-assets, dependencies, the determinism check, and (DE_HAS_COOKER builds)
 * the runtime pipeline cooking a source asset in-process. CTest names cooker.<test>.
 *
 * Sources are written into a scratch directory of the build tree, mounted as ckt:/.
 */
#include "cook_test_util.h"
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include "asset/manifest_internal.h"
#include "asset/slots_internal.h"
#include "cook/lib/de_cooker.h"
#include "platform/de_atomic.h"

#include <stdio.h>
#include <string.h>

#define WAIT_NS ((u64)60 * 1000000000u)

/* ---- Fixture: a cooker on a tracking allocator ------------------------------------------------
 */

typedef struct cook_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_cooker *c;
    const char *root; /* each test has its own directory: CTest runs tests in parallel */
} cook_fx;

static void cook_fx_setup(cook_fx *f) {
    ckt_mount();
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    de_cooker_desc d = {.allocator = &f->alloc, .verify_determinism = DE_COOK_VERIFY_ON};
    ASSERT_EQ(de_cooker_create(&d, &f->c), DE_OK);
    de_asset_type_desc t = ckt_upper_type(NULL);
    ASSERT_EQ(de_cooker_register_type(f->c, &t), DE_OK);
}

static void cook_fx_teardown(cook_fx *f) {
    de_cooker_destroy(f->c);
    f->c = NULL;
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
}

static de_result cook(cook_fx *f, const de__manifest_asset *a, de_cook_result *r) {
    de_cook_input in = {.asset = a, .asset_root = f->root};
    return de_cooker_cook(f->c, &in, r);
}

/* ---- cooker.registry ---------------------------------------------------------------------- */

TEST_F(cooker, registry, cook_fx) {
    f->root = "ckt:/ck_registry/assets";
    EXPECT_TRUE(de_cooker_has_type(f->c, "upper"));
    EXPECT_FALSE(de_cooker_has_type(f->c, "lower"));
    EXPECT_EQ(de_cooker_importer_version(f->c, "upper"), 7u);
    EXPECT_EQ(de_cooker_importer_version(f->c, "nope"), 0u);

    de_asset_type_desc t = ckt_upper_type(NULL);
    EXPECT_EQ(de_cooker_register_type(f->c, &t), DE_ERR_ALREADY_EXISTS);
    const char *bad[] = {"", "has space", "quo\"te", "back\\slash",
                         "0123456789012345678901234567890123456789012345678901234567890123"};
    for (u32 i = 0; i < 5; ++i) {
        t.name = bad[i];
        EXPECT_EQ(de_cooker_register_type(f->c, &t), DE_ERR_INVALID_ARGUMENT);
    }
    t.name = "blob";
    t.import = NULL; /* a pass-through type */
    EXPECT_EQ(de_cooker_register_type(f->c, &t), DE_OK);
    const char *names[4] = {0};
    EXPECT_EQ(de_cooker_type_names(f->c, names, 4), 2u);
    EXPECT_STREQ(names[0], "upper");
    EXPECT_STREQ(names[1], "blob");
    EXPECT_EQ(de_cooker_type_names(f->c, NULL, 0), 2u);

    u32 n = 0;
    const char *const *engine = de_cooker_engine_type_names(&n);
    /* Independent contract: preserve registration order and the DENG-190 sprite type. */
    const char *const expected[] = {"texture", "model",      "material",     "shader",
                                    "sound",   "font",       "aseprite",     "tilemap",
                                    "strings", "stylesheet", "video",        "scene",
                                    "prefab",  "blob",       "string_table", "sprite_clip"};
    ASSERT_EQ(n, (u32)(sizeof expected / sizeof expected[0]));
    for (u32 i = 0; i < n; ++i) {
        EXPECT_STREQ(engine[i], expected[i]);
        EXPECT_TRUE(de_cooker_is_engine_type(expected[i]));
        for (u32 j = 0; j < i; ++j)
            EXPECT_TRUE(strcmp(engine[i], engine[j]) != 0);
    }
    EXPECT_FALSE(de_cooker_is_engine_type("Sprite_clip"));
    /* Tool availability is independent of the runtime DE_HAS_COOKER switch. */
    const char *tool_types[] = {"scene", "prefab", "sprite_clip"};
    for (u32 i = 0; i < sizeof tool_types / sizeof tool_types[0]; ++i) {
        de_asset_type_desc builtin;
        ASSERT_TRUE(de_cooker_builtin_importer(tool_types[i], &builtin));
        EXPECT_STREQ(builtin.name, tool_types[i]);
        EXPECT_TRUE(builtin.import != NULL);
        EXPECT_TRUE(builtin.load == NULL);
        EXPECT_TRUE(builtin.user == NULL);
    }
    EXPECT_FALSE(de_cooker_is_engine_type("upper"));

    EXPECT_EQ((int)de_cooker_platform(f->c), (int)de_cook_platform_host());
    EXPECT_NE((int)de_cooker_platform(f->c), (int)DE_COOK_PLATFORM_HOST);
    EXPECT_TRUE(de_cooker_verifies(f->c));
    EXPECT_STREQ(de_cook_platform_name(DE_COOK_PLATFORM_LINUX), "linux");
    de_cooker *c2 = NULL;
    EXPECT_EQ(de_cooker_create(&(de_cooker_desc){.platform = (de_cook_platform)9}, &c2),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_cooker_create(&(de_cooker_desc){.platform = DE_COOK_PLATFORM_WINDOWS,
                                                 .cache_dir = "cache:/decache"},
                               &c2),
              DE_OK);
    EXPECT_EQ((int)de_cooker_platform(c2), (int)DE_COOK_PLATFORM_WINDOWS);
    EXPECT_STREQ(de_cooker_cache_dir(c2), "cache:/decache");
    de_cooker_destroy(c2);
}

/* ---- cooker.test_importer_roundtrip ---------------------------------------------------------- */

TEST_F(cooker, test_importer_roundtrip, cook_fx) {
    f->root = "ckt:/ck_test_importer_roundtrip/assets";
    ckt_put("ck_test_importer_roundtrip/assets/text/hello.txt", "hello world");
    const char *const items[] = {"x", "y"};
    de__manifest_kv kv[] = {
        {"level", {.type = DE__MANIFEST_INT, .u.i = 3}},
        {"sub", {.type = DE__MANIFEST_BOOL, .u.b = false}},
        {"tags", {.type = DE__MANIFEST_STRING_ARRAY, .u.arr = {items, 2}}},
    };
    de__manifest_asset a = ckt_asset("text/hello", "upper", "text/hello.txt", kv, 3);
    de_cook_result r;
    ASSERT_EQ(cook(f, &a, &r), DE_OK);
    EXPECT_EQ(r.size, 11u);
    EXPECT_MEMEQ(r.data, "HELLO WORLD", 11);
    EXPECT_STREQ(r.source, "text/hello.txt");
    EXPECT_EQ(r.dependency_count, 0u);
    EXPECT_EQ(r.subasset_count, 0u);
    EXPECT_EQ(r.extra_source_count, 0u);
    EXPECT_EQ(r.settings_hash, de_cook_settings_hash(kv, 3));
    EXPECT_NE(r.settings_hash, de_cook_settings_hash(NULL, 0));
    EXPECT_EQ(r.importer_version, 7u);
    EXPECT_EQ((int)r.platform, (int)de_cook_platform_host());
    u64 first = r.output_hash;
    de_cook_result_free(&r);
    EXPECT_TRUE(r.data == NULL);
    de_cook_result_free(&r); /* a zeroed result is ignored */

    /* Same inputs, same bytes and hash. */
    ASSERT_EQ(cook(f, &a, &r), DE_OK);
    EXPECT_EQ(r.output_hash, first);
    de_cook_result_free(&r);

    /* A locale override cooks another source under the same ID. */
    ckt_put("ck_test_importer_roundtrip/assets/text/hello_ja.txt", "konnichiwa");
    de_cook_input in = {.asset = &a,
                        .asset_root = "ckt:/ck_test_importer_roundtrip/assets",
                        .source = "text/hello_ja.txt"};
    ASSERT_EQ(de_cooker_cook(f->c, &in, &r), DE_OK);
    EXPECT_MEMEQ(r.data, "KONNICHIWA", 10);
    EXPECT_STREQ(r.source, "text/hello_ja.txt");
    EXPECT_NE(r.output_hash, first);
    de_cook_result_free(&r);

    /* A type without an importer: the source bytes as they are. */
    de_asset_type_desc t = {.name = "raw"};
    ASSERT_EQ(de_cooker_register_type(f->c, &t), DE_OK);
    a.type = "raw";
    ASSERT_EQ(cook(f, &a, &r), DE_OK);
    EXPECT_EQ(r.size, 11u);
    EXPECT_MEMEQ(r.data, "hello world", 11);
    EXPECT_EQ(r.importer_version, 0u);
    de_cook_result_free(&r);

    /* An empty source cooks to empty bytes (never NULL). */
    ckt_put("ck_test_importer_roundtrip/assets/empty.txt", "");
    a = ckt_asset("empty", "upper", "empty.txt", NULL, 0);
    ASSERT_EQ(cook(f, &a, &r), DE_OK);
    EXPECT_EQ(r.size, 0u);
    EXPECT_TRUE(r.data != NULL);
    de_cook_result_free(&r);
}

TEST_F(cooker, failures, cook_fx) {
    f->root = "ckt:/ck_failures/assets";
    ckt_put("ck_failures/assets/text/hello.txt", "hello world");
    de_cook_result r;
    de__manifest_asset a = ckt_asset("text/hello", "nobody", "text/hello.txt", NULL, 0);
    EXPECT_EQ(cook(f, &a, &r), DE_ERR_NOT_FOUND); /* type not registered */
    EXPECT_TRUE(strstr(de_error_last()->message, "nobody") != NULL);
    EXPECT_TRUE(r.data == NULL);

    a = ckt_asset("text/missing", "upper", "text/missing.txt", NULL, 0);
    EXPECT_EQ(cook(f, &a, &r), DE_ERR_NOT_FOUND);
    EXPECT_TRUE(strstr(de_error_last()->message, "text/missing.txt") != NULL);

    const char *bad_sources[] = {"../outside.txt", "/abs.txt", "ckt:/ck_failures/x.txt", "a\\b.txt",
                                 ""};
    for (u32 i = 0; i < 5; ++i) {
        a = ckt_asset("text/bad", "upper", bad_sources[i], NULL, 0);
        EXPECT_EQ(cook(f, &a, &r), DE_ERR_INVALID_ARGUMENT);
    }
    a = ckt_asset("bad//id", "upper", "text/hello.txt", NULL, 0);
    EXPECT_EQ(cook(f, &a, &r), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_cooker_cook(f->c, NULL, &r), DE_ERR_INVALID_ARGUMENT);
    de_cook_input in = {.asset = &a, .asset_root = "not a vpath"};
    a = ckt_asset("text/hello", "upper", "text/hello.txt", NULL, 0);
    EXPECT_EQ(de_cooker_cook(f->c, &in, &r), DE_ERR_INVALID_ARGUMENT);
}

/* ---- cooker.subasset ---------------------------------------------------------------------- */

static de_result two_subs_import(de_asset_import_ctx *ctx) {
    DE_TRY(de_asset_import_write(ctx, "parent", 6));
    DE_TRY(de_asset_import_add_subasset(ctx, "mesh/0", "mesh", "m0", 2));
    DE_TRY(de_asset_import_add_subasset(ctx, "skel", "skeleton", NULL, 0));
    /* Refused, and not fatal for the import. */
    if (de_asset_import_add_subasset(ctx, "skel", "skeleton", "x", 1) != DE_ERR_ALREADY_EXISTS)
        return de_asset_import_fail(ctx, DE_ERR_INTERNAL, "duplicate accepted");
    if (de_asset_import_add_subasset(ctx, "a#b", "mesh", "x", 1) != DE_ERR_INVALID_ARGUMENT ||
        de_asset_import_add_subasset(ctx, "", "mesh", "x", 1) != DE_ERR_INVALID_ARGUMENT ||
        de_asset_import_add_subasset(ctx, "n", "", "x", 1) != DE_ERR_INVALID_ARGUMENT ||
        de_asset_import_add_subasset(ctx, "bad\x01", "mesh", "x", 1) != DE_ERR_INVALID_ARGUMENT ||
        de_asset_import_add_subasset(ctx, "n", "mesh", NULL, 1) != DE_ERR_INVALID_ARGUMENT)
        return de_asset_import_fail(ctx, DE_ERR_INTERNAL, "bad sub-asset accepted");
    return DE_OK;
}

TEST_F(cooker, subasset, cook_fx) {
    f->root = "ckt:/ck_subasset/assets";
    ckt_put("ck_subasset/assets/text/hello.txt", "Hello");
    de__manifest_asset a = ckt_asset("text/hello", "upper", "text/hello.txt", NULL, 0);
    de_cook_result r;
    ASSERT_EQ(cook(f, &a, &r), DE_OK);
    ASSERT_EQ(r.subasset_count, 1u);
    const de_cook_subasset *s = de_cook_result_subasset(&r, "lower");
    ASSERT_TRUE(s != NULL);
    EXPECT_STREQ(s->name, "lower");
    EXPECT_STREQ(s->type, "txt");
    EXPECT_EQ(s->id.hash, de_asset_id_from("text/hello#lower").hash);
    EXPECT_EQ(s->size, 5u);
    EXPECT_MEMEQ(s->data, "Hello", 5);
    EXPECT_TRUE(de_cook_result_subasset(&r, "upper") == NULL);
    de_cook_result_free(&r);

    de_asset_type_desc t = {.name = "multi", .import = two_subs_import};
    ASSERT_EQ(de_cooker_register_type(f->c, &t), DE_OK);
    a = ckt_asset("model/robot", "multi", "text/hello.txt", NULL, 0);
    ASSERT_EQ(cook(f, &a, &r), DE_OK);
    EXPECT_MEMEQ(r.data, "parent", 6);
    ASSERT_EQ(r.subasset_count, 2u);
    EXPECT_STREQ(r.subassets[0].name, "mesh/0"); /* declaration order */
    EXPECT_EQ(r.subassets[0].id.hash, de_asset_id_from("model/robot#mesh/0").hash);
    EXPECT_MEMEQ(r.subassets[0].data, "m0", 2);
    EXPECT_STREQ(r.subassets[1].name, "skel");
    EXPECT_STREQ(r.subassets[1].type, "skeleton");
    EXPECT_EQ(r.subassets[1].size, 0u);
    EXPECT_TRUE(r.subassets[1].data != NULL);
    de_cook_result_free(&r);
}

/* ---- cooker.dependency -------------------------------------------------------------------- */

static de_result deps_import(de_asset_import_ctx *ctx) {
    DE_TRY(de_asset_import_add_dependency(ctx, de_asset_id_from("tex/b")));
    DE_TRY(de_asset_import_add_dependency(ctx, de_asset_id_from("tex/a")));
    DE_TRY(de_asset_import_add_dependency(ctx, de_asset_id_from("tex/b"))); /* counts once */
    if (de_asset_import_add_dependency(ctx, (de_asset_id){0}) != DE_ERR_INVALID_ARGUMENT)
        return de_asset_import_fail(ctx, DE_ERR_INTERNAL, "ID 0 accepted");
    return DE_OK;
}

TEST_F(cooker, dependency, cook_fx) {
    f->root = "ckt:/ck_dependency/assets";
    ckt_put("ck_dependency/assets/text/hello.txt", "hello");
    const de__manifest_kv kv[] = {{"dep", {.type = DE__MANIFEST_STRING, .u.s = "text/other"}}};
    de__manifest_asset a = ckt_asset("text/hello", "upper", "text/hello.txt", kv, 1);
    de_cook_result r;
    ASSERT_EQ(cook(f, &a, &r), DE_OK);
    ASSERT_EQ(r.dependency_count, 1u);
    EXPECT_EQ(r.dependencies[0].hash, de_asset_id_from("text/other").hash);
    de_cook_result_free(&r);

    de_asset_type_desc t = {.name = "mat", .import = deps_import};
    ASSERT_EQ(de_cooker_register_type(f->c, &t), DE_OK);
    a = ckt_asset("mat/stone", "mat", "text/hello.txt", NULL, 0);
    ASSERT_EQ(cook(f, &a, &r), DE_OK);
    ASSERT_EQ(r.dependency_count, 2u);
    EXPECT_EQ(r.dependencies[0].hash, de_asset_id_from("tex/b").hash); /* declaration order */
    EXPECT_EQ(r.dependencies[1].hash, de_asset_id_from("tex/a").hash);
    EXPECT_EQ(r.size, 0u);
    de_cook_result_free(&r);
}

/* ---- cooker.verify_determinism_catches ------------------------------------------------------ */

static de_atomic_u32 g_counter;

/* Writes a counter that moves on every call: the textbook nondeterministic importer. */
static de_result counter_import(de_asset_import_ctx *ctx) {
    u32 v = de_atomic_fetch_add_u32(&g_counter, 1, DE_ATOMIC_ACQ_REL);
    return de_asset_import_write(ctx, &v, sizeof v);
}

TEST_F(cooker, verify_determinism_catches, cook_fx) {
    f->root = "ckt:/ck_verify_determinism_catches/assets";
    ckt_put("ck_verify_determinism_catches/assets/text/hello.txt", "hello");
    de_atomic_init_u32(&g_counter, 0);
    de_asset_type_desc t = {.name = "counter", .import = counter_import};
    ASSERT_EQ(de_cooker_register_type(f->c, &t), DE_OK);
    de__manifest_asset a = ckt_asset("text/count", "counter", "text/hello.txt", NULL, 0);
    de_cook_result r;
    EXPECT_EQ(cook(f, &a, &r), DE_ERR_INTERNAL);
    EXPECT_TRUE(strstr(de_error_last()->message, "not deterministic") != NULL);
    EXPECT_TRUE(strstr(de_error_last()->message, "text/count") != NULL);
    EXPECT_TRUE(r.data == NULL);
    EXPECT_EQ(de_atomic_load_u32(&g_counter, DE_ATOMIC_ACQUIRE), 2u); /* it ran twice */

    /* A deterministic importer passes the same check. */
    a = ckt_asset("text/hello", "upper", "text/hello.txt", NULL, 0);
    ASSERT_EQ(cook(f, &a, &r), DE_OK);
    de_cook_result_free(&r);

    /* With the check off the counter cooks (once). */
    de_cooker *c = NULL;
    de_cooker_desc d = {.allocator = &f->alloc, .verify_determinism = DE_COOK_VERIFY_OFF};
    ASSERT_EQ(de_cooker_create(&d, &c), DE_OK);
    ASSERT_FALSE(de_cooker_verifies(c));
    ASSERT_EQ(de_cooker_register_type(c, &t), DE_OK);
    a = ckt_asset("text/count", "counter", "text/hello.txt", NULL, 0);
    de_cook_input in = {.asset = &a, .asset_root = "ckt:/ck_verify_determinism_catches/assets"};
    EXPECT_EQ(de_cooker_cook(c, &in, &r), DE_OK);
    EXPECT_EQ(de_atomic_load_u32(&g_counter, DE_ATOMIC_ACQUIRE), 3u);
    de_cook_result_free(&r);
    de_cooker_destroy(c);

    /* The default follows the build: on in Debug, off otherwise. */
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.allocator = &f->alloc}, &c), DE_OK);
#if defined(DE_DEBUG)
    EXPECT_TRUE(de_cooker_verifies(c));
#else
    EXPECT_FALSE(de_cooker_verifies(c));
#endif
    de_cooker_destroy(c);
}

/* ---- cooker.runtime_source_mode (DE_HAS_COOKER) ---------------------------------------------
 *
 * The acceptance test of the runtime integration: in SOURCE mode the asset pipeline cooks an
 * `upper` asset in-process, loads the cooked bytes, loads the dependency the importer declared,
 * and then loads the sub-asset `text/hello#lower` with the `txt` loader. */

#if DE_HAS_COOKER
typedef struct rt_obj {
    u64 size;
    char text[64];
} rt_obj;

typedef struct rt_state {
    const de_allocator *alloc;
    de_atomic_u32 loads, unloads;
} rt_state;

static de_result rt_load(de_asset_load_ctx *ctx) {
    rt_state *st = (rt_state *)de_asset_load_ctx_user(ctx);
    u64 n = 0;
    const char *p = (const char *)de_asset_load_ctx_data(ctx, &n);
    rt_obj *o = (rt_obj *)DE_ALLOC(st->alloc, sizeof(rt_obj), DE_ALIGNOF(rt_obj), DE_MEM_ASSET);
    if (o == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    memset(o, 0, sizeof *o);
    o->size = n;
    memcpy(o->text, p, (size_t)(n < sizeof o->text - 1u ? n : sizeof o->text - 1u));
    de_asset_load_ctx_set_object(ctx, o, sizeof *o);
    (void)de_atomic_fetch_add_u32(&st->loads, 1, DE_ATOMIC_ACQ_REL);
    return DE_OK;
}

static void rt_unload(void *object, void *user) {
    rt_state *st = (rt_state *)user;
    DE_FREE(st->alloc, object, sizeof(rt_obj));
    (void)de_atomic_fetch_add_u32(&st->unloads, 1, DE_ATOMIC_ACQ_REL);
}

static const rt_obj *rt_obj_of(de_asset_system *s, const char *id) {
    u32 h = de__slot_find(s, de_asset_id_from(id));
    return h != 0 ? (const rt_obj *)de__asset_object(s, h) : NULL;
}
#endif

TEST(cooker, runtime_source_mode) {
#if !DE_HAS_COOKER
    SKIP("this build does not link the cooker (DE_HAS_COOKER: Debug and Dev with tools)");
#else
    ckt_put("rt/assets/text/hello.txt", "hello world");
    ckt_put("rt/assets/text/other.txt", "the dependency");
    ckt_put("rt/assets.toml", "[project]\n"
                              "asset_root = \"assets\"\n"
                              "[assets]\n"
                              "\"text/hello\" = { type = \"upper\", source = \"text/hello.txt\", "
                              "dep = \"text/other\" }\n"
                              "\"text/other\" = { type = \"txt\", source = \"text/other.txt\" }\n");
    de_tracking_allocator track;
    de_allocator alloc = de_tracking_allocator_init(&track, NULL, DE_MEM_ASSET);
    rt_state st = {.alloc = &alloc};
    de_atomic_init_u32(&st.loads, 0);
    de_atomic_init_u32(&st.unloads, 0);
    de_fp_env_init_thread();
    de_task_system *tasks = NULL;
    ASSERT_EQ(de_task_system_create(
                  &(de_task_system_desc){.worker_count = 2, .background_worker_count = 1}, &tasks),
              DE_OK);
    de_asset_system *s = NULL;
    ASSERT_EQ(
        de_asset_system_create(
            &(de_asset_system_desc){.allocator = &alloc, .tasks = tasks, .io_threads = 1}, &s),
        DE_OK);
    de_asset_type_desc up = ckt_upper_type(&st);
    up.load = rt_load;
    up.unload = rt_unload;
    ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(s, &up)));
    de_asset_type_desc txt = {.name = "txt", .load = rt_load, .unload = rt_unload, .user = &st};
    ASSERT_TRUE(DE_HANDLE_VALID(de_asset_type_register(s, &txt)));
    ASSERT_EQ(de_asset_manifest_load(s, "ckt:/rt/assets.toml"), DE_OK);

    /* Before the parent is cooked the sub-asset is unknown. */
    de_asset_id sub_id = de_asset_id_from("text/hello#lower");
    de_load_handle hs = de_asset_load(s, sub_id, 0);
    EXPECT_EQ((int)de_asset_wait(s, hs, WAIT_NS), (int)DE_LOAD_FAILED);
    de_asset_release(s, hs);

    de_load_handle h = de_asset_load(s, de_asset_id_from("text/hello"), 0);
    de_load_info info;
    EXPECT_EQ((int)de_asset_wait(s, h, WAIT_NS), (int)DE_LOAD_READY);
    (void)de_asset_status(s, h, &info);
    if (info.first_error != DE_OK)
        printf("  load failed: %s\n", info.first_error_msg);
    EXPECT_EQ((int)de_asset_status_of(s, de_asset_id_from("text/other")), (int)DE_LOAD_READY);
    de_blob none = de_asset_blob(s, de_asset_id_from("text/hello")); /* wrong type: refused */
    EXPECT_FALSE(DE_HANDLE_VALID(none));

    /* The cooked bytes reached the loader; the dependency is held by the asset. */
    const rt_obj *o = rt_obj_of(s, "text/hello");
    ASSERT_TRUE(o != NULL);
    EXPECT_EQ(o->size, 11u);
    EXPECT_STREQ(o->text, "HELLO WORLD");
    o = rt_obj_of(s, "text/other");
    ASSERT_TRUE(o != NULL);
    EXPECT_STREQ(o->text, "the dependency");
    const de__slot *sl = de__slot_get(s, de__slot_find(s, de_asset_id_from("text/hello")));
    ASSERT_TRUE(sl != NULL);
    ASSERT_EQ(sl->dep_count, 1u);
    EXPECT_EQ(sl->deps[0].hash, de_asset_id_from("text/other").hash);

    /* The sub-asset is known now: it loads with the `txt` loader. */
    de_load_handle hs2 = de_asset_load(s, sub_id, 0);
    EXPECT_EQ((int)de_asset_wait(s, hs2, WAIT_NS), (int)DE_LOAD_READY);
    (void)de_asset_status(s, hs2, &info);
    if (info.first_error != DE_OK)
        printf("  sub-asset load failed: %s\n", info.first_error_msg);
    o = rt_obj_of(s, "text/hello#lower");
    ASSERT_TRUE(o != NULL);
    EXPECT_STREQ(o->text, "hello world");
    EXPECT_EQ(de_atomic_load_u32(&st.loads, DE_ATOMIC_ACQUIRE), 3u);
    de_blob wrong = de_asset_blob(s, sub_id); /* a `txt` sub-asset is not a blob */
    EXPECT_FALSE(DE_HANDLE_VALID(wrong));

    de_asset_release(s, hs2);
    de_asset_release(s, h);
    de_asset_system_destroy(s);
    de_task_system_destroy(tasks);
    EXPECT_EQ(de_atomic_load_u32(&st.unloads, DE_ATOMIC_ACQUIRE), 3u);
    EXPECT_EQ(de_tracking_allocator_stats(&track).bytes_live, 0u);
#endif
}
