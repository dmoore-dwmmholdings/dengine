/*
 * test_renderer_settings.c: de_renderer create/destroy, settings, Base/RT tiers, quality presets,
 * cvars and the frame loop (DENG-174; SPEC §12.8, §3.3, §5.8, R7). CTest names renderer.<test>.
 *
 *   - Pure settings tests on synthetic de_gpu_caps: the clamp to the device, the preset tables,
 *     the caps-only auto selection (Apple M1/M2, Steam Deck, RTX 3070, M3).
 *   - The renderer on the null backend: create/destroy with an asset system (tracking allocators,
 *     nothing leaks), argument checks, a device below the Base tier, cvars applied at the next
 *     frame, 100 frames without heap allocation.
 *   - The renderer on Metal (macOS host): create/destroy, and 100 frames of clear + "present"
 *     into the offscreen target without engine heap allocation, then the clear colour read back.
 *     They SKIP without a Metal device, or on one below the Base tier (the hosted CI runners'
 *     paravirtual device), with the renderer's reason.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "de_test.h"

#include "de_alloc_guard.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>
#include <dengine/de_task.h>

#include "render/loaders/render_assets.h"
#include "render/loaders/upload_ring.h"
#include "render/renderer_internal.h"
#include "render/world/render_world.h"
#if defined(DE_RENDER_PUMP_PACK_TEST)
#include "loader_test_util.h"
#include "dpak_writer.h"
#if defined(_WIN32)
#include <process.h>
#define pump_test_pid _getpid
#else
#include <unistd.h>
#define pump_test_pid getpid
#endif
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_RR_TEST_TMP_DIR
#error "DE_RR_TEST_TMP_DIR must be defined by tests/unit/render/CMakeLists.txt"
#endif

#define WAIT_NS (30ull * 1000000000ull)

/* ---- Synthetic caps -------------------------------------------------------------------------- */

/* A device that meets the Base tier on Vulkan, without ray tracing. */
static de_gpu_caps base_caps(u32 vendor, u32 device, const char *name) {
    de_gpu_caps c;
    memset(&c, 0, sizeof c);
    c.backend = DE_GPU_VULKAN;
    snprintf(c.device_name, sizeof c.device_name, "%s", name);
    c.vendor_id = vendor;
    c.device_id = device;
    c.raytracing = DE_RT_NONE;
    c.max_bindless_textures = 500000;
    c.max_bindless_buffers = 500000;
    c.max_bindless_storage_textures = 500000;
    c.max_bindless_samplers = 2048;
    c.max_push_constant_bytes = DE_GPU_MAX_PUSH_CONSTANT_BYTES;
    c.base_tier_ok = true;
    return c;
}

static de_gpu_caps rt_caps(u32 vendor, u32 device, const char *name, de_rt_support rt) {
    de_gpu_caps c = base_caps(vendor, device, name);
    c.raytracing = rt;
    c.ray_query = true;
    c.rt_pipelines = true;
    c.max_bindless_accels = 1024;
    return c;
}

static de_gpu_caps apple_caps(const char *name, de_rt_support rt) {
    de_gpu_caps c = rt_caps(0x106Bu, 0, name, rt);
    c.backend = DE_GPU_METAL;
    c.unified_memory = true;
    c.rt_pipelines = false;
    return c;
}

static de_render_settings clamped(const de_gpu_caps *caps, de_render_settings s) {
    de_gpu_tier_report rep;
    de_gpu_evaluate_tier(caps, &rep);
    de__render_settings_clamp(caps, &rep, &s);
    return s;
}

static void auto_select(const de_gpu_caps *caps, de_tier *t, de_quality *q) {
    de_gpu_tier_report rep;
    de_gpu_evaluate_tier(caps, &rep);
    de__render_auto_select(caps, &rep, t, q);
}

static bool any_rt(const de_render_settings *s) {
    return s->rt_shadows || s->rt_ao || s->rt_reflections || s->rt_gi || s->rt_refraction ||
           s->path_tracer;
}

/* ---- Clamp ----------------------------------------------------------------------------------- */

