/* test_d3d12_device.c — the D3D12 backend (DENG-285).
 *
 * Every build host:
 *   - d3d12.caps_evaluator: fake feature tables through de__d3d12_caps_evaluate (Base tier and
 *     its reason, ray tracing, mesh shaders, wave sizes, bindless table sizes).
 *   - d3d12.bindless_shared_numbering: the front-end's shared resource numbering that the D3D12
 *     backend asks for, on the null backend with the vtable flag set and small tables.
 * Windows with the D3D12 backend (CI: WARP on the windows-2022 runners):
 *   - d3d12.device_create: every adapter (hardware ones and WARP) and the best-adapter choice;
 *     caps printed; the Agility runtime is the one loaded.
 *   - d3d12.resources_1000: 1000 buffers, textures and samplers (and placed transient ones)
 *     created and destroyed twice on WARP with the debug layer: no debug-layer errors, no leaks.
 *   - d3d12.bindless_indices: a bindless index is the descriptor slot, at the expected place of
 *     its space's range; creating the object writes that slot in the mirror and in the
 *     shader-visible heap, and not the next one. A buffer also writes its read-only view at
 *     index + DE__D3D12_BUFFER_SRV_OFFSET (DENG-410).
 */
#include "de_test.h"

#include <dengine/de_gpu.h>

#include "gpu/d3d12/d3d12_internal.h"
#include "gpu/gpu_backend.h"

#include <stdio.h>
#include <string.h>

static const char *errmsg(void) {
    const de_error *e = de_error_last();
    return e ? e->message : "";
}

/* ---------- Caps evaluator (portable) ---------- */

/* An RTX 3070-class adapter: Base and RT tier. */
static de__d3d12_features rtx_class(void) {
    de__d3d12_features f;
    memset(&f, 0, sizeof f);
    snprintf(f.adapter_name, sizeof f.adapter_name, "Fake GPU 3070");
    f.vendor_id = 0x10de;
    f.device_id = 0x2484;
    f.dedicated_vram = (u64)8 << 30;
    f.shared_memory = (u64)16 << 30;
    f.feature_level = 0xc200;
    f.shader_model = 0x67;
    f.resource_binding_tier = 3;
    f.resource_heap_tier = 2;
    f.raytracing_tier = DE__D3D12_RAYTRACING_TIER_1_1;
    f.mesh_shader_tier = DE__D3D12_MESH_SHADER_TIER_1;
    f.enhanced_barriers = true;
    f.wave_lane_min = f.wave_lane_max = 32;
    f.tearing = true;
    f.timestamp_frequency = 10000000;
    return f;
}

