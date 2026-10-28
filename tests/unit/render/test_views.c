/*
 * test_views.c: views and the per-view frame pipeline (DENG-176; SPEC §12.4, §12.5, §12.9,
 * §20.1). Executable test_views, CTest names views.<test>.
 *
 * Null backend (every host): the main view and camera binding, split screen (two graph
 * instances), render to texture consumed in the same frame (order), independent render scales,
 * the compiled-graph cache in a steady frame (and no heap allocation), deferred release of a
 * destroyed view, hooks and registered stages in slot order then registration order, per-view
 * settings. Metal (macOS; Metal API validation on; SKIPs without a device or below the Base
 * tier): split screen with a half-resolution view and a render-to-texture inset, read back; views
 * created and destroyed while frames are in flight.
 *
 * Each test drives frames like the host loop (PostUpdate, Extract, de__render_extract_and_kick),
 * unpipelined, so the render side can be read between frames.
 */
#include "de_test.h"

#include "de_alloc_guard.h"

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>

#include "gpu/gpu_backend.h"
#include "render/components_internal.h"
#include "render/graph/rg_internal.h"
#include "render/renderer_internal.h"
#include "render/views_internal.h"
#include "render/world/render_world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS ((u64)30 * 1000000000u)

/* ---- Fixture -------------------------------------------------------------------------------- */

typedef struct vfx {
    de_tracking_allocator track, dev_track;
    de_allocator alloc, dev_alloc;
    de_type_registry *types;
    de_world *w;
    de_gpu_device *d;
    de_renderer *r;
    DeRenderComponents ids;
    de_builtin_component_ids bi;
    char why[600];
} vfx;

static void set_cvar(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (DE_HANDLE_VALID(cv))
        (void)de_cvar_set(cv, value);
}

/* World + device + renderer (offscreen, 2 frames in flight) attached, unpipelined, Base High
 * settings (render_scale 1). SKIPs a Metal run without a usable device. */
static void start(vfx *f, de_gpu_backend b) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    f->dev_alloc = de_tracking_allocator_init(&f->dev_track, NULL, DE_MEM_RENDER);
    de_fp_env_init_thread();
#if defined(__APPLE__)
    if (b == DE_GPU_METAL)
        setenv("MTL_DEBUG_LAYER", "1", 0); /* Metal API validation, before the device */
#endif
    const de_result dr = de_gpu_device_create(&(de_gpu_device_desc){.backend = b,
                                                                    .debug_layer = b != DE_GPU_NULL,
                                                                    .allocator = &f->dev_alloc,
                                                                    .pipeline_cache_dir = ""},
                                              &f->d);
    if (dr != DE_OK) {
        snprintf(f->why, sizeof f->why, "no %s device", de_gpu_backend_name(b));
        SKIP(f->why);
    }
    const de_gpu_caps *caps = de_gpu_get_caps(f->d);
    if (!caps->base_tier_ok) {
        snprintf(f->why, sizeof f->why, "below the Base tier: %.400s", caps->base_tier_reason);
        SKIP(f->why);
    }
    ASSERT_EQ(de_type_registry_create(&f->alloc, &f->types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &f->alloc, .types = f->types}, &f->w),
              DE_OK);
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->d,
                                                     .types = f->types,
                                                     .allocator = &f->alloc,
                                                     .frames_in_flight = 2},
                                 &f->r),
              DE_OK);
    de_renderer_attach(f->r, f->w);
    const DeRenderComponents *ids = de__render_components(f->w);
    ASSERT_TRUE(ids != NULL);
    f->ids = *ids;
    f->bi = de_builtin_components(f->w);
    set_cvar("r.pipelined", "0");
    const de_render_settings s = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_HIGH);
    de_renderer_set_settings(f->r, &s);
}

static void stop(vfx *f) {
    if (f->r != NULL)
        de_renderer_destroy(f->r);
    if (f->w != NULL)
        de_world_destroy(f->w);
    if (f->d != NULL)
        de_gpu_device_destroy(f->d);
    if (f->types != NULL)
        de_type_registry_destroy(f->types);
    f->r = NULL;
    f->w = NULL;
    f->d = NULL;
    f->types = NULL;
    set_cvar("r.pipelined", "1");
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->dev_track).bytes_live, 0u);
}

static void vfx_setup(vfx *f) {
    (void)f;
}

static void vfx_teardown(vfx *f) {
    if (f->r != NULL || f->w != NULL || f->d != NULL)
        stop(f);
}

static de_result frame(vfx *f) {
    de_result r = de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 1.0f / 60.0f, 0.0f);
    if (r == DE_OK)
        r = de_world_run_schedule(f->w, DE_SCHEDULE_EXTRACT, 1.0f / 60.0f, 0.0f);
    if (r == DE_OK)
        r = de__render_extract_and_kick(f->r, f->w);
    return r;
}

static de_entity camera(vfx *f, de_view v, f64 x) {
    const de_component comps[2] = {f->bi.transform, f->ids.camera};
    de_entity e = DE_ENTITY_NULL;
    if (de_entity_spawn_with(f->w, comps, 2, &e) != DE_OK)
        return DE_ENTITY_NULL;
    de_transform t = de_transform_identity();
    t.position = (de_dvec3){x, 0.0, 2.0};
    *(DeTransform *)de_entity_get(f->w, e, f->bi.transform) = t;
    ((DeCamera *)de_entity_get(f->w, e, f->ids.camera))->view = v;
    return e;
}

static de_view view(vfx *f, const char *name, f32 x, f32 y, f32 w, f32 h, i32 order,
                    de_gpu_texture target) {
    return de_view_create(
        f->r,
        &(de_view_desc){.name = name, .viewport = {x, y, w, h}, .order = order, .target = target});
}

static de__view_info info(vfx *f, de_view v) {
    de__view_info i;
    memset(&i, 0, sizeof i);
    EXPECT_TRUE(de__view_info_get(f->r, v, &i));
    return i;
}

/* ---- Main view, cameras, settings ----------------------------------------------------------- */

