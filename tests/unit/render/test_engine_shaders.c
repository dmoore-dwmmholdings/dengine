/*
 * test_engine_shaders.c: the renderer's engine shaders in every build type (DENG-421;
 * src/render/shaders/engine_shaders.h). Executable test_engine_shaders, CTest names
 * engine_shaders.<test>.
 *
 *   table              shaders/engine_shaders.cmake made the table; blobs are embedded when the
 *                      build cooks at build time (DE_ESH_TEST_EMBEDDED)
 *   errors             unknown shader, entry point or permutation; bad arguments
 *   null_placeholder   the null backend gets placeholder code with the embedded reflection
 *   dev_cache          (Debug/Dev with the cooker) a cold cache cooks, the next load is a hit
 *   embedded_pipelines the embedded blobs make working pipelines (in-process cook off), on the
 *                      first GPU backend of this build that has a Base-tier device
 *   renderer_second_create  two renderers: the second one's composite is a cache hit in Dev
 *                      builds with the cooker, embedded elsewhere, and never missing
 */
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_platform.h>
#include <dengine/de_render.h>

#include "render/renderer_internal.h"
#include "render/shaders/engine_shaders.h"
#include "render/views_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_composite[2] = {"vs_main", "ps_main"};
static const char *const k_scatter[2] = {"cs_scatter_instances", "cs_scatter_lights"};

/* A Base-tier device of the first GPU backend of this build that has one, or NULL. */
static de_gpu_device *gpu_device(char *why, size_t cap) {
    static const de_gpu_backend order[] = {DE_GPU_METAL, DE_GPU_VULKAN, DE_GPU_D3D12};
    snprintf(why, cap, "no GPU backend with a Base-tier device");
#if defined(__APPLE__)
    setenv("MTL_DEBUG_LAYER", "1", 0);
#endif
    for (u32 i = 0; i < 3; ++i) {
        de_gpu_device *d = NULL;
        if (de_gpu_device_create(&(de_gpu_device_desc){.backend = order[i],
                                                       .debug_layer = true,
                                                       .pipeline_cache_dir = ""},
                                 &d) != DE_OK)
            continue;
        if (de_gpu_get_caps(d)->base_tier_ok)
            return d;
        snprintf(why, cap, "%s: below the Base tier: %.300s", de_gpu_backend_name(order[i]),
                 de_gpu_get_caps(d)->base_tier_reason);
        de_gpu_device_destroy(d);
    }
    return NULL;
}

TEST(engine_shaders, table) {
    ASSERT_GE(de__engine_shader_count, 2u);
    const de__engine_shader_info *c = de__engine_shader_find("composite");
    const de__engine_shader_info *s = de__engine_shader_find("scene_scatter");
    ASSERT_TRUE(c != NULL);
    ASSERT_TRUE(s != NULL);
    EXPECT_STREQ(c->source, "common/composite.slang");
    EXPECT_STREQ(s->source, "scene/scatter.slang");
    EXPECT_TRUE(de__engine_shader_find("nope") == NULL);
    EXPECT_TRUE(de__engine_shader_find(NULL) == NULL);
    for (u32 i = 0; i < de__engine_shader_count; ++i) {
        const de__engine_shader_info *e = &de__engine_shader_table[i];
#if DE_ESH_TEST_EMBEDDED
        EXPECT_TRUE(e->blob != NULL && e->blob_size > 32u);
        EXPECT_EQ(((uintptr_t)e->blob) % 16u, 0u);
        if (e->blob != NULL)
            EXPECT_EQ(memcmp(e->blob, "DSHD", 4), 0);
#else
        EXPECT_TRUE(e->blob == NULL);
#endif
    }
    EXPECT_STREQ(de__engine_shader_origin_name(DE__ENGINE_SHADER_CACHED), "cached");
    EXPECT_STREQ(de__engine_shader_origin_name(DE__ENGINE_SHADER_EMBEDDED), "embedded");
}

