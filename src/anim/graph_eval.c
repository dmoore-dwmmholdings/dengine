#include "anim/graph_internal.h"
#include "anim/anim_math.h"
#include "asset/slots_internal.h"
#include "platform/de_atomic.h"
#include <string.h>

/* Generation zero is invalid for asset slots. IDs are never reused, even across
 * animation systems, so stale/foreign programmatic handles cannot alias. */
static de_atomic_u32 graph_ids;
static u32 graph_id(void) {
    u32 current = de_atomic_load_u32(&graph_ids, DE_ATOMIC_RELAXED);
    while (current < DE__SLOT_INDEX_MASK)
        if (de_atomic_cas_u32(&graph_ids, &current, current + 1, DE_ATOMIC_RELAXED,
                              DE_ATOMIC_RELAXED))
            return current + 1;
    return 0;
}
de__anim_graph *de__anim_graph_get(const de_anim_system *s, de_anim_graph handle) {
    if (!s || !handle.id || de__slot_handle_generation(handle.id))
        return NULL;
    for (de__anim_graph *g = s->graphs; g; g = g->next)
        if (g->handle.id == handle.id)
            return g;
    return NULL;
}
static i32 param_index(const de_anim_graph_desc *d, de_name name, de_anim_param_kind kind) {
    for (u32 i = 0; i < d->param_count; ++i)
        if (d->params[i].name.hash == name.hash && d->params[i].kind == kind)
            return (i32)i;
    return -1;
}
i32 de__anim_graph_param(const de__anim_graph *g, de_name name, de_anim_param_kind kind) {
    return g ? param_index(&g->desc, name, kind) : -1;
}
static bool weight_valid(const de_anim_graph_desc *d, de_name name, f32 weight) {
    return weight >= 0 && weight <= 1 &&
           (!name.hash || param_index(d, name, DE_ANIM_PARAM_FLOAT) >= 0);
}
static bool clip_valid(const de_anim_system *s, de_anim_clip clip) {
    const de__slot *slot = s->assets ? de__slot_get(s->assets, clip.id) : NULL;
    return slot && slot->type_hash == de_hash64_str("anim_clip");
}
static bool additive_valid(const de_anim_system *s, const de_anim_graph_desc *d, u32 input,
                           de_anim_clip reference) {
    return reference.id ? clip_valid(s, reference) : d->nodes[input].kind == DE_ANIM_NODE_CLIP;
}
static de_result validate(const de_anim_system *s, const de_anim_graph_desc *d, u32 *point_count,
                          u32 *mask_count) {
    if (!s || !d || !d->nodes || !d->node_count || d->node_count > 1024 ||
        d->output_node >= d->node_count || d->param_count > DE_ANIM_MAX_PARAMS ||
        (d->param_count && !d->params) || d->layer_count > 256 || (d->layer_count && !d->layers))
        return DE_ERR_INVALID_ARGUMENT;
    for (u32 i = 0; i < d->param_count; ++i) {
        const de_anim_param_desc *p = &d->params[i];
        if (!p->name.hash || (u32)p->kind > DE_ANIM_PARAM_TRIGGER ||
            (p->kind == DE_ANIM_PARAM_FLOAT && !de__anim_finite(p->float_default)))
            return DE_ERR_INVALID_ARGUMENT;
        for (u32 j = 0; j < i; ++j)
            if (d->params[j].name.hash == p->name.hash)
                return DE_ERR_INVALID_ARGUMENT;
    }
    u64 points = 0, masks = 0;
    for (u32 i = 0; i < d->layer_count; ++i) {
        const de_anim_layer_desc *l = &d->layers[i];
        if (l->node >= d->node_count || (u32)l->mode > DE_ANIM_LAYER_ADDITIVE ||
            !weight_valid(d, l->weight_param, l->weight) ||
            !(l->root_weight >= 0 && l->root_weight <= 1) || (l->mask_count && !l->mask) ||
            l->mask_count > 65535)
            return DE_ERR_INVALID_ARGUMENT;
        if (l->mode == DE_ANIM_LAYER_ADDITIVE && !additive_valid(s, d, l->node, l->reference))
            return DE_ERR_INVALID_ARGUMENT;
        masks += l->mask_count;
        if (masks > 65535)
            return DE_ERR_INVALID_ARGUMENT;
        for (u32 j = 0; j < l->mask_count; ++j)
            if (!l->mask[j].bone.hash || !(l->mask[j].weight >= 0 && l->mask[j].weight <= 1))
                return DE_ERR_INVALID_ARGUMENT;
    }
    for (u32 i = 0; i < d->node_count; ++i) {
        const de_anim_node_desc *n = &d->nodes[i];
        switch (n->kind) {
        case DE_ANIM_NODE_CLIP:
            if (!clip_valid(s, n->data.clip.clip) || !de__anim_finite(n->data.clip.speed))
                return DE_ERR_INVALID_ARGUMENT;
            break;
        case DE_ANIM_NODE_BLEND1D:
        case DE_ANIM_NODE_BLEND2D:
            if (param_index(d, n->data.blend.x_param, DE_ANIM_PARAM_FLOAT) < 0 ||
                (n->kind == DE_ANIM_NODE_BLEND2D &&
                 param_index(d, n->data.blend.y_param, DE_ANIM_PARAM_FLOAT) < 0) ||
                !n->data.blend.points || !n->data.blend.point_count ||
                n->data.blend.point_count > 65535)
                return DE_ERR_INVALID_ARGUMENT;
            points += n->data.blend.point_count;
            if (points > 65535)
                return DE_ERR_INVALID_ARGUMENT;
            for (u32 j = 0; j < n->data.blend.point_count; ++j)
                if (n->data.blend.points[j].node >= i)
                    return DE_ERR_INVALID_ARGUMENT;
            break;
        case DE_ANIM_NODE_ADDITIVE:
            if (n->data.additive.base >= i || n->data.additive.input >= i ||
                !weight_valid(d, n->data.additive.weight_param, n->data.additive.weight) ||
                !additive_valid(s, d, n->data.additive.input, n->data.additive.reference))
                return DE_ERR_INVALID_ARGUMENT;
            break;
        case DE_ANIM_NODE_LAYER:
            if (n->data.layer.base >= i || n->data.layer.first_layer > d->layer_count ||
                n->data.layer.layer_count > d->layer_count - n->data.layer.first_layer)
                return DE_ERR_INVALID_ARGUMENT;
            for (u32 j = 0; j < n->data.layer.layer_count; ++j)
                if (d->layers[n->data.layer.first_layer + j].node >= i)
                    return DE_ERR_INVALID_ARGUMENT;
            break;
        case DE_ANIM_NODE_STATE_MACHINE:
        default:
            return DE_ERR_INVALID_ARGUMENT;
        }
    }
    *point_count = (u32)points;
    *mask_count = (u32)masks;
    return DE_OK;
}
static size_t section(size_t *offset, size_t bytes, size_t align) {
    size_t start = (*offset + align - 1) & ~(align - 1);
    *offset = start + bytes;
    return start;
}
static bool retain_clip(de_anim_system *s, de__anim_graph *g, de_anim_clip clip) {
    if (!clip.id)
        return true;
    const de__slot *slot = de__slot_get(s->assets, clip.id);
    de_load_handle ref =
        slot ? de_asset_load(s->assets, slot->id, DE_LOAD_NO_EVENTS) : (de_load_handle){0};
    if (!ref.id)
        return false;
    g->references[g->reference_count++] = ref;
    return true;
}
static void release_graph(de_anim_system *s, de__anim_graph *g) {
    for (u32 i = 0; i < g->desc.node_count; ++i)
        de__anim_blend_space_destroy(g->spaces[i]);
    for (u32 i = 0; i < g->reference_count; ++i)
        de_asset_release(s->assets, g->references[i]);
    DE_FREE(s->alloc, g, g->bytes);
}
de_result de_anim_graph_create(de_anim_system *s, const de_anim_graph_desc *d, de_anim_graph *out) {
    if (!out)
        return DE_ERR_INVALID_ARGUMENT;
    *out = (de_anim_graph){0};
    u32 points = 0, masks = 0;
    de_result result = validate(s, d, &points, &masks);
    if (result != DE_OK)
        return DE_FAIL(result, "invalid animation graph descriptor");
    size_t bytes = sizeof(de__anim_graph);
    size_t params_at =
        section(&bytes, (size_t)d->param_count * sizeof *d->params, DE_ALIGNOF(de_anim_param_desc));
    size_t nodes_at =
        section(&bytes, (size_t)d->node_count * sizeof *d->nodes, DE_ALIGNOF(de_anim_node_desc));
    size_t layers_at =
        section(&bytes, (size_t)d->layer_count * sizeof *d->layers, DE_ALIGNOF(de_anim_layer_desc));
    size_t spaces_at = section(&bytes, (size_t)d->node_count * sizeof(de__anim_blend_space *),
                               DE_ALIGNOF(de__anim_blend_space *));
    size_t refs_at =
        section(&bytes, ((size_t)d->node_count * 2 + d->layer_count) * sizeof(de_load_handle),
                DE_ALIGNOF(de_load_handle));
    size_t points_at = section(&bytes, (size_t)points * sizeof(de_anim_blend_point_desc),
                               DE_ALIGNOF(de_anim_blend_point_desc));
    size_t masks_at = section(&bytes, (size_t)masks * sizeof(de_anim_bone_mask_desc),
                              DE_ALIGNOF(de_anim_bone_mask_desc));
    de__anim_graph *g = DE_ALLOC(s->alloc, bytes, 16, DE_MEM_GENERAL);
    if (!g)
        return DE_ERR_OUT_OF_MEMORY;
    memset(g, 0, bytes);
    u8 *base = (u8 *)g;
    g->bytes = bytes;
    g->params = (de_anim_param_desc *)(base + params_at);
    g->nodes = (de_anim_node_desc *)(base + nodes_at);
    g->layers = (de_anim_layer_desc *)(base + layers_at);
    g->spaces = (de__anim_blend_space **)(base + spaces_at);
    g->references = (de_load_handle *)(base + refs_at);
    de_anim_blend_point_desc *point = (de_anim_blend_point_desc *)(base + points_at);
    de_anim_bone_mask_desc *mask = (de_anim_bone_mask_desc *)(base + masks_at);
    g->desc = *d;
    g->desc.nodes = g->nodes;
    g->desc.params = g->params;
    g->desc.layers = g->layers;
    if (d->param_count)
        memcpy(g->params, d->params, (size_t)d->param_count * sizeof *d->params);
    memcpy(g->nodes, d->nodes, (size_t)d->node_count * sizeof *d->nodes);
    if (d->layer_count)
        memcpy(g->layers, d->layers, (size_t)d->layer_count * sizeof *d->layers);
    for (u32 i = 0; i < d->layer_count; ++i) {
        de_anim_layer_desc *l = &g->layers[i];
        if (l->mask_count)
            memcpy(mask, l->mask, (size_t)l->mask_count * sizeof *mask);
        l->mask = mask;
        mask += l->mask_count;
        if (l->mode == DE_ANIM_LAYER_ADDITIVE && !retain_clip(s, g, l->reference)) {
            result = DE_ERR_OUT_OF_MEMORY;
            goto failed;
        }
    }
    for (u32 i = 0; i < d->node_count; ++i) {
        de_anim_node_desc *n = &g->nodes[i];
        if (n->kind == DE_ANIM_NODE_BLEND1D || n->kind == DE_ANIM_NODE_BLEND2D) {
            u32 count = n->data.blend.point_count;
            memcpy(point, n->data.blend.points, (size_t)count * sizeof *point);
            n->data.blend.points = point;
            de__anim_blend_point *converted =
                DE_ALLOC(s->alloc, (size_t)count * sizeof *converted,
                         DE_ALIGNOF(de__anim_blend_point), DE_MEM_SCRATCH);
            if (!converted) {
                result = DE_ERR_OUT_OF_MEMORY;
                goto failed;
            }
            for (u32 j = 0; j < count; ++j)
                converted[j] = (de__anim_blend_point){point[j].position, point[j].node};
            result = de__anim_blend_space_create(s->alloc, n->kind == DE_ANIM_NODE_BLEND1D ? 1 : 2,
                                                 converted, count, &g->spaces[i]);
            DE_FREE(s->alloc, converted, (size_t)count * sizeof *converted);
            if (result != DE_OK)
                goto failed;
            point += count;
        } else if (n->kind == DE_ANIM_NODE_CLIP) {
            if (!retain_clip(s, g, n->data.clip.clip)) {
                result = DE_ERR_OUT_OF_MEMORY;
                goto failed;
            }
        } else if (n->kind == DE_ANIM_NODE_ADDITIVE &&
                   !retain_clip(s, g, n->data.additive.reference)) {
            result = DE_ERR_OUT_OF_MEMORY;
            goto failed;
        }
    }
    g->handle.id = graph_id();
    if (!g->handle.id) {
        result = DE_ERR_OUT_OF_MEMORY;
        goto failed;
    }
    g->next = s->graphs;
    s->graphs = g;
    *out = g->handle;
    return DE_OK;
failed:
    release_graph(s, g);
    return DE_FAIL(result, "animation graph build failed");
}
void de_anim_graph_destroy(de_anim_system *s, de_anim_graph handle) {
    if (!s)
        return;
    de__anim_graph **link = &s->graphs;
    while (*link && (*link)->handle.id != handle.id)
        link = &(*link)->next;
    if (*link) {
        de__anim_graph *g = *link;
        for (u32 i = 0; i < s->capacity; ++i)
            if (s->entries[i].graph_handle == handle.id) {
                de__anim_graph_runtime_release(s, &s->entries[i]);
                s->entries[i].ready = s->entries[i].finalized = false;
            }
        *link = g->next;
        release_graph(s, g);
    }
}
void de__anim_graphs_shutdown(de_anim_system *s) {
    while (s->graphs)
        de_anim_graph_destroy(s, s->graphs->handle);
}
