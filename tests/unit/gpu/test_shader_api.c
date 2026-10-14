/*
 * test_shader_api.c: the public shader API (include/dengine/de_shader.h; DENG-409) and the runtime
 * `shader` type with no importer named by the caller: in Debug and Dev builds with the tools
 * (DE_HAS_COOKER) both take the cooker's built-in shader importer, so SOURCE mode cooks `.slang`
 * sources; elsewhere the type has no importer and a `.slang` source fails to load as a cooked
 * blob. CTest names gpu_shader_api.shader_api.<test>.
 *
 * Sources are written into a scratch directory of the build tree (mounted as sapi:/). The cooks
 * need the Slang compiler of this host; without it they skip.
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_shader.h>
#include <dengine/de_task.h>

#include "gpu/shader/shader_internal.h"
#if DE_HAS_COOKER
#include "cook/importers/shader/slang_compile.h"
#endif

#include <stdio.h>
#include <string.h>

#define WAIT_NS ((u64)60 * 1000000000u)

/* A compute shader (8 bytes of push constants, 4x2x1 threads) and a vertex/pixel pair. */
static const char k_compute[] = "import bindless;\n"
                                "struct Push { uint out_buf; float scale; };\n"
                                "[shader(\"compute\")]\n"
                                "[numthreads(4, 2, 1)]\n"
                                "void cs_main(uint3 id : SV_DispatchThreadID, uniform Push pc)\n"
                                "{\n"
                                "    de_buffer<float>(pc.out_buf)[id.x] = pc.scale;\n"
                                "}\n";
static const char k_manifest[] =
    "[assets]\n"
    "\"shaders/fill\" = { type = \"shader\", source = \"shaders/fill.slang\" }\n";

typedef struct sapi_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_task_system *tasks;
    de_asset_system *assets;
    char root[64];
} sapi_fx;

static void sapi_fx_setup(sapi_fx *f) {
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("sapi", DE_SHADER_API_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
        mounted = true;
    }
    memset(f, 0, sizeof *f);
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    de_fp_env_init_thread();
}

static void sapi_fx_teardown(sapi_fx *f) {
    if (f->assets != NULL)
        de_asset_system_destroy(f->assets);
    if (f->tasks != NULL)
        de_task_system_destroy(f->tasks);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
}

static void put_text(const char *vpath, const char *text) {
    ASSERT_EQ(de_file_write_all_atomic(vpath, text, strlen(text)), DE_OK);
}

/* sapi:/<name>/ with the manifest and the shader, and a SOURCE-mode asset system on it. */
static void make_project(sapi_fx *f, const char *name) {
    snprintf(f->root, sizeof f->root, "sapi:/%s", name);
    (void)de_dir_create(f->root);
    char path[128];
    snprintf(path, sizeof path, "%s/shaders", f->root);
    (void)de_dir_create(path);
    snprintf(path, sizeof path, "%s/shaders/fill.slang", f->root);
    put_text(path, k_compute);
    snprintf(path, sizeof path, "%s/assets.toml", f->root);
    put_text(path, k_manifest);

    de_task_system_desc td = {.worker_count = 2, .background_worker_count = 1};
    ASSERT_EQ(de_task_system_create(&td, &f->tasks), DE_OK);
    de_asset_system_desc d = {0};
    d.allocator = &f->alloc;
    d.tasks = f->tasks;
    d.io_threads = 1;
    d.mode = DE_ASSET_MODE_SOURCE; /* no cache_dir: every run cooks */
    ASSERT_EQ(de_asset_system_create(&d, &f->assets), DE_OK);
}

#if DE_HAS_COOKER
static bool slang_ok(void) {
    char why[256];
    if (de__slang_available(why, sizeof why))
        return true;
    printf("  (Slang not available: %s)\n", why);
    return false;
}
#endif

/* Loads shaders/fill and returns the load handle; READY expected with the cooker. */
static de_load_state load_fill(sapi_fx *f, de_load_handle *h) {
    char path[128];
    snprintf(path, sizeof path, "%s/assets.toml", f->root);
    if (de_asset_manifest_load(f->assets, path) != DE_OK)
        return DE_LOAD_FAILED;
    *h = de_asset_load(f->assets, de_asset_id_from("shaders/fill"), 0);
    return de_asset_wait(f->assets, *h, WAIT_NS);
}

/* ---- shader_api.register_and_get_code -------------------------------------------------------- */

/* de_shader_register_type with no importer named: SOURCE mode cooks through the built-in one;
 * de_shader_get_code hands out the code and the reflection; a second registration is refused. */
