#include "anim/sync_groups.h"
#include "anim/anim_math.h"
#include "anim/anim_internal.h"
#include <string.h>
static bool finite64(f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof bits);
    return (bits & UINT64_C(0x7ff0000000000000)) != UINT64_C(0x7ff0000000000000);
}
static de_cooked_clip_marker ring_marker(const de__anim_marker_ring *ring, u32 i) {
    de_cooked_clip_marker marker =
        de_cooked_clip_get_marker(ring->clip, (ring->start + i) % ring->count);
    if (marker.time == ring->clip->duration)
        marker.time = 0;
    return marker;
}
static bool marker_ring(const de_cooked_clip_view *clip, de__anim_marker_ring *out) {
    if (!clip || !clip->markers || clip->marker_count < 2 ||
        clip->marker_count > DE_COOKED_CLIP_MAX_MARKERS || !de__anim_finite(clip->duration) ||
        !(clip->duration > 0))
        return false;
    de__anim_marker_ring ring = {.clip = clip, .count = clip->marker_count};
    de_cooked_clip_marker first = de_cooked_clip_get_marker(clip, 0);
    de_cooked_clip_marker last = de_cooked_clip_get_marker(clip, ring.count - 1);
    if (last.time == clip->duration) {
        if (first.time == 0) {
            if (first.name.hash != last.name.hash)
                return false;
            --ring.count;
        } else
            ring.start = ring.count - 1;
    }
    if (ring.count < 2)
        return false;
    f32 previous = -1;
    for (u32 i = 0; i < ring.count; ++i) {
        de_cooked_clip_marker marker = ring_marker(&ring, i);
        if (!marker.name.hash || !de__anim_finite(marker.time) ||
            !(marker.time >= 0 && marker.time > previous && marker.time < clip->duration))
            return false;
        previous = marker.time;
    }
    /* Booth's minimum cyclic name rotation: bounded linear work even for long
     * nearly periodic tracks, without allocating a prefix/suffix table. */
    u32 i = 0, j = 1, k = 0;
    while (i < ring.count && j < ring.count && k < ring.count) {
        u64 a = ring_marker(&ring, (i + k) % ring.count).name.hash;
        u64 b = ring_marker(&ring, (j + k) % ring.count).name.hash;
        if (a == b) {
            ++k;
            continue;
        }
        if (a > b) {
            i += k + 1;
            if (i == j)
                ++i;
        } else {
            j += k + 1;
            if (i == j)
                ++j;
        }
        k = 0;
    }
    ring.pivot = i < j ? i : j;
    /* Only divisors can be periods; comparing them avoids quadratic rotation
     * matching for long nearly periodic marker sequences. */
    ring.period = ring.count;
    for (u32 period = 1; period < ring.count; ++period) {
        if (ring.count % period)
            continue;
        bool repeated = true;
        for (u32 n = period; n < ring.count && repeated; ++n)
            repeated = ring_marker(&ring, (ring.pivot + n) % ring.count).name.hash ==
                       ring_marker(&ring, (ring.pivot + n % period) % ring.count).name.hash;
        if (repeated) {
            ring.period = period;
            break;
        }
    }
    *out = ring;
    return true;
}
bool de__anim_marker_map_init(de__anim_marker_map *out, const de_cooked_clip_view *reference,
                              const de_cooked_clip_view *clip) {
    if (!out)
        return false;
    memset(out, 0, sizeof *out);
    if (!marker_ring(reference, &out->reference) || !marker_ring(clip, &out->clip) ||
        out->reference.period != out->clip.period)
        return false;
    u32 n = out->reference.period;
    for (u32 i = 0; i < n; ++i)
        if (ring_marker(&out->reference, (out->reference.pivot + i) % out->reference.count)
                .name.hash !=
            ring_marker(&out->clip, (out->clip.pivot + i) % out->clip.count).name.hash)
            return false;
    out->shift = (i32)out->clip.pivot - (i32)out->reference.pivot;
    out->valid = true;
    return true;
}
static f64 marker_time(const de__anim_marker_ring *ring, u32 i) {
    return (f64)ring_marker(ring, i).time / (f64)ring->clip->duration;
}
/* Convert normalized clip cycles through an unbounded marker-interval index.
 * Signed pivot displacement aligns names; exchanging rings and negating it is
 * the inverse, including clips containing different numbers of foot cycles. */