TEST(renderer, settings_clamp_caps) {
    /* RT requested on a device without ray tracing: Base, every RT effect off. */
    const de_gpu_caps none = base_caps(0x10DEu, 0x1C03u, "NVIDIA GeForce GTX 1060");
    de_render_settings s = de_render_settings_preset(DE_TIER_RT, DE_QUALITY_ULTRA);
    s.path_tracer = true;
    s.render_scale = 2.0f;
    de_render_settings c = clamped(&none, s);
    EXPECT_EQ(c.tier, DE_TIER_BASE);
    EXPECT_FALSE(any_rt(&c));
    EXPECT_FLOAT_EQ(c.render_scale, 1.0f, 0.0);
    EXPECT_EQ(c.shadows, DE_QUALITY_ULTRA); /* qualities stay */
    /* RT Ultra at scale 2.0, clamped to Base at scale 1.0: Base Ultra. */
    const de_render_settings base_ultra = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_ULTRA);
    EXPECT_TRUE(de__render_settings_equal(&c, &base_ultra));
    /* RT Ultra's own render scale (0.77) at Base is not a Base preset. */
    c = clamped(&none, de_render_settings_preset(DE_TIER_RT, DE_QUALITY_ULTRA));
    EXPECT_EQ(c.tier, DE_TIER_BASE);
    EXPECT_FALSE(de_render_settings_is_preset(&c));

    /* Ray tracing without inline ray queries: Base too. */
    de_gpu_caps no_query = rt_caps(0x10DEu, 0x2484u, "NVIDIA GeForce RTX 3070", DE_RT_HARDWARE);
    no_query.ray_query = false;
    EXPECT_EQ(clamped(&no_query, s).tier, DE_TIER_BASE);

    /* A device below Base never gets RT. */
    de_gpu_caps below = rt_caps(0x10DEu, 0x2484u, "NVIDIA GeForce RTX 3070", DE_RT_HARDWARE);
    below.base_tier_ok = false;
    EXPECT_EQ(clamped(&below, s).tier, DE_TIER_BASE);

    /* Hardware RT keeps it; API-only RT (M1/M2) keeps it too when asked for (SPEC §3.3). */
    const de_gpu_caps hw = rt_caps(0x10DEu, 0x2484u, "NVIDIA GeForce RTX 3070", DE_RT_HARDWARE);
    c = clamped(&hw, s);
    EXPECT_EQ(c.tier, DE_TIER_RT);
    EXPECT_TRUE(c.rt_refraction);
    EXPECT_TRUE(c.path_tracer);
    const de_gpu_caps m1 = apple_caps("Apple M1", DE_RT_API_ONLY);
    EXPECT_EQ(clamped(&m1, s).tier, DE_TIER_RT);

    /* Ranges. */
    s = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH);
    s.render_scale = 0.1f;
    EXPECT_FLOAT_EQ(clamped(&hw, s).render_scale, 0.33f, 0.0);
    s.render_scale = NAN;
    EXPECT_FLOAT_EQ(clamped(&hw, s).render_scale, 1.0f, 0.0);
    s.render_scale = 0.5f;
    EXPECT_FLOAT_EQ(clamped(&hw, s).render_scale, 0.5f, 0.0);
    s.path_max_bounces = 0;
    EXPECT_EQ(clamped(&hw, s).path_max_bounces, 1u);
    s.path_max_bounces = 1000;
    EXPECT_EQ(clamped(&hw, s).path_max_bounces, 64u);
    s.dynamic_res_target_ms = -4.0f;
    EXPECT_FLOAT_EQ(clamped(&hw, s).dynamic_res_target_ms, 0.0f, 0.0);
    s.shadows = (de_quality)42;
    s.upscaler = (de_upscaler)42;
    s.tier = (de_tier)7;
    c = clamped(&hw, s);
    EXPECT_EQ(c.shadows, DE_QUALITY_OFF);
    EXPECT_EQ(c.upscaler, DE_UPSCALE_AUTO);
    EXPECT_EQ(c.tier, DE_TIER_BASE);

    /* A zeroed desc is valid. */
    c = clamped(&hw, (de_render_settings){0});
    EXPECT_EQ(c.tier, DE_TIER_BASE);
    EXPECT_FLOAT_EQ(c.render_scale, 0.33f, 0.0);
    EXPECT_EQ(c.path_max_bounces, 1u);

    /* Every preset survives the clamp on a device that can run it. */
    for (int t = DE_TIER_BASE; t <= DE_TIER_RT; ++t)
        for (int q = DE_QUALITY_LOW; q <= DE_QUALITY_ULTRA; ++q) {
            const de_render_settings p = de_render_settings_preset((de_tier)t, (de_quality)q);
            const de_render_settings pc = clamped(&hw, p);
            EXPECT_TRUE(de_render_settings_is_preset(&pc));
            EXPECT_EQ(pc.tier, p.tier);
        }
}

/* ---- Presets --------------------------------------------------------------------------------- */

TEST(renderer, presets_roundtrip) {
    for (int t = DE_TIER_BASE; t <= DE_TIER_RT; ++t) {
        u32 prev_rt = 0;
        for (int q = DE_QUALITY_LOW; q <= DE_QUALITY_ULTRA; ++q) {
            const de_render_settings p = de_render_settings_preset((de_tier)t, (de_quality)q);
            EXPECT_TRUE(de_render_settings_is_preset(&p));
            de_quality m = DE_QUALITY_OFF;
            EXPECT_TRUE(de__render_settings_match(&p, &m));
            EXPECT_EQ(m, (de_quality)q);
            EXPECT_EQ(p.tier, (de_tier)t);
            EXPECT_EQ(p.upscaler, DE_UPSCALE_AUTO);
            EXPECT_FALSE(p.path_tracer);
            EXPECT_FLOAT_EQ(p.dynamic_res_target_ms, 0.0f, 0.0);
            EXPECT_TRUE(p.render_scale >= 0.33f);
            EXPECT_TRUE(p.render_scale <= 1.0f);
            /* Base presets have no RT effect; RT presets add effects as quality rises. */
            const u32 rt = (u32)p.rt_shadows + (u32)p.rt_ao + (u32)p.rt_reflections + (u32)p.rt_gi +
                           (u32)p.rt_refraction;
            if (t == DE_TIER_BASE) {
                EXPECT_EQ(rt, 0u);
            } else {
                EXPECT_GT(rt, prev_rt);
                EXPECT_TRUE(p.rt_shadows);
            }
            prev_rt = rt;
            /* Any change of a field makes it Custom. */
            de_render_settings c = p;
            c.render_scale -= 0.01f;
            EXPECT_FALSE(de_render_settings_is_preset(&c));
            c = p;
            c.path_max_bounces += 1;
            EXPECT_FALSE(de_render_settings_is_preset(&c));
            c = p;
            c.sss = c.sss == DE_QUALITY_OFF ? DE_QUALITY_LOW : DE_QUALITY_OFF;
            EXPECT_FALSE(de_render_settings_is_preset(&c));
            c = p;
            c.dynamic_res_target_ms = 16.6f;
            EXPECT_FALSE(de_render_settings_is_preset(&c));
            /* Presets differ from each other. */
            for (int q2 = DE_QUALITY_LOW; q2 < q; ++q2) {
                const de_render_settings o = de_render_settings_preset((de_tier)t, (de_quality)q2);
                EXPECT_TRUE(o.render_scale != p.render_scale || o.shadows != p.shadows);
            }
        }
    }
    /* Out-of-range arguments clamp. */
    de_render_settings a = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_OFF);
    de_render_settings b = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW);
    EXPECT_TRUE(de__render_settings_equal(&a, &b));
    a = de_render_settings_preset(DE_TIER_RT, (de_quality)99);
    b = de_render_settings_preset(DE_TIER_RT, DE_QUALITY_ULTRA);
    EXPECT_TRUE(de__render_settings_equal(&a, &b));
    a = de_render_settings_preset((de_tier)5, DE_QUALITY_HIGH);
    EXPECT_EQ(a.tier, DE_TIER_BASE);
    EXPECT_FALSE(de_render_settings_is_preset(NULL));
    /* A settings struct with a bad tier is never a preset. */
    a = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH);
    a.tier = (de_tier)9;
    EXPECT_FALSE(de_render_settings_is_preset(&a));
    /* Auto without a renderer: Base Medium. */
    a = de_render_settings_auto(NULL);
    b = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_MEDIUM);
    EXPECT_TRUE(de__render_settings_equal(&a, &b));
}

