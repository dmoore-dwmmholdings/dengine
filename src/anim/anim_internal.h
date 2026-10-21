#ifndef DE_ANIM_INTERNAL_H
#define DE_ANIM_INTERNAL_H
#include <dengine/de_anim.h>
#include <dengine/de_task.h>
#include <dengine/de_render.h>
#include "anim/formats/de_cooked_clip.h"
#include "anim/formats/de_cooked_skeleton.h"
typedef struct de__anim_asset {
    de_allocator allocator;
    size_t bytes;
    u32 kind;
    de_cooked_skeleton_view skeleton;
    de_cooked_clip_view clip;
} de__anim_asset;
typedef struct de__anim_entry {
    de_entity entity;
    u64 seen;
    u32 skeleton_handle, clip_handle;
    de_load_handle skeleton_ref, clip_ref;
    de_cooked_skeleton_view skeleton;
    de_cooked_clip_view clip;
    DeAnimator *animator;
    void *block;
    size_t bytes;
    u32 joints, mesh_joints;
    de_cooked_joint_pose *local;
    de_affine *model, *palette;
    u64 *names;
    bool ready, ended, finalized;
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
 * Register on the main thread between schedules; user storage outlives s. */
typedef void (*de__anim_stage_fn)(de_anim_system *s, void *user);
typedef struct de__anim_stage_hook {
    de__anim_stage_fn fn;
    void *user;
} de__anim_stage_hook;
#define DE__ANIM_STAGE_HOOK_CAPACITY 8u
struct de_anim_system {
    const de_allocator *alloc;
    de_asset_system *assets;
    de_task_system *tasks;
    de_renderer *renderer;
    bool renderer_explicit;
    de_world *world;
    de_component component;
    de_builtin_component_ids builtins;
    de_resource_id resource;
    de_event_type events;
    de_system sample, finalize;
    de__anim_entry *entries;
    u32 capacity;
    u32 *jobs;
    u32 job_capacity, job_count;
    u64 epoch;
    f32 dt;
    de__anim_stage_hook stages[DE_ANIM_STAGE_COUNT][DE__ANIM_STAGE_HOOK_CAPACITY];
    u32 stage_counts[DE_ANIM_STAGE_COUNT];
    bool dispatching_stages;
};
de_result de_anim__register_stage(de_anim_system *, de__anim_stage, de__anim_stage_fn, void *);
void de__anim_run_stage(de_anim_system *, de__anim_stage);
bool de__anim_sample(const de_cooked_clip_view *, const de_cooked_skeleton_view *, f32,
                     de_cooked_joint_pose *);
f32 de__anim_time(f32 time, f32 duration, f32 speed, f32 dt, bool loop, bool *ended);
void de__anim_pose(const de_cooked_skeleton_view *, const de_cooked_joint_pose *, de_affine *,
                   de_affine *);
de_result de__anim_register_assets(de_anim_system *s);
const de__anim_asset *de__anim_asset_get(de_anim_system *, u32, const char *);
#endif
