/* DENG-644: exercise production selection without creating a platform/GPU device. */
#include "de_test.h"
#include "render/world/render_world.h"
#include "render/shadows/shadows_internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct admission {
    de__render_world_frame world;
    de__rw_view camera;
    u64 entities[2], light_entities[2];
    u32 masks[2], selected[DE__ATLAS_LIGHTS];
    u8 flags[2];
    de_aabb bounds[2];
    DeLight lights[2];
    de_affine transforms[2];
} admission;
static void admission_setup(admission *f) {
    memset(f, 0, sizeof *f);
    f->entities[0] = 100;
    f->entities[1] = 101;
    f->light_entities[0] = 200;
    f->light_entities[1] = 201;
    f->masks[0] = 1;
    f->masks[1] = 2;
    f->flags[0] = f->flags[1] = DE__RW_INST_HAS_BOUNDS;
    f->bounds[0] = (de_aabb){{-2, -2, -1}, {2, 2, 1}};
    f->bounds[1] = (de_aabb){{998, -2, -1}, {1002, 2, 1}};
    f->world.instances = (de__rw_instances){.count = 2,
                                            .entity = f->entities,
                                            .layer_mask = f->masks,
                                            .flags = f->flags,
                                            .bounds = f->bounds};
    for (u32 i = 0; i < 2; ++i) {
        f->lights[i] = (DeLight){.kind = DE_LIGHT_POINT,
                                 .color = {1, 1, 1},
                                 .intensity = i ? 1e9f : 10,
                                 .range = 8,
                                 .cast_shadows = true};
        f->transforms[i] = de_affine_from_trs((de_vec3){i ? 1000 : 0, 0, 4}, de_quat_identity(),
                                              (de_vec3){1, 1, 1});
    }
    f->world.lights = (de__rw_lights){
        .count = 2, .entity = f->light_entities, .light = f->lights, .transform = f->transforms};
    f->camera.offset = (de_vec3){0, 0, 20}; /* Outside both finite influence spheres. */
}
static void admission_teardown(admission *f) {
    (void)f;
}
TEST_F(shadow_admission, irrelevant_light_cannot_evict_office, admission) {
    ASSERT_EQ(de__atlas_select_lights(&f->world, &f->camera, 1, false, 1, f->selected), 1);
    EXPECT_EQ(f->selected[0], 0);
    printf("office budget=1 selected-light=%llu expected=200\n",
           (unsigned long long)f->light_entities[f->selected[0]]);
}
TEST_F(shadow_admission, distant_view_keeps_distant_light, admission) {
    ASSERT_EQ(de__atlas_select_lights(&f->world, &f->camera, 2, false, 1, f->selected), 1);
    EXPECT_EQ(f->selected[0], 1);
}
TEST_F(shadow_admission, camera_outside_and_non_caster_receives, admission) {
    f->world.lights.count = 1;
    ASSERT_EQ(de__atlas_select_lights(&f->world, &f->camera, 1, false, 1, f->selected), 1);
    EXPECT_EQ(f->selected[0], 0); /* Receiver does not cast shadows. */
}
TEST_F(shadow_admission, empty_and_layer_changes, admission) {
    EXPECT_EQ(de__atlas_select_lights(&f->world, &f->camera, 4, false, 32, f->selected), 0);
    f->masks[0] = 4;
    ASSERT_EQ(de__atlas_select_lights(&f->world, &f->camera, 4, false, 32, f->selected), 1);
    EXPECT_EQ(f->selected[0], 0);
    f->entities[0] = 0;
    EXPECT_EQ(de__atlas_select_lights(&f->world, &f->camera, 4, false, 32, f->selected), 0);
}
TEST_F(shadow_admission, shared_mask_and_zero_means_all, admission) {
    ASSERT_EQ(de__atlas_select_lights(&f->world, &f->camera, 0, false, 32, f->selected), 2);
    EXPECT_EQ(f->selected[0], 1);
    f->masks[1] = 1;
    ASSERT_EQ(de__atlas_select_lights(&f->world, &f->camera, 1, false, 32, f->selected), 2);
    EXPECT_EQ(f->selected[0], 1);
}
TEST_F(shadow_admission, unknown_bounds_and_custom_forward_are_conservative, admission) {
    f->masks[1] = 1;
    f->flags[1] = 0;
    ASSERT_EQ(de__atlas_select_lights(&f->world, &f->camera, 1, false, 1, f->selected), 1);
    EXPECT_EQ(f->selected[0], 1);
    ASSERT_EQ(de__atlas_select_lights(&f->world, &f->camera, 4, true, 1, f->selected), 1);
    EXPECT_EQ(f->selected[0], 1);
}
TEST_F(shadow_admission, limits_null_and_snapshot_immutability, admission) {
    admission before = *f;
    EXPECT_EQ(de__atlas_select_lights(NULL, &f->camera, 1, false, 32, f->selected), 0);
    EXPECT_EQ(de__atlas_select_lights(&f->world, NULL, 1, false, 32, f->selected), 0);
    EXPECT_EQ(de__atlas_select_lights(&f->world, &f->camera, 1, false, 0, f->selected), 0);
    EXPECT_EQ(de__atlas_select_lights(&f->world, &f->camera, 1, false, 32, NULL), 0);
    (void)de__atlas_select_lights(&f->world, &f->camera, 1, false, 100, f->selected);
    memcpy(before.selected, f->selected, sizeof f->selected);
    EXPECT_EQ(memcmp(&before, f, sizeof before), 0);
}

TEST_F(shadow_admission, moving_and_rebased_receivers_rescan, admission) {
    ASSERT_EQ(de__atlas_select_lights(&f->world, &f->camera, 1, false, 1, f->selected), 1);
    EXPECT_EQ(f->selected[0], 0);
    f->bounds[0] = f->bounds[1];
    ASSERT_EQ(de__atlas_select_lights(&f->world, &f->camera, 1, false, 1, f->selected), 1);
    EXPECT_EQ(f->selected[0], 1);
    for (u32 i = 0; i < 2; ++i) {
        f->bounds[i].min.x -= 1024;
        f->bounds[i].max.x -= 1024;
        f->transforms[i].m[9] -= 1024;
    }
    f->camera.offset.x -= 1024;
    f->world.origin.x += 1024;
    ASSERT_EQ(de__atlas_select_lights(&f->world, &f->camera, 1, false, 1, f->selected), 1);
    EXPECT_EQ(f->selected[0], 1);
    f->transforms[1].m[9] += 100;
    EXPECT_EQ(de__atlas_select_lights(&f->world, &f->camera, 1, false, 1, f->selected), 0);
}
TEST_F(shadow_admission, tangent_and_spot_cone_are_conservative, admission) {
    f->world.lights.count = 1;
    f->lights[0].kind = DE_LIGHT_SPOT;
    f->lights[0].spot_outer = .2f;
    f->bounds[0] = (de_aabb){{8, 0, 4}, {9, 1, 5}};
    ASSERT_EQ(de__atlas_select_lights(&f->world, &f->camera, 1, false, 1, f->selected), 1);
    f->bounds[0].min.x = 8.1f;
    EXPECT_EQ(de__atlas_select_lights(&f->world, &f->camera, 1, false, 1, f->selected), 0);
    f->bounds[0].min.x = NAN;
    EXPECT_EQ(de__atlas_select_lights(&f->world, &f->camera, 1, false, 1, f->selected), 1);
}