TEST_F(views, default_main_view, vfx) {
    start(f, DE_GPU_NULL);
    const de_view mv = de_renderer_main_view(f->r);
    ASSERT_TRUE(DE_HANDLE_VALID(mv));
    /* No camera yet: the main view is skipped; the output graph still clears. */
    ASSERT_EQ(frame(f), DE_OK);
    EXPECT_FALSE(info(f, mv).active);
    u32 n = de__views_last_order(f->r, NULL, 0);
    EXPECT_EQ(n, 0u);
    /* A camera with view zero renders through the main view, at the whole offscreen target. */
    (void)camera(f, (de_view){0}, 0.0);
    ASSERT_EQ(frame(f), DE_OK);
    de__view_info i = info(f, mv);
    EXPECT_TRUE(i.active);
    EXPECT_EQ(i.width, DE__RENDER_OFFSCREEN_WIDTH);
    EXPECT_EQ(i.height, DE__RENDER_OFFSCREEN_HEIGHT);
    EXPECT_EQ(i.viewport_px[2], DE__RENDER_OFFSCREEN_WIDTH);
    EXPECT_EQ(i.executed, 1u);
    /* A second view without a camera is skipped; a camera naming a stale view is ignored. */
    const de_view other = view(f, "other", 0, 0, 0, 0, 1, (de_gpu_texture){0});
    ASSERT_TRUE(DE_HANDLE_VALID(other));
    (void)camera(f, (de_view){.id = 0x7777u}, 1.0);
    ASSERT_EQ(frame(f), DE_OK);
    EXPECT_FALSE(info(f, other).active);
    de_view order[4];
    n = de__views_last_order(f->r, order, 4);
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(order[0].id, mv.id);
    /* The main view stays. */
    de_view_destroy(f->r, mv);
    ASSERT_EQ(frame(f), DE_OK);
    EXPECT_TRUE(info(f, mv).active);
    EXPECT_EQ(info(f, mv).executed, 3u);
    /* Bad descs. */
    EXPECT_FALSE(DE_HANDLE_VALID(de_view_create(f->r, NULL)));
    EXPECT_FALSE(DE_HANDLE_VALID(view(f, "bad", 0.5f, 0, 0.6f, 1, 0, (de_gpu_texture){0})));
    EXPECT_FALSE(DE_HANDLE_VALID(view(f, "bad", 0, 0, -1, 1, 0, (de_gpu_texture){0})));
    const de_gpu_texture sampled_only = de_gpu_texture_create(
        f->d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                     .width = 8,
                                     .height = 8,
                                     .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST});
    EXPECT_FALSE(DE_HANDLE_VALID(view(f, "bad", 0, 0, 0, 0, 0, sampled_only)));
    de_gpu_texture_destroy(f->d, sampled_only);
    EXPECT_FALSE(DE_HANDLE_VALID(de_view_create(NULL, &(de_view_desc){0})));
    /* The table holds DE__VIEW_MAX views. */
    de_view many[DE__VIEW_MAX];
    u32 made = 0;
    for (u32 k = 0; k < DE__VIEW_MAX; ++k) {
        many[k] = view(f, NULL, 0, 0, 0, 0, 0, (de_gpu_texture){0});
        if (DE_HANDLE_VALID(many[k]))
            ++made;
    }
    EXPECT_EQ(made, DE__VIEW_MAX - 2u); /* main and "other" hold two */
    for (u32 k = 0; k < DE__VIEW_MAX; ++k)
        de_view_destroy(f->r, many[k]);
    ASSERT_EQ(frame(f), DE_OK);
    stop(f);
}

TEST_F(views, frame_limit_pause_and_resume, vfx) {
    start(f, DE_GPU_NULL);
    const de_view main = de_renderer_main_view(f->r);
    ASSERT_TRUE(de_entity_alive(f->w, camera(f, main, 1)));
    const de_view inset = view(f, "limited", 0, 0, 0.5f, 0.5f, 1, (de_gpu_texture){0});
    ASSERT_TRUE(DE_HANDLE_VALID(inset));
    ASSERT_TRUE(de_entity_alive(f->w, camera(f, inset, 0)));
    de_view_set_max_fps(f->r, inset, 1);
    ASSERT_EQ(frame(f), DE_OK);
    const u64 first = info(f, inset).executed;
    ASSERT_EQ(first, 1u);
    for (u32 i = 0; i < 3; ++i)
        ASSERT_EQ(frame(f), DE_OK);
    EXPECT_EQ(info(f, inset).executed, first);
    EXPECT_TRUE(info(f, main).executed > first);
    de_view_set_max_fps(f->r, inset, 0);
    ASSERT_EQ(frame(f), DE_OK);
    EXPECT_EQ(info(f, inset).executed, first + 1);
    de_view_set_paused(f->r, inset, true);
    ASSERT_EQ(frame(f), DE_OK);
    EXPECT_EQ(info(f, inset).executed, first + 1);
    de_view_set_paused(f->r, inset, false);
    ASSERT_EQ(frame(f), DE_OK);
    EXPECT_EQ(info(f, inset).executed, first + 2);
    /* A changed cap must take effect immediately, even while the old deadline is far away. */
    de_view_set_max_fps(f->r, inset, 1);
    ASSERT_EQ(frame(f), DE_OK);
    EXPECT_EQ(info(f, inset).executed, first + 3);
    de_view_set_max_fps(f->r, inset, 120);
    ASSERT_EQ(frame(f), DE_OK);
    EXPECT_EQ(info(f, inset).executed, first + 4);
    de_view_destroy(f->r, inset);
    de_view_set_paused(f->r, inset, false);
    de_view_set_max_fps(f->r, inset, 30);
    ASSERT_EQ(frame(f), DE_OK);
    stop(f);
}

typedef struct display_observer {
    de_renderer *renderer;
    de_view view;
    de_view_display last;
    u32 hooks, post;
} display_observer;
static void observe_display(de_rg *g, de_view v, void *user) {
    (void)g;
    display_observer *o = user;
    if (v.id != o->view.id)
        return;
    de_view_frame_info info;
    if (de_view_frame_info_get(o->renderer, v, &info)) {
        o->last = info.display;
        ++o->hooks;
    }
}
static void observe_post(de__view_frame *vf, void *user) {
    display_observer *o = user;
    if (vf->view.id == o->view.id)
        ++o->post;
}