TEST(engine_shaders, errors) {
    de_gpu_device *d = NULL;
    ASSERT_EQ(de_gpu_device_create(
                  &(de_gpu_device_desc){.backend = DE_GPU_NULL, .pipeline_cache_dir = ""}, &d),
              DE_OK);
    de__engine_shader_code c;
    char why[600];
    EXPECT_EQ(de__engine_shader_load(d, NULL, "nope", 0, k_composite, 2, &c, why, sizeof why),
              DE_ERR_NOT_FOUND);
    EXPECT_TRUE(strstr(why, "engine_shaders.cmake") != NULL);
    EXPECT_EQ(de__engine_shader_load(d, NULL, NULL, 0, k_composite, 2, &c, NULL, 0),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__engine_shader_load(d, NULL, "composite", 0, k_composite, 0, &c, NULL, 0),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__engine_shader_load(d, NULL, "composite", 0, k_composite,
                                     DE__ENGINE_SHADER_MAX_ENTRIES + 1u, &c, NULL, 0),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__engine_shader_load(NULL, NULL, "composite", 0, k_composite, 2, &c, NULL, 0),
              DE_ERR_INVALID_ARGUMENT);
    de__engine_shader_code_free(NULL);
    de_gpu_device_destroy(d);

    char gwhy[400];
    de_gpu_device *g = gpu_device(gwhy, sizeof gwhy);
    if (g == NULL)
        return; /* the rest needs real code */
    static const char *const bad_entry[1] = {"no_such_entry"};
    EXPECT_EQ(de__engine_shader_load(g, NULL, "composite", 0, bad_entry, 1, &c, why, sizeof why),
              DE_ERR_NOT_FOUND);
    printf("missing entry: %s\n", why);
    EXPECT_EQ(de__engine_shader_load(g, NULL, "composite", 7, k_composite, 2, &c, why, sizeof why),
              DE_ERR_NOT_FOUND); /* no permutation 7 */
    EXPECT_EQ(c.origin, DE__ENGINE_SHADER_NONE);
    de_gpu_device_destroy(g);
}

TEST(engine_shaders, null_placeholder) {
    de_gpu_device *d = NULL;
    ASSERT_EQ(de_gpu_device_create(
                  &(de_gpu_device_desc){.backend = DE_GPU_NULL, .pipeline_cache_dir = ""}, &d),
              DE_OK);
    de__engine_shader_code c;
    ASSERT_EQ(de__engine_shader_load(d, NULL, "scene_scatter", 0, k_scatter, 2, &c, NULL, 0),
              DE_OK);
    EXPECT_EQ(c.origin, DE__ENGINE_SHADER_PLACEHOLDER);
    EXPECT_EQ(c.count, 2u);
    EXPECT_STREQ(c.entries[1].blob.entry, "cs_scatter_lights");
    EXPECT_GT(c.entries[0].blob.size, 0u);
#if DE_ESH_TEST_EMBEDDED
    /* The reflection of the embedded SPIR-V. */
    EXPECT_GT(c.entries[0].threadgroup[0], 0u);
    EXPECT_GT(c.push_constant_bytes, 0u);
#endif
    de__engine_shader_code_free(&c);
    de_gpu_device_destroy(d);
}