/* ---- Auto selection -------------------------------------------------------------------------- */

TEST(renderer, auto_tier_api_only) {
    de_tier t = DE_TIER_RT;
    de_quality q = DE_QUALITY_OFF;
    /* Apple M1 and M2: Metal ray tracing without hardware acceleration. */
    const char *const names[] = {"Apple M1", "Apple M1 Pro", "Apple M2 Max"};
    for (u32 i = 0; i < 3; ++i) {
        const de_gpu_caps c = apple_caps(names[i], DE_RT_API_ONLY);
        auto_select(&c, &t, &q);
        EXPECT_EQ(t, DE_TIER_BASE);
        EXPECT_EQ(q, DE_QUALITY_MEDIUM);
    }
    /* API-only RT on any device (not on the list, discrete): Base Medium. */
    de_gpu_caps soft = rt_caps(0x8086u, 0x56A0u, "Intel Arc A770", DE_RT_API_ONLY);
    auto_select(&soft, &t, &q);
    EXPECT_EQ(t, DE_TIER_BASE);
    EXPECT_EQ(q, DE_QUALITY_MEDIUM);
    /* M3 (Apple9: hardware RT, unified memory): RT Medium. */
    const de_gpu_caps m3 = apple_caps("Apple M3 Pro", DE_RT_HARDWARE);
    auto_select(&m3, &t, &q);
    EXPECT_EQ(t, DE_TIER_RT);
    EXPECT_EQ(q, DE_QUALITY_MEDIUM);
    /* A discrete GPU with hardware RT: RT High. */
    const de_gpu_caps rtx = rt_caps(0x10DEu, 0x2484u, "NVIDIA GeForce RTX 3070", DE_RT_HARDWARE);
    auto_select(&rtx, &t, &q);
    EXPECT_EQ(t, DE_TIER_RT);
    EXPECT_EQ(q, DE_QUALITY_HIGH);
    /* A discrete Base-only GPU: Base High (desktop Base: 1080p60, DENG-2 Q4). */
    const de_gpu_caps gtx = base_caps(0x10DEu, 0x1C03u, "NVIDIA GeForce GTX 1060");
    auto_select(&gtx, &t, &q);
    EXPECT_EQ(t, DE_TIER_BASE);
    EXPECT_EQ(q, DE_QUALITY_HIGH);
}

TEST(renderer, auto_tier_deck) {
    /* Steam Deck LCD (Van Gogh) and OLED: RDNA2 ray queries, Base Medium by device ID (R7). The
     * caps say "discrete" on purpose: the device ID decides, not the memory kind. */
    const u32 ids[] = {0x163Fu, 0x1435u};
    for (u32 i = 0; i < 2; ++i) {
        const de_gpu_caps deck = rt_caps(0x1002u, ids[i], "AMD Custom GPU 0405", DE_RT_HARDWARE);
        de_tier t = DE_TIER_RT;
        de_quality q = DE_QUALITY_OFF;
        auto_select(&deck, &t, &q);
        EXPECT_EQ(t, DE_TIER_BASE);
        EXPECT_EQ(q, DE_QUALITY_MEDIUM);
        /* RT stays available by choice. */
        EXPECT_EQ(clamped(&deck, de_render_settings_preset(DE_TIER_RT, DE_QUALITY_LOW)).tier,
                  DE_TIER_RT);
    }
    /* Another AMD RDNA2 device is not the Deck. */
    const de_gpu_caps rx = rt_caps(0x1002u, 0x73BFu, "AMD Radeon RX 6800", DE_RT_HARDWARE);
    de_tier t;
    de_quality q;
    auto_select(&rx, &t, &q);
    EXPECT_EQ(t, DE_TIER_RT);
    EXPECT_EQ(q, DE_QUALITY_HIGH);
}

/* ---- Renderer fixture ------------------------------------------------------------------------ */

typedef struct rfx {
    de_tracking_allocator track, dev_track;
    de_allocator alloc, dev_alloc;
    de_task_system *tasks;
    de_asset_system *assets;
    de_type_registry *types;
    de_gpu_device *d;
    de_renderer *r;
    de_world *world;
    char upload_pack_path[160];
    char why[1200];
} rfx;

/* Puts the selection cvars back to "auto", so tests do not see each other's choices. */
static void reset_cvars(void) {
    static const char *const names[] = {"r.tier", "r.quality"};
    for (u32 i = 0; i < 2; ++i) {
        const de_cvar cv = de_cvar_find(names[i]);
        if (DE_HANDLE_VALID(cv))
            (void)de_cvar_set(cv, "auto");
    }
}

static void rfx_setup(rfx *f) {
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("rr", DE_RR_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
        mounted = true;
    }
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    f->dev_alloc = de_tracking_allocator_init(&f->dev_track, NULL, DE_MEM_RENDER);
    de_fp_env_init_thread(); /* the task system checks the main thread's FP mode */
    reset_cvars();
}