TEST_F(views, display_policy_is_frozen_and_independent, vfx) {
    start(f, DE_GPU_NULL);
    const de_view main = de_renderer_main_view(f->r);
    const de_view inset = view(f, "display", 0, 0, 0.5f, 0.5f, 1, (de_gpu_texture){0});
    ASSERT_TRUE(de_entity_alive(f->w, camera(f, main, 1)));
    ASSERT_TRUE(de_entity_alive(f->w, camera(f, inset, 0)));
    display_observer o = {.renderer = f->r, .view = inset};
    de_render_add_hook(f->r, DE_HOOK_UI, observe_display, &o);
    ASSERT_EQ(de__view_register_stage(f->r, DE__VIEW_SLOT_POST, 100, "test.post", observe_post, &o),
              DE_OK);
    de_view_display policy = {.mode = DE_VIEW_RENDER_DEPTH,
                              .disabled_features = DE_VIEW_HIDE_UI | DE_VIEW_DISABLE_POST_EFFECTS};
    ASSERT_EQ(de_view_set_display(f->r, inset, &policy), DE_OK);
    policy.mode = DE_VIEW_RENDER_NORMALS; /* The setter copied it. */
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 1.0f / 60, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_EXTRACT, 1.0f / 60, 0), DE_OK);
    de_view_frame_info frozen, other;
    ASSERT_TRUE(de_renderer_extract_view_info(f->r, inset, &frozen));
    ASSERT_TRUE(de_renderer_extract_view_info(f->r, main, &other));
    EXPECT_EQ(frozen.display.mode, DE_VIEW_RENDER_DEPTH);
    EXPECT_EQ(other.display.mode, DE_VIEW_RENDER_LIT);
    EXPECT_EQ(other.display.disabled_features, 0u);
    ASSERT_EQ(de_view_set_display(f->r, inset, &policy), DE_OK);
    ASSERT_EQ(de__render_extract_and_kick(f->r, f->w), DE_OK);
    EXPECT_EQ(o.last.mode, DE_VIEW_RENDER_DEPTH); /* This Extract frame owns the old policy. */
    EXPECT_EQ(o.hooks, 1u); /* Hiding UI must not suppress the editor's output/readback hook. */
    EXPECT_EQ(o.post, 0u);
    ASSERT_EQ(frame(f), DE_OK);
    EXPECT_EQ(o.last.mode, DE_VIEW_RENDER_NORMALS);
    const de_view_display invalid = {.mode = DE_VIEW_RENDER_MODE_COUNT};
    EXPECT_EQ(de_view_set_display(f->r, inset, &invalid), DE_ERR_INVALID_ARGUMENT);
    const de_view_display bad_flags = {.disabled_features = 0x80000000u};
    EXPECT_EQ(de_view_set_display(f->r, inset, &bad_flags), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    ASSERT_EQ(de_view_set_display(f->r, inset, NULL), DE_OK);
    ASSERT_EQ(frame(f), DE_OK);
    EXPECT_EQ(o.last.mode, DE_VIEW_RENDER_LIT);
    EXPECT_EQ(o.last.disabled_features, 0u);
    EXPECT_EQ(o.post, 1u);
    de_view_destroy(f->r, inset);
    EXPECT_EQ(de_view_set_display(f->r, inset, NULL), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    de_render_remove_hook(f->r, DE_HOOK_UI, observe_display, &o);
    de__view_unregister_stage(f->r, observe_post, &o);
}

TEST_F(views, settings_per_view, vfx) {
    start(f, DE_GPU_NULL);
    const de_view mv = de_renderer_main_view(f->r);
    const de_view v = view(f, "pip", 0.75f, 0, 0.25f, 0.25f, 1, (de_gpu_texture){0});
    ASSERT_TRUE(DE_HANDLE_VALID(v));
    /* By default a view follows the renderer. */
    de_render_settings rs = de_renderer_get_settings(f->r);
    de_render_settings vs = de_view_get_settings(f->r, v);
    EXPECT_TRUE(de__render_settings_equal(&rs, &vs));
    /* Own settings, clamped; the renderer's tier caps it (the null device is Base anyway). */
    de_render_settings own = de_render_settings_preset(DE_TIER_RT, DE_QUALITY_LOW);
    own.render_scale = 5.0f;
    de_view_set_settings(f->r, v, &own);
    vs = de_view_get_settings(f->r, v);
    EXPECT_EQ(vs.tier, DE_TIER_BASE);
    EXPECT_FALSE(vs.rt_shadows);
    EXPECT_EQ(vs.shadows, DE_QUALITY_LOW);
    EXPECT_FLOAT_EQ(vs.render_scale, 1.0f, 1e-6);
    /* The render scale overrides the settings' one; 0 and NaN restore it. */
    de_view_set_render_scale(f->r, v, 0.5f);
    EXPECT_FLOAT_EQ(de_view_get_settings(f->r, v).render_scale, 0.5f, 1e-6);
    de_view_set_render_scale(f->r, v, 0.1f);
    EXPECT_FLOAT_EQ(de_view_get_settings(f->r, v).render_scale, 0.33f, 1e-6);
    de_view_set_render_scale(f->r, v, 0.0f);
    EXPECT_FLOAT_EQ(de_view_get_settings(f->r, v).render_scale, 1.0f, 1e-6);
    de_view_set_settings(f->r, v, NULL);
    vs = de_view_get_settings(f->r, v);
    rs = de_renderer_get_settings(f->r);
    EXPECT_TRUE(de__render_settings_equal(&rs, &vs));
    /* What the render side used. */
    de_view_set_render_scale(f->r, v, 0.5f);
    (void)camera(f, (de_view){0}, 0.0);
    (void)camera(f, v, 1.0);
    ASSERT_EQ(frame(f), DE_OK);
    const de__view_info i = info(f, v);
    EXPECT_FLOAT_EQ(i.settings.render_scale, 0.5f, 1e-6);
    EXPECT_EQ(i.viewport_px[0], 960u);
    EXPECT_EQ(i.viewport_px[2], 320u);
    EXPECT_EQ(i.viewport_px[3], 180u);
    EXPECT_EQ(i.width, 160u);
    EXPECT_EQ(i.height, 90u);
    EXPECT_FLOAT_EQ(info(f, mv).settings.render_scale, 1.0f, 1e-6);
    /* Stale handles are ignored. */
    de_view_destroy(f->r, v);
    de_view_set_render_scale(f->r, v, 0.5f);
    de_view_set_settings(f->r, v, &own);
    vs = de_view_get_settings(f->r, v); /* stale: the renderer's */
    EXPECT_TRUE(de__render_settings_equal(&rs, &vs));
    ASSERT_EQ(frame(f), DE_OK);
    de__view_info dead;
    EXPECT_FALSE(de__view_info_get(f->r, v, &dead));
    stop(f);
}

TEST(views, hidden_policy_large_copied_and_generation_safe) {
    de_tracking_allocator track;
    de_allocator alloc = de_tracking_allocator_init(&track, NULL, DE_MEM_RENDER);
    const u32 count = 1000000;
    u64 *input = DE_ALLOC(&alloc, (u64)count * sizeof(u64), 8, DE_MEM_RENDER);
    ASSERT_TRUE(input != NULL);
    for (u32 i = 0; i < count; ++i)
        input[i] = (UINT64_C(7) << 32) | (count - i);
    input[0] = 0;
    input[1] = input[2];
    const de__view_hidden *policy = NULL;
    ASSERT_EQ(de__view_hidden_create(&alloc, input, count, &policy), DE_OK);
    EXPECT_EQ(de__view_hidden_count(policy), count - 2);
    memset(input, 0, (size_t)count * sizeof(u64));
    EXPECT_TRUE(de__view_hidden_contains(policy, (UINT64_C(7) << 32) | 12345));
    EXPECT_FALSE(de__view_hidden_contains(policy, (UINT64_C(8) << 32) | 12345));
    EXPECT_FALSE(de__view_hidden_contains(policy, 0));
    const de__view_hidden *copy = de__view_hidden_retain(policy);
    de__view_hidden_release(policy);
    EXPECT_TRUE(de__view_hidden_contains(copy, (UINT64_C(7) << 32) | 1));
    de__view_hidden_release(copy);
    DE_FREE(&alloc, input, (u64)count * sizeof(u64));
    EXPECT_EQ(de_tracking_allocator_stats(&track).blocks_live, 0u);
}

typedef struct hidden_observer {
    de_view view;
    u32 count, other_count;
    bool found;
} hidden_observer;
static void observe_hidden(de__view_frame *vf, void *user) {
    hidden_observer *o = user;
    if (vf->view.id == o->view.id) {
        o->count = de__view_hidden_count(vf->hidden);
        o->found = de__view_hidden_contains(vf->hidden, 55);
    } else {
        o->other_count = de__view_hidden_count(vf->hidden);
    }
}
TEST_F(views, hidden_policy_frozen_independent_and_zero_alloc, vfx) {
    start(f, DE_GPU_NULL);
    const de_view main = de_renderer_main_view(f->r);
    const de_view inset = view(f, "filtered", 0, 0, 0.5f, 0.5f, 1, (de_gpu_texture){0});
    ASSERT_TRUE(de_entity_alive(f->w, camera(f, main, 1)));
    ASSERT_TRUE(de_entity_alive(f->w, camera(f, inset, 0)));
    hidden_observer observer = {.view = inset};
    ASSERT_EQ(de__view_register_stage(f->r, DE__VIEW_SLOT_VISIBILITY, 10, "test.hidden",
                                      observe_hidden, &observer),
              DE_OK);
    const u64 ids[] = {55, 0, 55, UINT64_C(1) << 32};
    ASSERT_EQ(de_view_set_hidden(f->r, inset, ids, 4), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 1.0f / 60, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_EXTRACT, 1.0f / 60, 0), DE_OK);
    ASSERT_EQ(de_view_set_hidden(f->r, inset, NULL, 0), DE_OK);
    ASSERT_EQ(de__render_extract_and_kick(f->r, f->w), DE_OK);
    EXPECT_EQ(observer.count, 2u);
    EXPECT_TRUE(observer.found);
    EXPECT_EQ(observer.other_count, 0u);
    ASSERT_EQ(frame(f), DE_OK);
    EXPECT_EQ(observer.count, 0u);
    EXPECT_EQ(de_view_set_hidden(f->r, inset, NULL, 1), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    ASSERT_EQ(de_view_set_hidden(f->r, inset, ids, 4), DE_OK);
    for (u32 i = 0; i < 8; ++i)
        ASSERT_EQ(frame(f), DE_OK);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    de_result result = DE_OK;
    for (u32 i = 0; i < 50 && result == DE_OK; ++i)
        result = frame(f);
    de_alloc_guard_end(&guard);
    ASSERT_EQ(result, DE_OK);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    de_view_destroy(f->r, inset);
    EXPECT_EQ(de_view_set_hidden(f->r, inset, NULL, 0), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    de__view_unregister_stage(f->r, observe_hidden, &observer);
}

/* ---- Split screen, render scale, cache ------------------------------------------------------ */

TEST_F(views, split_screen_two_graphs, vfx) {
    start(f, DE_GPU_NULL);
    const de_view l = view(f, "left", 0, 0, 0.5f, 1, 0, (de_gpu_texture){0});
    const de_view r = view(f, "right", 0.5f, 0, 0.5f, 1, 1, (de_gpu_texture){0});
    (void)camera(f, r, 1.0); /* spawned first: order decides, not the camera */
    (void)camera(f, l, 0.0);
    for (u32 k = 0; k < 3; ++k)
        ASSERT_EQ(frame(f), DE_OK);
    de_view order[4];
    ASSERT_EQ(de__views_last_order(f->r, order, 4), 2u);
    EXPECT_EQ(order[0].id, l.id);
    EXPECT_EQ(order[1].id, r.id);
    const de__view_info il = info(f, l), ir = info(f, r);
    EXPECT_EQ(il.viewport_px[0], 0u);
    EXPECT_EQ(il.viewport_px[2], 640u);
    EXPECT_EQ(ir.viewport_px[0], 640u);
    EXPECT_EQ(ir.viewport_px[2], 640u);
    EXPECT_EQ(ir.viewport_px[3], 720u);
    /* Two graph instances. Each compiled twice: in its first frame its output is imported as
     * new (NONE), from then on in SHADER_READ; every later frame is a cache hit. */
    u64 cl = 0, cr = 0;
    de__view_graph_stats(f->r, l, &cl, NULL);
    de__view_graph_stats(f->r, r, &cr, NULL);
    EXPECT_EQ(cl, 2u);
    EXPECT_EQ(cr, 2u);
    /* The output graph composites both (one pass) and the stats name every pass of the frame
     * (published one frame late): two debug clears, then the output. */
    const de_render_stats *st = de_renderer_stats(f->r);
    ASSERT_EQ(st->pass_count, 3u);
    EXPECT_STREQ(st->pass_names[0], "view.debug_clear");
    EXPECT_STREQ(st->pass_names[1], "view.debug_clear");
    EXPECT_STREQ(st->pass_names[2], "output");
    /* Swap the orders: the next frame follows. */
    de_view_destroy(f->r, l);
    const de_view l2 = view(f, "left", 0, 0, 0.5f, 1, 2, (de_gpu_texture){0});
    (void)camera(f, l2, 2.0);
    ASSERT_EQ(frame(f), DE_OK);
    ASSERT_EQ(de__views_last_order(f->r, order, 4), 2u);
    EXPECT_EQ(order[0].id, r.id);
    EXPECT_EQ(order[1].id, l2.id);
    stop(f);
}

TEST_F(views, render_scale_independent, vfx) {
    start(f, DE_GPU_NULL);
    const de_view l = view(f, "left", 0, 0, 0.5f, 1, 0, (de_gpu_texture){0});
    const de_view r = view(f, "right", 0.5f, 0, 0.5f, 1, 1, (de_gpu_texture){0});
    (void)camera(f, l, 0.0);
    (void)camera(f, r, 1.0);
    de_view_set_render_scale(f->r, r, 0.5f);
    ASSERT_EQ(frame(f), DE_OK);
    de__view_info il = info(f, l), ir = info(f, r);
    EXPECT_EQ(il.width, 640u);
    EXPECT_EQ(il.height, 720u);
    EXPECT_EQ(ir.width, 320u);
    EXPECT_EQ(ir.height, 360u);
    EXPECT_EQ(ir.viewport_px[2], 640u); /* composited back to its whole viewport */
    /* The renderer's scale applies to views without their own; the right one keeps 0.5. */
    de_render_settings s = de_renderer_get_settings(f->r);
    s.render_scale = 0.75f;
    de_renderer_set_settings(f->r, &s);
    ASSERT_EQ(frame(f), DE_OK);
    il = info(f, l);
    ir = info(f, r);
    EXPECT_EQ(il.width, 480u);
    EXPECT_EQ(il.height, 540u);
    EXPECT_EQ(ir.width, 320u);
    /* A size change recompiles once (the transient-free graph keys on its imports only, so the
     * same plan serves: compiles stay at 1). */
    u64 compiles = 0;
    de__view_graph_stats(f->r, l, &compiles, NULL);
    EXPECT_EQ(compiles, 1u);
    stop(f);
}

TEST_F(views, graph_cache_steady_state, vfx) {
    start(f, DE_GPU_NULL);
    de_gpu_texture t = de_gpu_texture_create(
        f->d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                     .width = 256,
                                     .height = 128,
                                     .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED,
                                     .name = "monitor"});
    const de_view views3[3] = {
        view(f, "left", 0, 0, 0.5f, 1, 0, (de_gpu_texture){0}),
        view(f, "right", 0.5f, 0, 0.5f, 1, 1, (de_gpu_texture){0}),
        view(f, "monitor", 0, 0, 0.5f, 0.5f, -1, t), /* a part of its target */
    };
    for (u32 k = 0; k < 3; ++k)
        (void)camera(f, views3[k], (f64)k);
    de_view_set_render_scale(f->r, views3[1], 0.5f);
    for (u32 k = 0; k < 5; ++k)
        ASSERT_EQ(frame(f), DE_OK);
    u64 c0[3], h0[3];
    for (u32 k = 0; k < 3; ++k)
        de__view_graph_stats(f->r, views3[k], &c0[k], &h0[k]);
    const de__rg_stats out0 = de__rg_compile_stats(f->r->graph);
    /* 50 steady frames: every graph hits its cache every frame, and nothing is allocated. */
    de_alloc_guard guard = {0};
    const de_tracking_stats before = de_tracking_allocator_stats(&f->track);
    de_alloc_guard_begin(&guard);
    de_result res = DE_OK;
    for (u32 k = 0; k < 50 && res == DE_OK; ++k)
        res = frame(f);
    de_alloc_guard_end(&guard);
    ASSERT_EQ(res, DE_OK);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    const de_tracking_stats after = de_tracking_allocator_stats(&f->track);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    for (u32 k = 0; k < 3; ++k) {
        u64 c = 0, h = 0;
        de__view_graph_stats(f->r, views3[k], &c, &h);
        EXPECT_EQ(c, c0[k]);
        EXPECT_EQ(h, h0[k] + 50u);
        EXPECT_LE(c, 2u); /* the first frame, then its output's steady import state */
    }
    const de__rg_stats out1 = de__rg_compile_stats(f->r->graph);
    EXPECT_EQ(out1.compiles, out0.compiles);
    EXPECT_EQ(out1.hits, out0.hits + 50u);
    de__view_info im = info(f, views3[2]);
    EXPECT_EQ(im.viewport_px[2], 128u);
    EXPECT_EQ(im.viewport_px[3], 64u);
    de_renderer_destroy(f->r); /* before the target it renders into */
    f->r = NULL;
    de_gpu_texture_destroy(f->d, t);
    stop(f);
}

/* ---- Render to texture ---------------------------------------------------------------------- */

typedef struct inset_hook {
    de_renderer *r;
    de_view consumer;
    de_gpu_texture tex;
    u32 calls;
    u32 drawn; /* frames whose consumer graph sampled the texture */
} inset_hook;

/* In the consumer view: the texture drawn into the top-left quarter of the view's colour. */
static void inset_fn(de_rg *g, de_view v, void *user) {
    inset_hook *h = (inset_hook *)user;
    ++h->calls;
    if (v.id != h->consumer.id)
        return;
    const de_rg_resource color = de_view_color(h->r, v);
    u32 w = 0, hh = 0;
    de_view_size(h->r, v, &w, &hh);
    if (!DE_HANDLE_VALID(color) || w == 0)
        return;
    const de_rg_resource src = de_rg_import_texture(g, h->tex, DE_ACCESS_SHADER_READ);
    de_rg_set_name(g, src, "monitor");
    const de__composite_item it = {.src = src, .rect = {0.0f, 0.0f, (f32)(w / 2u), (f32)(hh / 2u)}};
    if (de__composite_add(de__views_composite(h->r), g, g->arena, "test.inset", color,
                          DE__VIEW_OUTPUT_FORMAT, NULL, &it, 1) != NULL)
        ++h->drawn;
}

TEST_F(views, render_to_texture_same_frame, vfx) {
    start(f, DE_GPU_NULL);
    const de_gpu_texture t = de_gpu_texture_create(
        f->d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                     .width = 256,
                                     .height = 256,
                                     .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED,
                                     .name = "monitor"});
    ASSERT_TRUE(DE_HANDLE_VALID(t));
    const de_view mv = de_renderer_main_view(f->r);
    const de_view rtt = view(f, "monitor", 0, 0, 0, 0, -1, t);
    ASSERT_TRUE(DE_HANDLE_VALID(rtt));
    (void)camera(f, (de_view){0}, 0.0);
    (void)camera(f, rtt, 5.0);
    inset_hook h = {.r = f->r, .consumer = mv, .tex = t};
    de_render_add_hook(f->r, DE_HOOK_UI, inset_fn, &h);
    de__gpu_null_log_enable(f->d, true);
    ASSERT_EQ(frame(f), DE_OK);
    de__gpu_null_log_enable(f->d, false);
    /* The producer renders first, into its output, then composites that into its target. */
    de_view order[4];
    ASSERT_EQ(de__views_last_order(f->r, order, 4), 2u);
    EXPECT_EQ(order[0].id, rtt.id);
    EXPECT_EQ(order[1].id, mv.id);
    EXPECT_EQ(info(f, rtt).width, 256u);
    EXPECT_EQ(h.calls, 2u); /* once per view */
    EXPECT_EQ(h.drawn, 1u);
    /* In the command log: the target goes RENDER_TARGET -> SHADER_READ in the producer's
     * submission, before the consumer's submission samples it (no barrier needed there). */
    const u32 n = de__gpu_null_log_count(f->d);
    i32 rt_to_read = -1, consumer_marker = -1;
    for (u32 i = 0; i < n; ++i) {
        const de__gpu_null_entry *e = de__gpu_null_log_entry(f->d, i);
        if (e->op == DE__GPU_NULL_OP_BARRIERS) {
            const de_gpu_barrier *b = de__gpu_null_log_barriers(f->d, e);
            for (u32 k = 0; k < e->data_count; ++k)
                if (b[k].texture.id == t.id && b[k].before == DE_ACCESS_RENDER_TARGET &&
                    b[k].after == DE_ACCESS_SHADER_READ && rt_to_read < 0)
                    rt_to_read = (i32)i;
        }
        if (e->op == DE__GPU_NULL_OP_MARKER_PUSH && strcmp(e->text, "test.inset") == 0)
            consumer_marker = (i32)i;
    }
    EXPECT_GE(rt_to_read, 0);
    EXPECT_GT(consumer_marker, rt_to_read);
    /* A render-to-texture view at half scale is composited into its target. */
    de_view_set_render_scale(f->r, rtt, 0.5f);
    ASSERT_EQ(frame(f), DE_OK);
    EXPECT_EQ(info(f, rtt).width, 128u);
    EXPECT_EQ(h.drawn, 2u);
    de_renderer_destroy(f->r);
    f->r = NULL;
    de_gpu_texture_destroy(f->d, t);
    stop(f);
}

