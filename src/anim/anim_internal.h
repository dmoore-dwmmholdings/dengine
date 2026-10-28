#ifndef DE_ANIM_INTERNAL_H
#define DE_ANIM_INTERNAL_H
#include <dengine/de_anim.h>
#include <dengine/de_task.h>
#include <dengine/de_render.h>
#include "anim/formats/de_cooked_clip.h"
#include "anim/formats/de_cooked_skeleton.h"
#include "anim/pose.h"
#include "anim/sprite_clip_format.h"
typedef struct de__anim_asset {
    de_allocator allocator;
    size_t bytes;
    u32 kind;
    de_cooked_skeleton_view skeleton;
    de_cooked_clip_view clip;
    de__sprite_clip_view sprite;
    u64 *sprite_prefix;
    struct de__anim_prepared_clip *prepared;
} de__anim_asset;
de_result de__sprite_asset_load(de_asset_load_ctx *ctx);
void de__sprite_asset_unload(void *object, void *user);
de_result de__aseprite_register_asset(de_anim_system *system);
/* Immutable asset-owned sampling cache. Oversized clips retain compressed sampling. */
typedef struct de__anim_prepared_track {
    u32 joint, channel, components, key_count;
    const f32 *times, *values;
} de__anim_prepared_track;
typedef struct de__anim_prepared_clip {
    size_t bytes;
    const u8 *base;
    u32 track_count;
    const de__anim_prepared_track *tracks;
} de__anim_prepared_clip;
bool de__anim_prepare_clip(const de_allocator *, const de_cooked_clip_view *,
                           de__anim_prepared_clip **);
typedef struct de__anim_clip_cursor {
    u32 *keys;
    u32 capacity;
    const u8 *clip_base;
    f32 time;
    bool valid;
    u64 binary_searches, forward_steps;
} de__anim_clip_cursor;
bool de__anim_sample_prepared(const de_cooked_clip_view *, const de__anim_prepared_clip *,
                              const de_cooked_skeleton_view *, f32, de__anim_local_pose *,
                              de__anim_clip_cursor *);
/* POD ECS snapshot state; no borrowed pointers or process-local asset handles. */
typedef struct de__anim_clock {
    u64 ticks, segment_ticks, clip_key, pose_hash;
    f64 phase, exact_time;
    f32 dt, speed, duration, output_time;
    u32 state_id; /* DENG-184 supplies state identity when state machines land. */
    bool initialized, playing;
} de__anim_clock;
typedef struct de__anim_entry {
    de_entity entity;
    struct de__anim_graph_runtime *graph_runtime;
    u32 graph_handle, graph_samples;
    u64 seen;
    u32 skeleton_handle, clip_handle;
    de_load_handle skeleton_ref, clip_ref;
    de_cooked_skeleton_view skeleton;
    de_cooked_clip_view clip;
    u32 clip_version;
    u64 clip_key;
    de__anim_clip_cursor cursor;
    DeAnimator *animator;
    void *block;
    de_pool *pose_pool;
    size_t bytes;
    u32 joints, mesh_joints;
    de__anim_local_pose local;
    de_affine *model, *palette;
    u64 *names;
    bool ready, ended, finalized;
    de__anim_clock pending_clock;
    de__anim_clock *clock;
    u64 fixed_pose_hash, pose_restore_count;
    f32 visual_time;
    u64 visual_fixed_ticks, visual_fixed_hash;
    bool clock_missing, clock_known, visual_output, visual_playing;
    de_entity sync_prepare_entity;
    bool sync_prepare_failed, graph_sync_seek;
    f32 graph_sync_input_time;
} de__anim_entry;
typedef enum de__anim_stage {
    DE_ANIM_STAGE_SAMPLE,
    DE_ANIM_STAGE_BLEND,
    DE_ANIM_STAGE_ROOT_MOTION,
    DE_ANIM_STAGE_IK,
    DE_ANIM_STAGE_FINALIZE,
    DE_ANIM_STAGE_COUNT
} de__anim_stage;
/* Stage callbacks run on the main thread between the built-in passes. They may
 * dispatch/join their own jobs over ready entries, but must not change world
 * structure (the gathered component pointers stay borrowed until finalize).
 * Iterate s->jobs[0..job_count): ready entries outside that list can hold cached
 * poses for the other timing mode and must not be changed by this pass.
 * Register on the main thread between schedules; user storage outlives s. */
