#include "anim_internal.h"
#include "asset/slots_internal.h"
#include <dengine/de_reflect.h>
#include <string.h>
typedef struct DeAnimSystemRef {
    de_anim_system *system;
} DeAnimSystemRef;
_Static_assert((size_t)DE_COOKED_SKEL_MAX_JOINTS *
                           (sizeof(de_quat) + 2 * sizeof(de_vec3) + 2 * sizeof(de_affine) +
                            sizeof(u64) + 3 * sizeof(u32)) +
                       64 <=
                   ((size_t)16 << (DE__ANIM_POSE_POOL_CLASSES - 1)),
               "pose pools must cover every validated cooked skeleton/clip layout");
static void release_entry(de_anim_system *s, de__anim_entry *entry) {
    if (entry->block)
        de_pool_free(entry->pose_pool, entry->block);
    if (s->assets) {
        if (entry->skeleton_ref.id)
            de_asset_release(s->assets, entry->skeleton_ref);
        if (entry->clip_ref.id)
            de_asset_release(s->assets, entry->clip_ref);
    }
    memset(entry, 0, sizeof *entry);
}
static bool reserve(de_anim_system *s, u32 index) {
    if (index < s->capacity)
        return true;
    u32 capacity = s->capacity ? s->capacity : 64;
    while (capacity <= index) {
        if (capacity > UINT32_MAX / 2)
            return false;
        capacity *= 2;
    }
    de__anim_entry *entries =
        DE_ALLOC(s->alloc, (size_t)capacity * sizeof *entries, 16, DE_MEM_GENERAL);
    u32 *jobs = DE_ALLOC(s->alloc, (size_t)capacity * sizeof *jobs, 16, DE_MEM_GENERAL);
    if (!entries || !jobs) {
        if (entries)
            DE_FREE(s->alloc, entries, (size_t)capacity * sizeof *entries);
        if (jobs)
            DE_FREE(s->alloc, jobs, (size_t)capacity * sizeof *jobs);
        return false;
    }
    memset(entries, 0, (size_t)capacity * sizeof *entries);
    if (s->entries) {
        memcpy(entries, s->entries, (size_t)s->capacity * sizeof *entries);
        DE_FREE(s->alloc, s->entries, (size_t)s->capacity * sizeof *entries);
    }
    if (s->jobs) {
        memcpy(jobs, s->jobs, (size_t)s->job_count * sizeof *jobs);
        DE_FREE(s->alloc, s->jobs, (size_t)s->job_capacity * sizeof *jobs);
    }
    s->entries = entries;
    s->jobs = jobs;
    s->capacity = s->job_capacity = capacity;
    for (u32 i = 0; i < capacity; ++i)
        if (entries[i].clock_missing)
            entries[i].clock = &entries[i].pending_clock;
    return true;
}
static void retain(de_anim_system *s, u32 handle, de_load_handle *out) {
    const de__slot *slot = s->assets ? de__slot_get(s->assets, handle) : NULL;
    *out = slot ? de_asset_load(s->assets, slot->id, DE_LOAD_NO_EVENTS) : (de_load_handle){0};
}
static de_pool *pose_pool(de_anim_system *s, size_t bytes) {
    size_t size = 16;
    u32 bucket = 0;
    while (size < bytes && bucket + 1 < DE__ANIM_POSE_POOL_CLASSES) {
        size *= 2;
        ++bucket;
    }
    if (size < bytes)
        return NULL;
    if (!s->pose_pools[bucket] && de_pool_create(&(de_pool_desc){.block_size = size,
                                                                 .block_align = 16,
                                                                 .backing = s->alloc,
                                                                 .tag = DE_MEM_GENERAL},
                                                 &s->pose_pools[bucket]) != DE_OK)
        return NULL;
    return s->pose_pools[bucket];
}
static bool buffers(de_anim_system *s, de__anim_entry *e, const de_cooked_skeleton_view *skel,
                    u32 tracks) {
    if (e->block && e->joints == skel->joint_count && e->mesh_joints == skel->mesh_joint_count &&
        e->cursor.capacity == tracks)
        return true;
    size_t rotations = (size_t)skel->joint_count * sizeof(de_quat);
    size_t vectors = (size_t)skel->joint_count * sizeof(de_vec3);
    size_t translations_offset = (rotations + 15) & ~(size_t)15;
    size_t scales_offset = (translations_offset + vectors + 15) & ~(size_t)15;
    size_t local = scales_offset + vectors;
    size_t models = (size_t)skel->joint_count * sizeof(de_affine);
    size_t palette = (size_t)skel->mesh_joint_count * sizeof(de_affine);
    size_t names = (size_t)skel->joint_count * sizeof(u64);
    size_t model_offset = (local + 15) & ~(size_t)15, palette_offset = model_offset + models;
    size_t names_offset = (palette_offset + palette + 7) & ~(size_t)7;
    size_t cursor_offset = names_offset + names,
           total = cursor_offset + (size_t)tracks * sizeof(u32);
    de_pool *pool = pose_pool(s, total);
    void *block = pool ? de_pool_alloc(pool) : NULL;
    if (!block)
        return false;
    if (e->block)
        de_pool_free(e->pose_pool, e->block);
    memset(block, 0, total);
    e->block = block;
    e->pose_pool = pool;
    e->bytes = total ? total : 16;
    e->joints = skel->joint_count;
    e->mesh_joints = skel->mesh_joint_count;
    e->finalized = false;
    e->local = (de__anim_local_pose){.count = skel->joint_count,
                                     .rotations = block,
                                     .translations = (de_vec3 *)((u8 *)block + translations_offset),
                                     .scales = (de_vec3 *)((u8 *)block + scales_offset),
                                     .root_rotation = {0, 0, 0, 1}};
    e->model = (de_affine *)((u8 *)block + model_offset);
    e->palette = (de_affine *)((u8 *)block + palette_offset);
    e->names = (u64 *)((u8 *)block + names_offset);
    e->cursor =
        (de__anim_clip_cursor){.keys = (u32 *)((u8 *)block + cursor_offset), .capacity = tracks};
    return true;
}
static void gather(const de_query_chunk *chunk, void *user) {
    de_anim_system *s = user;
    DeAnimator *animators = chunk->columns[0];
    de__anim_clock *clocks = chunk->columns[1];
    for (u32 i = 0; i < chunk->count; ++i) {
        de_entity entity = chunk->entities[i];
        u32 index = de_entity_index(entity);
        if (!reserve(s, index))
            continue;
        de__anim_entry *e = &s->entries[index];
        DeAnimator *a = &animators[i];
        if (e->entity.bits != entity.bits || e->skeleton_handle != a->skeleton.id ||
            e->clip_handle != a->clip.id) {
            /* Asset-cache replacement must not discard a same-actor timing
             * handoff. Otherwise a clip switch after a visual frame rebases
             * the fixed clock from that visual frame's advanced time. */
            bool handoff = e->entity.bits == entity.bits && e->visual_output;
            f32 visual_time = e->visual_time;
            bool visual_playing = e->visual_playing;
            u64 visual_ticks = e->visual_fixed_ticks, visual_hash = e->visual_fixed_hash;
            release_entry(s, e);
            if (handoff) {
                e->visual_output = true;
                e->visual_time = visual_time;
                e->visual_playing = visual_playing;
                e->visual_fixed_ticks = visual_ticks;
                e->visual_fixed_hash = visual_hash;
            }
            e->entity = entity;
            e->skeleton_handle = a->skeleton.id;
            e->clip_handle = a->clip.id;
            retain(s, a->skeleton.id, &e->skeleton_ref);
            retain(s, a->clip.id, &e->clip_ref);
        }
        e->seen = s->epoch;
        e->animator = a;
        e->clock_missing = clocks == NULL;
        if (clocks)
            e->clock = &clocks[i];
        else {
            /* Only a fixed evaluation consumes/installs pending clock state.
             * Visual-only actors need no per-frame clock clear. */
            if (s->fixed_evaluation && a->deterministic)
                memset(&e->pending_clock, 0, sizeof e->pending_clock);
            e->clock = &e->pending_clock;
        }
        if (a->deterministic != s->fixed_evaluation)
            continue;
        e->ready = false;
        e->finalized = false;
        if (a->graph.id)
            continue;
        const de__anim_asset *skel = de__anim_asset_get(s, a->skeleton.id, "skeleton");
        const de__anim_asset *clip = de__anim_asset_get(s, a->clip.id, "anim_clip");
        if (!skel || !clip || skel->skeleton.joint_count != clip->clip.joint_count ||
            !buffers(s, e, &skel->skeleton, clip->clip.track_count))
            continue;
        const de__slot *slot = de__slot_get(s->assets, a->clip.id);
        if (slot->version != e->clip_version)
            e->cursor.valid = false;
        e->clip_version = slot->version;
        e->clip_key = slot->id.hash;
        e->skeleton = skel->skeleton;
        e->clip = clip->clip;
        e->ready = true;
        for (u32 j = 0; j < e->joints; ++j)
            e->names[j] = de_cooked_skeleton_name_hash(&e->skeleton, j);
    }
}
static void sample_entry(de_anim_system *s, de__anim_entry *e, DeAnimator *a) {
    if (a->playing) {
        bool ended;
        a->time = de__anim_time(a->time, e->clip.duration, a->speed, s->dt, a->loop, &ended);
        if (ended) {
            a->playing = false;
            e->ended = true;
        }
    }
    if (!de__anim_sample_pose_cached(&e->clip, &e->skeleton, a->time, &e->local, &e->cursor))
        e->ready = false;
    e->visual_output = true;
    e->visual_time = a->time;
    e->visual_playing = a->playing;
    e->visual_fixed_ticks = e->clock->ticks;
    e->visual_fixed_hash = e->clock->pose_hash;
    e->clock_known = false;
}
static void sample_range(u32 begin, u32 end, void *user) {
    de_anim_system *s = user;
    for (u32 i = begin; i < end; ++i) {
        de__anim_entry *e = &s->entries[s->jobs[i]];
        sample_entry(s, e, e->animator);
    }
}
static void sample_chunk(const de_query_chunk *chunk, void *user) {
    de_anim_system *s = user;
    DeAnimator *animators = chunk->columns[0];
    for (u32 row = 0; row < chunk->count; ++row) {
        de_entity entity = chunk->entities[row];
        u32 index = de_entity_index(entity);
        if (index >= s->capacity)
            continue;
        de__anim_entry *e = &s->entries[index];
        if (!animators[row].deterministic && e->ready && e->seen == s->epoch &&
            e->entity.bits == entity.bits)
            sample_entry(s, e, &animators[row]);
    }
}
void de__anim_prepare(de_system_ctx *ctx, bool fixed) {
    de_anim_system *s = ctx->user;
    s->fixed_evaluation = fixed;
    s->epoch++;
    s->dt = ctx->dt;
    s->job_count = 0;
    de_query_each(ctx->world, ctx->queries[0], gather, s);
    /* Canonical publication and callback order, independent of archetype layout. */
    for (u32 i = 0; i < s->capacity; ++i) {
        de__anim_entry *e = &s->entries[i];
        if (e->entity.bits && e->seen != s->epoch) {
            release_entry(s, e);
            continue;
        }
        if (e->seen == s->epoch && e->ready && e->animator->deterministic == fixed)
            s->jobs[s->job_count++] = i;
    }
}
static void sample(de_system_ctx *ctx) {
    de_anim_system *s = ctx->user;
    de__anim_prepare(ctx, false);
    if (s->tasks && s->tasks == de_world_task_system(ctx->world))
        de_query_par_each(ctx->world, ctx->queries[0], 32, sample_chunk, s);
    else if (s->tasks)
        /* Retain S19's explicit separate task-system binding when it differs
         * from the world's scheduler; normal host wiring uses par_each above. */
        de_task_parallel_for(s->tasks, s->job_count, 32, sample_range, s);
    else
        sample_range(0, s->job_count, s);
    for (de__anim_stage stage = DE_ANIM_STAGE_SAMPLE; stage < DE_ANIM_STAGE_FINALIZE; ++stage)
        de__anim_run_stage(s, stage);
}
static void finalize_range(u32 begin, u32 end, void *user) {
    de_anim_system *s = user;
    for (u32 i = begin; i < end; ++i) {
        de__anim_entry *e = &s->entries[s->jobs[i]];
        if (!e->ready || !de_entity_alive(s->world, e->entity))
            continue;
        de__anim_pose(&e->skeleton, &e->local, e->model, e->palette);
        e->finalized = true;
    }
}
static void finalize(de_system_ctx *ctx) {
    de_anim_system *s = ctx->user;
    /* Pose arithmetic writes disjoint entry-owned buffers. Keep publication,
     * events and stage callbacks on the main thread in the gathered order. */
    if (s->tasks)
        de_task_parallel_for(s->tasks, s->job_count, 32, finalize_range, s);
    else
        finalize_range(0, s->job_count, s);
    for (u32 i = 0; i < s->capacity; ++i) {
        de__anim_entry *e = &s->entries[i];
        if (!e->ready || !de_entity_alive(s->world, e->entity) ||
            (e->animator->deterministic && !de__anim_fixed_pose_valid(s, e)))
            continue;
#if DE_HAS_RENDER
        if (s->renderer)
            de_render_set_skin_palette(s->renderer, e->entity, e->palette, e->mesh_joints);
#endif
        if (e->ended) {
            de_anim_event event = {.kind = DE_ANIM_EV_CLIP_END, .entity = e->entity};
            de_event_send(s->world, s->events, &event);
            e->ended = false;
        }
    }
    de__anim_run_stage(s, DE_ANIM_STAGE_FINALIZE);
}
de_result de_anim_create(const de_allocator *allocator, de_asset_system *assets,
                         de_anim_system **out) {
    if (!out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "animation output");
    *out = NULL;
    const de_allocator *a = allocator ? allocator : de_heap_allocator();
    de_anim_system *s = DE_NEW(a, de_anim_system, DE_MEM_GENERAL);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "animation system");
    memset(s, 0, sizeof *s);
    s->alloc = a;
    s->assets = assets;
    de_result result = de__anim_register_assets(s);
    if (result != DE_OK) {
        DE_FREE(a, s, sizeof *s);
        return result;
    }
    *out = s;
    return DE_OK;
}
void de_anim_destroy(de_anim_system *s) {
    if (!s)
        return;
    de__sprite_shutdown(s);
    if (s->world) {
        de_system_remove(s->world, s->sample);
        de_system_remove(s->world, s->finalize);
        if (s->fixed.id)
            de_system_remove(s->world, s->fixed);
        if (s->checksum_query.id)
            de_query_destroy(s->world, s->checksum_query);
        if (s->resource.id)
            de_resource_remove(s->world, s->resource);
    }
    for (u32 i = 0; i < s->capacity; ++i)
        release_entry(s, &s->entries[i]);
    for (u32 i = 0; i < DE__ANIM_POSE_POOL_CLASSES; ++i)
        de_pool_destroy(s->pose_pools[i]);
    if (s->entries)
        DE_FREE(s->alloc, s->entries, (size_t)s->capacity * sizeof *s->entries);
    if (s->jobs)
        DE_FREE(s->alloc, s->jobs, (size_t)s->job_capacity * sizeof *s->jobs);
    if (s->checksum_rows)
        DE_FREE(s->alloc, s->checksum_rows, (size_t)s->checksum_capacity * 4 * sizeof(u64));
    DE_FREE(s->alloc, s, sizeof *s);
}
static de_type_id register_type(de_type_registry *types, de_type_desc desc) {
    de_type_id id = de_type_find(types, desc.name);
    const de_type_desc *existing = de_type_get(types, id);
    if (id.id && (!existing || existing->size != desc.size || existing->align != desc.align)) {
        (void)DE_FAIL(DE_ERR_ALREADY_EXISTS, "animation type conflict: %s", desc.name);
        return (de_type_id){0};
    }
    if (!id.id)
        id = de_type_register(types, &desc);
    return id;
}
void de_anim_attach(de_anim_system *s, de_world *world) {
    if (!s || !world || s->world == world)
        return;
    if (s->world) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "animation already attached to another world");
        return;
    }
    de_type_registry *types = de_world_types(world);
    de_type_id ref_type = de_type_find(types, "DeAnimSystemRef");
    if (ref_type.id && de_resource_find(world, ref_type).id) {
        (void)DE_FAIL(DE_ERR_ALREADY_EXISTS, "world already has an animation system");
        return;
    }
    if (de__anim_register_components(s, world) != DE_OK)
        return;
    s->builtins = de_builtin_components(world);
    if (!s->builtins.transform.id || !s->builtins.global_transform.id)
        goto failed;
    de_type_id resource = register_type(types, (de_type_desc){.name = "DeAnimSystemRef",
                                                              .size = sizeof(DeAnimSystemRef),
                                                              .align = DE_ALIGNOF(DeAnimSystemRef),
                                                              .version = 1,
                                                              .is_pod = true});
    if (!resource.id)
        goto failed;
    s->resource = de_resource_insert(world, resource, &(DeAnimSystemRef){s});
    if (!s->resource.id)
        goto failed;
    de_type_id event = register_type(types, (de_type_desc){.name = "de_anim_event",
                                                           .size = sizeof(de_anim_event),
                                                           .align = DE_ALIGNOF(de_anim_event),
                                                           .version = 1,
                                                           .is_pod = true});
    if (!event.id)
        goto failed;
    s->events = de_event_register(world, event);
    if (!s->events.id)
        goto failed;
    if (de__anim_deterministic_init(s, world) != DE_OK)
        goto failed;
    const de_query_term terms[] = {{s->component, DE_WRITE},
                                   {s->clock_component, DE_OPTIONAL_WRITE}};
    const de_query_desc query = {.terms = terms, .term_count = 2};
    const char *before[] = {DE_TRANSFORM_PROPAGATE_SYSTEM};
    s->sample =
        de_system_add(world, &(de_system_desc){.name = "anim.evaluate",
                                               .schedule = DE_SCHEDULE_POST_UPDATE,
                                               .fn = sample,
                                               .user = s,
                                               .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE,
                                               .before = before,
                                               .before_count = 1,
                                               .queries = &query,
                                               .query_count = 1});
    if (!s->sample.id)
        goto failed;
    const char *after[] = {DE_TRANSFORM_PROPAGATE_SYSTEM};
    s->finalize =
        de_system_add(world, &(de_system_desc){.name = "anim.finalize",
                                               .schedule = DE_SCHEDULE_POST_UPDATE,
                                               .fn = finalize,
                                               .user = s,
                                               .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE,
                                               .after = after,
                                               .after_count = 1});
    if (!s->finalize.id)
        goto failed;
    if (de__anim_features_init(s, world) != DE_OK)
        goto failed;
    s->world = world;
    return;
