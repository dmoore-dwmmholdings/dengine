/* test_gpu_tier.c — de_gpu_evaluate_tier() on synthetic caps (DENG-290; SPEC §3.3, §12.8, R7).
 *
 * Table-driven: each row is a device as its backend would report it (see the backends' caps code:
 * d3d12_caps.c, vk_caps.c, mtl_caps.m) and the decision the renderer must get. */
#include "de_test.h"

#include <dengine/de_gpu.h>

#include <stdio.h>
#include <string.h>

/* Caps that meet every front-end requirement; the rows change what they test. */
static de_gpu_caps base_caps(de_gpu_backend b, const char *name, u32 vendor, u32 device) {
    de_gpu_caps c;
    memset(&c, 0, sizeof c);
    c.backend = b;
    snprintf(c.device_name, sizeof c.device_name, "%s", name);
    c.vendor_id = vendor;
    c.device_id = device;
    c.max_bindless_textures = 65536;
    c.max_bindless_storage_textures = 16384;
    c.max_bindless_buffers = 65536;
    c.max_bindless_samplers = 2048;
    c.max_push_constant_bytes = DE_GPU_MAX_PUSH_CONSTANT_BYTES;
    c.base_tier_ok = true;
    c.wave_size_min = c.wave_size_max = 32;
    return c;
}

static void with_mesh(de_gpu_caps *c, u32 verts, u32 prims) {
    c->mesh_shaders = true;
    c->mesh_max_output_vertices = verts;
    c->mesh_max_output_primitives = prims;
}

static void with_rt(de_gpu_caps *c, de_rt_support rt, bool ray_query) {
    c->raytracing = rt;
    c->ray_query = ray_query;
    c->rt_pipelines = rt != DE_RT_NONE;
}

/* True when one of the Base (or RT) reasons contains `needle`. */
static bool has_reason(const de_gpu_tier_report *t, bool rt, const char *needle) {
    const u32 n = rt ? t->rt_reason_count : t->base_reason_count;
    for (u32 i = 0; i < n; ++i)
        if (strstr(rt ? t->rt_reasons[i] : t->base_reasons[i], needle))
            return true;
    return false;
}

typedef struct tier_row {
    const char *name;
    de_gpu_caps caps;
    bool base_ok, rt_ok, mesh;
    de_gpu_tier recommended;
    const char *base_reason;    /* a substring of one base reason, or NULL (none expected) */
    const char *rt_reason;      /* a substring of one RT reason, or NULL */
    const char *default_base;   /* default_base_device, or "" */
    bool expect_recommend_note; /* recommended_reason set */
} tier_row;

#define ROWS_MAX 16