/* ---- Deferred release ----------------------------------------------------------------------- */

TEST_F(views, destroy_deferred, vfx) {
    start(f, DE_GPU_NULL);
    const de_view v = view(f, "doomed", 0, 0, 0.5f, 1, 0, (de_gpu_texture){0});
    (void)camera(f, v, 0.0);
    for (u32 k = 0; k < 3; ++k)
        ASSERT_EQ(frame(f), DE_OK);
    (void)de__gpu_collect(f->d);
    const u32 deferred_before = de__gpu_deferred_count(f->d);
    /* From here the GPU "has not finished" anything submitted (the RHI's queue timelines; the
     * frame fence is a user fence and still signals at submit on the null device). */
    de__gpu_null_set_manual_completion(f->d, true);
    ASSERT_EQ(frame(f), DE_OK);
    de_view_destroy(f->r, v);
    EXPECT_FALSE(DE_HANDLE_VALID(de_view_create(f->r, NULL))); /* (unrelated: refused) */
    ASSERT_EQ(frame(f), DE_OK);
    de__view_info dead;
    EXPECT_FALSE(de__view_info_get(f->r, v, &dead));
    /* The render side retired it once the frame fence passed its last frame; its output texture
     * and its graph's GPU objects wait in the RHI's deferred destruction while the queues still
     * have the frames that used them. */
    EXPECT_EQ(de__views_retired_count(f->r), 0u);
    (void)de__gpu_collect(f->d);
    EXPECT_GT(de__gpu_deferred_count(f->d), deferred_before);
    de__gpu_null_set_manual_completion(f->d, false); /* completes everything */
    (void)de__gpu_collect(f->d);
    EXPECT_EQ(de__gpu_deferred_count(f->d), 0u);
    ASSERT_EQ(frame(f), DE_OK);
    stop(f);
}