static void rfx_teardown(rfx *f) {
    if (f->assets != NULL)
        de_asset_system_destroy(f->assets); /* first: its unloads use the renderer's uploader */
    de_renderer_destroy(f->r);
    if (f->world != NULL)
        de_world_destroy(f->world);
    if (f->upload_pack_path[0])
        EXPECT_EQ(de_file_delete(f->upload_pack_path), DE_OK);
    if (f->d != NULL)
        de_gpu_device_destroy(f->d);
    if (f->types != NULL)
        de_type_registry_destroy(f->types);
    if (f->tasks != NULL)
        de_task_system_destroy(f->tasks);
    reset_cvars();
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->dev_track).bytes_live, 0u);
}

static bool backend_built(de_gpu_backend b) {
    switch (b) {
    case DE_GPU_NULL:
        return true;
#if DE_HAS_GPU_METAL
    case DE_GPU_METAL:
        return true;
#endif
    default:
        return false;
    }
}

static void error_chain(char *buf, size_t cap) {
    size_t n = 0;
    buf[0] = '\0';
    for (const de_error *e = de_error_last(); e != NULL && n + 1 < cap; e = e->cause) {
        const int w = snprintf(buf + n, cap - n, "%s%s", n ? ": " : "", e->message);
        if (w < 0)
            break;
        n += (size_t)w < cap - n ? (size_t)w : cap - n - 1;
    }
}

/* The device, the asset system (with a type registry) and the renderer; SKIPs without the device
 * or when the device is below the Base tier. */
static void start(rfx *f, de_gpu_backend b, bool with_assets) {
    if (!backend_built(b)) {
        snprintf(f->why, sizeof f->why, "the %s backend is not in this build",
                 de_gpu_backend_name(b));
        SKIP(f->why);
    }
#if !defined(_WIN32)
    if (b == DE_GPU_METAL)
        setenv("MTL_DEBUG_LAYER", "1", 0);
#endif
    de_gpu_device_desc dd = {.backend = b,
                             .debug_layer = b != DE_GPU_NULL,
                             .allocator = &f->dev_alloc,
                             .pipeline_cache_dir = ""};
    if (de_gpu_device_create(&dd, &f->d) != DE_OK) {
        char err[900];
        error_chain(err, sizeof err);
        snprintf(f->why, sizeof f->why, "no usable %s device: %s", de_gpu_backend_name(b), err);
        SKIP(f->why);
    }
    printf("device: \"%s\" (%s)\n", de_gpu_get_caps(f->d)->device_name, de_gpu_backend_name(b));
    if (with_assets) {
        ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &f->tasks),
                  DE_OK);
        ASSERT_EQ(de_type_registry_create(&f->alloc, &f->types), DE_OK);
        ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &f->alloc,
                                                                 .tasks = f->tasks,
                                                                 .io_threads = 1,
                                                                 .cache_dir = "rr:/decache",
                                                                 .types = f->types},
                                         &f->assets),
                  DE_OK);
    }
    const de_result r = de_renderer_create(
        &(de_renderer_desc){
            .device = f->d, .assets = f->assets, .types = f->types, .allocator = &f->alloc},
        &f->r);
    if (r == DE_ERR_UNSUPPORTED) {
        char err[900];
        error_chain(err, sizeof err);
        snprintf(f->why, sizeof f->why, "%s", err);
        SKIP(f->why); /* below the Base tier: the hosted CI runners' paravirtual Metal device */
    }
    ASSERT_EQ(r, DE_OK);
    ASSERT_TRUE(f->r != NULL);
}

static void run_create_destroy(rfx *f, de_gpu_backend b) {
    start(f, b, true);
    de_renderer *r = f->r;
    EXPECT_EQ(r->frames_in_flight, 2u);
    EXPECT_TRUE(de__renderer_uploader(r) != NULL);
    EXPECT_TRUE(DE_HANDLE_VALID(de__renderer_offscreen(r)));
    /* The settings start at auto, clamped: a preset of the device's recommended tier. */
    const de_render_settings s = de_renderer_get_settings(r);
    const de_render_settings a = de_render_settings_auto(r);
    EXPECT_TRUE(de__render_settings_equal(&s, &a));
    EXPECT_TRUE(de_render_settings_is_preset(&s));
    de_gpu_tier_report rep;
    de_gpu_evaluate_tier(de_gpu_get_caps(f->d), &rep);
    EXPECT_EQ(s.tier == DE_TIER_RT, rep.recommended == DE_GPU_TIER_RT);
    /* The render asset types are registered: a second renderer on the same asset system is
     * refused. */
    de_renderer *r2 = NULL;
    EXPECT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->d, .assets = f->assets}, &r2),
              DE_ERR_ALREADY_EXISTS);
    EXPECT_TRUE(r2 == NULL);
    /* A few frames, then destroy (the fixture checks that nothing leaks). */
    for (u32 i = 0; i < 4; ++i)
        ASSERT_EQ(de_renderer_render_frame(r), DE_OK);
    u64 frames = 0;
    (void)de__renderer_frame_fence(r, &frames);
    EXPECT_EQ(frames, 4u);
}

TEST_F(renderer, create_destroy_null, rfx) {
    run_create_destroy(f, DE_GPU_NULL);
}

TEST_F(renderer, create_destroy_metal, rfx) {
    run_create_destroy(f, DE_GPU_METAL);
}

#if defined(DE_RENDER_PUMP_PACK_TEST)
/* Real asset loads must progress without a view/frame; copy fences still gate publication.
 * Readback deliberately supplies no uploader, so it cannot repair a missing queue acquire. */
