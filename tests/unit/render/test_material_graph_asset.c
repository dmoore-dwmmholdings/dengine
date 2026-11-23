/*
 * test_material_graph_asset.c: the runtime `material_graph` asset type (DENG-770,
 * src/render/loaders/material_graph_loader.c). Executable test_render_material_graph; CTest names
 * render_material_graph.material_graph.<test>.
 *
 * A renderer on the null backend with a SOURCE-mode asset system on a temp directory: the graph
 * cooks in-process (the cooker's built-in material_graph importer) and its model registers.
 *
 *   registers_model            the graph loads READY; its model has the next game id, a resolve
 *                              pipeline and the graph's features; the sub-asset material resolves
 *                              to it and packs the graph's defaults and features
 *   material_loads_graph_dependency  loading only `#material` loads the graph and registers its
 *                              model before the material is READY; release and reload work
 *   param_edit_keeps_pipeline  de_material_set_params changes the block, not the model's pipeline
 *                              or the model count
 *   hot_reload_replaces        the source changes: the reload keeps the model id and the material's
 *                              model, and swaps the resolve pipeline
 *   name_taken_by_shader_model a graph whose model name a non-graph model holds fails to load
 *   cook_error_fails_load      a graph with a broken link fails to load with the node in the error
 *
 * SKIP without Slang or without the cooker (Release, Shipping).
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>
#include <dengine/de_task.h>

#include "asset/hot_reload_internal.h"
#include "asset/load_internal.h"
#include "render/loaders/render_assets.h"
#include "render/materials/material_instances.h"
#include "render/materials/material_models.h"
#include "render/materials/material_params.h"
#include "render/renderer_internal.h"

#if defined(DE_RMG_TEST_COOK) && DE_RMG_TEST_COOK && DE_HAS_COOKER
#include "cook/importers/shader/slang_compile.h"
#define RMG_COOK 1
#else
#define RMG_COOK 0
#endif

#include <stdio.h>
#include <string.h>

#define WAIT_NS ((u64)120 * 1000000000u)

#define HEADER "kind = \"material\"\nversion = 1\nnext_id = 64\n"

/* A colour and two floats; roughness from `rough`. */
#define ROCK_GRAPH(rough_value)                                                                    \
    HEADER "[[node]]\nid = \"n1\"\ntype = \"param.color\"\n"                                       \
           "values = { name = \"tint\", r = 0.5, g = 0.25, b = 1.0 }\n"                            \
           "[[node]]\nid = \"n2\"\ntype = \"param.float\"\n"                                       \
           "values = { name = \"wet\", value = 0.75, min = 0.0, max = 2.0 }\n"                     \
           "[[node]]\nid = \"n3\"\ntype = \"param.float\"\n"                                       \
           "values = { name = \"rough\", value = " rough_value " }\n"                              \
           "[[node]]\nid = \"n5\"\ntype = \"output.base\"\n"                                       \
           "[[link]]\nfrom = \"n1.out\"\nto = \"n5.base_color\"\n"                                 \
           "[[link]]\nfrom = \"n3.out\"\nto = \"n5.roughness\"\n"

typedef struct mg_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_gpu_device *dev;
    de_task_system *tasks;
    de_type_registry *types;
    de_asset_system *assets;
    de_renderer *r;
    char dir[96];
} mg_fx;

static void mg_fx_setup(mg_fx *f) {
    memset(f, 0, sizeof *f);
    de_fp_env_init_thread();
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    de_error_clear();
}

static void mg_fx_teardown(mg_fx *f) {
    if (f->r != NULL)
        de_renderer_destroy(f->r);
    if (f->assets != NULL)
        de_asset_system_destroy(f->assets);
    if (f->types != NULL)
        de_type_registry_destroy(f->types);
    if (f->tasks != NULL)
        de_task_system_destroy(f->tasks);
    if (f->dev != NULL)
        de_gpu_device_destroy(f->dev);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0);
}

#if RMG_COOK && DE__ASSET_RELOAD_ENABLED

static const char k_broken_graph[] = HEADER "[[node]]\nid = \"n5\"\ntype = \"output.base\"\n"
                                            "[[link]]\nfrom = \"n99.out\"\nto = \"n5.roughness\"\n";

static void put(mg_fx *f, const char *file, const char *text) {
    char path[160];
    snprintf(path, sizeof path, "%s/%s", f->dir, file);
    ASSERT_EQ(de_file_write_all_atomic(path, text, strlen(text)), DE_OK);
}

/* A null-backend renderer and a SOURCE-mode asset system with hot reload on rmg:/<name>, whose
 * manifest has the graph "mats/rock" (`graph` text, `extra` settings). Skips without Slang. */