TEST(d3d12, caps_evaluator) {
    de_gpu_caps c;
    de__d3d12_features f = rtx_class();
    de__d3d12_caps_evaluate(&f, &c);
    EXPECT_EQ(c.backend, DE_GPU_D3D12);
    EXPECT_STREQ(c.device_name, "Fake GPU 3070");
    EXPECT_EQ(c.vendor_id, 0x10deu);
    EXPECT_EQ(c.device_id, 0x2484u);
    EXPECT_EQ(c.dedicated_vram, (u64)8 << 30);
    EXPECT_EQ(c.shared_memory, (u64)16 << 30);
    EXPECT_TRUE(c.base_tier_ok);
    EXPECT_STREQ(c.base_tier_reason, "");
    EXPECT_EQ(c.raytracing, DE_RT_HARDWARE);
    EXPECT_TRUE(c.ray_query);
    EXPECT_TRUE(c.rt_pipelines);
    EXPECT_FALSE(c.opacity_micromaps);
    EXPECT_TRUE(c.mesh_shaders);
    EXPECT_EQ(c.mesh_max_output_vertices, 256u);
    EXPECT_EQ(c.mesh_max_output_primitives, 256u);
    EXPECT_TRUE(c.enhanced_barriers);
    EXPECT_TRUE(c.async_compute);
    EXPECT_TRUE(c.vrr);
    EXPECT_FALSE(c.hdr_output);
    EXPECT_EQ(c.wave_size_min, 32u);
    EXPECT_EQ(c.wave_size_max, 32u);
    EXPECT_EQ(c.timestamp_frequency, (u64)10000000);
    EXPECT_EQ(c.max_push_constant_bytes, DE_GPU_MAX_PUSH_CONSTANT_BYTES);
    /* The bindless tables, plus the buffers' read-only views (DENG-410: one more buffer-sized
     * range), fill the one 1,000,000-descriptor resource heap. */
    EXPECT_EQ(c.max_bindless_textures, DE__D3D12_BINDLESS_TEXTURES);
    EXPECT_EQ(c.max_bindless_storage_textures, DE__D3D12_BINDLESS_STORAGE_TEXTURES);
    EXPECT_EQ(c.max_bindless_buffers, DE__D3D12_BINDLESS_BUFFERS);
    EXPECT_EQ(c.max_bindless_accels, 0u);
    EXPECT_EQ(c.max_bindless_samplers, 2048u);
    EXPECT_EQ(c.max_bindless_textures + c.max_bindless_storage_textures +
                  2u * c.max_bindless_buffers + c.max_bindless_accels,
              1000000u);

    /* Below the Base tier: each missing item is named. */
    f = rtx_class();
    f.shader_model = 0x65;
    de__d3d12_caps_evaluate(&f, &c);
    EXPECT_FALSE(c.base_tier_ok);
    EXPECT_STREQ(c.base_tier_reason, "shader model 6.5 (needs 6.6)");
    EXPECT_TRUE(c.mesh_shaders); /* 6.5 is enough for mesh shaders */

    f = rtx_class();
    f.resource_binding_tier = 2;
    de__d3d12_caps_evaluate(&f, &c);
    EXPECT_FALSE(c.base_tier_ok);
    EXPECT_STREQ(c.base_tier_reason, "resource binding tier 2 (needs 3)");

    f = rtx_class();
    f.shader_model = 0x51;
    f.resource_binding_tier = 1;
    f.mesh_shader_tier = DE__D3D12_MESH_SHADER_TIER_1;
    de__d3d12_caps_evaluate(&f, &c);
    EXPECT_FALSE(c.base_tier_ok);
    EXPECT_STREQ(c.base_tier_reason,
                 "shader model 5.1 (needs 6.6); resource binding tier 1 (needs 3)");
    EXPECT_FALSE(c.mesh_shaders); /* the tier alone is not enough below SM 6.5 */
    EXPECT_EQ(c.mesh_max_output_vertices, 0u);

    /* Exactly the Base tier; newer shader models (6.10 = 0x6a) pass too. */
    f = rtx_class();
    f.shader_model = DE__D3D12_SHADER_MODEL_6_6;
    de__d3d12_caps_evaluate(&f, &c);
    EXPECT_TRUE(c.base_tier_ok);
    f.shader_model = 0x6a;
    de__d3d12_caps_evaluate(&f, &c);
    EXPECT_TRUE(c.base_tier_ok);

    /* Ray tracing tiers. */
    f = rtx_class();
    f.raytracing_tier = 0;
    de__d3d12_caps_evaluate(&f, &c);
    EXPECT_EQ(c.raytracing, DE_RT_NONE);
    EXPECT_FALSE(c.ray_query);
    EXPECT_FALSE(c.rt_pipelines);
    f.raytracing_tier = DE__D3D12_RAYTRACING_TIER_1_0;
    de__d3d12_caps_evaluate(&f, &c);
    EXPECT_EQ(c.raytracing, DE_RT_HARDWARE);
    EXPECT_TRUE(c.rt_pipelines);
    EXPECT_FALSE(c.ray_query); /* inline ray queries are DXR 1.1 */
    f.raytracing_tier = DE__D3D12_RAYTRACING_TIER_1_2;
    de__d3d12_caps_evaluate(&f, &c);
    EXPECT_TRUE(c.ray_query);
    EXPECT_TRUE(c.opacity_micromaps);

    /* WARP: software ray tracing, a long name cut to the caps field, wave max below min fixed. */
    f = rtx_class();
    f.software = true;
    f.vendor_id = 0x1414;
    f.device_id = 0x8c;
    f.wave_lane_min = 4;
    f.wave_lane_max = 0;
    f.mesh_shader_tier = 0;
    f.enhanced_barriers = false;
    memset(f.adapter_name, 'w', sizeof f.adapter_name - 1);
    f.adapter_name[sizeof f.adapter_name - 1] = '\0';
    de__d3d12_caps_evaluate(&f, &c);
    EXPECT_EQ(c.raytracing, DE_RT_API_ONLY);
    EXPECT_TRUE(c.ray_query);
    EXPECT_FALSE(c.mesh_shaders);
    EXPECT_FALSE(c.enhanced_barriers);
    EXPECT_EQ(c.wave_size_min, 4u);
    EXPECT_EQ(c.wave_size_max, 4u);
    EXPECT_EQ(strlen(c.device_name), sizeof c.device_name - 1);

    /* An all-zero table (nothing could be queried) is below the Base tier, not a crash. */
    memset(&f, 0, sizeof f);
    de__d3d12_caps_evaluate(&f, &c);
    EXPECT_FALSE(c.base_tier_ok);
    EXPECT_STREQ(c.base_tier_reason,
                 "shader model 0.0 (needs 6.6); resource binding tier 0 (needs 3)");
    EXPECT_STREQ(c.device_name, "");
}