static void pump_uploads_without_frames(rfx *f, de_gpu_backend backend) {
    start(f, backend, true);
    de_renderer_pump_uploads(NULL);
    const de_render_stats initial_stats = *de_renderer_stats(f->r);
    snprintf(f->upload_pack_path, sizeof f->upload_pack_path, "rr:/pump-%lu-%llu.dpak",
             (unsigned long)pump_test_pid(), (unsigned long long)de_time_now_ns());
    lt_blob texture = lt_dtex(DE_TEX_2D, DE_FMT_RGBA8_UNORM, 64, 32, 1, 1, 0, 7, false);
    lt_blob mesh = lt_dmsh(300, 0, 11);
    ASSERT_TRUE(texture.data != NULL && mesh.data != NULL);
    dpak_writer *writer = NULL;
    ASSERT_EQ(dpak_writer_create(&(dpak_writer_desc){.include_names = true}, &writer), DE_OK);
    ASSERT_EQ(dpak_writer_add(writer, de_asset_id_from("pump/t").hash, "texture", texture.data,
                              texture.size, NULL, 0, "pump/t"),
              DE_OK);
    ASSERT_EQ(dpak_writer_add(writer, de_asset_id_from("pump/m").hash, "mesh", mesh.data, mesh.size,
                              NULL, 0, "pump/m"),
              DE_OK);
    ASSERT_EQ(dpak_writer_add(writer, de_asset_id_from("pump/bad").hash, "texture", "broken", 6,
                              NULL, 0, "pump/bad"),
              DE_OK);
    const u64 ids[] = {de_asset_id_from("pump/t").hash, de_asset_id_from("pump/m").hash};
    ASSERT_EQ(dpak_writer_add_bundle(writer, "pump", ids, 2, NULL, 0, false, 0), DE_OK);
    ASSERT_EQ(dpak_writer_finish(writer, f->upload_pack_path), DE_OK);
    dpak_writer_destroy(writer);
    ASSERT_EQ(de_asset_pack_mount(f->assets, f->upload_pack_path, 0), DE_OK);
    de_render_uploader *u = de__renderer_uploader(f->r);
    de__uploader_test_hold_fence(u, 0);
    de_load_handle load = de_asset_load_bundle(f->assets, "pump", 0);
    u64 deadline = de_time_now_ns() + WAIT_NS;
    while (de_render_uploader_stats(u).submitted == 0 && de_time_now_ns() < deadline) {
        de_renderer_pump_uploads(f->r);
        (void)de_asset_wait(f->assets, load, 1000000);
    }
    ASSERT_GT(de_render_uploader_stats(u).submitted, 0u);
    for (u32 i = 0; i < 10; ++i) {
        de_renderer_pump_uploads(f->r);
        EXPECT_EQ(de_asset_wait(f->assets, load, 1000000), DE_LOAD_LOADING);
    }
    de__uploader_test_hold_fence(u, UINT64_MAX);
    de_load_state state = DE_LOAD_LOADING;
    deadline = de_time_now_ns() + WAIT_NS;
    while (state == DE_LOAD_LOADING && de_time_now_ns() < deadline) {
        de_renderer_pump_uploads(f->r);
        state = de_asset_wait(f->assets, load, 1000000);
    }
    de_load_info info;
    (void)de_asset_status(f->assets, load, &info);
    printf("upload bundle: ready %u failed %u first %s\n", info.assets_ready, info.assets_failed,
           info.first_error_msg);
    ASSERT_EQ(state, DE_LOAD_READY);
    u64 frames = UINT64_MAX;
    (void)de__renderer_frame_fence(f->r, &frames);
    EXPECT_EQ(frames, 0u);
    EXPECT_MEMEQ(&initial_stats, de_renderer_stats(f->r), sizeof initial_stats);
    EXPECT_EQ(de_render_uploader_stats(u).pending_acquires, 0u);
    const de_render_texture *t =
        de_render_texture_get(f->assets, de_asset_texture(f->assets, de_asset_id_from("pump/t")));
    const de_render_mesh *m =
        de_render_mesh_get(f->assets, de_asset_mesh(f->assets, de_asset_id_from("pump/m")));
    ASSERT_TRUE(t != NULL && m != NULL);
    EXPECT_EQ(t->width, 64u);
    EXPECT_EQ(m->vertex_count, 300u);
    if (backend != DE_GPU_NULL) {
        lt_tex_read read = {.row_pitch = 256, .rows = 32};
        u64 offsets[DE_RENDER_MESH_BUFFER_COUNT], size;
        u8 *pixels = NULL;
        ASSERT_TRUE(lt_readback(f->d, NULL, t->texture, &read, 1, m->buffers, m->buffer_bytes,
                                offsets, DE_RENDER_MESH_BUFFER_COUNT, &pixels, &size));
        de_cooked_texture_subresource sub = lt_dtex_sub(&texture, 0);
        EXPECT_MEMEQ(pixels + read.dst_off, texture.data + sub.offset, 256u * 32u);
        de_cooked_mesh_view view;
        ASSERT_TRUE(de_cooked_mesh_parse(mesh.data, mesh.size, &view, NULL));
        for (u32 i = 0; i < DE_COOKED_STREAM_COUNT; ++i)
            if (view.streams[i] != NULL)
                EXPECT_MEMEQ(pixels + offsets[DE_RENDER_MESH_VERTICES] + m->stream_offset[i],
                             view.streams[i],
                             (size_t)view.vertex_count *
                                 de_cooked_vertex_format_size(view.stream_format[i]));
        EXPECT_MEMEQ(pixels + offsets[DE_RENDER_MESH_MESHLETS], view.meshlets,
                     (size_t)m->buffer_bytes[DE_RENDER_MESH_MESHLETS]);
        EXPECT_MEMEQ(pixels + offsets[DE_RENDER_MESH_MESHLET_VERTICES], view.meshlet_vertices,
                     (size_t)m->buffer_bytes[DE_RENDER_MESH_MESHLET_VERTICES]);
        EXPECT_MEMEQ(pixels + offsets[DE_RENDER_MESH_MESHLET_TRIANGLES], view.meshlet_triangles,
                     (size_t)m->buffer_bytes[DE_RENDER_MESH_MESHLET_TRIANGLES]);
        EXPECT_MEMEQ(pixels + offsets[DE_RENDER_MESH_INDICES], view.indices,
                     (size_t)m->buffer_bytes[DE_RENDER_MESH_INDICES]);
        free(pixels);
    }
    de_load_handle bad = de_asset_load(f->assets, de_asset_id_from("pump/bad"), 0);
    state = DE_LOAD_LOADING;
    deadline = de_time_now_ns() + WAIT_NS;
    while (state == DE_LOAD_LOADING && de_time_now_ns() < deadline) {
        de_renderer_pump_uploads(f->r);
        state = de_asset_wait(f->assets, bad, 1000000);
    }
    EXPECT_EQ(state, DE_LOAD_FAILED);
    (void)de__renderer_frame_fence(f->r, &frames);
    EXPECT_EQ(frames, 0u);
    de_asset_release(f->assets, bad);
    /* A pending CPU render is consumed before the next pump. in_flight is cleared only by
     * the main-thread wait, even if the worker already finished. No timing threshold needed. */
    ASSERT_EQ(de_world_create(
                  &(de_world_desc){.types = f->types, .tasks = f->tasks, .allocator = &f->alloc},
                  &f->world),
              DE_OK);
    de_renderer_attach(f->r, f->world);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.pipelined"), "1"), DE_OK);
    ASSERT_EQ(de__render_extract_and_kick(f->r, f->world), DE_OK);
    ASSERT_TRUE(de__rw_of(f->r)->rt.in_flight);
    de_renderer_pump_uploads(f->r);
    EXPECT_FALSE(de__rw_of(f->r)->rt.in_flight);
    (void)de__renderer_frame_fence(f->r, &frames);
    EXPECT_EQ(frames, 1u);
    ASSERT_EQ(de_renderer_render_frame(f->r), DE_OK);
    (void)de__renderer_frame_fence(f->r, &frames);
    EXPECT_EQ(frames, 2u);
    de_asset_release(f->assets, load);
    lt_free(&texture);
    lt_free(&mesh);
}
TEST_F(renderer, pump_uploads_without_frames_null, rfx) {
    pump_uploads_without_frames(f, DE_GPU_NULL);
}
TEST_F(renderer, pump_uploads_without_frames_metal, rfx) {
    pump_uploads_without_frames(f, DE_GPU_METAL);
}