/* ---- Hooks and stages ----------------------------------------------------------------------- */

#define MAX_CALLS 128u

typedef struct call_log {
    u32 count;
    u32 view[MAX_CALLS];
    char what[MAX_CALLS][24];
} call_log;

static call_log g_calls;

static void log_call(u32 view_id, const char *what) {
    if (g_calls.count < MAX_CALLS) {
        g_calls.view[g_calls.count] = view_id;
        snprintf(g_calls.what[g_calls.count], sizeof g_calls.what[0], "%s", what);
        ++g_calls.count;
    }
}

/* One object for register and unregister: identical string literals need not share storage. */
static char k_vis_a[] = "vis.a";

static void stage_fn(de__view_frame *vf, void *user) {
    log_call(vf->view.id, (const char *)user);
}

static void hook_fn(de_rg *g, de_view v, void *user) {
    (void)g;
    log_call(v.id, (const char *)user);
}

TEST_F(views, hooks_called_in_order, vfx) {
    start(f, DE_GPU_NULL);
    memset(&g_calls, 0, sizeof g_calls);
    de_renderer *r = f->r;
    /* Registered out of order on purpose. */
    ASSERT_EQ(
        de__view_register_stage(r, DE__VIEW_SLOT_OUTPUT, DE__VIEW_ORDER_UI, "out", stage_fn, "out"),
        DE_OK);
    ASSERT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_POST, 5, "post+", stage_fn, "post+"), DE_OK);
    ASSERT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_POST, DE__VIEW_ORDER_TONEMAP, "tonemap",
                                      stage_fn, "tonemap"),
              DE_OK);
    ASSERT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_POST, -1, "post-", stage_fn, "post-"),
              DE_OK);
    ASSERT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_TRANSPARENT, 0, "transparent", stage_fn,
                                      "transparent"),
              DE_OK);
    ASSERT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_RT, 0, "rt", stage_fn, "rt"), DE_OK);
    ASSERT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_LIGHTING, 0, "light", stage_fn, "light"),
              DE_OK);
    ASSERT_EQ(
        de__view_register_stage(r, DE__VIEW_SLOT_VOLUMETRICS, 0, "volume", stage_fn, "volume"),
        DE_OK);
    ASSERT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_VISIBILITY, 0, "vis.a", stage_fn, k_vis_a),
              DE_OK);
    ASSERT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_VISIBILITY, 0, "vis.b", stage_fn, "vis.b"),
              DE_OK);
    ASSERT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_VISIBILITY, -10, "vis.first", stage_fn,
                                      "vis.first"),
              DE_OK);
    de_render_add_hook(r, DE_HOOK_UI, hook_fn, "H.ui");
    de_render_add_hook(r, DE_HOOK_AFTER_TONEMAP, hook_fn, "H.after_tm");
    de_render_add_hook(r, DE_HOOK_BEFORE_TONEMAP, hook_fn, "H.before_tm");
    de_render_add_hook(r, DE_HOOK_AFTER_TRANSPARENT, hook_fn, "H.after_tr");
    de_render_add_hook(r, DE_HOOK_AFTER_LIGHTING, hook_fn, "H.after_light");
    de_render_add_hook(r, DE_HOOK_AFTER_GBUFFER, hook_fn, "H.after_gbuffer");
    de_render_add_hook(r, DE_HOOK_UI, hook_fn, "H.ui2");
    /* Refused: unknown point, NULL callback. */
    de_render_add_hook(r, (de_rg_hook)42, hook_fn, "bad");
    de_render_add_hook(r, DE_HOOK_UI, NULL, NULL);
    EXPECT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_COUNT, 0, "bad", stage_fn, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_POST, 0, "bad", NULL, NULL),
              DE_ERR_INVALID_ARGUMENT);

    const de_view a = view(f, "a", 0, 0, 0.5f, 1, 0, (de_gpu_texture){0});
    const de_view b = view(f, "b", 0.5f, 0, 0.5f, 1, 1, (de_gpu_texture){0});
    (void)camera(f, b, 1.0);
    (void)camera(f, a, 0.0);
    ASSERT_EQ(frame(f), DE_OK);
    static const char *const want[] = {
        "vis.first", "vis.a",         "vis.b",       "H.after_gbuffer", "light",
        "rt",        "H.after_light", "transparent", "H.after_tr",      "volume",
        "post-",     "H.before_tm",   "tonemap",     "H.after_tm",      "post+",
        "out",       "H.ui",          "H.ui2"};
    const u32 nw = (u32)(sizeof want / sizeof want[0]);
    ASSERT_EQ(g_calls.count, 2u * nw);
    for (u32 vi = 0; vi < 2; ++vi)
        for (u32 i = 0; i < nw; ++i) {
            EXPECT_EQ(g_calls.view[vi * nw + i], vi == 0 ? a.id : b.id);
            EXPECT_STREQ(g_calls.what[vi * nw + i], want[i]);
        }
    /* Unregistering takes effect in the next frame. */
    de__view_unregister_stage(r, stage_fn, k_vis_a); /* by (fn, user) */
    memset(&g_calls, 0, sizeof g_calls);
    ASSERT_EQ(frame(f), DE_OK);
    EXPECT_EQ(g_calls.count, 2u * (nw - 1u));
    EXPECT_STREQ(g_calls.what[1], "vis.b");
    stop(f);
}