TEST(engine_shaders, dev_cache) {
#if !DE_HAS_COOKER
    SKIP("no in-process cook in this build (Release, Shipping or no tools): embedded only");
#else
    char why[600];
    de_gpu_device *g = gpu_device(why, sizeof why);
    if (g == NULL)
        SKIP(why);
    /* A cold cache of its own. */
    char dir[DE_FS_PATH_MAX];
    snprintf(dir, sizeof dir, "%s/cold_%u", DE_ESH_TEST_TMP_DIR,
             (unsigned)(de_time_now_ns() % 1000000u));
    ASSERT_EQ(de_fs_mount("eshtmp", DE_ESH_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    char vdir[64];
    snprintf(vdir, sizeof vdir, "eshtmp:/%s", strrchr(dir, '/') + 1);
    ASSERT_EQ(de_dir_create(vdir), DE_OK);
    de__engine_shaders_set_cache_dir(dir);
    de__engine_shader_code a, b;
    const de_result ra =
        de__engine_shader_load(g, NULL, "composite", 0, k_composite, 2, &a, why, sizeof why);
    if (ra != DE_OK) {
        de__engine_shaders_set_cache_dir(NULL);
        de_gpu_device_destroy(g);
        printf("%s\n", why);
        SKIP("this host cannot cook the backend's target (no Slang or DXC)");
    }
    EXPECT_EQ(a.origin, DE__ENGINE_SHADER_COOKED);
    ASSERT_EQ(de__engine_shader_load(g, NULL, "composite", 0, k_composite, 2, &b, why, sizeof why),
              DE_OK);
    EXPECT_EQ(b.origin, DE__ENGINE_SHADER_CACHED);
    /* The same code either way. */
    EXPECT_EQ(a.entries[0].blob.size, b.entries[0].blob.size);
    EXPECT_EQ(
        memcmp(a.entries[0].blob.code, b.entries[0].blob.code, (size_t)a.entries[0].blob.size), 0);
    de__engine_shader_code_free(&a);
    de__engine_shader_code_free(&b);
    de__engine_shaders_set_cache_dir(NULL);
    (void)de_fs_unmount("eshtmp", DE_ESH_TEST_TMP_DIR);
    de_gpu_device_destroy(g);
#endif
}

TEST(engine_shaders, embedded_pipelines) {
#if !DE_ESH_TEST_EMBEDDED
    SKIP("this build embeds no engine shaders (no build-time cook)");
#else
    char why[600];
    de_gpu_device *g = gpu_device(why, sizeof why);
    if (g == NULL)
        SKIP(why);
    de__engine_shaders_set_source_cook(false);
    de__engine_shader_code c, s;
    const de_result rc =
        de__engine_shader_load(g, NULL, "composite", 0, k_composite, 2, &c, why, sizeof why);
    const de_result rs =
        de__engine_shader_load(g, NULL, "scene_scatter", 0, k_scatter, 2, &s, why, sizeof why);
    de__engine_shaders_set_source_cook(true);
    if (rc != DE_OK || rs != DE_OK) {
        de__engine_shader_code_free(&c);
        de__engine_shader_code_free(&s);
        de_gpu_device_destroy(g);
        printf("%s\n", why);
        SKIP("the build-time cook has no code for this backend's target (DXIL without DXC)");
    }
    EXPECT_EQ(c.origin, DE__ENGINE_SHADER_EMBEDDED);
    EXPECT_EQ(s.origin, DE__ENGINE_SHADER_EMBEDDED);
    EXPECT_TRUE(c.block == NULL); /* points into the library's static data */
    const de_gpu_pipeline gp = de_gpu_pipeline_create(
        g, &(de_gpu_pipeline_desc){.kind = DE_PIPE_GRAPHICS,
                                   .vs = c.entries[0].blob,
                                   .ps = c.entries[1].blob,
                                   .color_formats = {DE_FMT_RGBA8_UNORM},
                                   .color_count = 1,
                                   .push_constant_bytes = c.push_constant_bytes,
                                   .name = "embedded composite"});
    EXPECT_TRUE(DE_HANDLE_VALID(gp));
    for (u32 i = 0; i < 2; ++i) {
        const de_gpu_pipeline cp = de_gpu_pipeline_create(
            g, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                       .cs = s.entries[i].blob,
                                       .push_constant_bytes = s.push_constant_bytes,
                                       .threadgroup_size = {s.entries[i].threadgroup[0],
                                                            s.entries[i].threadgroup[1],
                                                            s.entries[i].threadgroup[2]},
                                       .name = "embedded scatter"});
        EXPECT_TRUE(DE_HANDLE_VALID(cp));
        if (DE_HANDLE_VALID(cp))
            de_gpu_pipeline_destroy(g, cp);
    }
    if (DE_HANDLE_VALID(gp))
        de_gpu_pipeline_destroy(g, gp);
    de__engine_shader_code_free(&c);
    de__engine_shader_code_free(&s);
    de_gpu_device_destroy(g);
#endif
}

TEST(engine_shaders, renderer_second_create) {
    char why[600];
    de_gpu_device *g = gpu_device(why, sizeof why);
    if (g == NULL)
        SKIP(why);
    u32 origin[2] = {0, 0};
    for (u32 k = 0; k < 2; ++k) {
        de_renderer *r = NULL;
        ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = g}, &r), DE_OK);
        origin[k] = de__composite_shader_origin(de__views_composite(r));
        printf("renderer %u: composite %s\n", k,
               de__engine_shader_origin_name((de__engine_shader_origin)origin[k]));
        de_renderer_destroy(r);
    }
    if (origin[1] == DE__ENGINE_SHADER_NONE) {
        de_gpu_device_destroy(g);
        SKIP("no composite code for this backend in this build (DXIL without DXC)");
    }
#if DE_HAS_COOKER
    EXPECT_EQ(origin[1], (u32)DE__ENGINE_SHADER_CACHED);
#else
    EXPECT_EQ(origin[1], (u32)DE__ENGINE_SHADER_EMBEDDED);
#endif
    de_gpu_device_destroy(g);
}