#endif

TEST_F(renderer, create_args, rfx) {
    start(f, DE_GPU_NULL, false);
    de_renderer_pump_uploads(f->r);
    de_renderer *r = NULL;
    EXPECT_EQ(de_renderer_create(NULL, &r), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->d}, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_renderer_create(&(de_renderer_desc){0}, &r), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(r == NULL);
    /* frames_in_flight: 0 = 2, clamped to 2..3. */
    const u32 asked[] = {1, 3, 7};
    const u32 got[] = {2, 3, 3};
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->d,
                                                         .allocator = &f->alloc,
                                                         .frames_in_flight = asked[i]},
                                     &r),
                  DE_OK);
        EXPECT_EQ(r->frames_in_flight, got[i]);
        EXPECT_TRUE(de__renderer_uploader(r) == NULL); /* no asset system */
        for (u32 k = 0; k < 5; ++k)
            EXPECT_EQ(de_renderer_render_frame(r), DE_OK);
        de_renderer_destroy(r);
        r = NULL;
    }
    /* NULL is harmless everywhere. */
    de_renderer_destroy(NULL);
    de_renderer_attach(NULL, NULL);
    de_renderer_set_settings(NULL, NULL);
    EXPECT_EQ(de_renderer_render_frame(NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(de_renderer_stats(NULL) == NULL);
    EXPECT_TRUE(de_renderer_stats(f->r) != NULL);
    EXPECT_EQ(de_renderer_stats(f->r)->pass_count, 0u);
    /* DENG-176: the pass list is the render graphs' published timings, one frame late on the
     * null device. No view has a camera, so the frame is the output graph's clear. */
    ASSERT_EQ(de_renderer_render_frame(f->r), DE_OK);
    ASSERT_EQ(de_renderer_render_frame(f->r), DE_OK);
    EXPECT_EQ(de_renderer_stats(f->r)->pass_count, 1u);
    ASSERT_TRUE(de_renderer_stats(f->r)->pass_names != NULL);
    EXPECT_STREQ(de_renderer_stats(f->r)->pass_names[0], "output");
}

TEST_F(renderer, below_base_refused, rfx) {
    start(f, DE_GPU_NULL, false);
    /* Make the null device look like one that fails the backend's Base check. */
    de_gpu_caps *caps = (de_gpu_caps *)de_gpu_get_caps(f->d);
    const de_gpu_caps saved = *caps;
    caps->base_tier_ok = false;
    snprintf(caps->base_tier_reason, sizeof caps->base_tier_reason, "%s",
             "paravirtual device without argument buffers tier 2");
    caps->max_push_constant_bytes = 64;
    de_renderer *r = NULL;
    const de_result res =
        de_renderer_create(&(de_renderer_desc){.device = f->d, .allocator = &f->alloc}, &r);
    *caps = saved;
    EXPECT_EQ(res, DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(r == NULL);
    const de_error *e = de_error_last();
    ASSERT_TRUE(e != NULL);
    printf("refused: %s\n", e->message);
    EXPECT_TRUE(strstr(e->message, "below the Base tier") != NULL);
    EXPECT_TRUE(strstr(e->message, "paravirtual device without argument buffers tier 2") != NULL);
    EXPECT_TRUE(strstr(e->message, "push constants are 64 bytes") != NULL);
}

/* ---- Settings through the renderer ----------------------------------------------------------- */

static const char *cvar_text(const char *name) {
    const de_cvar cv = de_cvar_find(name);
    const char *s = DE_HANDLE_VALID(cv) ? de_cvar_string(cv) : NULL;
    return s != NULL ? s : "?";
}

static void set_cvar(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    ASSERT_TRUE(DE_HANDLE_VALID(cv));
    ASSERT_EQ(de_cvar_set(cv, value), DE_OK);
}

static bool same(de_render_settings a, de_render_settings b) {
    return de__render_settings_equal(&a, &b);
}

TEST_F(renderer, set_settings_clamps, rfx) {
    start(f, DE_GPU_NULL, false); /* null: no ray tracing */
    de_render_settings s = de_render_settings_preset(DE_TIER_RT, DE_QUALITY_HIGH);
    s.render_scale = 2.0f;
    de_renderer_set_settings(f->r, &s);
    const de_render_settings g = de_renderer_get_settings(f->r);
    EXPECT_EQ(g.tier, DE_TIER_BASE);
    EXPECT_FALSE(any_rt(&g));
    EXPECT_FLOAT_EQ(g.render_scale, 1.0f, 0.0);
    EXPECT_EQ(g.shadows, DE_QUALITY_HIGH);
    /* The cvars show what is in effect: the explicit tier, and RT High clamped to Base at scale
     * 1.0 is the Base High preset. */
    EXPECT_STREQ(cvar_text("r.tier"), "base");
    EXPECT_STREQ(cvar_text("r.quality"), "high");
    EXPECT_STREQ(cvar_text("r.shadow_quality"), "high");
    EXPECT_EQ(de_cvar_bool(de_cvar_find("r.rt_shadows")), false);
    /* Anything else is Custom. */
    s = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH);
    s.ao = DE_QUALITY_ULTRA;
    de_renderer_set_settings(f->r, &s);
    EXPECT_STREQ(cvar_text("r.quality"), "custom");
    EXPECT_STREQ(cvar_text("r.ao_quality"), "ultra");
    /* A preset is named. */
    const de_render_settings low = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW);
    de_renderer_set_settings(f->r, &low);
    EXPECT_STREQ(cvar_text("r.quality"), "low");
    EXPECT_STREQ(cvar_text("r.volumetric_quality"), "off");
    EXPECT_FLOAT_EQ((f32)de_cvar_float(de_cvar_find("r.render_scale")), 0.67f, 0.0);
    ASSERT_EQ(de_renderer_render_frame(f->r), DE_OK);
    EXPECT_TRUE(same(de_renderer_get_settings(f->r), low)); /* the frame did not undo it */
    EXPECT_TRUE(same(f->r->frame, low));
}

