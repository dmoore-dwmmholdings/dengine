#define _CRT_SECURE_NO_WARNINGS
#include "anim_fixture.h"
#include "asset/hot_reload_internal.h"
#include "asset/slots_internal.h"
#include "cook/lib/de_cooker.h"
#include "dpak_writer.h"
#include "render/formats/de_cooked_texture.h"
#include "render/sprite/sprite_internal.h"

/* The CPU texture loader validates actual DTEX bytes and records their hash.
 * It is not a GPU upload or a rendered-image qualification. */
typedef struct ase_test_texture {
    de_allocator allocator;
    u64 hash;
} ase_test_texture;
static de_result ase_texture_load(de_asset_load_ctx *ctx) {
    u64 size;
    const void *bytes = de_asset_load_ctx_data(ctx, &size);
    de_cooked_texture_view view;
    if (!de_cooked_texture_parse(bytes, size, &view, NULL))
        return DE_ERR_PARSE;
    const de_allocator *allocator = de_asset_load_ctx_user(ctx);
    ase_test_texture *texture = DE_NEW(allocator, ase_test_texture, DE_MEM_ASSET);
    if (!texture)
        return DE_ERR_OUT_OF_MEMORY;
    texture->allocator = *allocator;
    texture->hash = de_hash64(bytes, (size_t)size, 0);
    de_asset_load_ctx_set_object(ctx, texture, sizeof *texture);
    return DE_OK;
}
static void ase_texture_unload(void *object, void *user) {
    (void)user;
    ase_test_texture *texture = object;
    de_allocator allocator = texture->allocator;
    DE_FREE(&allocator, texture, sizeof *texture);
}
typedef struct ase_runtime_fx {
    anim_fixture base;
    de_component sprite;
    de_anim_clip clip, layer;
    de_texture texture;
    de_load_handle load, layer_load;
    u8 source[8192];
    size_t source_bytes;
    bool packed;
} ase_runtime_fx;
static void ase_runtime_fx_setup(ase_runtime_fx *f) {
    anim_fixture_setup_options(&f->base, true);
    de__asset_reload_set_watching(f->base.assets, false);
    de_type_id type = de_type_register(f->base.types, &(de_type_desc){.name = "DeSprite",
                                                                      .size = sizeof(DeSprite),
                                                                      .align = DE_ALIGNOF(DeSprite),
                                                                      .version = 1,
                                                                      .is_pod = true});
    ASSERT_TRUE(type.id);
    f->sprite = de_component_register(f->base.world, type, 0);
    ASSERT_TRUE(f->sprite.id);
    ASSERT_TRUE(
        de_asset_type_register(f->base.assets, &(de_asset_type_desc){.name = "texture",
                                                                     .load = ase_texture_load,
                                                                     .unload = ase_texture_unload,
                                                                     .user = &f->base.allocator})
            .id);
    FILE *file = fopen(DE_ASEPRITE_DATA_DIR "/playback.aseprite", "rb");
    ASSERT_TRUE(file != NULL);
    f->source_bytes = fread(f->source, 1, sizeof f->source, file);
    bool ok = feof(file) && !ferror(file);
    fclose(file);
    ASSERT_TRUE(ok);
}
static void ase_runtime_fx_teardown(ase_runtime_fx *f) {
    if (f->load.id)
        de_asset_release(f->base.assets, f->load);
    if (f->layer_load.id)
        de_asset_release(f->base.assets, f->layer_load);
    anim_fixture_teardown(&f->base);
    if (f->packed)
        EXPECT_EQ(de_file_delete("animtest:/aseprite.dpak"), DE_OK);
    else if (f->load.id) {
        EXPECT_EQ(de_file_delete("animtest:/aseprite-assets.toml"), DE_OK);
        EXPECT_EQ(de_file_delete("animtest:/aseprite-source.aseprite"), DE_OK);
    }
}
static void packed_load(ase_runtime_fx *f) {
    de_cooker *cooker = NULL;
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.allocator = &f->base.allocator,
                                                 .verify_determinism = DE_COOK_VERIFY_ON},
                               &cooker),
              DE_OK);
    de_asset_type_desc descriptor;
    ASSERT_TRUE(de_cooker_builtin_importer("aseprite", &descriptor));
    ASSERT_EQ(de_cooker_register_type(cooker, &descriptor), DE_OK);
    ASSERT_EQ(de_fs_mount("asefixtures", DE_ASEPRITE_DATA_DIR, 0, 0), DE_OK);
    de__manifest_kv keep = {.key = "layers",
                            .value = {.type = DE_ASSET_SETTING_STRING, .u.s = "keep"}};
    de__manifest_asset asset = {.id = "hero/sprites",
                                .type = "aseprite",
                                .source = "playback.aseprite",
                                .file = "asefixtures:/assets.toml",
                                .settings = &keep,
                                .setting_count = 1};
    de_cook_result result = {0};
    ASSERT_EQ(de_cooker_cook(cooker,
                             &(de_cook_input){.asset = &asset, .asset_root = "asefixtures:/"},
                             &result),
              DE_OK);
    dpak_writer *writer = NULL;
    ASSERT_EQ(
        dpak_writer_create(
            &(dpak_writer_desc){.allocator = &f->base.allocator, .include_names = true}, &writer),
        DE_OK);
    ASSERT_EQ(result.dependency_count, 1u);
    u64 dependency = result.dependencies[0].hash;
    ASSERT_EQ(dpak_writer_add(writer, de_asset_id_from(asset.id).hash, "aseprite", result.data,
                              result.size, &dependency, 1, asset.id),
              DE_OK);
    for (u32 i = 0; i < result.subasset_count; ++i) {
        const de_cook_subasset *sub = &result.subassets[i];
        char name[256];
        int n = snprintf(name, sizeof name, "%s#%s", asset.id, sub->name);
        ASSERT_TRUE(n > 0 && (size_t)n < sizeof name);
        ASSERT_EQ(
            dpak_writer_add(writer, sub->id.hash, sub->type, sub->data, sub->size, NULL, 0, name),
            DE_OK);
    }
    ASSERT_EQ(dpak_writer_finish(writer, "animtest:/aseprite.dpak"), DE_OK);
    dpak_writer_destroy(writer);
    de_cook_result_free(&result);
    de_cooker_destroy(cooker);
    ASSERT_EQ(de_fs_unmount("asefixtures", DE_ASEPRITE_DATA_DIR), DE_OK);
    ASSERT_EQ(de_asset_pack_mount(f->base.assets, "animtest:/aseprite.dpak", 0), DE_OK);
    f->packed = true;
}
static void source_load(ase_runtime_fx *f) {
    ASSERT_EQ(
        de_file_write_all_atomic("animtest:/aseprite-source.aseprite", f->source, f->source_bytes),
        DE_OK);
    const char *manifest = "[defaults.aseprite]\nlayers='keep'\npixel_art=true\n"
                           "pixels_per_meter=20\n[assets.\"hero/sprites\"]\ntype='aseprite'\n"
                           "source='aseprite-source.aseprite'\n";
    ASSERT_EQ(
        de_file_write_all_atomic("animtest:/aseprite-assets.toml", manifest, strlen(manifest)),
        DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->base.assets, "animtest:/aseprite-assets.toml"), DE_OK);
}
static void finish_load(ase_runtime_fx *f) {
    u32 reflected =
        de__asset_handle_of(f->base.assets, de_asset_id_from("hero/sprites"), "sprite_clip");
    ASSERT_TRUE(reflected);
    f->clip = de_anim_clip_asset(f->base.anim, de_asset_id_from("hero/sprites"));
    ASSERT_TRUE(f->clip.id);
    EXPECT_EQ(f->clip.id, reflected);
    EXPECT_EQ(de_anim_clip_kind_get(f->base.anim, f->clip), DE_ANIM_CLIP_SPRITE);
    EXPECT_EQ(de_anim_sprite_frame_count(f->base.anim, f->clip), 0u);
    f->load = de_asset_load(f->base.assets, de_asset_id_from("hero/sprites"), DE_LOAD_NO_EVENTS);
    ASSERT_EQ(de_asset_wait(f->base.assets, f->load, 10000000000ull), DE_LOAD_READY);
    f->layer = de_anim_clip_asset(f->base.anim, de_asset_id_from("hero/sprites#layer/Body"));
    ASSERT_TRUE(f->layer.id);
    f->layer_load = de_asset_load(f->base.assets, de_asset_id_from("hero/sprites#layer/Body"),
                                  DE_LOAD_NO_EVENTS);
    ASSERT_EQ(de_asset_wait(f->base.assets, f->layer_load, 10000000000ull), DE_LOAD_READY);
    f->texture = de_asset_texture(f->base.assets, de_asset_id_from("hero/sprites#atlas"));
    ASSERT_TRUE(f->texture.id);
    EXPECT_EQ(de_asset_status_of(f->base.assets, de_asset_id_from("hero/sprites#atlas")),
              DE_LOAD_READY);
    EXPECT_EQ(de_anim_sprite_frame_count(f->base.anim, f->clip), 4u);
    EXPECT_EQ(de_anim_sprite_frame_count(f->base.anim, f->layer), 4u);
    EXPECT_EQ(de__asset_handle_of(f->base.assets, de_asset_id_from("hero/sprites"), "sprite_clip"),
              reflected);
    EXPECT_EQ(de__asset_handle_of(f->base.assets, de_asset_id_from("hero/sprites"), "aseprite"),
              reflected);
    EXPECT_EQ(de__asset_handle_of(f->base.assets, de_asset_id_from("hero/sprites"), "anim_clip"),
              0u);
    EXPECT_EQ(de__asset_handle_of(f->base.assets, de_asset_id_from("hero/sprites"), "texture"), 0u);
    EXPECT_EQ(de__asset_handle_of(f->base.assets, de_asset_id_from("hero/sprites#layer/Body"),
                                  "sprite_clip"),
              f->layer.id);
    EXPECT_EQ(de__asset_handle_of(f->base.assets, de_asset_id_from("hero/sprites#layer/Body"),
                                  "aseprite"),
              0u);
    EXPECT_EQ(
        de__asset_handle_of(f->base.assets, de_asset_id_from("hero/sprites#atlas"), "sprite_clip"),
        0u);
}
static de_entity spawn(ase_runtime_fx *f, const char *tag) {
    de_component components[] = {f->base.anim->sprite_component, f->sprite};
    de_entity e;
    ASSERT_EQ(de_entity_spawn_with(f->base.world, components, 2, &e), DE_OK);
    DeSpriteAnimator *a = de_entity_get(f->base.world, e, components[0]);
    a->clip = f->clip;
    a->tag = de_name_from(tag);
    a->speed = 1;
    a->playing = a->loop = true;
    return e;
}
static void tick(ase_runtime_fx *f, f32 dt) {
    ASSERT_EQ(de_world_run_schedule(f->base.world, DE_SCHEDULE_FIRST, dt, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->base.world, DE_SCHEDULE_UPDATE, dt, 0), DE_OK);
}
static void expect_frame(ase_runtime_fx *f, de_entity e, u32 index) {
    de_sprite_frame_info frame;
    ASSERT_TRUE(de_anim_sprite_frame(f->base.anim, f->clip, index, &frame));
    const DeSprite *sprite = de_entity_get_const(f->base.world, e, f->sprite);
    EXPECT_EQ(sprite->texture.id, f->texture.id);
    ASSERT_MEMEQ(&sprite->uv_rect, &frame.uv_rect, sizeof frame.uv_rect);
    ASSERT_MEMEQ(&sprite->size, &frame.size, sizeof frame.size);
    ASSERT_MEMEQ(&sprite->pivot, &frame.pivot, sizeof frame.pivot);
}
TEST_F(aseprite_runtime, real_pack_loads_alias_layers_geometry_and_keyed_slices, ase_runtime_fx) {
    packed_load(f);
    /* A packed layer independently loads its atlas without loading the primary clip. */
    f->layer = de_anim_clip_asset(f->base.anim, de_asset_id_from("hero/sprites#layer/Body"));
    ASSERT_TRUE(f->layer.id);
    f->layer_load = de_asset_load(f->base.assets, de_asset_id_from("hero/sprites#layer/Body"),
                                  DE_LOAD_NO_EVENTS);
    ASSERT_EQ(de_asset_wait(f->base.assets, f->layer_load, 10000000000ull), DE_LOAD_READY);
    EXPECT_EQ(de_anim_sprite_frame_count(f->base.anim, f->layer), 4u);
    EXPECT_EQ(de_asset_status_of(f->base.assets, de_asset_id_from("hero/sprites#atlas")),
              DE_LOAD_READY);
    de_asset_release(f->base.assets, f->layer_load);
    f->layer_load = (de_load_handle){0};
    finish_load(f);
    de_sprite_frame_info frame;
    ASSERT_TRUE(de_anim_sprite_frame(f->base.anim, f->clip, 0, &frame));
    EXPECT_TRUE(frame.has_geometry);
    EXPECT_EQ(frame.canvas_width, 8u);
    EXPECT_EQ(frame.trim[0], 1u);
    EXPECT_FLOAT_EQ(frame.size.x, .01f, 0);
    de_sprite_slice_info slice;
    ASSERT_TRUE(de_anim_sprite_slice(f->base.anim, f->clip, 0, de_name_from("origin"), &slice));
    EXPECT_EQ(slice.origin.x, 1);
    EXPECT_EQ(slice.pivot.y, 2);
    EXPECT_TRUE(slice.has_pivot && slice.has_nine_slice);
    EXPECT_FLOAT_EQ(slice.nine_slice.w, 1, 0);
    ASSERT_TRUE(de_anim_sprite_slice(f->base.anim, f->layer, 2, de_name_from("origin"), &slice));
    EXPECT_EQ(slice.origin.y, -1);
    EXPECT_EQ(slice.center_origin.y, 2);
    de_sprite_slice_info copy = slice;
    EXPECT_FALSE(de_anim_sprite_slice(f->base.anim, f->clip, 3, de_name_from("origin"), &slice));
    ASSERT_MEMEQ(&copy, &slice, sizeof slice);
    EXPECT_FALSE(de_anim_sprite_slice(f->base.anim, f->clip, 0, de_name_from("late"), &slice));
    ASSERT_TRUE(de_anim_sprite_slice(f->base.anim, f->clip, 1, de_name_from("late"), &slice));
    EXPECT_EQ(slice.center_origin.x, -1);
    EXPECT_FALSE(de_anim_sprite_frame(f->base.anim, f->clip, 4, &frame));
    EXPECT_FALSE(de_anim_sprite_frame(NULL, f->clip, 0, &frame));
    de_entity e = spawn(f, "walk");
    tick(f, 0);
    expect_frame(f, e, 0);
}
/* Expanded authored traversals, independent of cooked prefix sums and runtime phase math. */
static u32 oracle(u32 *frames, u32 direction, u32 repeats) {
    const u32 duration[] = {7, 13, 29, 41};
    bool reverse = direction == 1 || direction == 3;
    u32 count = 0;
    i32 frame = reverse ? 3 : 0, step = reverse ? -1 : 1;
    for (u32 pass = 0; pass < repeats; ++pass) {
        if (pass) {
            if (direction >= 2) {
                step = -step;
                frame += step;
            } else
                frame = reverse ? 3 : 0;
        }
        for (;;) {
            for (u32 ms = 0; ms < duration[frame]; ++ms)
                frames[count++] = (u32)frame;
            if (frame == (step > 0 ? 3 : 0))
                break;
            frame += step;
        }
    }
    return count;
}
TEST_F(aseprite_runtime, all_directions_finite_traversals_and_negative_speed_match_oracle,
       ase_runtime_fx) {
    packed_load(f);
    finish_load(f);
    de_event_reader reader =
        de_event_reader_create(f->base.world, de_anim_event_type(f->base.anim));
    for (u32 direction = 0; direction < 4; ++direction)
        for (u32 repeats = 1; repeats <= 4; ++repeats)
            for (u32 backwards = 0; backwards < 2; ++backwards) {
                u32 frames[512];
                u32 total = oracle(frames, direction, repeats);
                char tag[32];
                snprintf(tag, sizeof tag, "d%u-r%u", direction, repeats);
                de_entity e = spawn(f, tag);
                DeSpriteAnimator *a =
                    de_entity_get(f->base.world, e, f->base.anim->sprite_component);
                if (backwards) {
                    a->speed = -1;
                    a->time = (f32)(total + 10) / 1000;
                }
                u32 ends = 0;
                for (u32 ms = 0; ms <= total; ++ms) {
                    tick(f, ms ? .001f : 0);
                    u32 at = backwards ? total - ms : ms;
                    expect_frame(f, e, frames[at < total ? at : total - 1]);
                    const de_anim_event *event;
                    while ((event = de_event_next(f->base.world, &reader)) != NULL)
                        if (event->entity.bits == e.bits && event->kind == DE_ANIM_EV_CLIP_END)
                            ++ends;
                }
                a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
                EXPECT_FALSE(a->playing); /* Authored repeats constrain even loop=true. */
                EXPECT_EQ(ends, 1u);
                a->playing = true;
                tick(f, .1f);
                EXPECT_FALSE(de_event_next(f->base.world, &reader));
                de_entity_despawn(f->base.world, e);
                tick(f, 0);
            }
}
TEST_F(aseprite_runtime, unlimited_directions_and_component_pingpong_match_expanded_cycles,
       ase_runtime_fx) {
    packed_load(f);
    finish_load(f);
    const u32 paths[4][6] = {{0, 1, 2, 3}, {3, 2, 1, 0}, {0, 1, 2, 3, 2, 1}, {3, 2, 1, 0, 1, 2}};
    const u32 durations[] = {7, 13, 29, 41};
    for (u32 direction = 0; direction < 4; ++direction)
        for (u32 override = 0; override <= (direction < 2 ? 1u : 0u); ++override)
            for (u32 backwards = 0; backwards < 2; ++backwards) {
                u32 frames[256], period = 0, path = override ? direction + 2 : direction;
                for (u32 i = 0; i < (path < 2 ? 4u : 6u); ++i)
                    for (u32 ms = 0; ms < durations[paths[path][i]]; ++ms)
                        frames[period++] = paths[path][i];
                char tag[32];
                snprintf(tag, sizeof tag, "d%u-r0", direction);
                de_entity e = spawn(f, tag);
                DeSpriteAnimator *a =
                    de_entity_get(f->base.world, e, f->base.anim->sprite_component);
                a->speed = backwards ? -1 : 1;
                a->ping_pong = override != 0;
                for (u32 ms = 0; ms < period * 3u; ++ms) {
                    tick(f, ms ? .001f : 0);
                    u32 at = backwards ? (period - ms % period) % period : ms % period;
                    expect_frame(f, e, frames[at]);
                }
                a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
                EXPECT_TRUE(a->playing);
                de_entity_despawn(f->base.world, e);
                tick(f, 0);
            }
}
TEST_F(aseprite_runtime, named_walk_pingpong_and_single_frame_repeat_duration, ase_runtime_fx) {
    packed_load(f);
    finish_load(f);
    de_entity e = spawn(f, "walk");
    DeSpriteAnimator *a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
    a->ping_pong = true;
    const u32 path[] = {0, 1, 2, 3, 2, 1};
    const u32 durations[] = {7, 13, 29, 41};
    tick(f, 0);
    for (u32 cycle = 0; cycle < 2; ++cycle)
        for (u32 step = 0; step < sizeof path / sizeof path[0]; ++step)
            for (u32 ms = 0; ms < durations[path[step]]; ++ms) {
                expect_frame(f, e, path[step]);
                tick(f, .001f);
            }
    de_entity_despawn(f->base.world, e);
    tick(f, 0);
    for (u32 direction = 0; direction < 5; ++direction) {
        char name[32];
        if (direction == 4)
            snprintf(name, sizeof name, "single-loop");
        else
            snprintf(name, sizeof name, "single%u", direction);
        e = spawn(f, name);
        a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
        a->ping_pong = true;
        tick(f, 0);
        for (u32 ms = 0; ms < 39; ++ms) {
            expect_frame(f, e, 1);
            a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
            EXPECT_TRUE(a->playing);
            tick(f, .001f);
        }
        expect_frame(f, e, 1);
        a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
        EXPECT_EQ(a->playing, direction == 4);
        de_entity_despawn(f->base.world, e);
        tick(f, 0);
    }
}
TEST_F(aseprite_runtime, trimmed_geometry_keeps_top_origin_source_pixels_at_their_world_position,
       ase_runtime_fx) {
    source_load(f); /* 20 source pixels per meter. */
    finish_load(f);
    de_entity e = spawn(f, "walk");
    const f32 source_origin[][2] = {{3, 3}, {3, 3}, {3, 2}, {4, 3}};
    const f32 seek[] = {0, .008f, .021f, .050f};
    for (u32 i = 0; i < 4; ++i) {
        DeSpriteAnimator *a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
        a->playing = false;
        a->time = seek[i];
        tick(f, 0);
        const DeSprite *sprite = de_entity_get_const(f->base.world, e, f->sprite);
        de_affine local = de__sprite_local_transform(sprite, DE_PLANE_TOP_XY);
        /* Unit quad (0,1) samples the source's top-left; (1,0) its bottom-right. */
        const f32 top_x = ((f32)i + 1 - source_origin[i][0]) / 20;
        const f32 top_y = (source_origin[i][1] - 2) / 20;
        EXPECT_FLOAT_EQ(local.m[9], top_x, 0.000001f);
        EXPECT_FLOAT_EQ(local.m[10] + local.m[4], top_y, 0.000001f);
        EXPECT_FLOAT_EQ(local.m[9] + local.m[0], top_x + .05f, 0.000001f);
        EXPECT_FLOAT_EQ(local.m[10], top_y - .05f, 0.000001f);
        de_sprite_frame_info frame;
        ASSERT_TRUE(de_anim_sprite_frame(f->base.anim, f->clip, i, &frame));
        EXPECT_FLOAT_EQ(frame.source_pivot.y, source_origin[i][1] / 6, 0);
    }
}
TEST_F(aseprite_runtime,
       source_recook_swaps_clip_layer_and_atlas_behind_handles_and_retains_failure,
       ase_runtime_fx) {
    source_load(f);
    finish_load(f);
    de_sprite_frame_info old;
    ASSERT_TRUE(de_anim_sprite_frame(f->base.anim, f->clip, 0, &old));
    EXPECT_FLOAT_EQ(old.size.x, .05f, 0);
    u32 handles[] = {f->clip.id, f->layer.id, f->texture.id};
    u32 versions[3];
    for (u32 i = 0; i < 3; ++i)
        versions[i] = de__slot_get(f->base.assets, handles[i])->version;
    const ase_test_texture *texture = de__slot_object(de__slot_get(f->base.assets, f->texture.id));
    u64 old_hash = texture->hash;
    f->source[136] = 17; /* First frame's duration in the actual binary source. */
    bool changed = false;
    const u8 red[] = {40, 0, 0, 255};
    for (size_t i = 128; i + 4 <= f->source_bytes; ++i)
        if (!memcmp(f->source + i, red, sizeof red)) {
            f->source[i] = 90;
            ASSERT_TRUE(i >= 18);
            f->source[i - 18] = 2; /* Raw cel's x changes from1 to2; its trim/pivot must reload. */
            f->source[i - 17] = 0;
            changed = true;
            break;
        }
    ASSERT_TRUE(changed);
    ASSERT_EQ(
        de_file_write_all_atomic("animtest:/aseprite-source.aseprite", f->source, f->source_bytes),
        DE_OK);
    de__asset_reload_on_change(f->base.assets, "animtest:/aseprite-source.aseprite",
                               DE_WATCH_MODIFIED, false);
    u64 deadline = de_time_now_ns() + 10000000000ull;
    for (;;) {
        de_asset_system_update(f->base.assets, NULL);
        bool ready = true;
        for (u32 i = 0; i < 3; ++i)
            ready = ready && de__slot_get(f->base.assets, handles[i])->version > versions[i];
        if (ready)
            break;
        ASSERT_TRUE(de_time_now_ns() < deadline);
        de_sleep_ns(1000000);
    }
    EXPECT_EQ(de_anim_clip_asset(f->base.anim, de_asset_id_from("hero/sprites")).id, handles[0]);
    EXPECT_EQ(de_anim_clip_asset(f->base.anim, de_asset_id_from("hero/sprites#layer/Body")).id,
              handles[1]);
    EXPECT_EQ(de_asset_texture(f->base.assets, de_asset_id_from("hero/sprites#atlas")).id,
              handles[2]);
    de_sprite_frame_info fresh;
    ASSERT_TRUE(de_anim_sprite_frame(f->base.anim, f->clip, 0, &fresh));
    EXPECT_FLOAT_EQ(fresh.duration, .017f, 0);
    EXPECT_FLOAT_EQ(old.duration, .007f, 0);
    EXPECT_EQ(fresh.trim[0], 2u);
    EXPECT_EQ(old.trim[0], 1u);
    de_entity e = spawn(f, "walk");
    tick(f, 0);
    expect_frame(f, e, 0);
    texture = de__slot_object(de__slot_get(f->base.assets, f->texture.id));
    EXPECT_NE(texture->hash, old_hash);
    for (u32 i = 0; i < 3; ++i)
        versions[i] = de__slot_get(f->base.assets, handles[i])->version;
    ASSERT_EQ(de_file_write_all_atomic("animtest:/aseprite-source.aseprite", "bad", 3), DE_OK);
    de__asset_reload_on_change(f->base.assets, "animtest:/aseprite-source.aseprite",
                               DE_WATCH_MODIFIED, false);
    deadline = de_time_now_ns() + 10000000000ull;
    const de_asset_id ids[] = {de_asset_id_from("hero/sprites"),
                               de_asset_id_from("hero/sprites#layer/Body"),
                               de_asset_id_from("hero/sprites#atlas")};
    for (;;) {
        de_asset_system_update(f->base.assets, NULL);
        bool failed = true;
        for (u32 i = 0; i < 3; ++i)
            failed = failed && de_asset_last_error(f->base.assets, ids[i], NULL, 0) != DE_OK;
        if (failed)
            break;
        ASSERT_TRUE(de_time_now_ns() < deadline);
        de_sleep_ns(1000000);
    }
    for (u32 i = 0; i < 3; ++i)
        EXPECT_EQ(de__slot_get(f->base.assets, handles[i])->version, versions[i]);
    ASSERT_TRUE(de_anim_sprite_frame(f->base.anim, f->clip, 0, &fresh));
    EXPECT_FLOAT_EQ(fresh.duration, .017f, 0);
}