/* Stages that add a pass loading and writing the view's colour, named after themselves. */
static void pass_stage(de__view_frame *vf, void *user) {
    de_rg_pass *p = de_rg_add_pass(vf->graph, (const char *)user, DE_QUEUE_GRAPHICS, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    de_rg_read(p, vf->color, DE_ACCESS_RENDER_TARGET);
    de_rg_write(p, vf->color, DE_ACCESS_RENDER_TARGET);
}

TEST_F(views, pass_registration_order, vfx) {
    start(f, DE_GPU_NULL);
    de_renderer *r = f->r;
    /* Slot, then order, then registration: the passes end up p1 .. p6 in every view. */
    ASSERT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_POST, 0, "p6", pass_stage, "p6"), DE_OK);
    ASSERT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_LIGHTING, 3, "p4", pass_stage, "p4"), DE_OK);
    ASSERT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_LIGHTING, 3, "p5", pass_stage, "p5"), DE_OK);
    ASSERT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_VISIBILITY, 1, "p2", pass_stage, "p2"),
              DE_OK);
    ASSERT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_LIGHTING, -3, "p3", pass_stage, "p3"),
              DE_OK);
    ASSERT_EQ(de__view_register_stage(r, DE__VIEW_SLOT_VISIBILITY, 0, "p1", pass_stage, "p1"),
              DE_OK);
    const de_view a = view(f, "a", 0, 0, 0.5f, 1, 0, (de_gpu_texture){0});
    const de_view b = view(f, "b", 0.5f, 0, 0.5f, 1, 1, (de_gpu_texture){0});
    (void)camera(f, a, 0.0);
    (void)camera(f, b, 1.0);
    de__gpu_null_log_enable(f->d, true);
    ASSERT_EQ(frame(f), DE_OK);
    de__gpu_null_log_enable(f->d, false);
    /* "gpu_scene.view" (DENG-177): the GPU scene's stage right after the debug clear, which
     * uploads each view's new scene constants in its first frame. */
    static const char *const want[] = {
        "view.debug_clear", "gpu_scene.view", "p1", "p2", "p3", "p4", "p5", "p6"};
    const u32 nw = (u32)(sizeof want / sizeof want[0]);
    u32 seen[2] = {0, 0};
    bool ok = true;
    const u32 n = de__gpu_null_log_count(f->d);
    for (u32 i = 0; i < n; ++i) {
        const de__gpu_null_entry *e = de__gpu_null_log_entry(f->d, i);
        if (e->op != DE__GPU_NULL_OP_MARKER_PUSH)
            continue;
        /* Lists are named "<graph> b<batch>.<chunk>": "view:a ..." or "view:b ...". */
        const u32 vi = strncmp(e->list, "view:a", 6) == 0   ? 0u
                       : strncmp(e->list, "view:b", 6) == 0 ? 1u
                                                            : 2u;
        if (vi == 2u)
            continue;
        if (seen[vi] >= nw || strcmp(e->text, want[seen[vi]]) != 0) {
            printf("view %u: marker %u is %s\n", vi, seen[vi], e->text);
            ok = false;
        }
        ++seen[vi];
    }
    EXPECT_TRUE(ok);
    EXPECT_EQ(seen[0], nw);
    EXPECT_EQ(seen[1], nw);
    stop(f);
}