TEST_F(renderer, cvars_apply, rfx) {
    start(f, DE_GPU_NULL, false);
    de_renderer *r = f->r;
    ASSERT_TRUE(r->cvars.owner);
    const de_render_settings start_s = de_renderer_get_settings(r);
    EXPECT_STREQ(cvar_text("r.quality"), "auto");
    EXPECT_STREQ(cvar_text("r.shadow_quality"),
                 start_s.shadows == DE_QUALITY_HIGH ? "high" : "medium"); /* written back */

    /* A preset by cvar: applied at the next frame, not before. */
    set_cvar("r.quality", "low");
    EXPECT_TRUE(same(de_renderer_get_settings(r), start_s));
    ASSERT_EQ(de_renderer_render_frame(r), DE_OK);
    EXPECT_TRUE(
        same(de_renderer_get_settings(r), de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW)));
    EXPECT_TRUE(same(r->frame, de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW)));
    EXPECT_STREQ(cvar_text("r.shadow_quality"), "low");

    /* One effect changed under a preset: Custom, and r.quality says so. */
    set_cvar("r.shadow_quality", "ultra");
    ASSERT_EQ(de_renderer_render_frame(r), DE_OK);
    de_render_settings g = de_renderer_get_settings(r);
    EXPECT_EQ(g.shadows, DE_QUALITY_ULTRA);
    EXPECT_EQ(g.ao, DE_QUALITY_LOW);
    EXPECT_FALSE(de_render_settings_is_preset(&g));
    EXPECT_STREQ(cvar_text("r.quality"), "custom");

    /* Back to the preset's value: the preset is named again. */
    set_cvar("r.shadow_quality", "low");
    ASSERT_EQ(de_renderer_render_frame(r), DE_OK);
    EXPECT_STREQ(cvar_text("r.quality"), "low");

    /* Custom values stay custom; render scale by cvar. */
    set_cvar("r.render_scale", "0.5");
    ASSERT_EQ(de_renderer_render_frame(r), DE_OK);
    g = de_renderer_get_settings(r);
    EXPECT_FLOAT_EQ(g.render_scale, 0.5f, 0.0);
    EXPECT_STREQ(cvar_text("r.quality"), "custom");
    set_cvar("r.path_max_bounces", "12");
    ASSERT_EQ(de_renderer_render_frame(r), DE_OK);
    EXPECT_EQ(de_renderer_get_settings(r).path_max_bounces, 12u);

    /* RT effects and the RT tier on a device without ray tracing: clamped, and written back. */
    set_cvar("r.rt_shadows", "1");
    set_cvar("r.tier", "rt");
    ASSERT_EQ(de_renderer_render_frame(r), DE_OK);
    g = de_renderer_get_settings(r);
    EXPECT_EQ(g.tier, DE_TIER_BASE);
    EXPECT_FALSE(g.rt_shadows);
    EXPECT_EQ(de_cvar_bool(de_cvar_find("r.rt_shadows")), false);
    EXPECT_STREQ(cvar_text("r.tier"), "rt"); /* the selection stays */

    /* A preset selection replaces custom values. */
    set_cvar("r.quality", "high");
    ASSERT_EQ(de_renderer_render_frame(r), DE_OK);
    EXPECT_TRUE(same(de_renderer_get_settings(r),
                     de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH)));
    EXPECT_STREQ(cvar_text("r.shadow_quality"), "high");
    EXPECT_FLOAT_EQ(de_cvar_float(de_cvar_find("r.render_scale")), 1.0, 0.0);

    /* Only the first renderer owns the cvars. */
    de_renderer *other = NULL;
    ASSERT_EQ(
        de_renderer_create(&(de_renderer_desc){.device = f->d, .allocator = &f->alloc}, &other),
        DE_OK);
    EXPECT_FALSE(other->cvars.owner);
    const de_render_settings other_s = de_renderer_get_settings(other);
    set_cvar("r.quality", "ultra");
    ASSERT_EQ(de_renderer_render_frame(other), DE_OK);
    EXPECT_TRUE(same(de_renderer_get_settings(other), other_s));
    ASSERT_EQ(de_renderer_render_frame(r), DE_OK);
    EXPECT_TRUE(same(de_renderer_get_settings(r),
                     de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_ULTRA)));
    de_renderer_destroy(other);

    /* Created with the cvars set: the renderer starts from them. */
    de_renderer_destroy(f->r);
    f->r = NULL;
    set_cvar("r.tier", "base");
    set_cvar("r.quality", "custom");
    set_cvar("r.gi_quality", "off");
    ASSERT_EQ(
        de_renderer_create(&(de_renderer_desc){.device = f->d, .allocator = &f->alloc}, &f->r),
        DE_OK);
    EXPECT_TRUE(f->r->cvars.owner);
    g = de_renderer_get_settings(f->r);
    EXPECT_EQ(g.gi, DE_QUALITY_OFF);
    EXPECT_EQ(g.shadows, DE_QUALITY_ULTRA); /* from the "ultra" preset written back above */
    EXPECT_FALSE(de_render_settings_is_preset(&g));
}