/* ---------- Shared bindless numbering (front-end, null backend) ---------- */

/* The null backend with small tables, asking for the D3D12 numbering. */
static de_result small_create(de_gpu_device *d, const de_gpu_device_desc *desc, de_gpu_caps *caps) {
    de_result r = de__gpu_null_vtable.device_create(d, desc, caps);
    caps->max_bindless_textures = 4;         /* indices 1 .. 3 (0 is the null descriptor) */
    caps->max_bindless_storage_textures = 3; /* 4 .. 6 */
    caps->max_bindless_buffers = 2;          /* 7 .. 8 */
    caps->max_bindless_samplers = 3;         /* its own table: 1 .. 2 */
    return r;
}

TEST(d3d12, bindless_shared_numbering) {
    de__gpu_backend_vtable vt = de__gpu_null_vtable;
    vt.device_create = small_create;
    vt.bindless_shared_resource_heap = true;
    de__gpu_register_backend(DE_GPU_NULL, &vt);
    de_gpu_device *d = NULL;
    de_result r = de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &d);
    de__gpu_register_backend(DE_GPU_NULL, NULL);
    ASSERT_EQ(r, DE_OK);

    de_gpu_texture_desc td = {.format = DE_FMT_RGBA16_FLOAT,
                              .width = 64,
                              .height = 64,
                              .mip_count = 3,
                              .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE,
                              .name = "t"};
    de_gpu_texture t = de_gpu_texture_create(d, &td);
    ASSERT_TRUE(DE_HANDLE_VALID(t));
    EXPECT_EQ(de_gpu_texture_bindless(d, t, 0, false), 1u);
    EXPECT_EQ(de_gpu_texture_bindless(d, t, 0, true), 4u);
    EXPECT_EQ(de_gpu_texture_bindless(d, t, 1, true), 5u);
    EXPECT_EQ(de_gpu_texture_bindless(d, t, 2, true), 6u);

    de_gpu_buffer_desc bd = {.size = 256, .usage = DE_USAGE_STORAGE, .stride = 16, .name = "b"};
    de_gpu_buffer b0 = de_gpu_buffer_create(d, &bd);
    de_gpu_buffer b1 = de_gpu_buffer_create(d, &bd);
    EXPECT_EQ(de_gpu_buffer_bindless(d, b0), 7u);
    EXPECT_EQ(de_gpu_buffer_bindless(d, b1), 8u);
    de_gpu_sampler s = de_gpu_sampler_create(d, &(de_gpu_sampler_desc){0});
    EXPECT_EQ(de_gpu_sampler_bindless(d, s), 1u);

    /* Each range is full on its own: no space borrows another's slots. */
    de_gpu_buffer b2 = de_gpu_buffer_create(d, &bd);
    EXPECT_FALSE(DE_HANDLE_VALID(b2));
    EXPECT_TRUE(strstr(errmsg(), "bindless buffer table is full") != NULL);
    EXPECT_TRUE(strstr(errmsg(), "indices 7 to 8") != NULL);
    de_gpu_texture_desc sd = td;
    sd.usage = DE_USAGE_STORAGE;
    sd.mip_count = 1;
    de_gpu_texture t2 = de_gpu_texture_create(d, &sd);
    EXPECT_FALSE(DE_HANDLE_VALID(t2)); /* 4 .. 6 are taken by t's mips */
    sd.usage = DE_USAGE_SAMPLED;
    de_gpu_texture t3 = de_gpu_texture_create(d, &sd);
    de_gpu_texture t4 = de_gpu_texture_create(d, &sd);
    EXPECT_EQ(de_gpu_texture_bindless(d, t3, 0, false), 2u);
    EXPECT_EQ(de_gpu_texture_bindless(d, t4, 0, false), 3u);
    EXPECT_FALSE(DE_HANDLE_VALID(de_gpu_texture_create(d, &sd)));

    /* Freed indices come back in their own range after the GPU is done. */
    de_gpu_buffer_destroy(d, b0);
    de_gpu_texture_destroy(d, t);
    de_gpu_wait_idle(d);
    de_gpu_buffer b3 = de_gpu_buffer_create(d, &bd);
    EXPECT_EQ(de_gpu_buffer_bindless(d, b3), 7u);
    sd.usage = DE_USAGE_STORAGE;
    de_gpu_texture t5 = de_gpu_texture_create(d, &sd);
    const u32 st = de_gpu_texture_bindless(d, t5, 0, true);
    EXPECT_GE(st, 4u);
    EXPECT_LE(st, 6u);
    EXPECT_EQ(de__gpu_bindless_count(d, DE__GPU_BINDLESS_STORAGE_TEXTURE), 1u);
    de_gpu_device_destroy(d);
}

