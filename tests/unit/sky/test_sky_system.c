/*
 * test_sky_system.c: de_sky_attach without a renderer (DENG-206): the sky's state, the driven
 * directional light, and no per-frame heap allocations in the sky's tag.
 */
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>

#include <math.h>
#include <string.h>

typedef struct skyfx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_type_registry *types;
    de_world *w;
    de_sky *sky;
    de_entity sky_e, light_e;
} skyfx;

static void skyfx_setup(skyfx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_GENERAL);
    de_fp_env_init_thread();
    ASSERT_EQ(de_type_registry_create(&f->alloc, &f->types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &f->alloc, .types = f->types}, &f->w),
              DE_OK);
}

static void skyfx_teardown(skyfx *f) {
    de_sky_detach(f->sky);
    if (f->w != NULL)
        de_world_destroy(f->w);
    if (f->types != NULL)
        de_type_registry_destroy(f->types);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
}

static void scene(skyfx *f, f32 hours, f32 moon_phase) {
    ASSERT_EQ(de_render_register_components(f->w), DE_OK);
    const de_component cs = de_component_find(f->w, "DeSky");
    const de_component cl = de_component_find(f->w, "DeLight");
    const de_component ct = de_component_find(f->w, "DeTransform");
    f->sky_e = de_entity_spawn(f->w);
    DeSky *s = (DeSky *)de_entity_add(f->w, f->sky_e, cs);
    *s = (DeSky){.time_of_day_hours = hours, .latitude_deg = 45.0f, .turbidity = 2.0f,
                 .day_of_year = 80.0f, .moon_phase = moon_phase, .ground_albedo = 0.3f,
                 .drive_sun = true};
    f->light_e = de_entity_spawn(f->w);
    DeLight *l = (DeLight *)de_entity_add(f->w, f->light_e, cl);
    l->kind = DE_LIGHT_DIRECTIONAL;
    l->intensity = 1.0f;
    l->color = (de_vec3){1.0f, 1.0f, 1.0f};
    DeTransform *t = (DeTransform *)de_entity_add(f->w, f->light_e, ct);
    t->rotation = (de_quat){0.0f, 0.0f, 0.0f, 1.0f};
    t->scale = (de_vec3){1.0f, 1.0f, 1.0f};
    ASSERT_EQ(de_sky_attach(f->w, NULL, &f->sky), DE_OK);
}

static void set_hours(skyfx *f, f32 hours) {
    DeSky *s = (DeSky *)de_entity_get(f->w, f->sky_e, de_component_find(f->w, "DeSky"));
    s->time_of_day_hours = hours;
}

static void frame(skyfx *f) {
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 1.0f / 60.0f, 0.0f), DE_OK);
}

TEST_F(sky_system, attach_errors, skyfx) {
    de_sky *s = NULL;
    EXPECT_EQ(de_sky_attach(NULL, NULL, &s), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_sky_attach(f->w, NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_sky_attach(f->w, NULL, &s), DE_ERR_NOT_FOUND); /* components not registered */
    EXPECT_TRUE(s == NULL);
    de_sky_detach(NULL);
}

TEST_F(sky_system, noon_drives_sun, skyfx) {
    scene(f, 12.0f, -1.0f);
    de_sky_state st;
    EXPECT_FALSE(de_sky_get_state(f->sky, &st));
    frame(f);
    ASSERT_TRUE(de_sky_get_state(f->sky, &st));
    EXPECT_FLOAT_EQ(st.sun_elevation_deg, 45.0f, 1.0f);
    EXPECT_FALSE(st.light_is_moon);
    /* Clear noon at 45 degrees: tens of klx at the ground, below the top of the atmosphere. */
    EXPECT_TRUE((st.light_illuminance) > (50000.0f));
    EXPECT_TRUE((st.light_illuminance) < (DE_SKY_SOLAR_ILLUMINANCE));
    EXPECT_FLOAT_EQ(fmaxf(st.light_color.x, fmaxf(st.light_color.y, st.light_color.z)), 1.0f,
                    1e-6f);
    EXPECT_TRUE((st.light_color.z) < (st.light_color.x)); /* sunlight at the ground is warm */

    const DeLight *l = (const DeLight *)de_entity_get_const(f->w, f->light_e,
                                                            de_component_find(f->w, "DeLight"));
    EXPECT_FLOAT_EQ(l->intensity, st.light_illuminance, 1e-3f);
    const DeTransform *t = (const DeTransform *)de_entity_get_const(
        f->w, f->light_e, de_component_find(f->w, "DeTransform"));
    const de_vec3 z = de_quat_rotate(t->rotation, (de_vec3){0.0f, 0.0f, 1.0f});
    EXPECT_FLOAT_EQ(z.x, st.sun_direction.x, 1e-4f);
    EXPECT_FLOAT_EQ(z.y, st.sun_direction.y, 1e-4f);
    EXPECT_FLOAT_EQ(z.z, st.sun_direction.z, 1e-4f);
    EXPECT_TRUE((st.sun_direction.y) < (0.0f)); /* the noon sun is south at latitude 45 N */

    /* Sunset is redder and dimmer than noon. */
    const f32 noon = st.light_illuminance, noon_b = st.light_color.z;
    set_hours(f, 17.8f);
    frame(f);
    ASSERT_TRUE(de_sky_get_state(f->sky, &st));
    EXPECT_TRUE((st.sun_elevation_deg) > (0.0f));
    EXPECT_TRUE((st.light_illuminance) < (noon));
    EXPECT_TRUE((st.light_color.z) < (noon_b));
}

TEST_F(sky_system, night_moon, skyfx) {
    scene(f, 0.0f, 0.5f); /* midnight, full moon: the moon is opposite the sun, high */
    frame(f);
    de_sky_state st;
    ASSERT_TRUE(de_sky_get_state(f->sky, &st));
    EXPECT_TRUE((st.sun_elevation_deg) < (0.0f));
    EXPECT_TRUE((st.moon_elevation_deg) > (0.0f));
    EXPECT_TRUE((st.moon_illuminated_fraction) >= (0.99f));
    EXPECT_TRUE(st.light_is_moon);
    EXPECT_TRUE((st.light_illuminance) > (0.0f));
    EXPECT_TRUE((st.light_illuminance) < (1.0f)); /* moonlight is well under 1 lux */

    /* New moon at midnight: nothing lights the scene. */
    DeSky *s = (DeSky *)de_entity_get(f->w, f->sky_e, de_component_find(f->w, "DeSky"));
    s->moon_phase = 0.0f;
    frame(f);
    ASSERT_TRUE(de_sky_get_state(f->sky, &st));
    EXPECT_TRUE((st.light_illuminance) < (1e-3f));
}

TEST_F(sky_system, no_per_frame_allocations, skyfx) {
    scene(f, 6.0f, -1.0f);
    for (int i = 0; i < 4; ++i)
        frame(f); /* warm up the schedule */
#if DE_MEM_TRACKING
    const u64 before = de_mem_stats_for_tag(DE_MEM_RENDER).alloc_count;
    for (int i = 0; i < 120; ++i) {
        set_hours(f, 6.0f + 0.1f * (f32)i); /* the sky recomputes every frame */
        frame(f);
    }
    EXPECT_EQ(de_mem_stats_for_tag(DE_MEM_RENDER).alloc_count, before);
#else
    SKIP("heap tag stats need DE_MEM_TRACKING (Debug or Dev)");
#endif
}