/* ---- Frame loop ------------------------------------------------------------------------------ */

#define WARMUP_FRAMES   10u
#define MEASURED_FRAMES 100u

static void run_frame_loop(rfx *f, de_gpu_backend b) {
    start(f, b, true);
    de_renderer *r = f->r;
    r->clear_color[0] = 0.25f;
    r->clear_color[1] = 0.5f;
    r->clear_color[2] = 0.75f;
    r->clear_color[3] = 1.0f;
    /* Warm-up: pools, the graph's size hints, the first log line of each thread. */
    for (u32 i = 0; i < WARMUP_FRAMES; ++i) {
        ASSERT_EQ(de_renderer_render_frame(r), DE_OK);
        de_asset_system_update(f->assets, NULL);
    }
    de_alloc_guard guard = {0};
    /* The Metal driver and the Objective-C runtime call malloc inside Apple's frameworks for each
     * command buffer and encoder; the engine itself must not allocate. */
    if (b == DE_GPU_METAL)
        de_alloc_guard_allow(&guard, DE_ALLOC_GUARD_TAG_RAW);
    const de_tracking_stats before = de_tracking_allocator_stats(&f->track);
    const de_tracking_stats dev_before = de_tracking_allocator_stats(&f->dev_track);
    de_alloc_guard_begin(&guard);
    de_result res = DE_OK;
    for (u32 i = 0; i < MEASURED_FRAMES && res == DE_OK; ++i) {
        res = de_renderer_render_frame(r);
        de_asset_system_update(f->assets, NULL);
    }
    de_alloc_guard_end(&guard);
    ASSERT_EQ(res, DE_OK);
    printf("%u frames: %llu engine frees, %llu allowed raw allocations, raw counted: %s\n",
           MEASURED_FRAMES, (unsigned long long)guard.engine_frees,
           (unsigned long long)guard.allowed_allocs, de_alloc_guard_counts_raw() ? "yes" : "no");
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    const de_tracking_stats after = de_tracking_allocator_stats(&f->track);
    const de_tracking_stats dev_after = de_tracking_allocator_stats(&f->dev_track);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.realloc_count, before.realloc_count);
    EXPECT_EQ(dev_after.alloc_count, dev_before.alloc_count);
    EXPECT_EQ(dev_after.realloc_count, dev_before.realloc_count);
    u64 frames = 0;
    (void)de__renderer_frame_fence(r, &frames);
    EXPECT_EQ(frames, (u64)(WARMUP_FRAMES + MEASURED_FRAMES));

    /* The target holds the clear colour (read back on real devices). */
    if (b == DE_GPU_NULL)
        return;
    const de_gpu_texture target = de__renderer_offscreen(r);
    const u32 pitch = DE__RENDER_OFFSCREEN_WIDTH * 4u; /* a multiple of 256 */
    de_gpu_buffer rb = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){.size = (u64)pitch * DE__RENDER_OFFSCREEN_HEIGHT,
                                    .usage = DE_USAGE_COPY_DST,
                                    .memory = DE_MEM_READBACK,
                                    .name = "renderer readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(rb));
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "readback");
    ASSERT_TRUE(c != NULL);
    de_gpu_cmd_copy_texture_to_buffer(c, rb, 0, pitch, target, 0, 0); /* left in COPY_SRC */
    const de_gpu_fence fence = de_gpu_fence_create(f->d, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(fence));
    const de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = fence, .signal_value = 1};
    ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS, &s), DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(f->d, fence, 1, WAIT_NS), DE_OK);
    const u8 *px = (const u8 *)de_gpu_buffer_map(f->d, rb);
    ASSERT_TRUE(px != NULL);
    const u8 want[4] = {64, 128, 191, 255};
    u32 bad = 0;
    const u32 xs[] = {0, DE__RENDER_OFFSCREEN_WIDTH / 2, DE__RENDER_OFFSCREEN_WIDTH - 1};
    const u32 ys[] = {0, DE__RENDER_OFFSCREEN_HEIGHT / 2, DE__RENDER_OFFSCREEN_HEIGHT - 1};
    for (u32 yi = 0; yi < 3; ++yi)
        for (u32 xi = 0; xi < 3; ++xi)
            for (u32 k = 0; k < 4; ++k) {
                const int got = px[(size_t)ys[yi] * pitch + (size_t)xs[xi] * 4u + k];
                if (abs(got - (int)want[k]) > 1)
                    ++bad;
            }
    EXPECT_EQ(bad, 0u);
    de_gpu_buffer_destroy(f->d, rb);
    de_gpu_fence_destroy(f->d, fence);
}

TEST_F(renderer, frame_loop_no_heap_null, rfx) {
    run_frame_loop(f, DE_GPU_NULL);
}

TEST_F(renderer, frame_loop_no_heap_metal, rfx) {
    run_frame_loop(f, DE_GPU_METAL);
}
