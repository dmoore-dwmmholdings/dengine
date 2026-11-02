#include "de_test.h"
#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>
#include "gpu/gpu_backend.h"
#include "render/rt/rt_internal.h"
#include "render/rt/rtao.h"
#include "render/rt/restir_di.h"
#include "render/rt/rt_shadows.h"
#include "render/components_internal.h"
#include "render/world/render_world.h"
#include <string.h>

typedef struct lazy_fx {
    de_gpu_device *device;
    de_renderer *renderer;
    de_type_registry *types;
    de_world *world;
    de_gpu_texture targets[2];
    de_tracking_allocator memory, gpu_memory;
    de_allocator alloc, gpu_alloc;
    const de__gpu_backend_vtable *native;
    de__gpu_backend_vtable instrumented;
    u32 calls, successes, failures, rt_creates, fail_at, source_calls, reservoir_calls;
    const char *fail_name;
} lazy_fx;
static lazy_fx *active;

/* Forward every successful creation to the real backend; fail only the selected native call. */
static de_result pipeline_probe(de_gpu_device *device, de__gpu_pipeline *pipeline,
                                const de_gpu_pipeline_desc *desc) {
    lazy_fx *f = active;
    ++f->calls;
    if (desc->name && (!strcmp(desc->name, "cs_generate") || !strcmp(desc->name, "cs_sources")))
        ++f->rt_creates;
    if (desc->name && (!strcmp(desc->name, "cs_sources") || !strcmp(desc->name, "cs_alias")))
        ++f->source_calls;
    if (desc->name && !strncmp(desc->name, "cs_initial", 10))
        ++f->reservoir_calls;
    if ((f->fail_at && f->calls == f->fail_at) ||
        (f->fail_name && desc->name && !strcmp(desc->name, f->fail_name))) {
        ++f->failures;
        return DE_FAIL(DE_ERR_UNSUPPORTED, "injected native pipeline failure");
    }
    de_result result = f->native->pipeline_create(device, pipeline, desc);
    if (result == DE_OK)
        ++f->successes;
    return result;
}
static void lazy_fx_setup(lazy_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->memory, NULL, DE_MEM_RENDER);
    f->gpu_alloc = de_tracking_allocator_init(&f->gpu_memory, NULL, DE_MEM_RENDER);
    de_result result = de_gpu_device_create(&(de_gpu_device_desc){.debug_layer = true,
                                                                  .allocator = &f->gpu_alloc,
                                                                  .pipeline_cache_dir = ""},
                                            &f->device);
    if (result == DE_ERR_UNSUPPORTED || result == DE_ERR_NOT_FOUND)
        SKIP("native GPU unavailable");
    ASSERT_EQ(result, DE_OK);
    const de_gpu_caps *caps = de_gpu_get_caps(f->device);
    if (!caps->ray_query || !caps->max_bindless_accels)
        SKIP("native ray queries unavailable");
    f->native = f->device->vt;
    f->instrumented = *f->native;
    f->instrumented.pipeline_create = pipeline_probe;
    active = f;
    f->device->vt = &f->instrumented;
    ASSERT_EQ(de_type_registry_create(NULL, &f->types), DE_OK);
    ASSERT_EQ(
        de_renderer_create(
            &(de_renderer_desc){.device = f->device, .types = f->types, .allocator = &f->alloc},
            &f->renderer),
        DE_OK);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.rt.restir_di"), "0"), DE_OK);
}
static void lazy_fx_teardown(lazy_fx *f) {
    if (f->device && f->native)
        f->device->vt = f->native;
    if (f->renderer)
        de_renderer_destroy(f->renderer);
    if (f->world)
        de_world_destroy(f->world);
    if (f->device)
        for (u32 i = 0; i < 2; ++i)
            de_gpu_texture_destroy(f->device, f->targets[i]);
    if (f->types)
        de_type_registry_destroy(f->types);
    if (f->device)
        de_gpu_device_destroy(f->device);
    active = NULL;
    if (f->world)
        (void)de_cvar_set(de_cvar_find("r.pipelined"), "1");
    EXPECT_EQ(de_tracking_allocator_stats(&f->memory).blocks_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->gpu_memory).blocks_live, 0u);
}
TEST_F(rt_lazy, renderer_registration_does_not_create_hybrid_pipelines, lazy_fx) {
    /* All six original eager owners create cs_generate or cs_sources. Observe real calls. */
    EXPECT_EQ(f->rt_creates, 0u);
}
TEST(rt_lazy, partial_ao_owner_allocation_failure_releases_state) {
    de_tracking_allocator memory;
    de_allocator alloc = de_tracking_allocator_init(&memory, NULL, DE_MEM_RENDER);
    /* The injected second allocation fails before this owner uses device/view state. */
    de_renderer renderer = {.alloc = &alloc};
    de__rtao *extra = NULL;
    de_tracking_allocator_fail_after(&memory, 1);
    de_result result = de__rtao_create(&renderer, &extra);
    de_tracking_allocator_fail_after(&memory, -1);
    EXPECT_EQ(result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_TRUE(extra == NULL);
    EXPECT_EQ(de_tracking_allocator_stats(&memory).blocks_live, 0u);
}
TEST(rt_lazy, partial_shadow_owner_allocation_failure_releases_state) {
    de_tracking_allocator memory;
    de_allocator alloc = de_tracking_allocator_init(&memory, NULL, DE_MEM_RENDER);
    /* The injected second allocation fails before this owner uses device/view state. */
    de_renderer renderer = {.alloc = &alloc};
    de__rt_shadows *extra = NULL;
    de_tracking_allocator_fail_after(&memory, 1);
    de_result result = de__rt_shadows_create(&renderer, &extra);
    de_tracking_allocator_fail_after(&memory, -1);
    EXPECT_EQ(result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_TRUE(extra == NULL);
    EXPECT_EQ(de_tracking_allocator_stats(&memory).blocks_live, 0u);
}

/* Selection tests call the same effective-view preparation used by production views.c. */
static de_render_settings selected(lazy_fx *f, u32 effect) {
    de_render_settings s = {.tier = DE_TIER_RT, .render_scale = 1};
    switch (effect) {
    case 0:
        s.rt_gi = true;
        s.gi = DE_QUALITY_HIGH;
        s.gi_mode = DE_GI_RT;
        break;
    case 1:
        s.rt_reflections = true;
        s.reflections = DE_QUALITY_HIGH;
        break;
    case 2:
        s.rt_refraction = true;
        break;
    case 3:
        s.rt_ao = true;
        s.ao = DE_QUALITY_HIGH;
        break;
    case 4:
        s.rt_shadows = true;
        s.shadows = DE_QUALITY_HIGH;
        break;
    case 5:
        ASSERT_EQ(de_cvar_set(de_cvar_find("r.rt.restir_di"), "1"), DE_OK);
        break;
    default:
        FAIL("invalid RT effect");
    }
    (void)f;
    return s;
}
static void selection(lazy_fx *f, u32 effect, u32 count) {
    ASSERT_EQ(f->rt_creates, 0u);
    de_render_settings s = selected(f, effect), base = s;
    base.tier = DE_TIER_BASE;
    u32 before = de__gpu_live_count(f->device, DE__GPU_PIPELINE);
    f->calls = f->successes = 0;
    ASSERT_EQ(de__rt_effects_prepare(f->renderer, &base, false), DE_OK);
    ASSERT_EQ(de__rt_effects_prepare(f->renderer, &s, true), DE_OK);
    ASSERT_EQ(f->calls, 0u);
    ASSERT_EQ(de__rt_effects_prepare(f->renderer, &s, false), DE_OK);
    EXPECT_EQ(f->calls, count);
    EXPECT_EQ(f->successes, count);
    EXPECT_EQ(de__gpu_live_count(f->device, DE__GPU_PIPELINE), before + count);
    u64 host_allocs = de_tracking_allocator_stats(&f->memory).alloc_count;
    u64 gpu_allocs = de_tracking_allocator_stats(&f->gpu_memory).alloc_count;
    for (u32 i = 0; i < 8; ++i) {
        ASSERT_EQ(de__rt_effects_prepare(f->renderer, &s, false), DE_OK);
        ASSERT_EQ(de__rt_effects_prepare(f->renderer, &base, false), DE_OK);
        ASSERT_EQ(de__rt_effects_prepare(f->renderer, &s, true), DE_OK);
    }
    EXPECT_EQ(f->calls, count);
    EXPECT_EQ(de__gpu_live_count(f->device, DE__GPU_PIPELINE), before + count);
    EXPECT_EQ(de_tracking_allocator_stats(&f->memory).alloc_count, host_allocs);
    EXPECT_EQ(de_tracking_allocator_stats(&f->gpu_memory).alloc_count, gpu_allocs);
}
static void failure(lazy_fx *f, u32 effect) {
    ASSERT_EQ(f->rt_creates, 0u);
    de_render_settings s = selected(f, effect);
    u32 before = de__gpu_live_count(f->device, DE__GPU_PIPELINE);
    f->calls = f->successes = f->failures = 0;
    f->fail_at = 2; /* One real native pipeline succeeds before the injected failure. */
    de_result result = de__rt_effects_prepare(f->renderer, &s, false);
    if (effect == 3)
        EXPECT_EQ(result, DE_OK); /* RTAO retains its existing GTAO fallback. */
    else
        EXPECT_NE(result, DE_OK);
    EXPECT_GT(f->successes, 0u);
    EXPECT_EQ(f->failures, 1u);
    EXPECT_EQ(de__gpu_live_count(f->device, DE__GPU_PIPELINE), before);
    u32 attempts = f->calls;
    f->fail_at = 0;
    EXPECT_EQ(de__rt_effects_prepare(f->renderer, &s, false), result);
    EXPECT_EQ(f->calls, attempts);
    EXPECT_EQ(de__gpu_live_count(f->device, DE__GPU_PIPELINE), before);
    de_render_settings base = s;
    base.tier = DE_TIER_BASE;
    EXPECT_EQ(de__rt_effects_prepare(f->renderer, &base, false), DE_OK);
    EXPECT_EQ(de__rt_effects_prepare(f->renderer, &s, true), DE_OK);
    EXPECT_EQ(f->calls, attempts);
    EXPECT_FALSE(de_gpu_device_is_lost(f->device));
}

static void render_frame(lazy_fx *f) {
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_POST_UPDATE, 1.f / 60, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 1.f / 60, 0), DE_OK);
    ASSERT_EQ(de__render_extract_and_kick(f->renderer, f->world), DE_OK);
    de_gpu_wait_idle(f->device);
}
TEST_F(rt_lazy, actual_views_use_effective_settings_and_share_pipelines, lazy_fx) {
    ASSERT_EQ(
        de_world_create(&(de_world_desc){.allocator = &f->alloc, .types = f->types}, &f->world),
        DE_OK);
    de_renderer_attach(f->renderer, f->world);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.pipelined"), "0"), DE_OK);
    de_render_settings rt = selected(f, 2);
    de_render_settings base = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW);
    de_renderer_set_settings(f->renderer, &rt);
    const DeRenderComponents *components = de__render_components(f->world);
    ASSERT_TRUE(components != NULL);
    de_builtin_component_ids builtin = de_builtin_components(f->world);
    de_view views[2];
    for (u32 i = 0; i < 2; ++i) {
        f->targets[i] = de_gpu_texture_create(
            f->device, &(de_gpu_texture_desc){.width = 16,
                                              .height = 16,
                                              .format = DE_FMT_RGBA16_FLOAT,
                                              .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED});
        ASSERT_TRUE(f->targets[i].id);
        views[i] = de_view_create(
            f->renderer,
            &(de_view_desc){.name = "lazy RT view", .target = f->targets[i], .order = (i32)i});
        ASSERT_TRUE(views[i].id);
        de_view_set_settings(f->renderer, views[i], &base);
        de_component ids[] = {builtin.transform, components->camera};
        de_entity camera;
        ASSERT_EQ(de_entity_spawn_with(f->world, ids, 2, &camera), DE_OK);
        *(DeTransform *)de_entity_get(f->world, camera, builtin.transform) =
            de_transform_identity();
        ((DeCamera *)de_entity_get(f->world, camera, components->camera))->view = views[i];
    }
    /* A diagnostic RT view and a Base override must ignore the renderer's RT selection. */
    de_view_set_settings(f->renderer, views[0], &rt);
    ASSERT_EQ(de_view_set_display(f->renderer, views[0],
                                  &(de_view_display){.mode = DE_VIEW_RENDER_BASE_COLOR}),
              DE_OK);
    render_frame(f);
    EXPECT_EQ(f->rt_creates, 0u);
    de_view_set_settings(f->renderer, views[0], &base);
    ASSERT_EQ(de_view_set_display(f->renderer, views[0], &(de_view_display){0}), DE_OK);
    render_frame(f);
    EXPECT_EQ(f->rt_creates, 0u);
    de_view_set_settings(f->renderer, views[0], &rt);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_POST_UPDATE, 1.f / 60, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 1.f / 60, 0), DE_OK);
    /* The live edit is for the next Extract; this submitted view still selects RT. */
    de_view_set_settings(f->renderer, views[0], &base);
    ASSERT_EQ(de__render_extract_and_kick(f->renderer, f->world), DE_OK);
    de_gpu_wait_idle(f->device);
    EXPECT_EQ(f->rt_creates, 1u);
    de_view_set_settings(f->renderer, views[1], &rt);
    render_frame(f);
    EXPECT_EQ(f->rt_creates, 1u);
    for (u32 i = 0; i < 2; ++i) {
        de__view_info info;
        ASSERT_TRUE(de__view_info_get(f->renderer, views[i], &info));
        EXPECT_TRUE(info.active);
        EXPECT_EQ(info.executed, 4u);
    }
}
/* An actual path-traced view with a live local-light source exercises the shared alias path. */
static de_view path_view(lazy_fx *f, de_entity *light) {
    if (!de_gpu_get_caps(f->device)->rt_pipelines)
        SKIP("native RT pipelines unavailable");
    ASSERT_EQ(
        de_world_create(&(de_world_desc){.allocator = &f->alloc, .types = f->types}, &f->world),
        DE_OK);
    de_renderer_attach(f->renderer, f->world);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.pipelined"), "0"), DE_OK);
    de_render_settings settings = {
        .tier = DE_TIER_RT, .render_scale = 1, .path_tracer = true, .path_max_bounces = 1};
    de_renderer_set_settings(f->renderer, &settings);
    const DeRenderComponents *c = de__render_components(f->world);
    de_builtin_component_ids b = de_builtin_components(f->world);
    f->targets[0] = de_gpu_texture_create(
        f->device, &(de_gpu_texture_desc){.width = 16,
                                          .height = 16,
                                          .format = DE_FMT_RGBA16_FLOAT,
                                          .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED});
    ASSERT_TRUE(f->targets[0].id);
    de_view view = de_view_create(f->renderer, &(de_view_desc){.target = f->targets[0]});
    ASSERT_TRUE(view.id);
    de_component camera_ids[] = {b.transform, c->camera};
    de_entity camera;
    ASSERT_EQ(de_entity_spawn_with(f->world, camera_ids, 2, &camera), DE_OK);
    *(DeTransform *)de_entity_get(f->world, camera, b.transform) = de_transform_identity();
    ((DeCamera *)de_entity_get(f->world, camera, c->camera))->view = view;
    de_component light_ids[] = {b.transform, c->light};
    ASSERT_EQ(de_entity_spawn_with(f->world, light_ids, 2, light), DE_OK);
    *(DeTransform *)de_entity_get(f->world, *light, b.transform) = de_transform_identity();
    *(DeLight *)de_entity_get(f->world, *light, c->light) =
        (DeLight){.kind = DE_LIGHT_POINT, .color = {1, 1, 1}, .intensity = 10, .range = 10};
    return view;
}
static void expect_executed(lazy_fx *f, de_view view, u64 count) {
    de__view_info info;
    ASSERT_TRUE(de__view_info_get(f->renderer, view, &info));
    EXPECT_EQ(info.executed, count);
    EXPECT_FALSE(de_gpu_device_is_lost(f->device));
}
TEST_F(rt_lazy, path_sources_are_lazy_and_reused_by_hybrid_DI, lazy_fx) {
    de_entity light;
    de_view view = path_view(f, &light);
    EXPECT_EQ(f->source_calls, 0u);
    render_frame(f);
    expect_executed(f, view, 1);
    EXPECT_EQ(f->source_calls, 2u);
    EXPECT_EQ(f->reservoir_calls, 0u);
    EXPECT_EQ(f->rt_creates, 1u); /* Only cs_sources; no hybrid RT owner is selected. */
    de_render_settings di = selected(f, 5);
    f->calls = 0;
    ASSERT_EQ(de__rt_effects_prepare(f->renderer, &di, false), DE_OK);
    EXPECT_EQ(f->calls, 8u);
    EXPECT_EQ(f->source_calls, 2u);
    EXPECT_EQ(f->reservoir_calls, 2u);
    render_frame(f);
    expect_executed(f, view, 2);
    EXPECT_EQ(f->source_calls, 2u);
}
TEST_F(rt_lazy, path_source_partial_failure_is_cleaned_and_cached, lazy_fx) {
    de_entity light;
    de_view view = path_view(f, &light);
    f->fail_name = "cs_alias";
    render_frame(f);
    expect_executed(f, view, 0);
    EXPECT_EQ(f->source_calls, 2u);
    EXPECT_EQ(f->failures, 1u);
    u32 live = de__gpu_live_count(f->device, DE__GPU_PIPELINE);
    f->fail_name = NULL;
    render_frame(f);
    expect_executed(f, view, 0);
    EXPECT_EQ(f->source_calls, 2u);
    EXPECT_EQ(de__gpu_live_count(f->device, DE__GPU_PIPELINE), live);
    de_render_settings di = selected(f, 5);
    EXPECT_NE(de__rt_effects_prepare(f->renderer, &di, false), DE_OK);
    EXPECT_EQ(f->source_calls, 2u);
    EXPECT_EQ(f->reservoir_calls, 0u);
    de_render_settings base = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW);
    de_renderer_set_settings(f->renderer, &base);
    render_frame(f);
    expect_executed(f, view, 1);
}
TEST_F(rt_lazy, hybrid_DI_failure_retains_working_path_sources, lazy_fx) {
    de_entity light;
    de_view view = path_view(f, &light);
    render_frame(f);
    expect_executed(f, view, 1);
    u32 live = de__gpu_live_count(f->device, DE__GPU_PIPELINE);
    de_render_settings di = selected(f, 5);
    f->fail_name = "cs_initial";
    EXPECT_NE(de__rt_effects_prepare(f->renderer, &di, false), DE_OK);
    EXPECT_EQ(f->source_calls, 2u);
    EXPECT_EQ(de__gpu_live_count(f->device, DE__GPU_PIPELINE), live);
    f->fail_name = NULL;
    const DeRenderComponents *c = de__render_components(f->world);
    ((DeLight *)de_entity_get(f->world, light, c->light))->intensity = 20;
    render_frame(f); /* New source signature executes both shared kernels again. */
    expect_executed(f, view, 2);
    EXPECT_EQ(f->source_calls, 2u);
    u32 calls = f->calls;
    EXPECT_NE(de__rt_effects_prepare(f->renderer, &di, false), DE_OK);
    EXPECT_EQ(f->calls, calls);
}
#define EFFECT_CASES(name, effect, count)                                                          \
    TEST_F(rt_lazy, name##_selected_once_and_reused, lazy_fx) {                                    \
        selection(f, effect, count);                                                               \
    }                                                                                              \
    TEST_F(rt_lazy, name##_partial_failure_is_cleaned_and_cached, lazy_fx) {                       \
        failure(f, effect);                                                                        \
    }
EFFECT_CASES(gi, 0, 8)
EFFECT_CASES(reflections, 1, 5)
EFFECT_CASES(refraction, 2, 6)
EFFECT_CASES(ao, 3, 3)
EFFECT_CASES(shadows, 4, 2)
EFFECT_CASES(direct_lighting, 5, 10)