static u32 make_rows(tier_row *rows) {
    u32 n = 0;
    tier_row *r;

    /* GTX 1060 (Pascal) on D3D12: SM 6.6 and binding tier 3; DXR 1.0 through the driver's
     * compute fallback (tier 1.0: no inline ray queries); no mesh shaders. */
    r = &rows[n++];
    memset(r, 0, sizeof *r);
    r->name = "GTX 1060";
    r->caps = base_caps(DE_GPU_D3D12, "NVIDIA GeForce GTX 1060 6GB", 0x10DE, 0x1C03);
    with_rt(&r->caps, DE_RT_HARDWARE, false);
    r->base_ok = true;
    r->recommended = DE_GPU_TIER_BASE;
    r->rt_reason = "no inline ray queries";
    r->default_base = "";

    /* RTX 3070 on D3D12: DXR 1.1, mesh shaders. */
    r = &rows[n++];
    memset(r, 0, sizeof *r);
    r->name = "RTX 3070";
    r->caps = base_caps(DE_GPU_D3D12, "NVIDIA GeForce RTX 3070", 0x10DE, 0x2484);
    with_rt(&r->caps, DE_RT_HARDWARE, true);
    with_mesh(&r->caps, 256, 256);
    r->base_ok = r->rt_ok = r->mesh = true;
    r->recommended = DE_GPU_TIER_RT;
    r->default_base = "";

    /* Apple M1 on Metal: Base (Metal 3, Apple7, AB tier 2); ray tracing API only. */
    r = &rows[n++];
    memset(r, 0, sizeof *r);
    r->name = "Apple M1";
    r->caps = base_caps(DE_GPU_METAL, "Apple M1", 0x106B, 0x1234);
    with_rt(&r->caps, DE_RT_API_ONLY, true);
    r->caps.rt_pipelines = false;
    with_mesh(&r->caps, 256, 512);
    r->base_ok = r->mesh = true;
    r->recommended = DE_GPU_TIER_BASE;
    r->rt_reason = "API only";
    r->default_base = "Apple M1";

    /* Apple M2 Max through MoltenVK, if it ever called its ray tracing hardware: the default-to-
     * Base list keeps it on Base. */
    r = &rows[n++];
    memset(r, 0, sizeof *r);
    r->name = "Apple M2 Max (hardware RT claimed)";
    r->caps = base_caps(DE_GPU_VULKAN, "Apple M2 Max", 0x106B, 0x2);
    with_rt(&r->caps, DE_RT_HARDWARE, true);
    r->base_ok = r->rt_ok = true;
    r->recommended = DE_GPU_TIER_BASE;
    r->default_base = "Apple M2";
    r->expect_recommend_note = true;

    /* Apple M3 Pro on Metal: Apple9, hardware ray tracing: RT. */
    r = &rows[n++];
    memset(r, 0, sizeof *r);
    r->name = "Apple M3";
    r->caps = base_caps(DE_GPU_METAL, "Apple M3 Pro", 0x106B, 0x5678);
    with_rt(&r->caps, DE_RT_HARDWARE, true);
    r->caps.rt_pipelines = false;
    with_mesh(&r->caps, 256, 512);
    r->base_ok = r->rt_ok = r->mesh = true;
    r->recommended = DE_GPU_TIER_RT;
    r->default_base = "";

    /* Steam Deck (Van Gogh, RADV): ray queries in hardware, defaults to Base (DENG-2 Q4). */
    r = &rows[n++];
    memset(r, 0, sizeof *r);
    r->name = "Steam Deck";
    r->caps = base_caps(DE_GPU_VULKAN, "AMD Custom GPU 0405 (RADV VANGOGH)", 0x1002, 0x163F);
    with_rt(&r->caps, DE_RT_HARDWARE, true);
    with_mesh(&r->caps, 256, 256);
    r->base_ok = r->rt_ok = r->mesh = true;
    r->recommended = DE_GPU_TIER_BASE;
    r->default_base = "Steam Deck";
    r->expect_recommend_note = true;

    /* Steam Deck OLED (Sephiroth). */
    r = &rows[n++];
    memset(r, 0, sizeof *r);
    r->name = "Steam Deck OLED";
    r->caps = base_caps(DE_GPU_VULKAN, "AMD Custom GPU 0932 (RADV VANGOGH)", 0x1002, 0x1435);
    with_rt(&r->caps, DE_RT_HARDWARE, true);
    r->base_ok = r->rt_ok = true;
    r->recommended = DE_GPU_TIER_BASE;
    r->default_base = "Steam Deck OLED";
    r->expect_recommend_note = true;

    /* Missing SM 6.6 (D3D12 adapter at SM 6.5): not Base, the backend's reason quoted, and RT
     * fails too although DXR 1.1 is there. */
    r = &rows[n++];
    memset(r, 0, sizeof *r);
    r->name = "missing SM 6.6";
    r->caps = base_caps(DE_GPU_D3D12, "Old Driver GPU", 0x10DE, 0x1B80);
    r->caps.base_tier_ok = false;
    snprintf(r->caps.base_tier_reason, sizeof r->caps.base_tier_reason,
             "shader model 6.5 (needs 6.6)");
    with_rt(&r->caps, DE_RT_HARDWARE, true);
    r->recommended = DE_GPU_TIER_NONE;
    r->base_reason = "d3d12: shader model 6.5 (needs 6.6)";
    r->rt_reason = "below the Base tier";
    r->default_base = "";

    /* WARP on CI: SM 6.2, DXR in software. */
    r = &rows[n++];
    memset(r, 0, sizeof *r);
    r->name = "WARP";
    r->caps = base_caps(DE_GPU_D3D12, "Microsoft Basic Render Driver", 0x1414, 0x8C);
    r->caps.base_tier_ok = false;
    snprintf(r->caps.base_tier_reason, sizeof r->caps.base_tier_reason,
             "shader model 6.2 (needs 6.6)");
    with_rt(&r->caps, DE_RT_API_ONLY, true);
    r->recommended = DE_GPU_TIER_NONE;
    r->base_reason = "shader model 6.2";
    r->rt_reason = "API only";
    r->default_base = "";

    /* MoltenVK on an M-series Mac: forced adapter, below Base for the bindless limits. */
    r = &rows[n++];
    memset(r, 0, sizeof *r);
    r->name = "MoltenVK";
    r->caps = base_caps(DE_GPU_VULKAN, "Apple M5 Pro", 0x106B, 0x1A020208);
    r->caps.base_tier_ok = false;
    snprintf(r->caps.base_tier_reason, sizeof r->caps.base_tier_reason,
             "limit maxPerStageDescriptorUpdateAfterBindSamplers is 16, below 2048");
    r->recommended = DE_GPU_TIER_NONE;
    r->base_reason = "vulkan: limit maxPerStageDescriptorUpdateAfterBindSamplers";
    r->rt_reason = "no ray tracing";
    r->default_base = "";

    /* The front-end's own requirements: a backend that says Base but has no sampler table and
     * 64-byte push constants. Every failure is listed. */
    r = &rows[n++];
    memset(r, 0, sizeof *r);
    r->name = "front-end requirements";
    r->caps = base_caps(DE_GPU_VULKAN, "Odd GPU", 0x8086, 0x1);
    r->caps.max_bindless_samplers = 0;
    r->caps.max_push_constant_bytes = 64;
    r->recommended = DE_GPU_TIER_NONE;
    r->base_reason = "push constants are 64 bytes (needs 128)";
    r->rt_reason = "below the Base tier";
    r->default_base = "";

    /* Mesh shaders reported without output limits are not usable. */
    r = &rows[n++];
    memset(r, 0, sizeof *r);
    r->name = "mesh without limits";
    r->caps = base_caps(DE_GPU_VULKAN, "Some GPU", 0x10DE, 0x2);
    r->caps.mesh_shaders = true;
    r->base_ok = true;
    r->recommended = DE_GPU_TIER_BASE;
    r->rt_reason = "no ray tracing";
    r->default_base = "";

    /* The name prefix ends at a space or the end: "Apple M10" is not an M1. */
    r = &rows[n++];
    memset(r, 0, sizeof *r);
    r->name = "Apple M10 (not M1)";
    r->caps = base_caps(DE_GPU_METAL, "Apple M10", 0x106B, 0x9);
    with_rt(&r->caps, DE_RT_HARDWARE, true);
    r->base_ok = r->rt_ok = true;
    r->recommended = DE_GPU_TIER_RT;
    r->default_base = "";

    DE_ASSERT(n <= ROWS_MAX);
    return n;
}