static f64 marker_phase(const de__anim_marker_ring *from, const de__anim_marker_ring *to, i32 shift,
                        f64 phase) {
    f64 fraction = de__anim_wrap_time(phase, 1);
    f64 cycles = phase - fraction;
    u32 begin;
    if (fraction < marker_time(from, 0)) {
        begin = from->count - 1;
        fraction += 1;
        cycles -= 1;
    } else {
        u32 lo = 0, hi = from->count;
        while (lo + 1 < hi) {
            u32 mid = lo + (hi - lo) / 2;
            if (marker_time(from, mid) <= fraction)
                lo = mid;
            else
                hi = mid;
        }
        begin = lo;
    }
    u32 next = (begin + 1) % from->count;
    f64 a = marker_time(from, begin), b = marker_time(from, next) + (next == 0 ? 1 : 0);
    f64 intervals = cycles * from->count + begin + (fraction - a) / (b - a) + shift;
    if (!finite64(intervals))
        return intervals;
    f64 remaining = de__anim_wrap_time(intervals, to->count);
    u32 target = (u32)remaining;
    u32 target_next = (target + 1) % to->count;
    f64 c = marker_time(to, target), d = marker_time(to, target_next) + (target_next == 0 ? 1 : 0);
    return (intervals - remaining) / to->count + c + (remaining - target) * (d - c);
}
static f64 to_leaf(const de__anim_marker_map *map, f64 phase) {
    if (!map || !map->valid || (map->reference.clip == map->clip.clip && !map->shift))
        return phase;
    return marker_phase(&map->reference, &map->clip, map->shift, phase);
}
static f64 to_reference(const de__anim_marker_map *map, f64 phase) {
    if (!map || !map->valid || (map->reference.clip == map->clip.clip && !map->shift))
        return phase;
    return marker_phase(&map->clip, &map->reference, -map->shift, phase);
}
bool de__anim_sync_step(de__anim_sync_clock *clock, de_name group, const de__anim_sync_leaf *leaves,
                        u32 count, f64 time, bool reset) {
    return de__anim_sync_step_marked(clock, group, leaves, count, NULL, time, reset);
}
bool de__anim_sync_step_marked(de__anim_sync_clock *clock, de_name group,
                               const de__anim_sync_leaf *leaves, u32 count,
                               const de__anim_marker_map *maps, f64 time, bool reset) {
    if (!clock || !group.hash || (count && !leaves) || !finite64(time))
        return false;
    const de__anim_sync_leaf *leader = NULL;
    u32 leader_index = 0;
    for (u32 i = 0; i < count; ++i) {
        const de__anim_sync_leaf *leaf = &leaves[i];
        if (!de__anim_finite(leaf->demand) || leaf->demand < 0 || leaf->demand > 1 ||
            !de__anim_finite(leaf->duration) || leaf->duration < 0 ||
            !de__anim_finite(leaf->speed) || leaf->node == DE__ANIM_SYNC_NONE)
            return false;
        if (leaf->group.hash != group.hash || !(leaf->demand > 1e-4f) || !(leaf->duration > 0))
            continue;
        if (!leader || leaf->demand > leader->demand ||
            (leaf->demand == leader->demand && leaf->node < leader->node)) {
            leader = leaf;
            leader_index = i;
        }
    }
    de__anim_sync_clock next = *clock;
    if (reset || next.group.hash != group.hash) {
        memset(&next, 0, sizeof next);
        next.group = group;
    }
    if (next.initialized && (!finite64(next.phase) || !finite64(next.time)))
        return false;
    next.leader = leader ? leader->node : DE__ANIM_SYNC_NONE;
    if (leader) {
        f64 rate = (f64)leader->speed / (f64)leader->duration;
        const de__anim_marker_map *map = maps ? &maps[leader_index] : NULL;
        f64 phase;
        if (!next.initialized)
            phase = to_reference(map, time * rate);
        else if (time == next.time)
            phase = next.phase;
        else if (!map || !map->valid)
            phase = next.phase + (time - next.time) * rate;
        else
            phase = to_reference(map, to_leaf(map, next.phase) + (time - next.time) * rate);
        if (!finite64(phase))
            return false;
        next.phase = phase;
        next.initialized = true;
    }
    next.time = time;
    *clock = next;
    return true;
}
bool de__anim_sync_sample_time(const de__anim_sync_clock *clock, f32 duration, bool loop,
                               f32 *time) {
    if (!clock || !clock->initialized || !time || !finite64(clock->phase) ||
        !de__anim_finite(duration) || duration < 0)
        return false;
    f64 phase = clock->phase;
    if (loop)
        phase = de__anim_wrap_time(phase, 1);
    else
        phase = phase < 0 ? 0 : phase > 1 ? 1 : phase;
    *time = (f32)(phase * (f64)duration);
    return true;
}

