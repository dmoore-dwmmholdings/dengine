#include "anim_fixture.h"
#include "cook/lib/cooker_internal.h"
#include "cook/importers/sprite_clip/sprite_clip_import.h"
#include "dpak_writer.h"
#include "de_alloc_guard.h"
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <process.h>
#define sprite_pid _getpid
#else
#include <unistd.h>
#define sprite_pid getpid
#endif
typedef struct sprite_fx {
    anim_fixture base;
    de_component sprite;
    de_anim_clip clip;
    de_load_handle load;
    char path[256];
} sprite_fx;
/* CPU-only fixture: animation needs the atlas asset identity, not a GPU image. */
static de_result sprite_test_atlas_load(de_asset_load_ctx *ctx) {
    static const u8 atlas = 0;
    de_asset_load_ctx_set_object(ctx, (void *)&atlas, sizeof atlas);
    return DE_OK;
}
static void sprite_fx_setup(sprite_fx *f) {
    anim_fixture_setup(&f->base);
    de_type_id type = de_type_register(f->base.types, &(de_type_desc){.name = "DeSprite",
                                                                      .size = sizeof(DeSprite),
                                                                      .align = DE_ALIGNOF(DeSprite),
                                                                      .version = 1,
                                                                      .is_pod = true});
    ASSERT_TRUE(type.id);
    f->sprite = de_component_register(f->base.world, type, 0);
    ASSERT_TRUE(f->sprite.id);
    ASSERT_TRUE(de_asset_type_register(
                    f->base.assets,
                    &(de_asset_type_desc){.name = "texture", .load = sprite_test_atlas_load})
                    .id);
}
static void sprite_fx_teardown(sprite_fx *f) {
    if (f->load.id)
        de_asset_release(f->base.assets, f->load);
    anim_fixture_teardown(&f->base);
    if (f->path[0])
        EXPECT_EQ(de_file_delete(f->path), DE_OK);
}
static void sprite_load_text(sprite_fx *f, const char *source, u64 size) {
    de_cooker *cooker = NULL;
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.allocator = &f->base.allocator}, &cooker), DE_OK);
    de__cooker_type type = {.name = "sprite_clip", .import = de_sprite_clip_import};
    de_asset_import_ctx ctx;
    de__import_ctx_init(&ctx, cooker, &type);
    ctx.source = source;
    ctx.source_size = size;
    ctx.source_rel = "sprite.toml";
    ctx.id_str = "sprite/clip";
    ctx.id = de_asset_id_from(ctx.id_str);
    ASSERT_EQ(de__cooker_run_import(&ctx), DE_OK);
    dpak_writer *writer = NULL;
    ASSERT_EQ(
        dpak_writer_create(
            &(dpak_writer_desc){.allocator = &f->base.allocator, .include_names = true}, &writer),
        DE_OK);
    const u8 texture = 0;
    ASSERT_EQ(dpak_writer_add(writer, de_asset_id_from("sprite/atlas").hash, "texture", &texture, 1,
                              NULL, 0, "sprite/atlas"),
              DE_OK);
    u64 atlas = de_asset_id_from("sprite/atlas").hash;
    ASSERT_EQ(dpak_writer_add(writer, ctx.id.hash, "sprite_clip", ctx.out, ctx.out_size, &atlas, 1,
                              "sprite/clip"),
              DE_OK);
    snprintf(f->path, sizeof f->path, "animtest:/sprite-%lu-%llu.dpak", (unsigned long)sprite_pid(),
             (unsigned long long)de_time_now_ns());
    ASSERT_EQ(dpak_writer_finish(writer, f->path), DE_OK);
    dpak_writer_destroy(writer);
    de__import_ctx_release(&ctx);
    de_cooker_destroy(cooker);
    ASSERT_EQ(de_asset_pack_mount(f->base.assets, f->path, 0), DE_OK);
    f->clip = de_anim_clip_asset(f->base.anim, de_asset_id_from("sprite/clip"));
    ASSERT_TRUE(f->clip.id);
    EXPECT_EQ(de_anim_clip_kind_get(f->base.anim, f->clip), DE_ANIM_CLIP_SPRITE);
    f->load = de_asset_load(f->base.assets, de_asset_id_from("sprite/clip"), DE_LOAD_NO_EVENTS);
    de_load_state state = de_asset_wait(f->base.assets, f->load, 10000000000ull);
    if (state != DE_LOAD_READY) {
        char error[512];
        (void)de_asset_last_error(f->base.assets, de_asset_id_from("sprite/clip"), error,
                                  sizeof error);
        fprintf(stderr, "sprite load: %s\n", error);
    }
    ASSERT_EQ(state, DE_LOAD_READY);
}
static void sprite_load(sprite_fx *f, u32 ms, bool markers) {
    char source[2048];
    int n = snprintf(source, sizeof source,
                     "atlas='sprite/atlas'\ndefault_ms=%u\n"
                     "grid={columns=4,rows=1,frame_size=[16,16]}\n"
                     "[[tags]]\nname='run'\nfrom=1\nto=3\n"
                     "[[tags]]\nname='reverse'\nfrom=0\nto=3\ndirection='reverse'\n"
                     "[[tags]]\nname='bounce'\nfrom=0\nto=3\ndirection='pingpong'\n",
                     ms);
    ASSERT_TRUE(n > 0);
    if (markers)
        for (u32 i = 0; i < 4; ++i) {
            int k = snprintf(source + n, sizeof source - (size_t)n,
                             "[[events]]\nframe=%u\nname='f%u'\n", i, i);
            ASSERT_TRUE(k > 0);
            n += k;
        }
    sprite_load_text(f, source, (u64)n);
}
static de_entity sprite_spawn(sprite_fx *f, const char *tag, bool loop) {
    de_entity e;
    de_component c[] = {f->base.anim->sprite_component, f->sprite};
    ASSERT_EQ(de_entity_spawn_with(f->base.world, c, 2, &e), DE_OK);
    DeSpriteAnimator *a = de_entity_get(f->base.world, e, c[0]);
    a->clip = f->clip;
    a->tag = tag ? de_name_from(tag) : (de_name){0};
    a->loop = loop;
    return e;
}
static void sprite_frame(sprite_fx *f, f32 dt) {
    ASSERT_EQ(de_world_run_schedule(f->base.world, DE_SCHEDULE_FIRST, dt, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->base.world, DE_SCHEDULE_UPDATE, dt, 0), DE_OK);
}
static u32 sprite_index(sprite_fx *f, de_entity e) {
    const DeSprite *s = de_entity_get_const(f->base.world, e, f->sprite);
    EXPECT_TRUE(s->texture.id != 0);
    return (u32)(s->uv_rect.x * 4 + .5f);
}
/* Independent expanded millisecond oracle; no cooked prefix tables or runtime helpers. */
TEST_F(sprite_anim, unequal_durations_match_millisecond_oracle_in_both_directions, sprite_fx) {
    const char *source = "atlas='sprite/atlas'\natlas_size=[64,16]\n"
                         "frames=[{rect=[0,0,16,16],ms=7},{rect=[16,0,16,16],ms=13},"
                         "{rect=[32,0,16,16],ms=29},{rect=[48,0,16,16],ms=41}]\n"
                         "[[tags]]\nname='forward'\nfrom=0\nto=3\n"
                         "[[tags]]\nname='reverse'\nfrom=0\nto=3\ndirection='reverse'\n"
                         "[[tags]]\nname='bounce'\nfrom=0\nto=3\ndirection='pingpong'\n"
                         "[[events]]\nframe=0\nname='f0'\n"
                         "[[events]]\nframe=1\nname='f1'\n"
                         "[[events]]\nframe=2\nname='f2'\n"
                         "[[events]]\nframe=3\nname='f3'\n";
    sprite_load_text(f, source, strlen(source));
    const u32 paths[3][6] = {{0, 1, 2, 3}, {3, 2, 1, 0}, {0, 1, 2, 3, 2, 1}};
    const char *tags[] = {"forward", "reverse", "bounce"};
    const u32 duration[] = {7, 13, 29, 41};
    const f32 dts[] = {.001f, .007f, .013f, .029f, .041f, .125f};
    de_name names[] = {de_name_from("f0"), de_name_from("f1"), de_name_from("f2"),
                       de_name_from("f3")};
    de_event_reader reader =
        de_event_reader_create(f->base.world, de_anim_event_type(f->base.anim));
    for (u32 direction = 0; direction < 3; ++direction) {
        u32 frame_at[132], boundary[132] = {0}, period = 0;
        for (u32 i = 0; i < (direction == 2 ? 6u : 4u); ++i) {
            boundary[period] = 1;
            for (u32 ms = 0; ms < duration[paths[direction][i]]; ++ms)
                frame_at[period++] = paths[direction][i];
        }
        for (u32 backwards = 0; backwards < 2; ++backwards) {
            de_entity e = sprite_spawn(f, tags[direction], true);
            DeSpriteAnimator *a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
            a->speed = backwards ? -3.25f : 3.25f;
            sprite_frame(f, 0);
            const de_anim_event *event = de_event_next(f->base.world, &reader);
            ASSERT_TRUE(event);
            EXPECT_EQ(event->name.hash, names[frame_at[0]].hash);
            EXPECT_FALSE(de_event_next(f->base.world, &reader));
            f64 total = 0;
            i64 previous = 0;
            for (u32 tick = 0; tick < 120; ++tick) {
                f32 dt = dts[tick % 6];
                total += (f64)dt * (backwards ? -3.25 : 3.25) * 1000;
                i64 now = (i64)total;
                sprite_frame(f, dt);
                i64 phase = now % period;
                if (phase < 0)
                    phase += period;
                EXPECT_EQ(sprite_index(f, e), frame_at[phase]);
                i64 step = backwards ? -1 : 1;
                for (i64 ms = previous; ms != now;) {
                    i64 before = ms % period;
                    if (before < 0)
                        before += period;
                    ms += step;
                    i64 after = ms % period;
                    if (after < 0)
                        after += period;
                    if ((backwards && boundary[before]) || (!backwards && boundary[after])) {
                        event = de_event_next(f->base.world, &reader);
                        ASSERT_TRUE(event);
                        EXPECT_EQ(event->entity.bits, e.bits);
                        EXPECT_EQ(event->name.hash, names[frame_at[after]].hash);
                    }
                }
                EXPECT_FALSE(de_event_next(f->base.world, &reader));
                previous = now;
            }
            de_entity_despawn(f->base.world, e);
            sprite_frame(f, 0);
        }
    }
}
TEST_F(sprite_anim, forward_reverse_pingpong_tag_restart_and_pause, sprite_fx) {
    sprite_load(f, 100, false);
    de_entity e = sprite_spawn(f, "run", true);
    for (u32 i = 0; i < 12; ++i) {
        sprite_frame(f, i ? .1f : 0);
        EXPECT_EQ(sprite_index(f, e), 1 + i % 3);
    }
    DeSpriteAnimator *a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
    a->tag = de_name_from("bounce");
    const u32 path[] = {0, 1, 2, 3, 2, 1, 0, 1};
    for (u32 i = 0; i < 8; ++i) {
        sprite_frame(f, i ? .1f : 0);
        EXPECT_EQ(sprite_index(f, e), path[i]);
    }
    a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
    a->playing = false;
    sprite_frame(f, 5);
    EXPECT_EQ(sprite_index(f, e), 1u);
    a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
    a->tag = de_name_from("reverse");
    a->playing = true;
    for (u32 i = 0; i < 5; ++i) {
        sprite_frame(f, i ? .1f : 0);
        EXPECT_EQ(sprite_index(f, e), 3 - i % 4);
    }
    a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
    a->tag = (de_name){0};
    a->speed = -1;
    sprite_frame(f, 0);
    EXPECT_EQ(sprite_index(f, e), 0u);
    sprite_frame(f, .1f);
    EXPECT_EQ(sprite_index(f, e), 3u);
}
TEST_F(sprite_anim, crossed_markers_and_nonloop_end_are_not_skipped_or_duplicated, sprite_fx) {
    sprite_load(f, 16, true);
    de_entity e = sprite_spawn(f, NULL, true);
    de_event_reader reader =
        de_event_reader_create(f->base.world, de_anim_event_type(f->base.anim));
    sprite_frame(f, 0);
    const de_anim_event *event = de_event_next(f->base.world, &reader);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->name.hash, de_name_from("f0").hash);
    ASSERT_FALSE(de_event_next(f->base.world, &reader));
    DeSpriteAnimator *a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
    a->speed = 10;
    sprite_frame(f, .016f);
    for (u32 i = 1; i <= 10; ++i) {
        event = de_event_next(f->base.world, &reader);
        ASSERT_TRUE(event);
        char name[8];
        snprintf(name, sizeof name, "f%u", i % 4);
        EXPECT_EQ(event->kind, DE_ANIM_EV_MARKER);
        EXPECT_EQ(event->name.hash, de_name_from(name).hash);
    }
    EXPECT_FALSE(de_event_next(f->base.world, &reader));
    a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
    a->time = 0;
    a->loop = false;
    a->speed = 1;
    sprite_frame(f, 0);
    while (de_event_next(f->base.world, &reader)) {
    }
    sprite_frame(f, .2f);
    for (u32 i = 1; i <= 3; ++i) {
        event = de_event_next(f->base.world, &reader);
        ASSERT_TRUE(event);
        EXPECT_EQ(event->kind, DE_ANIM_EV_MARKER);
    }
    event = de_event_next(f->base.world, &reader);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, DE_ANIM_EV_CLIP_END);
    EXPECT_FALSE(de_event_next(f->base.world, &reader));
    EXPECT_EQ(sprite_index(f, e), 3u);
    sprite_frame(f, 1);
    EXPECT_FALSE(de_event_next(f->base.world, &reader));
}
typedef struct sprite_observer {
    u32 rows;
} sprite_observer;
static void observe_chunk(const de_query_chunk *c, void *u) {
    ((sprite_observer *)u)->rows += c->count;
}
static void observe(de_system_ctx *ctx) {
    sprite_observer *o = ctx->user;
    o->rows = 0;
    de_query_each(ctx->world, ctx->queries[0], observe_chunk, o);
}
TEST_F(sprite_anim, sprite_changed_query_only_observes_actual_frame_changes, sprite_fx) {
    sprite_load(f, 100, false);
    (void)sprite_spawn(f, NULL, true);
    sprite_observer o = {0};
    de_query_term t = {f->sprite, DE_READ};
    de_query_filter filter = {f->sprite, DE_CHANGED};
    de_query_desc q = {.terms = &t, .term_count = 1, .filters = &filter, .filter_count = 1};
    const char *after[] = {"anim.sprite"};
    de_system observer =
        de_system_add(f->base.world, &(de_system_desc){.name = "sprite.observer",
                                                       .schedule = DE_SCHEDULE_UPDATE,
                                                       .fn = observe,
                                                       .user = &o,
                                                       .queries = &q,
                                                       .query_count = 1,
                                                       .after = after,
                                                       .after_count = 1,
                                                       .flags = DE_SYSTEM_MAIN_THREAD});
    ASSERT_TRUE(observer.id);
    sprite_frame(f, 0);
    EXPECT_EQ(o.rows, 1u);
    sprite_frame(f, .05f);
    EXPECT_EQ(o.rows, 0u);
    sprite_frame(f, .05f);
    EXPECT_EQ(o.rows, 1u);
    sprite_frame(f, .01f);
    EXPECT_EQ(o.rows, 0u);
    /* Crossing a full cycle still emits markers, but unchanged final UVs need no write. */
    sprite_frame(f, .4f);
    EXPECT_EQ(o.rows, 0u);
    de_system_remove(f->base.world, observer);
}
TEST_F(sprite_anim, paused_seek_invalid_tag_recovery_and_single_frame_clamp, sprite_fx) {
    const char *source = "atlas='sprite/atlas'\n"
                         "grid={columns=4,rows=1,frame_size=[16,16]}\n"
                         "[[tags]]\nname='still'\nfrom=2\nto=2\ndirection='pingpong'\n"
                         "[[events]]\nframe=2\nname='still'\n";
    sprite_load_text(f, source, strlen(source));
    de_entity e = sprite_spawn(f, NULL, true);
    DeSpriteAnimator *a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
    a->playing = false;
    a->time = .25f;
    sprite_frame(f, 1);
    EXPECT_EQ(sprite_index(f, e), 2u);
    a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
    EXPECT_FLOAT_EQ(a->time, .25f, 1e-6f);
    a->tag = de_name_from("missing");
    sprite_frame(f, 1);
    EXPECT_EQ(sprite_index(f, e), 2u);
    a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
    a->tag = de_name_from("still");
    a->playing = true;
    a->loop = false;
    de_event_reader reader =
        de_event_reader_create(f->base.world, de_anim_event_type(f->base.anim));
    sprite_frame(f, 0);
    EXPECT_EQ(sprite_index(f, e), 2u);
    const de_anim_event *event = de_event_next(f->base.world, &reader);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, DE_ANIM_EV_MARKER);
    EXPECT_EQ(event->name.hash, de_name_from("still").hash);
    EXPECT_FALSE(de_event_next(f->base.world, &reader));
    sprite_frame(f, .2f);
    event = de_event_next(f->base.world, &reader);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, DE_ANIM_EV_CLIP_END);
    EXPECT_FALSE(de_event_next(f->base.world, &reader));
    a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
    EXPECT_FALSE(a->playing);
    EXPECT_EQ(sprite_index(f, e), 2u);
    /* Attempting to resume an already-ended clip holds without a second end event. */
    a->playing = true;
    sprite_frame(f, .2f);
    EXPECT_FALSE(de_event_next(f->base.world, &reader));
    a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
    EXPECT_FALSE(a->playing);
}
TEST(sprite_anim, serial_and_parallel_frames_and_ordered_markers_match) {
    u64 traces[2][60] = {{0}};
    for (u32 mode = 0; mode < 2; ++mode) {
        sprite_fx f = {0};
        sprite_fx_setup(&f);
        sprite_load(&f, 16, true);
        de_anim_set_task_system(f.base.anim, mode ? f.base.tasks : NULL);
        de_entity entities[128];
        for (u32 i = 0; i < 128; ++i) {
            entities[i] = sprite_spawn(&f, i % 2 ? "bounce" : "reverse", true);
            DeSpriteAnimator *a =
                de_entity_get(f.base.world, entities[i], f.base.anim->sprite_component);
            a->speed = i % 3 ? 10.0f : -10.0f;
        }
        de_event_reader reader =
            de_event_reader_create(f.base.world, de_anim_event_type(f.base.anim));
        for (u32 tick = 0; tick < 60; ++tick) {
            sprite_frame(&f, tick ? .016f : 0);
            u64 hash = 14695981039346656037ull;
            for (u32 i = 0; i < 128; ++i)
                hash = (hash ^ sprite_index(&f, entities[i])) * 1099511628211ull;
            const de_anim_event *event;
            u32 previous = 0, count = 0;
            while ((event = de_event_next(f.base.world, &reader))) {
                u32 index = de_entity_index(event->entity);
                EXPECT_TRUE(index >= previous);
                previous = index;
                hash = (hash ^ index) * 1099511628211ull;
                hash = (hash ^ event->name.hash) * 1099511628211ull;
                ++count;
            }
            EXPECT_EQ(count, tick ? 1280u : 128u);
            traces[mode][tick] = hash;
        }
        sprite_fx_teardown(&f);
    }
    for (u32 tick = 0; tick < 60; ++tick)
        EXPECT_EQ(traces[0][tick], traces[1][tick]);
}
TEST_F(sprite_anim, ten_thousand_sprites_have_zero_warmed_frame_allocations, sprite_fx) {
    sprite_load(f, 16, false);
    de_entity first = {0}, last = {0};
    for (u32 i = 0; i < 10000; ++i) {
        last = sprite_spawn(f, i % 2 ? "run" : "bounce", true);
        if (!i)
            first = last;
    }
    for (u32 i = 0; i < 32; ++i)
        sprite_frame(f, .016f);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    u64 begin = de_time_now_ns();
    for (u32 i = 0; i < 100; ++i)
        sprite_frame(f, .016f);
    u64 elapsed = de_time_now_ns() - begin;
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    EXPECT_EQ(sprite_index(f, first), 0u);
    EXPECT_EQ(sprite_index(f, last), 1u);
    fprintf(stderr, "10000 sprites, whole Update + First: %.3f ms/frame; M1 gate requires M1\n",
            (f64)elapsed / 100000000.0);
    /* Keep the original workload and compare unrounded elapsed nanoseconds.
     * The qualifying workflow independently verifies native M1 hardware. */
#if defined(_WIN32)
    char *qualify_m1 = NULL;
    size_t qualify_bytes = 0;
    ASSERT_EQ(_dupenv_s(&qualify_m1, &qualify_bytes, "DE_SPRITE_M1_ACCEPTANCE"), 0);
#else
    const char *qualify_m1 = getenv("DE_SPRITE_M1_ACCEPTANCE");
#endif
    if (qualify_m1 && strcmp(qualify_m1, "1") == 0)
        EXPECT_LE(elapsed, UINT64_C(100) * UINT64_C(500000));
#if defined(_WIN32)
    free(qualify_m1);
#endif
}
TEST_F(sprite_anim, removed_and_recycled_entities_release_state_and_restart, sprite_fx) {
    sprite_load(f, 100, false);
    de_entity e = sprite_spawn(f, "run", true);
    sprite_frame(f, .1f);
    EXPECT_EQ(sprite_index(f, e), 2u);
    de_entity_remove(f->base.world, e, f->sprite);
    sprite_frame(f, .1f);
    (void)de_entity_add(f->base.world, e, f->sprite);
    DeSpriteAnimator *a = de_entity_get(f->base.world, e, f->base.anim->sprite_component);
    a->time = 0;
    sprite_frame(f, 0);
    EXPECT_EQ(sprite_index(f, e), 1u);
    de_entity_despawn(f->base.world, e);
    sprite_frame(f, 0);
    e = sprite_spawn(f, "bounce", true);
    sprite_frame(f, 0);
    EXPECT_EQ(sprite_index(f, e), 0u);
}
TEST_F(sprite_anim, clip_kind_rejects_wrong_types_and_identifies_both_clip_formats, sprite_fx) {
    sprite_load(f, 100, false);
    load_assets(&f->base, 3);
    EXPECT_EQ(de_anim_clip_kind_get(f->base.anim, f->base.clip), DE_ANIM_CLIP_SKELETAL);
    EXPECT_EQ(de_anim_clip_kind_get(f->base.anim, f->clip), DE_ANIM_CLIP_SPRITE);
    EXPECT_EQ(de_anim_clip_kind_get(f->base.anim, (de_anim_clip){0}), DE_ANIM_CLIP_NONE);
    EXPECT_EQ(de_anim_clip_kind_get(NULL, f->clip), DE_ANIM_CLIP_NONE);
    EXPECT_EQ(de_anim_clip_asset(f->base.anim, de_asset_id_from("sprite/atlas")).id, 0u);
    EXPECT_EQ(de_anim_clip_kind_get(f->base.anim, (de_anim_clip){f->base.skeleton.id}),
              DE_ANIM_CLIP_NONE);
}
#if DE_HAS_COOKER
TEST_F(sprite_anim, source_asset_pipeline_cooks_sprite_and_loads_atlas_dependency, sprite_fx) {
    const char *source = "atlas='source/atlas'\n"
                         "grid={columns=8,rows=4,count=30,frame_size=[16,16]}\n"
                         "[[tags]]\nname='run'\nfrom=8\nto=15\n";
    const char *manifest = "[assets.\"source/clip\"]\n"
                           "type='sprite_clip'\nsource='runtime.sprite.toml'\n"
                           "[assets.\"source/atlas\"]\n"
                           "type='texture'\nsource='runtime-atlas.bin'\n";
    const u8 atlas = 0;
    ASSERT_EQ(de_file_write_all_atomic("animtest:/runtime.sprite.toml", source, strlen(source)),
              DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("animtest:/runtime-atlas.bin", &atlas, sizeof atlas), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("animtest:/runtime-assets.toml", manifest, strlen(manifest)),
              DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->base.assets, "animtest:/runtime-assets.toml"), DE_OK);
    f->clip = de_anim_clip_asset(f->base.anim, de_asset_id_from("source/clip"));
    EXPECT_EQ(de_anim_clip_kind_get(f->base.anim, f->clip), DE_ANIM_CLIP_SPRITE);
    f->load = de_asset_load(f->base.assets, de_asset_id_from("source/clip"), DE_LOAD_NO_EVENTS);
    ASSERT_EQ(de_asset_wait(f->base.assets, f->load, 10000000000ull), DE_LOAD_READY);
    de_entity e = sprite_spawn(f, "run", true);
    for (u32 i = 0; i < 24; ++i) {
        sprite_frame(f, i ? .1f : 0);
        u32 expected = 8 + i % 8;
        const DeSprite *s = de_entity_get_const(f->base.world, e, f->sprite);
        EXPECT_FLOAT_EQ(s->uv_rect.x, (f32)(expected % 8) / 8, 1e-6f);
        EXPECT_FLOAT_EQ(s->uv_rect.y, (f32)(expected / 8) / 4, 1e-6f);
        EXPECT_TRUE(s->texture.id != 0);
    }
    ASSERT_EQ(de_file_delete("animtest:/runtime.sprite.toml"), DE_OK);
    ASSERT_EQ(de_file_delete("animtest:/runtime-atlas.bin"), DE_OK);
    ASSERT_EQ(de_file_delete("animtest:/runtime-assets.toml"), DE_OK);
}
#endif