typedef void (*de__anim_stage_fn)(de_anim_system *s, void *user);
typedef struct de__anim_stage_hook {
    de__anim_stage_fn fn;
    void *user;
} de__anim_stage_hook;
#define DE__ANIM_STAGE_HOOK_CAPACITY 8u
/* 16-byte through16MiB blocks cover the maximum validated cooked joint/track counts. */
#define DE__ANIM_POSE_POOL_CLASSES 21u
struct de_anim_system {
    struct de__anim_graph *graphs;
    struct de__anim_reference_cache *reference_cache;
    u64 reference_builds;
    const de_allocator *alloc;
    de_asset_system *assets;
    de_task_system *tasks;
    de_renderer *renderer;
    bool renderer_explicit;
    de_world *world;
    de_component component, clock_component, params_component;
    de_component ik_component, sprite_component;
    de_builtin_component_ids builtins;
    de_resource_id resource;
    de_event_type events;
    de_system sample, finalize, fixed;
    de__anim_entry *entries;
    u32 capacity;
    u32 *jobs;
    u32 job_capacity, job_count;
    u64 epoch, restore_count;
    f32 dt;
    de_pool *pose_pools[DE__ANIM_POSE_POOL_CLASSES];
    de__anim_stage_hook stages[DE_ANIM_STAGE_COUNT][DE__ANIM_STAGE_HOOK_CAPACITY];
    u32 stage_counts[DE_ANIM_STAGE_COUNT];
    bool dispatching_stages;
    struct de__sprite_runtime *sprites;
    bool fixed_evaluation;
    bool sync_possible;
    de_component sync_binding;
    de_query checksum_query;
    u64 *checksum_rows;
    u32 checksum_capacity;
};
de_result de__sprite_register_asset(de_anim_system *);
de_result de_anim__sprite_init(de_anim_system *, de_world *);
de_result de__anim_features_init(de_anim_system *, de_world *);
void de__sprite_shutdown(de_anim_system *);
void de__anim_prepare(de_system_ctx *, bool fixed);
de_anim_system *de__anim_for_world(de_world *);
de_result de__anim_deterministic_init(de_anim_system *, de_world *);
void de__anim_clock_step(de__anim_clock *, DeAnimator *, f32 dt, f32 duration, u64 clip_key,
                         bool seek, bool *ended);
f64 de__anim_wrap_time(f64 value, f64 period);
void de__anim_graph_clock_step(de__anim_clock *, DeAnimator *, f32 dt, u64 key, bool seek);
void de__anim_pose_deterministic(const de_cooked_skeleton_view *, const de__anim_local_pose *,
                                 de_affine *, de_affine *);
u64 de__anim_pose_checksum(const de__anim_local_pose *);
bool de__anim_fixed_pose_valid(const de_anim_system *, const de__anim_entry *);
de_result de_anim__register_stage(de_anim_system *, de__anim_stage, de__anim_stage_fn, void *);
void de__anim_run_stage(de_anim_system *, de__anim_stage);
bool de__anim_sample(const de_cooked_clip_view *, const de_cooked_skeleton_view *, f32,
                     de_cooked_joint_pose *);
bool de__anim_sample_cached(const de_cooked_clip_view *, const de_cooked_skeleton_view *, f32,
                            de_cooked_joint_pose *, de__anim_clip_cursor *);
bool de__anim_sample_pose_cached(const de_cooked_clip_view *, const de_cooked_skeleton_view *, f32,
                                 de__anim_local_pose *, de__anim_clip_cursor *);
f32 de__anim_time(f32 time, f32 duration, f32 speed, f32 dt, bool loop, bool *ended);
void de__anim_pose(const de_cooked_skeleton_view *, const de__anim_local_pose *, de_affine *,
                   de_affine *);
de_result de__anim_register_assets(de_anim_system *s);
de_pool *de__anim_pose_pool(de_anim_system *, size_t bytes);
bool de__anim_graph_prepare(de_anim_system *, de__anim_entry *);
bool de__anim_graph_evaluate(de_anim_system *, de__anim_entry *);
void de__anim_graph_runtime_release(de_anim_system *, de__anim_entry *);
void de__anim_graph_blend(de_anim_system *);
void de__anim_graph_params_install(de_system_ctx *);
de_result de_anim__graph_init(de_anim_system *, de_world *);
void de__anim_graphs_shutdown(de_anim_system *s);
de_result de__anim_register_components(de_anim_system *s, de_world *w);
const de__anim_asset *de__anim_asset_get(de_anim_system *, u32, const char *);
#endif