TEST(gpu_tier, table) {
    tier_row rows[ROWS_MAX];
    const u32 n = make_rows(rows);
    for (u32 i = 0; i < n; ++i) {
        const tier_row *row = &rows[i];
        de_gpu_tier_report t;
        memset(&t, 0xAB, sizeof t); /* every field must be written */
        de_gpu_evaluate_tier(&row->caps, &t);
        printf("  %-36s base %d rt %d mesh %d -> %s\n", row->name, t.base_ok, t.rt_ok,
               t.mesh_shaders, de_gpu_tier_name(t.recommended));
        EXPECT_EQ(t.base_ok, row->base_ok);
        EXPECT_EQ(t.rt_ok, row->rt_ok);
        EXPECT_EQ(t.mesh_shaders, row->mesh);
        EXPECT_EQ(t.recommended, row->recommended);
        EXPECT_STREQ(t.default_base_device, row->default_base);
        EXPECT_EQ(t.recommended_reason[0] != '\0', row->expect_recommend_note);
        /* A reason for every failed tier, none for a met one. */
        EXPECT_EQ(t.base_reason_count == 0, t.base_ok);
        EXPECT_EQ(t.rt_reason_count == 0, t.rt_ok);
        EXPECT_LE(t.base_reason_count, DE_GPU_TIER_MAX_REASONS);
        EXPECT_LE(t.rt_reason_count, DE_GPU_TIER_MAX_REASONS);
        if (row->base_reason)
            EXPECT_TRUE(has_reason(&t, false, row->base_reason));
        if (row->rt_reason)
            EXPECT_TRUE(has_reason(&t, true, row->rt_reason));
        for (u32 k = 0; k < t.base_reason_count; ++k)
            EXPECT_TRUE(memchr(t.base_reasons[k], '\0', DE_GPU_TIER_REASON_BYTES) != NULL);
        for (u32 k = 0; k < t.rt_reason_count; ++k)
            EXPECT_TRUE(memchr(t.rt_reasons[k], '\0', DE_GPU_TIER_REASON_BYTES) != NULL);
    }
}

