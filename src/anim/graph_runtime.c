#include "anim/graph_internal.h"
#include "anim/anim_math.h"
#include "anim/blend.h"
#include "asset/slots_internal.h"
#include <string.h>

typedef struct graph_pose {
    de__anim_local_pose pose;
    de__anim_clip_cursor cursor;
    de_pool *pool;
    void *block;
} graph_pose;
typedef struct graph_node {
    graph_pose output, reference;
    de_cooked_clip_view clip;
    de__anim_blend_weights blend;
    f32 demand, weight;
    bool valid;
} graph_node;
typedef struct graph_layer {
    graph_pose reference;
    f32 *mask;
    de_pool *mask_pool;
    f32 weight;
} graph_layer;
typedef struct de__anim_graph_runtime {
    const de__anim_graph *definition;
    u32 handle, node_count, layer_count;
    u64 fingerprint;
    size_t bytes;
    graph_node *nodes;
    graph_layer *layers;
    graph_pose scratch;
    DeAnimParams params;
    bool params_missing, deterministic;
} de__anim_graph_runtime;

static void free_pose(graph_pose *p) {
    if (p->block)
        de_pool_free(p->pool, p->block);
}
static bool make_pose(de_anim_system *s, u32 joints, u32 tracks, graph_pose *p) {
    size_t t = ((size_t)joints * sizeof(de_quat) + 15) & ~(size_t)15;
    size_t scale = (t + (size_t)joints * sizeof(de_vec3) + 15) & ~(size_t)15;
    size_t cursor = (scale + (size_t)joints * sizeof(de_vec3) + 15) & ~(size_t)15;
    p->pool = de__anim_pose_pool(s, cursor + (size_t)tracks * sizeof(u32));
    p->block = p->pool ? de_pool_alloc(p->pool) : NULL;
    if (!p->block)
        return false;
    p->pose = (de__anim_local_pose){.count = joints,
                                    .rotations = p->block,
                                    .translations = (de_vec3 *)((u8 *)p->block + t),
                                    .scales = (de_vec3 *)((u8 *)p->block + scale),
                                    .root_rotation = {0, 0, 0, 1}};
    p->cursor =
        (de__anim_clip_cursor){.keys = (u32 *)((u8 *)p->block + cursor), .capacity = tracks};
    return true;
}
static void destroy_runtime(de_anim_system *s, de__anim_graph_runtime *r) {
    if (!r)
        return;
    for (u32 i = 0; i < r->node_count; ++i) {
        free_pose(&r->nodes[i].output);
        free_pose(&r->nodes[i].reference);
    }
    for (u32 i = 0; i < r->layer_count; ++i) {
        free_pose(&r->layers[i].reference);
        if (r->layers[i].mask)
            de_pool_free(r->layers[i].mask_pool, r->layers[i].mask);
    }
    free_pose(&r->scratch);
    DE_FREE(s->alloc, r, r->bytes);
}
void de__anim_graph_runtime_release(de_anim_system *s, de__anim_entry *e) {
    destroy_runtime(s, e->graph_runtime);
    e->graph_runtime = NULL;
}
static bool clip_view(de_anim_system *s, de_anim_clip h, u32 joints, de_cooked_clip_view *out,
                      u64 *fingerprint) {
    const de__anim_asset *asset = de__anim_asset_get(s, h.id, "anim_clip");
    const de__slot *slot = s->assets ? de__slot_get(s->assets, h.id) : NULL;
    if (!asset || !slot || asset->clip.joint_count != joints)
        return false;
    *out = asset->clip;
    const u64 words[] = {h.id, slot->version, (u64)(uintptr_t)asset->clip.base};
    *fingerprint = de_hash64(words, sizeof words, *fingerprint);
    return true;
}
static de_anim_clip additive_reference(const de__anim_graph *g, u32 input, de_anim_clip reference) {
    return reference.id ? reference : g->nodes[input].data.clip.clip;
}
static bool reference_pose(de_anim_system *s, de__anim_entry *e, de_anim_clip clip,
                           graph_pose *out) {
    de_cooked_clip_view view;
    u64 ignored = 0;
    return clip_view(s, clip, e->joints, &view, &ignored) &&
           make_pose(s, e->joints, view.track_count, out) &&
           de__anim_sample_pose_cached(&view, &e->skeleton, 0, &out->pose, &out->cursor);
}
static bool make_mask(de_anim_system *s, de__anim_entry *e, const de_anim_layer_desc *d,
                      graph_layer *out) {
    if (!d->mask_count)
        return true;
    out->mask_pool = de__anim_pose_pool(s, (size_t)e->joints * sizeof(f32));
    out->mask = out->mask_pool ? de_pool_alloc(out->mask_pool) : NULL;
    de__anim_bone_mask *rules = DE_ALLOC(s->alloc, (size_t)d->mask_count * sizeof *rules,
                                         _Alignof(de__anim_bone_mask), DE_MEM_GENERAL);
    if (!rules)
        return false;
    for (u32 i = 0; i < d->mask_count; ++i)
        rules[i] =
            (de__anim_bone_mask){d->mask[i].bone, d->mask[i].weight, d->mask[i].include_children};
    bool ok = out->mask && de__anim_mask_resolve(&e->skeleton, rules, d->mask_count, out->mask);
    DE_FREE(s->alloc, rules, (size_t)d->mask_count * sizeof *rules);
    return ok;
}
bool de__anim_graph_prepare(de_anim_system *s, de__anim_entry *e) {
    const de__anim_graph *g = de__anim_graph_get(s, e->animator->graph);
    if (!g)
        return false;
    const de__slot *skeleton_slot = de__slot_get(s->assets, e->skeleton_handle);
    if (!skeleton_slot)
        return false;
    const u64 skeleton_key[] = {e->skeleton_handle, skeleton_slot->version,
                                (u64)(uintptr_t)e->skeleton.base};
    u64 fingerprint = de_hash64(skeleton_key, sizeof skeleton_key, g->handle.id);
    de_cooked_clip_view ignored;
    for (u32 i = 0; i < g->desc.node_count; ++i) {
        const de_anim_node_desc *n = &g->nodes[i];
        if (n->kind == DE_ANIM_NODE_CLIP &&
            !clip_view(s, n->data.clip.clip, e->joints, &ignored, &fingerprint))
            return false;
        if (n->kind == DE_ANIM_NODE_ADDITIVE &&
            !clip_view(s, additive_reference(g, n->data.additive.input, n->data.additive.reference),
                       e->joints, &ignored, &fingerprint))
            return false;
    }
    for (u32 i = 0; i < g->desc.layer_count; ++i) {
        const de_anim_layer_desc *d = &g->layers[i];
        if (d->mode == DE_ANIM_LAYER_ADDITIVE &&
            !clip_view(s, additive_reference(g, d->node, d->reference), e->joints, &ignored,
                       &fingerprint))
            return false;
    }
    de__anim_graph_runtime *r = e->graph_runtime;
    if (!r || r->handle != g->handle.id || r->fingerprint != fingerprint) {
        size_t bytes = sizeof *r + (size_t)g->desc.node_count * sizeof(graph_node) +
                       (size_t)g->desc.layer_count * sizeof(graph_layer);
        de__anim_graph_runtime *next = DE_ALLOC(s->alloc, bytes, 16, DE_MEM_GENERAL);
        if (!next)
            return false;
        memset(next, 0, bytes);
        next->bytes = bytes;
        next->definition = g;
        next->handle = g->handle.id;
        next->fingerprint = fingerprint;
        next->node_count = g->desc.node_count;
        next->layer_count = g->desc.layer_count;
        next->nodes = (graph_node *)(next + 1);
        next->layers = (graph_layer *)(next->nodes + next->node_count);
        bool ok = make_pose(s, e->joints, 0, &next->scratch);
        for (u32 i = 0; ok && i < next->node_count; ++i) {
            const de_anim_node_desc *n = &g->nodes[i];
            graph_node *node = &next->nodes[i];
            if (n->kind == DE_ANIM_NODE_CLIP)
                ok = clip_view(s, n->data.clip.clip, e->joints, &node->clip, &fingerprint);
            ok = ok && make_pose(s, e->joints, node->clip.track_count, &node->output);
            if (ok && n->kind == DE_ANIM_NODE_ADDITIVE)
                ok = reference_pose(
                    s, e, additive_reference(g, n->data.additive.input, n->data.additive.reference),
                    &node->reference);
        }
        for (u32 i = 0; ok && i < next->layer_count; ++i) {
            const de_anim_layer_desc *d = &g->layers[i];
            ok = make_mask(s, e, d, &next->layers[i]);
            if (ok && d->mode == DE_ANIM_LAYER_ADDITIVE)
                ok = reference_pose(s, e, additive_reference(g, d->node, d->reference),
                                    &next->layers[i].reference);
        }
        if (!ok) {
            destroy_runtime(s, next);
            return false;
        }
        destroy_runtime(s, r);
        r = e->graph_runtime = next;
    }
    e->clip_key = g->definition_hash;
    r->deterministic = s->fixed_evaluation;
    DeAnimParams *params = de_entity_get(s->world, e->entity, s->params_component);
    if (params && params->graph != g->handle.id)
        de__anim_params_defaults(params, g);
    r->params_missing = params == NULL;
    if (params)
        r->params = *params;
    else
        de__anim_params_defaults(&r->params, g);
    return true;
}
static f32 parameter(const de__anim_graph_runtime *r, de_name name, f32 fallback) {
    if (!name.hash)
        return fallback;
    i32 i = de__anim_graph_param(r->definition, name, DE_ANIM_PARAM_FLOAT);
    return i >= 0 && de__anim_finite(r->params.values[i]) ? r->params.values[i] : fallback;
}
static f32 weight(const de__anim_graph_runtime *r, de_name name, f32 fallback) {
    f32 v = parameter(r, name, fallback);
    return v < 0 ? 0 : v > 1 ? 1 : v;
}
static void demand(graph_node *n, f32 v) {
    n->demand += v;
    if (n->demand > 1)
        n->demand = 1;
}
static void copy_pose(de__anim_local_pose *out, const de__anim_local_pose *in) {
    memcpy(out->rotations, in->rotations, (size_t)in->count * sizeof(de_quat));
    memcpy(out->translations, in->translations, (size_t)in->count * sizeof(de_vec3));
    memcpy(out->scales, in->scales, (size_t)in->count * sizeof(de_vec3));
    out->root_rotation = in->root_rotation;
    out->root_translation = in->root_translation;
}
bool de__anim_graph_evaluate(de_anim_system *s, de__anim_entry *e) {
    de__anim_graph_runtime *r = e->graph_runtime;
    const de__anim_graph *g = r->definition;
    e->graph_samples = 0;
    for (u32 i = 0; i < r->node_count; ++i) {
        graph_node *n = &r->nodes[i];
        const de_anim_node_desc *d = &g->nodes[i];
        n->demand = 0;
        n->valid = false;
        if (d->kind == DE_ANIM_NODE_BLEND1D || d->kind == DE_ANIM_NODE_BLEND2D) {
            de_vec2 query = {parameter(r, d->data.blend.x_param, 0),
                             parameter(r, d->data.blend.y_param, 0)};
            if (!de__anim_blend_space_weights(g->spaces[i], query, &n->blend))
                return false;
        } else if (d->kind == DE_ANIM_NODE_ADDITIVE)
            n->weight = weight(r, d->data.additive.weight_param, d->data.additive.weight);
    }
    for (u32 i = 0; i < r->layer_count; ++i)
        r->layers[i].weight = weight(r, g->layers[i].weight_param, g->layers[i].weight);
    r->nodes[g->desc.output_node].demand = 1;
    for (u32 i = r->node_count; i-- > 0;) {
        graph_node *n = &r->nodes[i];
        const de_anim_node_desc *d = &g->nodes[i];
        if (d->kind == DE_ANIM_NODE_BLEND1D || d->kind == DE_ANIM_NODE_BLEND2D) {
            for (u32 j = 0; j < n->blend.count; ++j)
                demand(&r->nodes[n->blend.node[j]], n->demand * n->blend.weight[j]);
        } else if (d->kind == DE_ANIM_NODE_ADDITIVE) {
            demand(&r->nodes[d->data.additive.base], n->demand);
            demand(&r->nodes[d->data.additive.input], n->demand * n->weight);
        } else if (d->kind == DE_ANIM_NODE_LAYER) {
            demand(&r->nodes[d->data.layer.base], n->demand);
            for (u32 j = 0; j < d->data.layer.layer_count; ++j) {
                u32 l = d->data.layer.first_layer + j;
                demand(&r->nodes[g->layers[l].node], n->demand * r->layers[l].weight);
            }
        }
    }
    DeAnimator *a = e->animator;
    if (!s->fixed_evaluation && !de__anim_finite(a->time))
        a->time = 0;
    if (!s->fixed_evaluation && a->playing && de__anim_finite(a->speed) && de__anim_finite(s->dt)) {
        f32 next = a->time + a->speed * s->dt;
        if (de__anim_finite(next))
            a->time = next;
    }
    for (u32 i = 0; i < r->node_count; ++i) {
        graph_node *n = &r->nodes[i];
        const de_anim_node_desc *d = &g->nodes[i];
        if (d->kind != DE_ANIM_NODE_CLIP || !(n->demand > 1e-4f))
            continue;
        f32 t;
        if (s->fixed_evaluation) {
            f64 time = e->clock->exact_time * (f64)d->data.clip.speed;
            f64 duration = n->clip.duration;
            if (!(duration > 0))
                time = 0;
            else if (d->data.clip.loop)
                time = de__anim_wrap_time(time, duration);
            else
                time = time < 0 ? 0 : time > duration ? duration : time;
            t = (f32)time;
        } else {
            bool ended;
            t = de__anim_time(0, n->clip.duration, d->data.clip.speed, a->time, d->data.clip.loop,
                              &ended);
        }
        n->valid = de__anim_sample_pose_cached(&n->clip, &e->skeleton, t, &n->output.pose,
                                               &n->output.cursor);
        ++e->graph_samples;
        if (!n->valid)
            return false;
    }
    return true;
}
static de_quat mix_rotation(const de__anim_local_pose *const *poses, const f32 *weights, u32 count,
                            u32 joint, bool root, bool deterministic) {
    de_quat first = root ? poses[0]->root_rotation : poses[0]->rotations[joint];
    de_quat sum = {0};
    for (u32 i = 0; i < count; ++i) {
        de_quat q = root ? poses[i]->root_rotation : poses[i]->rotations[joint];
        f32 sign = first.x * q.x + first.y * q.y + first.z * q.z + first.w * q.w < 0 ? -1 : 1;
        f32 w = weights[i] * sign;
        sum.x += q.x * w;
        sum.y += q.y * w;
        sum.z += q.z * w;
        sum.w += q.w * w;
    }
    return de__anim_normalize(sum, deterministic);
}
static bool compose(de__anim_graph_runtime *r, graph_node *n) {
    const de__anim_local_pose *poses[3];
    f32 weights[3], total = 0;
    u32 count = 0;
    for (u32 j = 0; j < n->blend.count; ++j) {
        graph_node *child = &r->nodes[n->blend.node[j]];
        f32 w = n->blend.weight[j];
        if (!child->valid || !(w > 0))
            continue;
        poses[count] = &child->output.pose;
        weights[count++] = w;
        total += w;
    }
    if (!count)
        return false;
    if (count == 1) {
        copy_pose(&n->output.pose, poses[0]);
        return true;
    }
    for (u32 i = 0; i < count; ++i)
        weights[i] /= total;
    de__anim_local_pose *out = &n->output.pose;
    for (u32 j = 0; j < out->count; ++j) {
        de_vec3 translation = {0}, scale = {0};
        for (u32 i = 0; i < count; ++i) {
            translation = de__anim_add_delta(translation, poses[i]->translations[j], weights[i]);
            scale = de__anim_add_delta(scale, poses[i]->scales[j], weights[i]);
        }
        out->translations[j] = translation;
        out->scales[j] = scale;
        out->rotations[j] = mix_rotation(poses, weights, count, j, false, r->deterministic);
    }
    out->root_translation = (de_vec3){0};
    for (u32 i = 0; i < count; ++i)
        out->root_translation =
            de__anim_add_delta(out->root_translation, poses[i]->root_translation, weights[i]);
    out->root_rotation = mix_rotation(poses, weights, count, 0, true, r->deterministic);
    return true;
}
static void blend_range(u32 begin, u32 end, void *user) {
    de_anim_system *s = user;
    for (u32 actor = begin; actor < end; ++actor) {
        de__anim_entry *e = &s->entries[s->jobs[actor]];
        de__anim_graph_runtime *r = e->graph_runtime;
        if (!e->ready || !e->animator->graph.id || !r)
            continue;
        const de__anim_graph *g = r->definition;
        for (u32 i = 0; i < r->node_count; ++i) {
            graph_node *n = &r->nodes[i];
            const de_anim_node_desc *d = &g->nodes[i];
            if (!(n->demand > 0) || d->kind == DE_ANIM_NODE_CLIP)
                continue;
            if (d->kind == DE_ANIM_NODE_BLEND1D || d->kind == DE_ANIM_NODE_BLEND2D)
                n->valid = compose(r, n);
            else if (d->kind == DE_ANIM_NODE_ADDITIVE) {
                graph_node *base = &r->nodes[d->data.additive.base];
                graph_node *input = &r->nodes[d->data.additive.input];
                n->valid = base->valid;
                if (!n->valid)
                    continue;
                copy_pose(&n->output.pose, &base->output.pose);
                if (input->valid && n->weight > 0)
                    n->valid =
                        de__anim_pose_delta(&r->scratch.pose, &input->output.pose,
                                            &n->reference.pose, r->deterministic) &&
                        de__anim_pose_layer(&n->output.pose, &n->output.pose, &r->scratch.pose,
                                            n->weight, NULL, 1, true, r->deterministic);
            } else if (d->kind == DE_ANIM_NODE_LAYER) {
                graph_node *base = &r->nodes[d->data.layer.base];
                n->valid = base->valid;
                if (!n->valid)
                    continue;
                copy_pose(&n->output.pose, &base->output.pose);
                for (u32 j = 0; n->valid && j < d->data.layer.layer_count; ++j) {
                    u32 l = d->data.layer.first_layer + j;
                    graph_layer *layer = &r->layers[l];
                    graph_node *input = &r->nodes[g->layers[l].node];
                    if (!input->valid || !(layer->weight > 0))
                        continue;
                    const de__anim_local_pose *pose = &input->output.pose;
                    bool additive = g->layers[l].mode == DE_ANIM_LAYER_ADDITIVE;
                    if (additive) {
                        n->valid = de__anim_pose_delta(&r->scratch.pose, pose,
                                                       &layer->reference.pose, r->deterministic);
                        pose = &r->scratch.pose;
                    }
                    n->valid = n->valid && de__anim_pose_layer(&n->output.pose, &n->output.pose,
                                                               pose, layer->weight, layer->mask,
                                                               g->layers[l].root_weight, additive,
                                                               r->deterministic);
                }
            }
        }
        graph_node *out = &r->nodes[g->desc.output_node];
        e->ready = out->valid;
        if (e->ready)
            copy_pose(&e->local, &out->output.pose);
    }
}
void de__anim_graph_blend(de_anim_system *s) {
    if (s->tasks && (!s->fixed_evaluation || s->tasks == de_world_task_system(s->world)))
        de_task_parallel_for(s->tasks, s->job_count, 32, blend_range, s);
    else
        blend_range(0, s->job_count, s);
}
void de__anim_graph_params_install(de_system_ctx *ctx) {
    de_anim_system *s = ctx->user;
    for (u32 i = 0; i < s->job_count; ++i) {
        de__anim_entry *e = &s->entries[s->jobs[i]];
        de__anim_graph_runtime *r = e->graph_runtime;
        if (r && r->params_missing) {
            DeAnimParams *p = de_cmd_add(ctx->commands, e->entity, s->params_component);
            if (p)
                *p = r->params;
        }
    }
}