/* ---- Metal: pixels and validation ----------------------------------------------------------- */

/* The offscreen target read back (RGBA8, rows of DE__RENDER_OFFSCREEN_WIDTH * 4 bytes), or the
 * texture `t` of `w` x `h`. Caller frees with free(). */
static u8 *readback(vfx *f, de_gpu_texture t, u32 w, u32 h, u32 access) {
    de_gpu_wait_idle(f->d);
    const u32 pitch = (w * 4u + 255u) & ~255u;
    de_gpu_buffer rb = de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = (u64)pitch * h,
                                                                        .usage = DE_USAGE_COPY_DST,
                                                                        .memory = DE_MEM_READBACK,
                                                                        .name = "views readback"});
    if (!DE_HANDLE_VALID(rb))
        return NULL;
    de_gpu_cmd *c = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "views readback");
    if (access != DE_ACCESS_COPY_SRC) {
        const de_gpu_barrier b = {.texture = t, .before = access, .after = DE_ACCESS_COPY_SRC};
        de_gpu_cmd_barriers(c, &b, 1);
    }
    de_gpu_cmd_copy_texture_to_buffer(c, rb, 0, pitch, t, 0, 0);
    if (access != DE_ACCESS_COPY_SRC) {
        const de_gpu_barrier b = {.texture = t, .before = DE_ACCESS_COPY_SRC, .after = access};
        de_gpu_cmd_barriers(c, &b, 1);
    }
    const de_gpu_fence fence = de_gpu_fence_create(f->d, 0);
    const de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = fence, .signal_value = 1};
    u8 *out = NULL;
    if (de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS, &s) == DE_OK &&
        de_gpu_fence_wait(f->d, fence, 1, WAIT_NS) == DE_OK) {
        const u8 *px = (const u8 *)de_gpu_buffer_map(f->d, rb);
        out = (u8 *)malloc((size_t)w * h * 4u);
        if (px != NULL && out != NULL)
            for (u32 y = 0; y < h; ++y)
                memcpy(out + (size_t)y * w * 4u, px + (size_t)y * pitch, (size_t)w * 4u);
    }
    de_gpu_fence_destroy(f->d, fence);
    de_gpu_buffer_destroy(f->d, rb);
    return out;
}