/* ---------- Windows: real devices ---------- */
#if defined(_WIN32) && DE_HAS_GPU_D3D12

/* This test executable loads the Agility runtime from D3D12/ next to it (the CMakeLists.txt
 * copies it there). */
#include "gpu/d3d12/d3d12_agility_exports.h"

/* True when the thread's last error or one of its causes contains `text`. */
static bool error_chain_has(const char *text) {
    for (const de_error *e = de_error_last(); e; e = e->cause)
        if (strstr(e->message, text))
            return true;
    return false;
}

#define WARP_VENDOR 0x1414u
#define WARP_DEVICE 0x8cu

static bool is_warp(const de_gpu_caps *c) {
    return c->vendor_id == WARP_VENDOR && c->device_id == WARP_DEVICE;
}

static void print_caps(u32 adapter, const de_gpu_caps *c) {
    printf("  adapter %u: \"%s\" vendor 0x%04x device 0x%04x, %llu MiB dedicated, %llu MiB shared\n"
           "    base tier %s%s%s; rt %d (ray query %d, pipelines %d), mesh %d, enhanced barriers "
           "%d, waves %u..%u, hdr %d, vrr %d, timestamps %llu Hz\n",
           adapter, c->device_name, c->vendor_id, c->device_id,
           (unsigned long long)(c->dedicated_vram >> 20),
           (unsigned long long)(c->shared_memory >> 20), c->base_tier_ok ? "ok" : "NO",
           c->base_tier_ok ? "" : ": ", c->base_tier_reason, (int)c->raytracing, c->ray_query,
           c->rt_pipelines, c->mesh_shaders, c->enhanced_barriers, c->wave_size_min,
           c->wave_size_max, c->hdr_output, c->vrr, (unsigned long long)c->timestamp_frequency);
}

/* Opens WARP (the last adapters are the software ones). DE_ERR_NOT_FOUND when there is none. */
static de_result open_warp(bool debug, de_gpu_device **out) {
    *out = NULL;
    for (u32 n = 1;; ++n) {
        de_gpu_device *d = NULL;
        de_gpu_device_desc desc = {.backend = DE_GPU_D3D12, .adapter = n, .debug_layer = debug};
        de_result r = de_gpu_device_create(&desc, &d);
        if (r == DE_ERR_INVALID_ARGUMENT || r == DE_ERR_UNSUPPORTED)
            return DE_ERR_NOT_FOUND;
        if (r != DE_OK)
            continue;
        if (is_warp(de_gpu_get_caps(d))) {
            *out = d;
            return DE_OK;
        }
        de_gpu_device_destroy(d);
    }
}

static bool ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    if (a < b)
        return false;
    for (size_t i = 0; i < b; ++i) {
        char x = s[a - b + i], y = suffix[i];
        if (x >= 'A' && x <= 'Z')
            x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = (char)(y - 'A' + 'a');
        if (x != y)
            return false;
    }
    return true;
}

TEST(d3d12, device_create) {
    u32 created = 0;
    bool warp = false;
    printf("D3D12 adapters:\n");
    for (u32 n = 1;; ++n) {
        de_gpu_device *d = NULL;
        de_gpu_device_desc desc = {.backend = DE_GPU_D3D12, .adapter = n, .debug_layer = true};
        de_result r = de_gpu_device_create(&desc, &d);
        if (r == DE_ERR_INVALID_ARGUMENT) {
            EXPECT_TRUE(error_chain_has("requested; this machine has"));
            break;
        }
        ASSERT_LT(n, 64u);
        if (r != DE_OK) {
            printf("  adapter %u: no device (%s: %s)\n", n, de_result_str(r), errmsg());
            continue;
        }
        const de_gpu_caps *c = de_gpu_get_caps(d);
        print_caps(n, c);
        EXPECT_EQ(c->backend, DE_GPU_D3D12);
        EXPECT_NE(c->device_name[0], '\0');
        EXPECT_GT(c->timestamp_frequency, (u64)0);
        EXPECT_EQ(c->max_bindless_samplers, 2048u);
        EXPECT_EQ(c->base_tier_ok, c->base_tier_reason[0] == '\0');
        warp = warp || is_warp(c);
        ++created;
        /* An empty submit signals the queue timelines; wait_idle waits for them. */
        for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
            EXPECT_EQ(de_gpu_queue_submit(d, (de_gpu_queue)q, &(de_gpu_submit){0}), DE_OK);
        de_gpu_wait_idle(d);
        EXPECT_EQ(de__d3d12_debug_error_count(d), 0u);
        de_gpu_device_destroy(d);
    }
    EXPECT_GT(created, 0u);
    EXPECT_TRUE(warp); /* WARP is always listed */

    /* Adapter 0: the best one; the platform default is D3D12. */
    de_gpu_device *d = NULL;
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.debug_layer = true}, &d), DE_OK);
    EXPECT_EQ(de_gpu_get_caps(d)->backend, DE_GPU_D3D12);
    printf("best:\n");
    print_caps(0, de_gpu_get_caps(d));
    EXPECT_TRUE(de__d3d12_debug_layer_active(d));
    EXPECT_EQ(de__d3d12_debug_error_count(d), 0u);
    de_gpu_device_destroy(d);

    /* The exports above made the loader take the Agility runtime from D3D12/ next to us. */
    char path[512];
    de__d3d12_runtime_path(path, sizeof path);
    printf("D3D12 runtime: %s\n", path);
    EXPECT_TRUE(ends_with(path, "\\D3D12\\D3D12Core.dll"));
}

