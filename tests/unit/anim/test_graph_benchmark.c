#include "anim_fixture.h"
#include "anim/graph_internal.h"
#include "de_alloc_guard.h"

/* Every joint has two nonconstant T/R/S keys. Nine different assets exercise
 * eight locomotion leaves and a separate additive input, not handle aliases. */
static anim_blob benchmark_clip(u32 variant) {
    const u32 joints = 60, tracks = 80, times = tracks + joints * 3 * 16;
    const u32 data = times + 8, size = data + joints * 68;
    anim_blob seed = clip_blob(joints), blob = {calloc(1, size), size};
    ASSERT_TRUE(blob.data);
    memcpy(blob.data, seed.data, 80);
    de_cooked_put_u32(blob.data + 8, 0);
    de_cooked_put_u32(blob.data + 16, joints * 3);
    de_cooked_put_u32(blob.data + 24, 0);
    de_cooked_put_u32(blob.data + 36, tracks);
    de_cooked_put_u32(blob.data + 40, times);
    de_cooked_put_u32(blob.data + 44, data);
    de_cooked_put_u32(blob.data + 48, size - data);
    de_cooked_put_u32(blob.data + 52, size);
    de_cooked_put_f32(blob.data + times + 4, 1);
    for (u32 joint = 0; joint < joints; ++joint) {
        for (u32 channel = 0; channel < 3; ++channel) {
            u8 *track = blob.data + tracks + (joint * 3 + channel) * 16;
            put_u16(track, (u16)joint);
            track[2] = (u8)channel;
            track[3] = (u8)(channel == 0   ? DE_COOKED_CLIP_ENC_QUANT16
                            : channel == 1 ? DE_COOKED_CLIP_ENC_SMALLEST3
                                           : DE_COOKED_CLIP_ENC_RAW);
            track[4] = (u8)(channel == 0 ? 3 : channel == 1 ? 4 : 1);
            track[5] = (u8)((joint == 0 ? DE_COOKED_CLIP_TRACK_ROOT : 0) |
                            (channel == 2 ? DE_COOKED_CLIP_TRACK_UNIFORM : 0));
            de_cooked_put_u32(track + 8, 2);
            de_cooked_put_u32(track + 12, joint * 68 + (channel == 0 ? 0 : channel == 1 ? 40 : 56));
        }
        u8 *values = blob.data + data + joint * 68;
        de_cooked_put_f32(values, (f32)variant * .01f);
        de_cooked_put_f32(values + 4, joint ? 1.f : 0.f);
        de_cooked_put_f32(values + 12, 1 + (f32)variant * .05f);
        de_cooked_put_f32(values + 16, .15f);
        de_cooked_put_f32(values + 20, .25f);
        put_u16(values + 26, 1);
        for (u32 axis = 0; axis < 3; ++axis)
            put_u16(values + 34 + axis * 2, 65535);
        memcpy(values + 40, seed.data + de_canim_get_u32(seed.data + 44) + 40, 16);
        put_u16(values + 58, 1);
        de_cooked_put_f32(values + 60, 1);
        de_cooked_put_f32(values + 64, 1.025f + (f32)variant * .002f);
    }
    free(seed.data);
    de_cooked_clip_view view;
    ASSERT_TRUE(de_cooked_clip_parse(blob.data, blob.bytes, &view, NULL));
    ASSERT_EQ(view.track_count, 180u);
    for (u32 i = 0; i < view.track_count; ++i)
        ASSERT_NE(de_cooked_clip_get_track(&view, i).encoding, (u32)DE_COOKED_CLIP_ENC_CONST);
    return blob;
}
static de_anim_graph benchmark_graph(anim_fixture *f) {
    load_assets(f, 60);
    char manifest[4096] = "[assets.\"test/skeleton\"]\ntype='skeleton'\nsource='skeleton.dskl'\n"
                          "[assets.\"test/clip\"]\ntype='anim_clip'\nsource='clip.dclp'\n";
    size_t used = strlen(manifest);
    for (u32 i = 0; i < 9; ++i) {
        char path[64];
        snprintf(path, sizeof path, "animtest:/blend-%u.dclp", i);
        anim_blob blob = benchmark_clip(i);
        ASSERT_EQ(de_file_write_all_atomic(path, blob.data, blob.bytes), DE_OK);
        free(blob.data);
        int n = snprintf(manifest + used, sizeof manifest - used,
                         "[assets.\"test/blend-%u\"]\ntype='anim_clip'\nsource='blend-%u.dclp'\n",
                         i, i);
        ASSERT_TRUE(n > 0 && (size_t)n < sizeof manifest - used);
        used += (size_t)n;
    }
    ASSERT_EQ(de_file_write_all_atomic("animtest:/assets.toml", manifest, used), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->assets, "animtest:/assets.toml"), DE_OK);
    de_load_handle loads[9];
    de_anim_node_desc nodes[11] = {0};
    for (u32 i = 0; i < 9; ++i) {
        char id[32];
        snprintf(id, sizeof id, "test/blend-%u", i);
        loads[i] = de_asset_load(f->assets, de_asset_id_from(id), 0);
        ASSERT_EQ(de_asset_wait(f->assets, loads[i], 10000000000ull), DE_LOAD_READY);
        de_anim_clip clip = de_anim_clip_asset(f->anim, de_asset_id_from(id));
        ASSERT_TRUE(clip.id);
        nodes[i] = (de_anim_node_desc){
            .kind = DE_ANIM_NODE_CLIP,
            .data.clip = {.clip = clip, .speed = .75f + (f32)i * .0625f, .loop = true}};
    }
    de_anim_param_desc params[] = {{.name = de_name_from("x"), .float_default = .35f},
                                   {.name = de_name_from("y"), .float_default = .47f}};
    const de_vec2 positions[] = {{0, 0}, {1, 0}, {2, 0}, {2, 1}, {2, 2}, {1, 2}, {0, 2}, {0, 1}};
    de_anim_blend_point_desc points[8];
    for (u32 i = 0; i < 8; ++i)
        points[i] = (de_anim_blend_point_desc){positions[i], i};
    nodes[9] = (de_anim_node_desc){.kind = DE_ANIM_NODE_BLEND2D,
                                   .data.blend = {.x_param = params[0].name,
                                                  .y_param = params[1].name,
                                                  .points = points,
                                                  .point_count = 8}};
    de_anim_layer_desc layer = {
        .node = 8, .mode = DE_ANIM_LAYER_ADDITIVE, .weight = .25f, .root_weight = 1};
    nodes[10] = (de_anim_node_desc){.kind = DE_ANIM_NODE_LAYER,
                                    .data.layer = {.base = 9, .layer_count = 1}};
    de_anim_graph graph = {0};
    ASSERT_EQ(de_anim_graph_create(f->anim,
                                   &(de_anim_graph_desc){.params = params,
                                                         .param_count = 2,
                                                         .nodes = nodes,
                                                         .node_count = 11,
                                                         .output_node = 10,
                                                         .layers = &layer,
                                                         .layer_count = 1},
                                   &graph),
              DE_OK);
    for (u32 i = 0; i < 9; ++i)
        de_asset_release(f->assets, loads[i]);
    return graph;
}
static void benchmark_inputs(anim_fixture *f, const de_entity *actors, u32 frame) {
    const de_vec2 positions[] = {{.35f, .47f},  {.7f, .43f},  {1.23f, .63f}, {1.57f, 1.11f},
                                 {1.1f, 1.62f}, {.6f, 1.38f}, {.42f, 1.02f}, {1.62f, .35f}};
    for (u32 i = 0; i < 500; ++i) {
        de_vec2 position = positions[(i + frame) % 8];
        de_anim_set_float(f->world, actors[i], de_name_from("x"), position.x);
        de_anim_set_float(f->world, actors[i], de_name_from("y"), position.y);
    }
}
static int compare_ns(const void *a, const void *b) {
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}
TEST_F(anim_graph_benchmark, five_hundred_eight_clip_2d_additive_zero_alloc, anim_fixture) {
    ASSERT_TRUE(f->tasks);
    ASSERT_EQ(de_task_worker_count(f->tasks), 4u);
    de_anim_graph graph = benchmark_graph(f);
    de_entity actors[500];
    for (u32 i = 0; i < 500; ++i) {
        actors[i] = spawn_animator(f);
        ASSERT_EQ(de_anim_graph_bind(f->world, actors[i], graph), DE_OK);
        DeAnimator *actor = de_entity_get(f->world, actors[i], f->anim->component);
        actor->playing = actor->loop = true;
        actor->speed = 1;
        actor->time = (f32)(i % 17) / 64;
    }
    u64 warm[8], frames[1000], sorted[1000];
    for (u32 frame = 0; frame < 8; ++frame) {
        u64 begin = de_time_now_ns();
        benchmark_inputs(f, actors, frame);
        anim_frame(f, 1.f / 60);
        warm[frame] = de_time_now_ns() - begin;
    }
    for (u32 i = 0; i < 500; ++i) {
        de__anim_entry *entry = &f->anim->entries[de_entity_index(actors[i])];
        ASSERT_TRUE(entry->ready && entry->finalized);
        ASSERT_EQ(entry->joints, 60u);
        ASSERT_EQ(entry->graph_samples, 4u);
    }
    u64 before = de__anim_pose_checksum(&f->anim->entries[de_entity_index(actors[0])].local);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    u64 start = de_time_now_ns();
    for (u32 frame = 0; frame < 1000; ++frame) {
        u64 begin = de_time_now_ns();
        benchmark_inputs(f, actors, frame);
        anim_frame(f, 1.f / 60);
        frames[frame] = de_time_now_ns() - begin;
    }
    u64 elapsed = de_time_now_ns() - start;
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    EXPECT_NE(before, de__anim_pose_checksum(&f->anim->entries[de_entity_index(actors[0])].local));
    for (u32 i = 0; i < 500; ++i) {
        de__anim_entry *entry = &f->anim->entries[de_entity_index(actors[i])];
        EXPECT_TRUE(entry->ready && entry->finalized);
        EXPECT_EQ(entry->graph_samples, 4u);
    }
    de__anim_reference_stats references = de__anim_graph_reference_stats(f->anim);
    EXPECT_EQ(references.entries, 1u);
    EXPECT_EQ(references.builds, 1u);
    EXPECT_EQ(references.users, 500u);
    EXPECT_EQ(references.owners, 1u);
    memcpy(sorted, frames, sizeof frames);
    qsort(sorted, 1000, sizeof *sorted, compare_ns);
    printf("500x60, 8-clip2D + additive (9 assets), 8warm/1000measured: mean %.4fms p95 %.4fms p99 "
           "%.4fms max %.4fms; allocations %llu\n",
           (double)elapsed / 1e9, (double)sorted[949] / 1e6, (double)sorted[989] / 1e6,
           (double)sorted[999] / 1e6, (unsigned long long)de_alloc_guard_offences(&guard));
    char report[32768];
    int n = snprintf(
        report, sizeof report,
        "{\"actors\":500,\"joints\":60,\"frame_workers\":4,\"space_clips\":8,\"additive_clips\":1,"
        "\"warmup\":8,"
        "\"measured\":1000,\"elapsed_ns\":%llu,\"allocation_offences\":%llu,\"warmup_ns\":[",
        (unsigned long long)elapsed, (unsigned long long)de_alloc_guard_offences(&guard));
    ASSERT_TRUE(n > 0 && (size_t)n < sizeof report);
    size_t used = (size_t)n;
    for (u32 i = 0; i < 1008; ++i) {
        if (i == 8) {
            const char *separator = "],\"frames_ns\":[";
            ASSERT_TRUE(used + strlen(separator) < sizeof report);
            memcpy(report + used, separator, strlen(separator));
            used += strlen(separator);
        }
        n = snprintf(report + used, sizeof report - used, "%s%llu", i == 0 || i == 8 ? "" : ",",
                     (unsigned long long)(i < 8 ? warm[i] : frames[i - 8]));
        ASSERT_TRUE(n > 0 && (size_t)n < sizeof report - used);
        used += (size_t)n;
    }
    ASSERT_TRUE(used + 3 < sizeof report);
    memcpy(report + used, "]}\n", 3);
    used += 3;
    ASSERT_EQ(de_file_write_all_atomic("animtest:/blend-benchmark.json", report, used), DE_OK);
#if defined(_WIN32)
    char *qualify = NULL;
    size_t bytes = 0;
    ASSERT_EQ(_dupenv_s(&qualify, &bytes, "DE_ANIM_BLEND_M1_ACCEPTANCE"), 0);
#else
    const char *qualify = getenv("DE_ANIM_BLEND_M1_ACCEPTANCE");
#endif
    if (qualify && strcmp(qualify, "1") == 0)
        EXPECT_LE(elapsed, UINT64_C(3000000) * 1000);
#if defined(_WIN32)
    free(qualify);
#endif
}