TEST_F(shader_api, register_and_get_code, sapi_fx) {
    make_project(f, "public");
    ASSERT_EQ(de_shader_register_type(f->assets, &f->alloc), DE_OK);
    EXPECT_EQ(de_shader_register_type(f->assets, NULL), DE_ERR_ALREADY_EXISTS);
    EXPECT_EQ(de_shader_register_type(NULL, NULL), DE_ERR_INVALID_ARGUMENT);
#if !DE_HAS_COOKER
    /* No cooker: the type has no importer and loads cooked data only, so the `.slang` source
     * is read as a cooked blob and refused. */
    de_load_handle h = {0};
    EXPECT_EQ((int)load_fill(f, &h), (int)DE_LOAD_FAILED);
    de_asset_release(f->assets, h);
#else
    if (!slang_ok())
        SKIP("no Slang compiler for this host");
    const de_asset_id id = de_asset_id_from("shaders/fill");
    char path[128];
    snprintf(path, sizeof path, "%s/assets.toml", f->root);
    ASSERT_EQ(de_asset_manifest_load(f->assets, path), DE_OK);
    de_shader sh = de_asset_shader(f->assets, id);
    ASSERT_TRUE(DE_HANDLE_VALID(sh));
    de_gpu_shader_blob blob;
    de_shader_reflection refl;
    EXPECT_EQ(de_shader_get_code(f->assets, sh, DE_GPU_NULL, 0, "cs_main", &blob, &refl),
              DE_ERR_BUSY);
    de_load_handle h = de_asset_load(f->assets, id, 0);
    ASSERT_EQ((int)de_asset_wait(f->assets, h, WAIT_NS), (int)DE_LOAD_READY);

    /* NULL backend: SPIR-V, like Vulkan. */
    ASSERT_EQ(de_shader_get_code(f->assets, sh, DE_GPU_NULL, 0, "cs_main", &blob, &refl), DE_OK);
    ASSERT_GE(blob.size, 4u);
    u32 magic = 0;
    memcpy(&magic, blob.code, 4);
    EXPECT_EQ(magic, 0x07230203u);
    EXPECT_STREQ(blob.entry, "cs_main");
    EXPECT_EQ((int)refl.stage, (int)DE_SHADER_STAGE_COMPUTE);
    EXPECT_EQ(refl.threadgroup[0], 4u);
    EXPECT_EQ(refl.threadgroup[1], 2u);
    EXPECT_EQ(refl.threadgroup[2], 1u);
    EXPECT_EQ(refl.push_constant_bytes, 8u);
    EXPECT_EQ(refl.mesh_max_vertices, 0u);
    /* The same code through the internal accessor; `refl` may be NULL. */
    de_gpu_shader_blob same;
    ASSERT_EQ(de__shader_blob(f->assets, sh, DE_GPU_VULKAN, 0, "cs_main", &same, NULL), DE_OK);
    EXPECT_TRUE(same.code == blob.code && same.size == blob.size);
    ASSERT_EQ(de_shader_get_code(f->assets, sh, DE_GPU_VULKAN, 0, "cs_main", &blob, NULL), DE_OK);

    /* Errors: no such entry point or permutation, NULL arguments, not a shader handle. */
    EXPECT_EQ(de_shader_get_code(f->assets, sh, DE_GPU_NULL, 0, "nope", &blob, &refl),
              DE_ERR_NOT_FOUND);
    EXPECT_EQ(refl.push_constant_bytes, 0u); /* zeroed on failure */
    EXPECT_EQ(de_shader_get_code(f->assets, sh, DE_GPU_NULL, 1, "cs_main", &blob, NULL),
              DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_shader_get_code(f->assets, sh, DE_GPU_NULL, 0, NULL, &blob, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_shader_get_code(f->assets, sh, DE_GPU_NULL, 0, "cs_main", NULL, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_shader_get_code(f->assets, (de_shader){0}, DE_GPU_NULL, 0, "cs_main", &blob, NULL),
              DE_ERR_INVALID_ARGUMENT);
    de_asset_release(f->assets, h);
#endif
}

/* ---- shader_api.internal_desc_zero ----------------------------------------------------------- */

/* The internal registration with `(de__shader_type_desc){0}` (what the renderer does) also takes
 * the built-in importer in builds with the cooker. */
TEST_F(shader_api, internal_desc_zero, sapi_fx) {
    make_project(f, "internal");
    de__shader_types *st = NULL;
    de__shader_type_desc sd = {0};
    sd.allocator = &f->alloc;
    ASSERT_EQ(de__shader_type_register(f->assets, &sd, &st), DE_OK);
    de_load_handle h = {0};
#if !DE_HAS_COOKER
    EXPECT_EQ((int)load_fill(f, &h), (int)DE_LOAD_FAILED);
#else
    if (!slang_ok()) {
        de_asset_system_destroy(f->assets);
        f->assets = NULL;
        de__shader_types_destroy(st);
        SKIP("no Slang compiler for this host");
    }
    ASSERT_EQ((int)load_fill(f, &h), (int)DE_LOAD_READY);
    de_shader sh = de_asset_shader(f->assets, de_asset_id_from("shaders/fill"));
    de_gpu_shader_blob blob;
    de__shader_reflection refl;
    ASSERT_EQ(de__shader_blob(f->assets, sh, DE_GPU_NULL, 0, "cs_main", &blob, &refl), DE_OK);
    EXPECT_EQ(refl.push_constant_bytes, 8u);
#endif
    de_asset_release(f->assets, h);
    de_asset_system_destroy(f->assets);
    f->assets = NULL;
    de__shader_types_destroy(st);
}