failed:
    de__sprite_shutdown(s);
    if (s->sample.id)
        de_system_remove(world, s->sample);
    if (s->finalize.id)
        de_system_remove(world, s->finalize);
    if (s->fixed.id)
        de_system_remove(world, s->fixed);
    if (s->checksum_query.id)
        de_query_destroy(world, s->checksum_query);
    if (s->resource.id)
        de_resource_remove(world, s->resource);
    s->sample = s->finalize = s->fixed = (de_system){0};
    s->checksum_query = (de_query){0};
    s->resource = (de_resource_id){0};
    s->events = (de_event_type){0};
    s->component = s->clock_component = s->ik_component = s->sprite_component = (de_component){0};
}
void de_anim_set_renderer(de_anim_system *s, de_renderer *r) {
    if (s) {
        s->renderer = r;
        s->renderer_explicit = true;
    }
}
void de__anim_bind_renderer(de_anim_system *s, de_renderer *r) {
    if (s && !s->renderer_explicit)
        s->renderer = r;
}
void de_anim_set_task_system(de_anim_system *s, de_task_system *tasks) {
    if (s)
        s->tasks = tasks;
}
de_component de_anim_component(const de_anim_system *s) {
    return s ? s->component : (de_component){0};
}
de_event_type de_anim_event_type(const de_anim_system *s) {
    return s ? s->events : (de_event_type){0};
}
de_anim_system *de__anim_for_world(de_world *world) {
    if (!world)
        return NULL;
    de_type_id type = de_type_find(de_world_types(world), "DeAnimSystemRef");
    if (!type.id)
        return NULL;
    const DeAnimSystemRef *ref = de_resource_get(world, de_resource_find(world, type));
    return ref ? ref->system : NULL;
}
bool de_anim_bone_transform(de_world *world, de_entity entity, de_name bone, de_transform *out) {
    if (!world || !out || !de_entity_alive(world, entity))
        return false;
    de_anim_system *s = de__anim_for_world(world);
    if (!s)
        return false;
    u32 index = de_entity_index(entity);
    if (index >= s->capacity)
        return false;
    const de__anim_entry *e = &s->entries[index];
    if (e->entity.bits != entity.bits || !e->ready || !e->finalized)
        return false;
    const DeAnimator *a = de_entity_get_const(world, entity, s->component);
    if (!a || (a->deterministic && !de__anim_fixed_pose_valid(s, e)))
        return false;
    for (u32 j = 0; j < e->joints; ++j)
        if (e->names[j] == bone.hash) {
            de_transform local;
            if (!de_transform_from_affine(&e->model[j], (de_dvec3){0}, &local))
                return false;
            const DeGlobalTransform *global =
                de_entity_get_const(world, entity, s->builtins.global_transform);
            *out = global ? de_transform_mul(global, &local) : local;
            return true;
        }
    return false;
}