static void open_fx(mg_fx *f, const char *name, const char *graph, const char *extra) {
    char msg[200];
    if (!de__slang_available(msg, sizeof msg))
        SKIP("no Slang compiler for this host");
    static int mounted = -1;
    if (mounted < 0)
        mounted = de_fs_mount("rmg", DE_RMG_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0) == DE_OK;
    ASSERT_EQ(mounted, 1);
    snprintf(f->dir, sizeof f->dir, "rmg:/%s", name);
    if (!de_path_exists(f->dir))
        ASSERT_EQ(de_dir_create(f->dir), DE_OK);
    put(f, "rock.mgraph.toml", graph);
    char manifest[512];
    snprintf(
        manifest, sizeof manifest,
        "[assets]\n\"mats/rock\" = { type = \"material_graph\", source = \"rock.mgraph.toml\", "
        "targets = [\"spirv\"]%s }\n",
        extra);
    put(f, "assets.toml", manifest);

    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &f->dev), DE_OK);
    ASSERT_EQ(
        de_task_system_create(
            &(de_task_system_desc){.worker_count = 2, .background_worker_count = 1}, &f->tasks),
        DE_OK);
    ASSERT_EQ(de_type_registry_create(&f->alloc, &f->types), DE_OK);
    char cache[160];
    snprintf(cache, sizeof cache, "%s/decache", f->dir);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &f->alloc,
                                                             .tasks = f->tasks,
                                                             .io_threads = 1,
                                                             .mode = DE_ASSET_MODE_SOURCE,
                                                             .cache_dir = cache,
                                                             .hot_reload = true,
                                                             .reload_retire_frames = 1,
                                                             .types = f->types},
                                     &f->assets),
              DE_OK);
    de__asset_reload_set_watching(f->assets, false);
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->dev,
                                                     .assets = f->assets,
                                                     .types = f->types,
                                                     .allocator = &f->alloc},
                                 &f->r),
              DE_OK);
    char path[160];
    snprintf(path, sizeof path, "%s/assets.toml", f->dir);
    ASSERT_EQ(de_asset_manifest_load(f->assets, path), DE_OK);
}

static de_load_state load(mg_fx *f, const char *id, de_load_handle *h) {
    *h = de_asset_load(f->assets, de_asset_id_from(id), 0);
    const de_load_state st = de_asset_wait(f->assets, *h, WAIT_NS);
    if (st != DE_LOAD_READY) {
        char err[256] = "";
        (void)de_asset_last_error(f->assets, de_asset_id_from(id), err, sizeof err);
        printf("  %s: state %d: %s\n", id, (int)st, err);
    }
    return st;
}

static de__material_models *models_of(mg_fx *f) {
    return f->r->material_models;
}

/* The packed block of the asset material `m`. */
static bool pack_of(mg_fx *f, de_material m, de__material_params *out) {
    const de_render_material *rec = de_render_material_get(f->assets, m);
    if (rec == NULL)
        return false;
    de__material_pack(rec, models_of(f), NULL, NULL, out);
    return true;
}

static u32 rock_model(mg_fx *f) {
    return de__material_models_find(models_of(f), "rock");
}

