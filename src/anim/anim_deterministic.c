#include "anim_internal.h"
#include "ecs/ecs_internal.h"
#include <dengine/de_reflect.h>
#include <string.h>

static void evaluate_chunk(const de_query_chunk *chunk, void *user) {
    de_anim_system *s = user;
    DeAnimator *animators = chunk->columns[0];
    for (u32 row = 0; row < chunk->count; ++row) {
        DeAnimator *a = &animators[row];
        u32 index = de_entity_index(chunk->entities[row]);
        if (!a->deterministic || index >= s->capacity)
            continue;
        de__anim_entry *e = &s->entries[index];
        if (!e->ready || e->seen != s->epoch || e->entity.bits != chunk->entities[row].bits)
            continue;
        de__anim_clock *clock = e->clock;
        /* Visual frames between fixed ticks are not simulation time. An explicit
         * authored time change remains a seek, including after rollback. */
        bool visual = e->visual_output && clock->initialized &&
                      e->visual_fixed_ticks == clock->ticks &&
                      e->visual_fixed_hash == clock->pose_hash;
        bool seek = a->time != (visual ? e->visual_time : clock->output_time);
        if (visual && !seek) {
            a->time = clock->output_time;
            if (e->visual_playing == a->playing)
                a->playing = clock->playing;
        }
        de__anim_clock_step(clock, a, s->dt, e->clip.duration, e->clip_key, seek, &e->ended);
        e->visual_output = false;
        e->ready =
            de__anim_sample_pose_cached(&e->clip, &e->skeleton, a->time, &e->local, &e->cursor);
    }
}
static void finalize_range(u32 begin, u32 end, void *user) {
    de_anim_system *s = user;
    for (u32 i = begin; i < end; ++i) {
        de__anim_entry *e = &s->entries[s->jobs[i]];
        if (!e->ready)
            continue;
        de__anim_pose_deterministic(&e->skeleton, &e->local, e->model, e->palette);
        e->fixed_pose_hash = e->clock->pose_hash = de__anim_pose_checksum(&e->local);
        e->pose_restore_count = s->restore_count;
        e->clock_known = e->finalized = true;
    }
}
static void evaluate(de_system_ctx *ctx) {
    de_anim_system *s = ctx->user;
    ecs_snapshot_stats stats;
    ecs_snapshot_stats_get(ctx->world, &stats);
    s->restore_count = stats.restores;
    de__anim_prepare(ctx, true);
    /* The world's range scheduler carries the deterministic FP environment to
     * workers. The fixed grain is part of the simulation contract. */
    de_query_par_each(ctx->world, ctx->queries[0], 32, evaluate_chunk, s);
    for (de__anim_stage stage = DE_ANIM_STAGE_SAMPLE; stage <= DE_ANIM_STAGE_ROOT_MOTION; ++stage)
        de__anim_run_stage(s, stage);
    if (s->tasks && s->tasks == de_world_task_system(ctx->world))
        de_task_parallel_for(s->tasks, s->job_count, 32, finalize_range, s);
    else
        finalize_range(0, s->job_count, s);
    for (u32 i = 0; i < s->job_count; ++i) {
        de__anim_entry *e = &s->entries[s->jobs[i]];
        if (e->clock_missing) {
            de__anim_clock *state = de_cmd_add(ctx->commands, e->entity, s->clock_component);
            if (state)
                *state = e->pending_clock;
        }
        if (e->ended) {
            de_anim_event event = {.kind = DE_ANIM_EV_CLIP_END, .entity = e->entity};
            de_event_send(ctx->world, s->events, &event);
            e->ended = false;
        }
    }
    de__anim_run_stage(s, DE_ANIM_STAGE_FINALIZE);
}

bool de__anim_fixed_pose_valid(const de_anim_system *s, const de__anim_entry *e) {
    const de__anim_clock *clock = de_entity_get_const(s->world, e->entity, s->clock_component);
    const DeAnimator *a = de_entity_get_const(s->world, e->entity, s->component);
    if (!clock) {
        /* A following gameplay system can read the first fixed pose before the
         * deferred clock add is applied. Never reuse that pending state across
         * a restore of a snapshot taken before the clock existed, even when
         * authored time and the cached output happen to match. */
        ecs_snapshot_stats stats;
        ecs_snapshot_stats_get(s->world, &stats);
        if (!e->clock_missing || stats.restores != e->pose_restore_count)
            return false;
        clock = &e->pending_clock;
    }
    return a && clock && clock->initialized && e->clock_known &&
           clock->pose_hash == e->fixed_pose_hash && a->time == clock->output_time &&
           a->skeleton.id == e->skeleton_handle && a->clip.id == e->clip_handle;
}

de_result de__anim_deterministic_init(de_anim_system *s, de_world *w) {
    static const de_field_info fields[] = {
        DE_FIELD(de__anim_clock, ticks, DE_FK_U64, DE_FIELD_DEFAULT),
        DE_FIELD(de__anim_clock, segment_ticks, DE_FK_U64, DE_FIELD_DEFAULT),
        DE_FIELD(de__anim_clock, clip_key, DE_FK_U64, DE_FIELD_DEFAULT),
        DE_FIELD(de__anim_clock, pose_hash, DE_FK_U64, DE_FIELD_DEFAULT),
        DE_FIELD(de__anim_clock, phase, DE_FK_F64, DE_FIELD_DEFAULT),
        DE_FIELD(de__anim_clock, exact_time, DE_FK_F64, DE_FIELD_DEFAULT),
        DE_FIELD(de__anim_clock, dt, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(de__anim_clock, speed, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(de__anim_clock, duration, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(de__anim_clock, output_time, DE_FK_F32, DE_FIELD_DEFAULT),
        DE_FIELD(de__anim_clock, state_id, DE_FK_U32, DE_FIELD_DEFAULT),
        DE_FIELD(de__anim_clock, initialized, DE_FK_BOOL, DE_FIELD_DEFAULT),
        DE_FIELD(de__anim_clock, playing, DE_FK_BOOL, DE_FIELD_DEFAULT)};
    de_type_registry *types = de_world_types(w);
    de_type_desc desc = DE_TYPE_DESC(de__anim_clock, 1, fields);
    const de_type_desc *existing = de_type_get(types, de_type_find(types, desc.name));
    if (existing && (existing->size != desc.size || existing->align != desc.align ||
                     existing->version != desc.version))
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "animation clock reflection conflict");
    de_type_id type = de_type_register(types, &desc);
    if (!type.id)
        return DE_ERR_INTERNAL;
    s->clock_component = de_component_register(w, type, DE_COMP_SNAPSHOT);
    if (!s->clock_component.id)
        return DE_ERR_INTERNAL;
    const de_query_term terms[] = {{s->component, DE_WRITE},
                                   {s->clock_component, DE_OPTIONAL_WRITE}};
    const de_query_desc query = {.terms = terms, .term_count = 2};
    s->fixed =
        de_system_add(w, &(de_system_desc){.name = "anim.evaluate_fixed",
                                           .schedule = DE_SCHEDULE_FIXED_UPDATE,
                                           .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE |
                                                    DE_SYSTEM_DETERMINISTIC,
                                           .fn = evaluate,
                                           .user = s,
                                           .queries = &query,
                                           .query_count = 1});
    if (!s->fixed.id)
        return DE_ERR_INTERNAL;
    const de_query_term reads[] = {{s->component, DE_READ}, {s->clock_component, DE_OPTIONAL_READ}};
    s->checksum_query = de_query_create(w, &(de_query_desc){.terms = reads, .term_count = 2});
    return s->checksum_query.id ? DE_OK : DE_ERR_INTERNAL;
}