TEST(gpu_tier, every_failed_requirement_listed) {
    /* No bindless tables, no push constants, the backend's check failed, no concrete backend:
     * six base failures, capped at DE_GPU_TIER_MAX_REASONS; RT lists Base and the RT items. */
    de_gpu_caps c;
    memset(&c, 0, sizeof c);
    c.backend = DE_GPU_DEFAULT;
    de_gpu_tier_report t;
    de_gpu_evaluate_tier(&c, &t);
    EXPECT_FALSE(t.base_ok);
    EXPECT_EQ(t.base_reason_count, DE_GPU_TIER_MAX_REASONS);
    EXPECT_TRUE(has_reason(&t, false, "no concrete backend"));
    EXPECT_TRUE(has_reason(&t, false, "Base-tier check failed"));
    EXPECT_TRUE(has_reason(&t, false, "no bindless table for samplers"));
    EXPECT_FALSE(t.rt_ok);
    EXPECT_EQ(t.rt_reason_count, 3u);
    EXPECT_TRUE(has_reason(&t, true, "below the Base tier"));
    EXPECT_TRUE(has_reason(&t, true, "no ray tracing"));
    EXPECT_TRUE(has_reason(&t, true, "no inline ray queries"));
    EXPECT_EQ(t.recommended, DE_GPU_TIER_NONE);
}

TEST(gpu_tier, null_caps_and_names) {
    de_gpu_tier_report t;
    de_gpu_evaluate_tier(NULL, &t);
    EXPECT_FALSE(t.base_ok);
    EXPECT_FALSE(t.rt_ok);
    EXPECT_EQ(t.recommended, DE_GPU_TIER_NONE);
    EXPECT_EQ(t.base_reason_count, 1u);
    de_gpu_evaluate_tier(NULL, NULL); /* ignored */

    EXPECT_STREQ(de_gpu_tier_name(DE_GPU_TIER_NONE), "none");
    EXPECT_STREQ(de_gpu_tier_name(DE_GPU_TIER_BASE), "base");
    EXPECT_STREQ(de_gpu_tier_name(DE_GPU_TIER_RT), "rt");
    EXPECT_STREQ(de_gpu_tier_name((de_gpu_tier)42), "unknown");
}

TEST(gpu_tier, null_device) {
    /* The null backend: Base-capable, mesh shaders, no RT -> Base. */
    de_gpu_device_desc desc = {.backend = DE_GPU_NULL};
    de_gpu_device *d = NULL;
    ASSERT_EQ(de_gpu_device_create(&desc, &d), DE_OK);
    de_gpu_tier_report t;
    de_gpu_evaluate_tier(de_gpu_get_caps(d), &t);
    EXPECT_TRUE(t.base_ok);
    EXPECT_FALSE(t.rt_ok);
    EXPECT_TRUE(t.mesh_shaders);
    EXPECT_EQ(t.recommended, DE_GPU_TIER_BASE);
    EXPECT_STREQ(t.recommended_reason, "");
    de_gpu_device_destroy(d);
}