TEST_F(material_graph, registers_model, mg_fx) {
    open_fx(f, "registers", ROCK_GRAPH("0.375"), "");
    const u32 count0 = de__material_models_count(models_of(f));
    EXPECT_EQ(rock_model(f), DE__MATERIAL_MODEL_MAX);
    de_load_handle hg, hm;
    ASSERT_EQ((int)load(f, "mats/rock", &hg), (int)DE_LOAD_READY);
    const u32 id = rock_model(f);
    ASSERT_EQ(id, count0);
    EXPECT_EQ(de__material_models_count(models_of(f)), count0 + 1u);
    EXPECT_TRUE(DE_HANDLE_VALID(de__material_models_resolve_pipeline(models_of(f), id)));
    u32 features = 0xffffffffu;
    EXPECT_TRUE(de__material_models_graph_features(models_of(f), id, &features));
    EXPECT_EQ(features & (DE__MATERIAL_FEATURE_FORWARD | DE__MATERIAL_FEATURE_DOUBLE_SIDED), 0u);
    EXPECT_FALSE(de__material_models_graph_features(models_of(f), 0u, NULL));
    EXPECT_EQ(de_material_model_id(de_material_model_find(f->r, "rock")), id);

    ASSERT_EQ((int)load(f, "mats/rock#material", &hm), (int)DE_LOAD_READY);
    const de_material m = de_asset_material(f->assets, de_asset_id_from("mats/rock#material"));
    de__material_params b;
    ASSERT_TRUE(pack_of(f, m, &b));
    EXPECT_EQ(b.model, id);
    EXPECT_EQ(b.features, features);
    EXPECT_FLOAT_EQ(b.base_color.v[0], 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(b.base_color.v[1], 0.25f, 1e-6f);
    EXPECT_FLOAT_EQ(b.base_color.v[2], 1.0f, 1e-6f);
    /* Exposed parameters take raw rows in node-id order, first fit: tint = row 0 xyz, wet =
     * row 0 w, rough = row 1 x. The generated model reads them there. */
    EXPECT_FLOAT_EQ(b.base_color.v[3], 0.75f, 1e-6f);
    EXPECT_FLOAT_EQ(b.emissive.v[0], 0.375f, 1e-6f);
    de_asset_release(f->assets, hm);
    de_asset_release(f->assets, hg);
}

TEST_F(material_graph, material_loads_graph_dependency, mg_fx) {
    open_fx(f, "dependency", ROCK_GRAPH("0.375"), "");
    EXPECT_EQ(rock_model(f), DE__MATERIAL_MODEL_MAX);
    /* Only the sub-asset: the graph is not loaded by anything else (DENG-774). */
    de_load_handle hm;
    ASSERT_EQ((int)load(f, "mats/rock#material", &hm), (int)DE_LOAD_READY);
    const u32 id = rock_model(f);
    ASSERT_NE(id, DE__MATERIAL_MODEL_MAX);
    EXPECT_EQ((int)de_asset_status_of(f->assets, de_asset_id_from("mats/rock")),
              (int)DE_LOAD_READY);
    const de_material m = de_asset_material(f->assets, de_asset_id_from("mats/rock#material"));
    de__material_params b;
    ASSERT_TRUE(pack_of(f, m, &b));
    EXPECT_EQ(b.model, id);
    EXPECT_FLOAT_EQ(b.emissive.v[0], 0.375f, 1e-6f);

    /* Releasing the material releases the graph with it; loading it again works. */
    de_asset_release(f->assets, hm);
    ASSERT_EQ((int)load(f, "mats/rock#material", &hm), (int)DE_LOAD_READY);
    ASSERT_NE(rock_model(f), DE__MATERIAL_MODEL_MAX);
    de_asset_release(f->assets, hm);
}

TEST_F(material_graph, param_edit_keeps_pipeline, mg_fx) {
    open_fx(f, "params", ROCK_GRAPH("0.375"), ", double_sided = true, alpha_mode = \"mask\"");
    de_load_handle hg, hm;
    ASSERT_EQ((int)load(f, "mats/rock", &hg), (int)DE_LOAD_READY);
    ASSERT_EQ((int)load(f, "mats/rock#material", &hm), (int)DE_LOAD_READY);
    const u32 id = rock_model(f);
    ASSERT_NE(id, DE__MATERIAL_MODEL_MAX);
    const u32 count = de__material_models_count(models_of(f));
    const de_gpu_pipeline p0 = de__material_models_resolve_pipeline(models_of(f), id);
    const de_material m = de_asset_material(f->assets, de_asset_id_from("mats/rock#material"));
    de__material_params b;
    ASSERT_TRUE(pack_of(f, m, &b));
    EXPECT_NE(b.features & DE__MATERIAL_FEATURE_DOUBLE_SIDED, 0u);
    EXPECT_NE(b.features & DE__MATERIAL_FEATURE_ALPHA_MASK, 0u);

    const de_render_material *rec = de_render_material_get(f->assets, m);
    ASSERT_TRUE(rec != NULL);
    de_standard_material s = rec->standard;
    s.roughness = 0.875f;
    s.base_color.x = 0.125f;
    s.clearcoat = 1.0f; /* a value-derived feature: a graph model ignores it */
    ASSERT_EQ(de_material_set_params(f->r, m, &s), DE_OK);
    ASSERT_TRUE(pack_of(f, m, &b));
    EXPECT_EQ(b.model, id);
    EXPECT_FLOAT_EQ(b.pbr.v[1], 0.875f, 1e-6f);
    EXPECT_FLOAT_EQ(b.base_color.v[0], 0.125f, 1e-6f);
    EXPECT_EQ(b.features & DE__MATERIAL_FEATURE_CLEARCOAT, 0u);
    EXPECT_EQ(de__material_models_resolve_pipeline(models_of(f), id).id, p0.id);
    EXPECT_EQ(de__material_models_count(models_of(f)), count);
    de_asset_release(f->assets, hm);
    de_asset_release(f->assets, hg);
}

TEST_F(material_graph, hot_reload_replaces, mg_fx) {
    open_fx(f, "reload", ROCK_GRAPH("0.375"), "");
    de_load_handle hg, hm;
    ASSERT_EQ((int)load(f, "mats/rock", &hg), (int)DE_LOAD_READY);
    ASSERT_EQ((int)load(f, "mats/rock#material", &hm), (int)DE_LOAD_READY);
    const u32 id = rock_model(f);
    ASSERT_NE(id, DE__MATERIAL_MODEL_MAX);
    const u32 count = de__material_models_count(models_of(f));
    const de_gpu_pipeline p0 = de__material_models_resolve_pipeline(models_of(f), id);

    put(f, "rock.mgraph.toml", ROCK_GRAPH("0.625"));
    char path[160];
    snprintf(path, sizeof path, "%s/rock.mgraph.toml", f->dir);
    de__asset_reload_stats rs;
    de__asset_reload_get_stats(f->assets, &rs);
    const u64 swapped0 = rs.swapped, failed0 = rs.failed;
    de__asset_reload_on_change(f->assets, path, DE_WATCH_MODIFIED, false);
    const u64 t0 = de_time_now_ns();
    do {
        de_render_uploader_flush(de__renderer_uploader(f->r));
        de_asset_system_update(f->assets, NULL);
        de__asset_reload_get_stats(f->assets, &rs);
        ASSERT_EQ(rs.failed, failed0);
        ASSERT_TRUE(de_time_now_ns() - t0 < WAIT_NS);
        if (rs.swapped < swapped0 + 2u) /* the graph and its material */
            (void)de__asset_wake_block(f->assets, 2000000u);
    } while (rs.swapped < swapped0 + 2u);

    EXPECT_EQ(rock_model(f), id);
    EXPECT_EQ(de__material_models_count(models_of(f)), count);
    const de_gpu_pipeline p1 = de__material_models_resolve_pipeline(models_of(f), id);
    EXPECT_TRUE(DE_HANDLE_VALID(p1));
    EXPECT_NE(p1.id, p0.id);
    const de_material m = de_asset_material(f->assets, de_asset_id_from("mats/rock#material"));
    de__material_params b;
    ASSERT_TRUE(pack_of(f, m, &b));
    EXPECT_EQ(b.model, id);
    EXPECT_FLOAT_EQ(b.emissive.v[0], 0.625f, 1e-6f); /* `rough`: row 1 x */
    de_asset_release(f->assets, hm);
    de_asset_release(f->assets, hg);
}

TEST_F(material_graph, name_taken_by_shader_model, mg_fx) {
    open_fx(f, "taken", ROCK_GRAPH("0.375"), "");
    static const u8 k_code[16] = {0x03, 0x02, 0x23, 0x07};
    const de_gpu_shader_blob code = {
        .code = k_code, .size = sizeof k_code, .entry = "cs_resolve_model"};
    de_shader_reflection refl;
    memset(&refl, 0, sizeof refl);
    refl.stage = DE_SHADER_STAGE_COMPUTE;
    refl.threadgroup[0] = 64u;
    refl.threadgroup[1] = refl.threadgroup[2] = 1u;
    refl.push_constant_bytes = 112u;
    u32 id = 0;
    ASSERT_EQ(de__material_models_add(models_of(f), "rock", &code, &refl, &id), DE_OK);
    de_load_handle hg;
    EXPECT_EQ((int)load(f, "mats/rock", &hg), (int)DE_LOAD_FAILED);
    char err[256] = "";
    (void)de_asset_last_error(f->assets, de_asset_id_from("mats/rock"), err, sizeof err);
    EXPECT_TRUE(strstr(err, "not a graph") != NULL);
    EXPECT_FALSE(de__material_models_graph_features(models_of(f), id, NULL));
    de_asset_release(f->assets, hg);
    de_error_clear();
}

TEST_F(material_graph, cook_error_fails_load, mg_fx) {
    open_fx(f, "broken", k_broken_graph, "");
    const u32 count = de__material_models_count(models_of(f));
    de_load_handle hg;
    EXPECT_EQ((int)load(f, "mats/rock", &hg), (int)DE_LOAD_FAILED);
    char err[256] = "";
    (void)de_asset_last_error(f->assets, de_asset_id_from("mats/rock"), err, sizeof err);
    EXPECT_TRUE(strstr(err, "n99") != NULL || strstr(err, "n5") != NULL);
    EXPECT_EQ(de__material_models_count(models_of(f)), count);
    de_asset_release(f->assets, hg);
    de_error_clear();
}

#else

TEST_F(material_graph, registers_model, mg_fx) {
    (void)f;
    SKIP("the material graph importer and hot reload are compiled out of this build");
}

#endif
