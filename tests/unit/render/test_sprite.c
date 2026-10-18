#include "de_test.h"
#include "de_alloc_guard.h"
#include "test_render_world.h"
#include "render/sprite/sprite_internal.h"
#include <string.h>

static de_entity spawn_sprite(rwt *f, de_transform t) {
    const de_component cs[2] = {f->bi.transform, f->ids.sprite};
    de_entity e = {0};
    ASSERT_EQ(de_entity_spawn_with(f->w, cs, 2, &e), DE_OK);
    *(DeTransform *)de_entity_get(f->w, e, f->bi.transform) = t;
    return e;
}
static void rwt_setup(rwt *f) {
    memset(f, 0, sizeof *f);
    rwt_start(f, 0, false);
}
static void rwt_teardown(rwt *f) {
    rwt_stop(f);
}

TEST_F(sprite, extract_fields, rwt) {
    de_transform t = de_transform_identity();
    t.position = (de_dvec3){10, 20, 30};
    de_entity e = spawn_sprite(f, t);
    DeSprite *s = de_entity_get(f->w, e, f->ids.sprite);
    s->texture = (de_texture){12};
    s->size = (de_vec2){4, 6};
    s->pivot = (de_vec2){.25f, .5f};
    s->layer = -4;
    s->order = 7;
    s->flip_x = true;
    s->thickness = .3f;
    ASSERT_EQ(rwt_frame(f), DE_OK);
    const de__render_world_frame *fr = rwt_last(f);
    u32 at = rwt_inst_slot(fr, e);
    ASSERT_NE(at, DE__RW_NONE);
    ASSERT_EQ(fr->instances.sprite[at].texture.id, 12u);
    ASSERT_EQ(fr->instances.sprite[at].layer, -4);
    ASSERT_EQ(fr->instances.sprite[at].order, 7);
    ASSERT_TRUE(fr->instances.flags[at] & DE__RW_INST_SPRITE);
    ASSERT_FLOAT_EQ(fr->instances.transform[at].m[9], 9, .00001);
    ASSERT_FLOAT_EQ(fr->instances.transform[at].m[11], 27, .00001);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    ASSERT_EQ(rwt_last(f)->dirty_instance_count, 0u);
    rwt_move(f, e, (de_dvec3){11, 20, 30});
    ASSERT_EQ(rwt_frame(f), DE_OK);
    fr = rwt_last(f);
    ASSERT_FLOAT_EQ(fr->instances.transform[at].m[9], 10, .00001);
    ASSERT_FLOAT_EQ(fr->instances.prev_transform[at].m[9], 9, .00001);
}

TEST_F(sprite, plane_orientation, rwt) {
    DeSprite s = {.size = {2, 4}, .pivot = {.5f, .25f}};
    de_affine a = de__sprite_local_transform(&s, DE_PLANE_SIDE_XZ);
    ASSERT_FLOAT_EQ(a.m[0], 2, .00001);
    ASSERT_FLOAT_EQ(a.m[5], 4, .00001);
    ASSERT_FLOAT_EQ(a.m[7], -1, .00001);
    ASSERT_FLOAT_EQ(a.m[11], -1, .00001);
    a = de__sprite_local_transform(&s, DE_PLANE_TOP_XY);
    ASSERT_FLOAT_EQ(a.m[4], 4, .00001);
    ASSERT_FLOAT_EQ(a.m[8], 1, .00001);
    ASSERT_FLOAT_EQ(a.m[10], -1, .00001);
    de_entity e = spawn_sprite(f, de_transform_identity());
    ASSERT_EQ(rwt_frame(f), DE_OK);
    de_render_set_2d_plane(f->r, DE_PLANE_TOP_XY);
    ASSERT_EQ(rwt_frame(f), DE_OK);
    const de__render_world_frame *fr = rwt_last(f);
    u32 at = rwt_inst_slot(fr, e);
    ASSERT_FLOAT_EQ(fr->instances.transform[at].m[8], 1, .00001);
}

static u32 fake_texture(void *u, de_texture h) {
    (void)u;
    return h.id + 100;
}
TEST_F(sprite, material_maps_atlas_alpha, rwt) {
    DeSprite s = {.texture = {1},
                  .normal_tex = {2},
                  .rough_metal_tex = {3},
                  .emissive_tex = {4},
                  .height_tex = {5},
                  .uv_rect = {.25f, .125f, .5f, .25f},
                  .flip_x = true,
                  .color = {1, .5f, .25f, .75f}};
    de__gs_material m;
    de__sprite_pack_material(&s, &m, fake_texture, NULL);
    ASSERT_EQ(m.model, DE__GS_MATERIAL_MODEL_SPRITE);
    ASSERT_EQ(m.alpha_mode, 1u);
    ASSERT_FLOAT_EQ(m.pbr.v[3], .5f, .00001);
    ASSERT_EQ(de__material_params_texture(&m, 0), 101u);
    ASSERT_EQ(de__material_params_texture(&m, 2), 102u);
    ASSERT_EQ(de__material_params_texture(&m, 4), 104u);
    ASSERT_EQ(de__material_params_texture(&m, 18), 105u);
    ASSERT_FLOAT_EQ(m.subsurface.v[0], .75f, .00001);
    ASSERT_FLOAT_EQ(m.subsurface.v[2], -.5f, .00001);
    ASSERT_FLOAT_EQ(m.emissive.v[0], 1, .00001);
    s.translucent = true;
    de__sprite_pack_material(&s, &m, NULL, NULL);
    ASSERT_TRUE(m.features & DE__MATERIAL_FEATURE_FORWARD);
    ASSERT_EQ(m.alpha_mode, 2u);
}

TEST_F(sprite, ten_thousand_no_alloc, rwt) {
    for (u32 i = 0; i < 10000; ++i)
        (void)spawn_sprite(f, de_transform_identity());
    for (u32 i = 0; i < 8; ++i)
        ASSERT_EQ(rwt_frame(f), DE_OK);
    ASSERT_EQ(rwt_last(f)->instances.live, 10000u);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    de_result result = rwt_frame(f);
    de_alloc_guard_end(&guard);
    ASSERT_EQ(result, DE_OK);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
}