/* 1000 objects of every kind the backend creates (texture and buffer variants, samplers,
 * placed transient ones), destroyed, twice. */
TEST(d3d12, resources_1000) {
    de_gpu_device *d = NULL;
    de_result r = open_warp(true, &d);
    if (r != DE_OK)
        SKIP("no WARP adapter");
    ASSERT_TRUE(de__d3d12_debug_layer_active(d));

    enum { N = 1000 };
    static de_gpu_buffer bufs[N];
    static de_gpu_texture texs[N];
    static de_gpu_sampler smps[N];
    for (int round = 0; round < 2; ++round) {
        u32 nb = 0, nt = 0, ns = 0;
        for (u32 i = 0; i < N; ++i) {
            switch (i % 10) {
            case 0: {
                de_gpu_buffer_desc bd = {
                    .size = 4096, .usage = DE_USAGE_STORAGE, .stride = 16, .name = "structured"};
                bufs[nb] = de_gpu_buffer_create(d, &bd);
                ASSERT_TRUE(DE_HANDLE_VALID(bufs[nb]));
                ++nb;
                break;
            }
            case 1: {
                de_gpu_buffer_desc bd = {
                    .size = 1000, .usage = DE_USAGE_VERTEX | DE_USAGE_INDEX, .name = "raw"};
                bufs[nb] = de_gpu_buffer_create(d, &bd);
                ASSERT_TRUE(DE_HANDLE_VALID(bufs[nb]));
                ++nb;
                break;
            }
            case 2: {
                de_gpu_buffer_desc bd = {.size = 256,
                                         .usage = DE_USAGE_CONSTANT | DE_USAGE_COPY_SRC,
                                         .memory = DE_MEM_UPLOAD,
                                         .stride = 4,
                                         .name = "upload"};
                bufs[nb] = de_gpu_buffer_create(d, &bd);
                ASSERT_TRUE(DE_HANDLE_VALID(bufs[nb]));
                u8 *p = (u8 *)de_gpu_buffer_map(d, bufs[nb]);
                ASSERT_TRUE(p != NULL);
                memset(p, (int)(i & 0xff), 256);
                ++nb;
                break;
            }
            case 3: {
                de_gpu_buffer_desc bd = {.size = 512,
                                         .usage = DE_USAGE_COPY_DST,
                                         .memory = DE_MEM_READBACK,
                                         .name = "readback"};
                bufs[nb] = de_gpu_buffer_create(d, &bd);
                ASSERT_TRUE(DE_HANDLE_VALID(bufs[nb]));
                EXPECT_TRUE(de_gpu_buffer_map(d, bufs[nb]) != NULL);
                ++nb;
                break;
            }
            case 4: {
                de_gpu_texture_desc td = {.format = DE_FMT_RGBA8_SRGB,
                                          .width = 64,
                                          .height = 64,
                                          .mip_count = 7,
                                          .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST,
                                          .name = "albedo"};
                texs[nt] = de_gpu_texture_create(d, &td);
                ASSERT_TRUE(DE_HANDLE_VALID(texs[nt]));
                ++nt;
                break;
            }
            case 5: {
                de_gpu_texture_desc td = {.format = DE_FMT_RGBA16_FLOAT,
                                          .width = 128,
                                          .height = 64,
                                          .mip_count = 3,
                                          .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE |
                                                   DE_USAGE_RENDER_TARGET,
                                          .name = "hdr"};
                texs[nt] = de_gpu_texture_create(d, &td);
                ASSERT_TRUE(DE_HANDLE_VALID(texs[nt]));
                ++nt;
                break;
            }
            case 6: {
                static const de_gpu_format depth[] = {DE_FMT_D32_FLOAT, DE_FMT_D32_FLOAT_S8};
                de_gpu_texture_desc td = {.format = depth[(i / 10) % 2],
                                          .width = 128,
                                          .height = 128,
                                          .usage = DE_USAGE_DEPTH |
                                                   ((i / 20) % 2 ? DE_USAGE_SAMPLED : 0u),
                                          .name = "depth"};
                texs[nt] = de_gpu_texture_create(d, &td);
                ASSERT_TRUE(DE_HANDLE_VALID(texs[nt]));
                ++nt;
                break;
            }
            case 7: {
                static const de_gpu_texture_dim dims[] = {DE_TEX_CUBE, DE_TEX_3D, DE_TEX_2D_ARRAY,
                                                          DE_TEX_CUBE};
                const de_gpu_texture_dim dim = dims[(i / 10) % 4];
                de_gpu_texture_desc td = {.dim = dim,
                                          .format = DE_FMT_RGBA8_UNORM,
                                          .width = 32,
                                          .height = 32,
                                          .depth_or_layers = dim == DE_TEX_3D ? 8u
                                                             : dim == DE_TEX_CUBE
                                                                 ? ((i / 10) % 4 == 3 ? 12u : 6u)
                                                                 : 4u,
                                          .mip_count = 2,
                                          .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE,
                                          .name = "volume"};
                texs[nt] = de_gpu_texture_create(d, &td);
                ASSERT_TRUE(DE_HANDLE_VALID(texs[nt]));
                ++nt;
                break;
            }
            case 8: {
                const bool bc = (i / 10) % 2 == 0;
                de_gpu_texture_desc td = {.format = bc ? DE_FMT_BC7_SRGB : DE_FMT_BGRA8_UNORM,
                                          .width = 64,
                                          .height = 64,
                                          .sample_count = bc ? 1u : 4u,
                                          .usage = bc ? DE_USAGE_SAMPLED
                                                      : DE_USAGE_SAMPLED | DE_USAGE_RENDER_TARGET,
                                          .name = bc ? "bc7" : "msaa"};
                texs[nt] = de_gpu_texture_create(d, &td);
                ASSERT_TRUE(DE_HANDLE_VALID(texs[nt]));
                ++nt;
                break;
            }
            default: {
                de_gpu_sampler_desc sd = {.min = DE_FILTER_LINEAR,
                                          .mag = DE_FILTER_LINEAR,
                                          .mip = (de_gpu_filter)(i % 2),
                                          .u = (de_gpu_address)(i % 4),
                                          .v = (de_gpu_address)((i / 4) % 4),
                                          .w = DE_ADDR_CLAMP,
                                          .max_aniso = (f32)((i / 10) % 17),
                                          .compare_reverse_z = (i / 10) % 3 == 0};
                smps[ns] = de_gpu_sampler_create(d, &sd);
                ASSERT_TRUE(DE_HANDLE_VALID(smps[ns]));
                ++ns;
                break;
            }
            }
        }
        EXPECT_EQ(nb + nt + ns, (u32)N);
        EXPECT_EQ(de__gpu_live_count(d, DE__GPU_BUFFER), nb);
        EXPECT_EQ(de__gpu_live_count(d, DE__GPU_TEXTURE), nt);
        EXPECT_EQ(de__gpu_live_count(d, DE__GPU_SAMPLER), ns);

        /* Placed transient resources aliasing one region. */
        de_gpu_texture_desc rt = {.format = DE_FMT_RGBA16_FLOAT,
                                  .width = 256,
                                  .height = 256,
                                  .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED,
                                  .name = "transient"};
        de__gpu_mem_req req;
        ASSERT_EQ(de__gpu_texture_requirements(d, &rt, &req), DE_OK);
        u32 cls = 0;
        while (!(req.class_mask & (1u << cls)))
            ++cls;
        de__gpu_transient_region *region = NULL;
        ASSERT_EQ(de__gpu_transient_region_create(d, cls, req.size * 2, &region), DE_OK);
        de_gpu_texture p0 = de__gpu_place_transient_texture(region, 0, &rt);
        de_gpu_texture p1 = de__gpu_place_transient_texture(region, 0, &rt);
        EXPECT_TRUE(DE_HANDLE_VALID(p0));
        EXPECT_TRUE(DE_HANDLE_VALID(p1));
        de_gpu_texture_destroy(d, p0);
        de_gpu_texture_destroy(d, p1);
        de__gpu_transient_region_destroy(region);

        for (u32 i = 0; i < nb; ++i)
            de_gpu_buffer_destroy(d, bufs[i]);
        for (u32 i = 0; i < nt; ++i)
            de_gpu_texture_destroy(d, texs[i]);
        for (u32 i = 0; i < ns; ++i)
            de_gpu_sampler_destroy(d, smps[i]);
        /* The destroys wait for the queues' timelines; a submit moves them on. */
        EXPECT_EQ(de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){0}), DE_OK);
        de_gpu_wait_idle(d);
        EXPECT_EQ(de__gpu_deferred_count(d), 0u);
        EXPECT_EQ(de__gpu_live_count(d, DE__GPU_BUFFER), 0u);
        EXPECT_EQ(de__gpu_live_count(d, DE__GPU_TEXTURE), 0u);
        EXPECT_EQ(de__gpu_live_count(d, DE__GPU_SAMPLER), 0u);
        for (u32 s = 0; s < DE__GPU_BINDLESS_SPACE_COUNT; ++s)
            EXPECT_EQ(de__gpu_bindless_count(d, (de__gpu_bindless_space)s), 0u);
        de__gpu_mem_stats st;
        ASSERT_EQ(de__gpu_mem_class_stats(d, DE__GPU_MEM_CLASS_GPU_ONLY, &st), DE_OK);
        EXPECT_EQ(st.allocations, 0u);
    }
    EXPECT_EQ(de__d3d12_debug_error_count(d), 0u);
    de_gpu_device_destroy(d);
}