/* The pixel at (x, y) of a `w`-wide RGBA8 image is the view colour of `name` (+-2). */
static bool pixel_is(const u8 *img, u32 w, u32 x, u32 y, const char *name) {
    f32 c[4];
    de__view_debug_color(name, c);
    const u8 *p = img + ((size_t)y * w + x) * 4u;
    for (u32 k = 0; k < 3; ++k) {
        const int want = (int)(c[k] * 255.0f + 0.5f);
        if (abs((int)p[k] - want) > 2) {
            printf("(%u, %u) of %s: got %u %u %u, want %d %d %d\n", x, y, name, p[0], p[1], p[2],
                   (int)(c[0] * 255.0f + 0.5f), (int)(c[1] * 255.0f + 0.5f),
                   (int)(c[2] * 255.0f + 0.5f));
            return false;
        }
    }
    return true;
}

TEST_F(views, metal_split_rtt_readback, vfx) {
    start(f, DE_GPU_METAL);
    if (!de__composite_ready(de__views_composite(f->r)))
        SKIP("no composite shader in this build (the cooker and Slang are needed)");
    const de_gpu_texture t = de_gpu_texture_create(
        f->d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                     .width = 256,
                                     .height = 256,
                                     .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                              DE_USAGE_COPY_SRC,
                                     .name = "monitor"});
    ASSERT_TRUE(DE_HANDLE_VALID(t));
    const de_view l = view(f, "left", 0, 0, 0.5f, 1, 0, (de_gpu_texture){0});
    const de_view r = view(f, "right", 0.5f, 0, 0.5f, 1, 1, (de_gpu_texture){0});
    const de_view m = view(f, "monitor", 0, 0, 0, 0, -1, t);
    (void)camera(f, l, 0.0);
    (void)camera(f, r, 1.0);
    (void)camera(f, m, 2.0);
    de_view_set_render_scale(f->r, r, 0.5f);
    inset_hook h = {.r = f->r, .consumer = l, .tex = t};
    de_render_add_hook(f->r, DE_HOOK_UI, inset_fn, &h);
    for (u32 k = 0; k < 4; ++k)
        ASSERT_EQ(frame(f), DE_OK);
    EXPECT_EQ(info(f, r).width, 320u);
    EXPECT_EQ(info(f, l).width, 640u);
    const u32 W = DE__RENDER_OFFSCREEN_WIDTH, H = DE__RENDER_OFFSCREEN_HEIGHT;
    u8 *img = readback(f, de__renderer_offscreen(f->r), W, H, DE__RENDER_OFFSCREEN_FINAL_ACCESS);
    ASSERT_TRUE(img != NULL);
    EXPECT_TRUE(pixel_is(img, W, 100, 100, "monitor")); /* inset: left view's top-left quarter */
    EXPECT_TRUE(pixel_is(img, W, 300, 170, "monitor"));
    EXPECT_TRUE(pixel_is(img, W, 330, 190, "left")); /* outside the inset */
    EXPECT_TRUE(pixel_is(img, W, 600, 600, "left"));
    EXPECT_TRUE(pixel_is(img, W, 641, 10, "right")); /* half resolution, scaled up */
    EXPECT_TRUE(pixel_is(img, W, 1279, 719, "right"));
    free(img);
    u8 *tex = readback(f, t, 256, 256, DE_ACCESS_SHADER_READ);
    ASSERT_TRUE(tex != NULL);
    EXPECT_TRUE(pixel_is(tex, 256, 0, 0, "monitor"));
    EXPECT_TRUE(pixel_is(tex, 256, 255, 255, "monitor"));
    free(tex);
    de_renderer_destroy(f->r);
    f->r = NULL;
    de_gpu_texture_destroy(f->d, t);
    stop(f);
}

TEST_F(views, metal_destroy_in_flight, vfx) {
    start(f, DE_GPU_METAL);
    de_view keep = view(f, "keep", 0, 0, 0.5f, 1, 0, (de_gpu_texture){0});
    (void)camera(f, keep, 0.0);
    const de_gpu_texture t = de_gpu_texture_create(
        f->d, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                     .width = 64,
                                     .height = 64,
                                     .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED,
                                     .name = "churn target"});
    /* Views made and destroyed every few frames, with no wait: frames in flight still use the
     * destroyed views' graphs and outputs when the next frame retires them. */
    de_view churn[3] = {{0}, {0}, {0}};
    de_entity cams[3] = {DE_ENTITY_NULL, DE_ENTITY_NULL, DE_ENTITY_NULL};
    u32 max_retired = 0;
    for (u32 k = 0; k < 30; ++k) {
        const u32 i = k % 3u;
        if (DE_HANDLE_VALID(churn[i])) {
            de_view_destroy(f->r, churn[i]);
            de_entity_despawn(f->w, cams[i]);
        }
        churn[i] = view(f, NULL, 0.5f, 0, 0.5f, 1, (i32)i + 1, i == 2 ? t : (de_gpu_texture){0});
        if (i == 1)
            de_view_set_render_scale(f->r, churn[i], 0.5f);
        cams[i] = camera(f, churn[i], (f64)k);
        ASSERT_EQ(frame(f), DE_OK);
        const u32 rc = de__views_retired_count(f->r);
        max_retired = rc > max_retired ? rc : max_retired;
    }
    printf("most retired views waiting at once: %u\n", max_retired);
    de_gpu_wait_idle(f->d);
    ASSERT_EQ(frame(f), DE_OK);
    ASSERT_EQ(frame(f), DE_OK);
    EXPECT_EQ(de__views_retired_count(f->r), 0u);
    de_renderer_destroy(f->r);
    f->r = NULL;
    de_gpu_texture_destroy(f->d, t);
    stop(f);
}