bool de__anim_sync_sample_marked_time(const de__anim_sync_clock *clock,
                                      const de__anim_marker_map *map, f32 duration, bool loop,
                                      f32 *time) {
    if (!clock)
        return false;
    de__anim_sync_clock local = *clock;
    if (!finite64(local.phase))
        return false;
    local.phase = to_leaf(map, local.phase);
    return de__anim_sync_sample_time(&local, duration, loop, time);
}

/* Group state is an actor component with a size specialized by group count.
 * Schema/structural work happens on the exclusive main-thread preflight, before
 * gathering any borrowed animator/clock pointers for parallel evaluation. */
#include "anim/graph_internal.h"
#include <dengine/de_reflect.h>
#include <stdio.h>
typedef struct DeAnimSyncBinding {
    u32 count;
} DeAnimSyncBinding;
static de_type_id sync_type(de_type_registry *types, const de_type_desc *desc) {
    de_type_id type = de_type_find(types, desc->name);
    const de_type_desc *old = de_type_get(types, type);
    if (old &&
        (old->size != desc->size || old->align != desc->align || old->version != desc->version)) {
        (void)DE_FAIL(DE_ERR_ALREADY_EXISTS, "animation sync reflection conflict");
        return (de_type_id){0};
    }
    return type.id ? type : de_type_register(types, desc);
}
static de_component state_component(de_world *world, u32 count, bool create) {
    if (!count || count > 1024)
        return (de_component){0};
    char name[48];
    (void)snprintf(name, sizeof name, "DeAnimSyncState_%u", count);
    de_component component = de_component_find(world, name);
    de_type_registry *types = de_world_types(world);
    if (component.id) {
        const de_type_desc *existing = de_type_get(types, de_type_find(types, name));
        u32 bytes =
            (u32)(offsetof(de__anim_sync_state, clocks) + 2u * count * sizeof(de__anim_sync_clock));
        if (!existing || existing->size != bytes ||
            existing->align != DE_ALIGNOF(de__anim_sync_state) || existing->version != 1) {
            (void)DE_FAIL(DE_ERR_ALREADY_EXISTS, "animation sync state reflection conflict");
            return (de_component){0};
        }
        return component;
    }
    if (!create)
        return component;
    const u32 flags = DE_FIELD_DEFAULT | DE_FIELD_EDITOR_HIDDEN;
    const de_field_info clock_fields[] = {
        DE_FIELD(de__anim_sync_clock, group, DE_FK_NAME, flags),
        DE_FIELD(de__anim_sync_clock, time, DE_FK_F64, flags),
        DE_FIELD(de__anim_sync_clock, phase, DE_FK_F64, flags),
        DE_FIELD(de__anim_sync_clock, leader, DE_FK_U32, flags),
        DE_FIELD(de__anim_sync_clock, initialized, DE_FK_BOOL, flags)};
    de_type_desc clock = DE_TYPE_DESC(de__anim_sync_clock, 1, clock_fields);
    if (!sync_type(types, &clock).id)
        return (de_component){0};
    const de_field_info fields[] = {
        DE_FIELD(de__anim_sync_state, definition, DE_FK_U64, flags),
        DE_FIELD(de__anim_sync_state, fixed_valid, DE_FK_BOOL, flags),
        DE_FIELD(de__anim_sync_state, visual_valid, DE_FK_BOOL, flags),
        {.name = "clocks",
         .kind = DE_FK_FIXED_ARRAY,
         .offset = offsetof(de__anim_sync_state, clocks),
         .size = 2u * count * sizeof(de__anim_sync_clock),
         .flags = flags,
         .attrs = {.nested_type = "de__anim_sync_clock", .array_count = 2u * count}}};
    de_type_desc desc = {.name = name,
                         .size = (u32)(offsetof(de__anim_sync_state, clocks) +
                                       2u * count * sizeof(de__anim_sync_clock)),
                         .align = DE_ALIGNOF(de__anim_sync_state),
                         .version = 1,
                         .fields = fields,
                         .field_count = 4,
                         .is_pod = true};
    de_type_id type = sync_type(types, &desc);
    return type.id ? de_component_register(world, type, DE_COMP_SNAPSHOT) : (de_component){0};
}
static bool ensure_binding(de_anim_system *s) {
    if (s->sync_binding.id)
        return true;
    de_component existing = de_component_find(s->world, "DeAnimSyncBinding");
    if (existing.id) {
        de_type_registry *types = de_world_types(s->world);
        const de_type_desc *desc = de_type_get(types, de_type_find(types, "DeAnimSyncBinding"));
        if (!desc || desc->size != sizeof(DeAnimSyncBinding) ||
            desc->align != DE_ALIGNOF(DeAnimSyncBinding) || desc->version != 1) {
            (void)DE_FAIL(DE_ERR_ALREADY_EXISTS, "animation sync binding reflection conflict");
            return false;
        }
        s->sync_binding = existing;
        return true;
    }
    const de_field_info fields[] = {
        DE_FIELD(DeAnimSyncBinding, count, DE_FK_U32, DE_FIELD_DEFAULT | DE_FIELD_EDITOR_HIDDEN)};
    de_type_desc desc = DE_TYPE_DESC(DeAnimSyncBinding, 1, fields);
    de_type_id type = sync_type(de_world_types(s->world), &desc);
    if (type.id)
        s->sync_binding = de_component_register(s->world, type, DE_COMP_SNAPSHOT);
    return s->sync_binding.id != 0;
}
bool de__anim_graph_sync_prepare(de_anim_system *s, de_entity entity) {
    const DeAnimator *actor = de_entity_get_const(s->world, entity, s->component);
    de__anim_graph *g = actor ? de__anim_graph_get(s, actor->graph) : NULL;
    u32 count = g ? g->sync_count : 0;
    if (!count && !s->sync_binding.id && !de_component_find(s->world, "DeAnimSyncBinding").id)
        return true;
    if (!ensure_binding(s))
        return false;
    const DeAnimSyncBinding *binding = de_entity_get_const(s->world, entity, s->sync_binding);
    u32 old_count = binding ? binding->count : 0;
    de_component old = state_component(s->world, old_count, false);
    if (!count) {
        if (old.id)
            de_entity_remove(s->world, entity, old);
        if (old.id && de_entity_has(s->world, entity, old))
            return false;
        if (binding)
            de_entity_remove(s->world, entity, s->sync_binding);
        return !de_entity_has(s->world, entity, s->sync_binding);
    }
    de_component next = state_component(s->world, count, true);
    if (!next.id)
        return false;
    bool added = !de_entity_has(s->world, entity, next);
    if (added && !de_entity_add(s->world, entity, next))
        return false;
    if (old.id && old.id != next.id) {
        de_entity_remove(s->world, entity, old);
        if (de_entity_has(s->world, entity, old)) {
            if (added)
                de_entity_remove(s->world, entity, next);
            return false;
        }
    }
    DeAnimSyncBinding *dst = de_entity_get(s->world, entity, s->sync_binding);
    if (!dst)
        dst = de_entity_add(s->world, entity, s->sync_binding);
    if (!dst) {
        if (added)
            de_entity_remove(s->world, entity, next);
        return false;
    }
    dst->count = count;
    de__anim_sync_state *state = de_entity_get(s->world, entity, next);
    if (state->definition != g->definition_hash) {
        memset(state, 0,
               offsetof(de__anim_sync_state, clocks) + 2u * count * sizeof(de__anim_sync_clock));
        state->definition = g->definition_hash;
    }
    g->sync_component = next;
    return true;
}
u64 de__anim_graph_sync_hash(const de_anim_system *s, de_entity entity, const de__anim_graph *g,
                             u64 hash) {
    if (!g || !g->sync_count)
        return hash;
    const de__anim_sync_state *state =
        g->sync_component.id ? de_entity_get_const(s->world, entity, g->sync_component) : NULL;
    u64 count = state ? g->sync_count : 0;
    hash = de_hash64(&count, sizeof count, hash);
    if (!state)
        return hash;
    hash = de_hash64(&state->definition, sizeof state->definition, hash);
    hash = de_hash64(&state->fixed_valid, sizeof state->fixed_valid, hash);
    hash = de_hash64(&state->visual_valid, sizeof state->visual_valid, hash);
    for (u32 i = 0; i < 2u * g->sync_count; ++i) {
        const de__anim_sync_clock *c = &state->clocks[i];
        hash = de_hash64(&c->group.hash, sizeof c->group.hash, hash);
        hash = de_hash64(&c->time, sizeof c->time, hash);
        hash = de_hash64(&c->phase, sizeof c->phase, hash);
        hash = de_hash64(&c->leader, sizeof c->leader, hash);
        hash = de_hash64(&c->initialized, sizeof c->initialized, hash);
    }
    return hash;
}