/* Watching a descriptor slot. D3D12 descriptors are opaque, and WARP's depend on where they are
 * and when they were written (a copy into the shader-visible heap, or the same view written
 * again, gives other bytes), so the test cannot compare contents. It checks where writes land:
 * on a fresh device the front-end hands out each range's indices in order, so the test knows the
 * next index, reads that slot and the one after it in both heaps, creates the object, and
 * expects the object's index to be that slot, the slot to have changed in the mirror and in the
 * shader-visible heap, and the next slot to be untouched. */
typedef struct slot_watch {
    bool sampler;
    u32 index;
    u32 n;
    u8 mirror[256], visible[256], next[256];
} slot_watch;

static void watch_begin(de_gpu_device *d, bool sampler, u32 index, slot_watch *w) {
    w->sampler = sampler;
    w->index = index;
    w->n = de__d3d12_descriptor_bytes(d, sampler, false, index, w->mirror, sizeof w->mirror);
    ASSERT_GT(w->n, 0u);
    ASSERT_LE(w->n, (u32)sizeof w->mirror);
    ASSERT_EQ(de__d3d12_descriptor_bytes(d, sampler, true, index, w->visible, sizeof w->visible),
              w->n);
    ASSERT_EQ(de__d3d12_descriptor_bytes(d, sampler, true, index + 1, w->next, sizeof w->next),
              w->n);
}

