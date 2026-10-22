#include "anim_internal.h"
#include "asset/slots_internal.h"
#include <string.h>
typedef struct sprite_entry {
    de_entity entity;
    de_load_handle ref;
    u32 handle, version, from, to, frame, output_frame;
    u64 seen, tag, phase, period, leg, old_phase;
    i64 delta;
    f64 fraction;
    f32 published;
    bool ping, reverse, input_ping, ready, initial, apply, end_event, ended, was_playing;
    DeSpriteAnimator input;
    const de__anim_asset *asset;
    de_texture texture;
} sprite_entry;
struct de__sprite_runtime {
    de_anim_system *owner;
    de_world *world;
    de_system system;
    de_component sprite;
    sprite_entry *entries;
    u32 capacity;
    u64 epoch, type_hash;
    f32 dt;
};
static void release(struct de__sprite_runtime *r, sprite_entry *e) {
    if (e->ref.id)
        de_asset_release(r->owner->assets, e->ref);
    memset(e, 0, sizeof *e);
}
static bool reserve(struct de__sprite_runtime *r, u32 index) {
    if (index < r->capacity)
        return true;
    u32 capacity = r->capacity ? r->capacity : 64;
    while (capacity <= index) {
        if (capacity > UINT32_MAX / 2)
            return false;
        capacity *= 2;
    }
    sprite_entry *p = DE_ALLOC(r->owner->alloc, (size_t)capacity * sizeof *p, 16, DE_MEM_GENERAL);
    if (!p)
        return false;
    memset(p, 0, (size_t)capacity * sizeof *p);
    if (r->entries) {
        memcpy(p, r->entries, (size_t)r->capacity * sizeof *p);
        DE_FREE(r->owner->alloc, r->entries, (size_t)r->capacity * sizeof *p);
    }
    r->entries = p;
    r->capacity = capacity;
    return true;
}
/* Locate an integer millisecond in the directed path. Ping-pong omits repeated ends. */
static u32 locate(const sprite_entry *e, u64 phase, u32 *offset) {
    const u64 *p = e->asset->sprite_prefix;
    bool reverse = e->reverse;
    u64 key;
    bool terminal = phase == e->period;
    if (terminal)
        --phase;
    if (e->ping && phase >= e->leg) {
        phase -= e->leg;
        reverse = !reverse;
        key = reverse ? p[e->to] - 1 - phase : p[e->from + 1] + phase;
    } else
        key = reverse ? p[e->to + 1] - 1 - phase : p[e->from] + phase;
    u32 lo = e->from, hi = e->to + 1;
    while (lo + 1 < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (p[mid] <= key)
            lo = mid;
        else
            hi = mid;
    }
    *offset = (u32)(reverse ? p[lo + 1] - 1 - key : key - p[lo]);
    if (terminal)
        ++*offset;
    return lo;
}
static bool configure(sprite_entry *e, const de__anim_asset *asset, const DeSpriteAnimator *a) {
    e->from = 0;
    e->to = asset->sprite.frame_count - 1;
    e->reverse = false;
    if (a->tag.hash) {
        u32 lo = 0, hi = asset->sprite.tag_count;
        while (lo < hi) {
            u32 mid = lo + (hi - lo) / 2;
            if (de__sprite_tag_at(&asset->sprite, mid).name < a->tag.hash)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo == asset->sprite.tag_count)
            return false;
        de__sprite_tag t = de__sprite_tag_at(&asset->sprite, lo);
        if (t.name != a->tag.hash)
            return false;
        e->from = t.from;
        e->to = t.to;
        e->reverse = t.direction == 1;
        e->ping = a->ping_pong || t.direction == 2;
    } else
        e->ping = a->ping_pong;
    if (e->from == e->to)
        e->ping = false;
    e->leg = asset->sprite_prefix[e->to + 1] - asset->sprite_prefix[e->from];
    e->period = e->leg;
    if (e->ping)
        e->period += e->leg - de__sprite_frame_at(&asset->sprite, e->from).ms -
                     de__sprite_frame_at(&asset->sprite, e->to).ms;
    e->phase = 0;
    e->fraction = 0;
    e->ended = false;
    e->initial = true;
    e->tag = a->tag.hash;
    e->input_ping = a->ping_pong;
    return true;
}
static bool clock_ms(f64 value, i64 *whole) {
    if (!(value >= -9223372036854775808.0 && value < 9223372036854775808.0))
        return false;
    *whole = (i64)value;
    return true;
}
static void gather(const de_query_chunk *c, void *user) {
    struct de__sprite_runtime *r = user;
    const DeSpriteAnimator *input = c->columns[0];
    for (u32 i = 0; i < c->count; ++i) {
        de_entity entity = c->entities[i];
        u32 index = de_entity_index(entity);
        if (!de_entity_get_const(r->world, entity, r->sprite) || !reserve(r, index))
            continue;
        sprite_entry *e = &r->entries[index];
        const DeSpriteAnimator *a = &input[i];
        if (e->entity.bits != entity.bits || e->handle != a->clip.id) {
            f32 published = e->entity.bits == entity.bits ? e->published : 0;
            release(r, e);
            e->entity = entity;
            e->handle = a->clip.id;
            e->published = published;
            const de__slot *slot =
                r->owner->assets ? de__slot_get(r->owner->assets, a->clip.id) : NULL;
            if (slot && slot->type_hash == r->type_hash)
                e->ref = de_asset_load(r->owner->assets, slot->id, DE_LOAD_NO_EVENTS);
        }
        e->seen = r->epoch;
        e->input = *a;
        e->was_playing = a->playing;
        e->delta = 0;
        e->end_event = false;
        e->apply = false;
        const de__slot *slot = r->owner->assets ? de__slot_get(r->owner->assets, a->clip.id) : NULL;
        const de__anim_asset *asset =
            slot && slot->state == DE_LOAD_READY && slot->type_hash == r->type_hash
                ? de__slot_object(slot)
                : NULL;
        bool reset = asset && (e->asset != asset || e->version != slot->version ||
                               e->tag != a->tag.hash || e->input_ping != a->ping_pong || !e->ready);
        e->ready = false;
        if (!asset || asset->kind != 3)
            continue;
        e->asset = asset;
        if (reset) {
            if (!configure(e, asset, a))
                continue;
            e->texture = de_asset_texture(r->owner->assets, (de_asset_id){asset->sprite.atlas});
            if (!e->texture.id)
                continue;
        }
        e->version = slot->version;
        if (a->time != e->published) {
            i64 ms;
            if (!clock_ms((f64)a->time * 1000, &ms))
                continue;
            if (a->loop) {
                i64 phase = ms % (i64)e->period;
                if (phase < 0)
                    phase += (i64)e->period;
                e->phase = (u64)phase;
            } else
                e->phase = ms < 0 ? 0 : (u64)ms > e->period ? e->period : (u64)ms;
            e->fraction = 0;
            e->initial = true;
            e->ended = false;
        }
        e->old_phase = e->phase;
        e->ready = true;
    }
}
static void compute(const de_query_chunk *c, void *user) {
    struct de__sprite_runtime *r = user;
    for (u32 i = 0; i < c->count; ++i) {
        u32 index = de_entity_index(c->entities[i]);
        if (index >= r->capacity)
            continue;
        sprite_entry *e = &r->entries[index];
        if (e->seen != r->epoch || !e->ready)
            continue;
        if (e->input.playing) {
            f64 elapsed = (f64)r->dt * e->input.speed * 1000 + e->fraction;
            i64 delta;
            if (clock_ms(elapsed, &delta)) {
                e->fraction = elapsed - (f64)delta;
                if (e->input.loop) {
                    i64 phase = (i64)e->phase + delta % (i64)e->period;
                    if (phase < 0)
                        phase += (i64)e->period;
                    if ((u64)phase >= e->period)
                        phase -= (i64)e->period;
                    e->phase = (u64)phase;
                    e->delta = delta;
                    e->ended = false;
                } else {
                    bool end = false;
                    if (delta > 0 && (u64)delta >= e->period - e->phase) {
                        delta = (i64)(e->period - e->phase);
                        e->phase = e->period;
                        end = true;
                    } else if (delta < 0 && delta <= -(i64)e->phase) {
                        delta = -(i64)e->phase;
                        e->phase = 0;
                        end = true;
                    } else
                        e->phase = (u64)((i64)e->phase + delta);
                    e->delta = delta;
                    e->end_event = end && !e->ended;
                    e->ended = end;
                    if (end)
                        e->input.playing = false;
                }
            }
        }
        u32 offset;
        e->output_frame = locate(e, e->phase, &offset);
        e->apply = e->initial || e->frame != e->output_frame;
        /* Ranges own disjoint entities, and the exclusive system prevents any
         * other component access. Mutable gets stamp only actual output writes;
         * events still flush in entity order after all ranges join. */
        if (e->apply) {
            de__sprite_frame f = de__sprite_frame_at(&e->asset->sprite, e->output_frame);
            DeSprite *sprite = de_entity_get(r->world, e->entity, r->sprite);
            sprite->texture = e->texture;
            sprite->uv_rect = (de_vec4){f.rect[0], f.rect[1], f.rect[2], f.rect[3]};
            sprite->pivot = (de_vec2){f.pivot[0], f.pivot[1]};
            e->frame = e->output_frame;
        }
        e->published = (f32)((f64)e->phase / 1000);
        if (e->input.time != e->published || e->was_playing != e->input.playing) {
            DeSpriteAnimator *a = de_entity_get(r->world, e->entity, r->owner->sprite_component);
            a->time = e->published;
            a->playing = e->input.playing;
        }
    }
}
static void marker(struct de__sprite_runtime *r, sprite_entry *e, u32 frame) {
    const de__sprite_clip_view *v = &e->asset->sprite;
    u32 lo = 0, hi = v->event_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (de__sprite_event_at(v, mid).frame < frame)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (; lo < v->event_count; ++lo) {
        de__sprite_event event = de__sprite_event_at(v, lo);
        if (event.frame != frame)
            break;
        de_anim_event payload = {DE_ANIM_EV_MARKER, e->entity, {event.name}};
        de_event_send(r->world, r->owner->events, &payload);
    }
}
static void events(struct de__sprite_runtime *r, sprite_entry *e) {
    if (!e->asset->sprite.event_count)
        return;
    u64 phase = e->old_phase;
    u32 offset;
    if (e->initial)
        marker(r, e, locate(e, phase, &offset));
    u64 left = e->delta < 0 ? (u64)(-(e->delta + 1)) + 1 : (u64)e->delta;
    while (left) {
        u32 frame = locate(e, phase, &offset);
        if (e->delta > 0) {
            u64 step = de__sprite_frame_at(&e->asset->sprite, frame).ms - offset;
            if (left < step)
                break;
            left -= step;
            phase += step;
            if (phase == e->period) {
                if (!e->input.loop)
                    break;
                phase = 0;
            }
        } else {
            if (left <= offset)
                break;
            left -= (u64)offset + 1;
            if (phase < (u64)offset + 1) {
                if (!e->input.loop)
                    break;
                phase = e->period - 1;
            } else
                phase -= (u64)offset + 1;
        }
        marker(r, e, locate(e, phase, &offset));
    }
}
static void update(de_system_ctx *ctx) {
    struct de__sprite_runtime *r = ctx->user;
    r->sprite = de_component_find(ctx->world, "DeSprite");
    if (!r->sprite.id)
        return;
    r->dt = ctx->dt;
    ++r->epoch;
    de_query_each(ctx->world, ctx->queries[0], gather, r);
    if (r->owner->tasks && r->owner->tasks == de_world_task_system(ctx->world))
        de_query_par_each(ctx->world, ctx->queries[0], 32, compute, r);
    else
        de_query_each(ctx->world, ctx->queries[0], compute, r);
    /* Component writes are disjoint. Events alone need canonical publication. */
    for (u32 i = 0; i < r->capacity; ++i) {
        sprite_entry *e = &r->entries[i];
        if (e->entity.bits && e->seen != r->epoch) {
            release(r, e);
            continue;
        }
        if (!e->ready || e->seen != r->epoch)
            continue;
        events(r, e);
        if (e->end_event) {
            de_anim_event event = {DE_ANIM_EV_CLIP_END, e->entity, {0}};
            de_event_send(ctx->world, r->owner->events, &event);
        }
        e->initial = false;
    }
}
de_result de_anim__sprite_init(de_anim_system *s, de_world *w) {
    struct de__sprite_runtime *r = DE_NEW(s->alloc, struct de__sprite_runtime, DE_MEM_GENERAL);
    if (!r)
        return DE_ERR_OUT_OF_MEMORY;
    memset(r, 0, sizeof *r);
    r->owner = s;
    r->world = w;
    r->type_hash = de_hash64_str("sprite_clip");
    de_query_term term = {s->sprite_component, DE_READ};
    de_query_desc q = {.terms = &term, .term_count = 1};
    r->system =
        de_system_add(w, &(de_system_desc){.name = "anim.sprite",
                                           .schedule = DE_SCHEDULE_UPDATE,
                                           .fn = update,
                                           .user = r,
                                           .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE,
                                           .queries = &q,
                                           .query_count = 1});
    if (!r->system.id) {
        DE_FREE(s->alloc, r, sizeof *r);
        return DE_ERR_OUT_OF_MEMORY;
    }
    s->sprites = r;
    return DE_OK;
}
void de__sprite_shutdown(de_anim_system *s) {
    struct de__sprite_runtime *r = s->sprites;
    if (!r)
        return;
    de_system_remove(r->world, r->system);
    for (u32 i = 0; i < r->capacity; ++i)
        release(r, &r->entries[i]);
    if (r->entries)
        DE_FREE(s->alloc, r->entries, (size_t)r->capacity * sizeof *r->entries);
    DE_FREE(s->alloc, r, sizeof *r);
    s->sprites = NULL;
}