static void watch_end(de_gpu_device *d, u32 got, const slot_watch *w) {
    u8 mirror[256], visible[256], next[256];
    EXPECT_EQ(got, w->index);
    ASSERT_EQ(de__d3d12_descriptor_bytes(d, w->sampler, false, w->index, mirror, sizeof mirror),
              w->n);
    ASSERT_EQ(de__d3d12_descriptor_bytes(d, w->sampler, true, w->index, visible, sizeof visible),
              w->n);
    ASSERT_EQ(de__d3d12_descriptor_bytes(d, w->sampler, true, w->index + 1, next, sizeof next),
              w->n);
    EXPECT_TRUE(memcmp(mirror, w->mirror, w->n) != 0);   /* written in the mirror */
    EXPECT_TRUE(memcmp(visible, w->visible, w->n) != 0); /* and in the shader-visible heap */
    EXPECT_MEMEQ(next, w->next, w->n);                   /* at that slot only */
}

TEST(d3d12, bindless_indices) {
    de_gpu_device *d = NULL;
    if (open_warp(true, &d) != DE_OK)
        SKIP("no WARP adapter");
    const u32 tex0 = 1; /* slot 0 is the null descriptor */
    const u32 storage0 = DE__D3D12_BINDLESS_TEXTURES;
    const u32 buffer0 = DE__D3D12_BINDLESS_TEXTURES + DE__D3D12_BINDLESS_STORAGE_TEXTURES;
    slot_watch w;

    /* A structured GPU_ONLY buffer (a UAV), then a raw UPLOAD buffer (an SRV): the first two
     * slots of the buffer range. */
    de_gpu_buffer_desc bd = {.size = 1024, .usage = DE_USAGE_STORAGE, .stride = 16, .name = "b"};
    slot_watch ro; /* DENG-410: the read-only view (SRV), DE__D3D12_BUFFER_SRV_OFFSET slots on */
    watch_begin(d, false, buffer0, &w);
    watch_begin(d, false, buffer0 + DE__D3D12_BUFFER_SRV_OFFSET, &ro);
    de_gpu_buffer b = de_gpu_buffer_create(d, &bd);
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    watch_end(d, de_gpu_buffer_bindless(d, b), &w);
    watch_end(d, de_gpu_buffer_bindless(d, b) + DE__D3D12_BUFFER_SRV_OFFSET, &ro);
    EXPECT_EQ(buffer0 + DE__D3D12_BUFFER_SRV_OFFSET + DE__D3D12_BINDLESS_BUFFERS,
              DE__D3D12_RESOURCE_HEAP_SIZE); /* the read-only range is the last one */
    de_gpu_buffer_desc ud = {
        .size = 64, .usage = DE_USAGE_CONSTANT, .memory = DE_MEM_UPLOAD, .name = "u"};
    watch_begin(d, false, buffer0 + 1, &w);
    de_gpu_buffer u = de_gpu_buffer_create(d, &ud);
    ASSERT_TRUE(DE_HANDLE_VALID(u));
    watch_end(d, de_gpu_buffer_bindless(d, u), &w);

    /* A texture: its sampled view at the start of the texture range; one storage view per mip
     * at the start of the storage range. Each view is checked by creating a texture with only
     * that usage, then both together. */
    de_gpu_texture_desc td = {.format = DE_FMT_RGBA16_FLOAT,
                              .width = 64,
                              .height = 32,
                              .mip_count = 2,
                              .usage = DE_USAGE_SAMPLED,
                              .name = "t"};
    watch_begin(d, false, tex0, &w);
    de_gpu_texture t0 = de_gpu_texture_create(d, &td);
    ASSERT_TRUE(DE_HANDLE_VALID(t0));
    watch_end(d, de_gpu_texture_bindless(d, t0, 0, false), &w);
    td.usage = DE_USAGE_STORAGE;
    td.mip_count = 1;
    watch_begin(d, false, storage0, &w);
    de_gpu_texture t1 = de_gpu_texture_create(d, &td);
    ASSERT_TRUE(DE_HANDLE_VALID(t1));
    watch_end(d, de_gpu_texture_bindless(d, t1, 0, true), &w);
    td.usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE;
    td.mip_count = 2;
    de_gpu_texture t2 = de_gpu_texture_create(d, &td);
    ASSERT_TRUE(DE_HANDLE_VALID(t2));
    EXPECT_EQ(de_gpu_texture_bindless(d, t2, 0, false), tex0 + 1);
    EXPECT_EQ(de_gpu_texture_bindless(d, t2, 0, true), storage0 + 1);
    EXPECT_EQ(de_gpu_texture_bindless(d, t2, 1, true), storage0 + 2);

    /* A sampler: its own heap, from slot 1. */
    de_gpu_sampler_desc sd = {.min = DE_FILTER_LINEAR,
                              .mag = DE_FILTER_LINEAR,
                              .u = DE_ADDR_CLAMP,
                              .v = DE_ADDR_MIRROR,
                              .w = DE_ADDR_BORDER,
                              .max_aniso = 8,
                              .compare_reverse_z = true};
    D3D12_SAMPLER_DESC dsd;
    de__d3d12_sampler_desc(&sd, &dsd);
    EXPECT_EQ(dsd.ComparisonFunc, D3D12_COMPARISON_FUNC_GREATER_EQUAL);
    EXPECT_EQ(dsd.MaxAnisotropy, 8u);
    EXPECT_EQ(dsd.Filter, D3D12_FILTER_COMPARISON_ANISOTROPIC);
    watch_begin(d, true, 1, &w);
    de_gpu_sampler s = de_gpu_sampler_create(d, &sd);
    ASSERT_TRUE(DE_HANDLE_VALID(s));
    watch_end(d, de_gpu_sampler_bindless(d, s), &w);

    /* A recycled index is rewritten in place for its new object. */
    de_gpu_buffer_destroy(d, b);
    de_gpu_wait_idle(d);
    watch_begin(d, false, buffer0, &w);
    bd.stride = 0; /* now a raw view: other bytes */
    de_gpu_buffer b2 = de_gpu_buffer_create(d, &bd);
    ASSERT_TRUE(DE_HANDLE_VALID(b2));
    watch_end(d, de_gpu_buffer_bindless(d, b2), &w);

    de_gpu_buffer_destroy(d, b2);
    de_gpu_buffer_destroy(d, u);
    de_gpu_texture_destroy(d, t0);
    de_gpu_texture_destroy(d, t1);
    de_gpu_texture_destroy(d, t2);
    de_gpu_sampler_destroy(d, s);
    EXPECT_EQ(de__d3d12_debug_error_count(d), 0u);
    de_gpu_device_destroy(d);
}

#endif /* _WIN32 && DE_HAS_GPU_D3D12 */
