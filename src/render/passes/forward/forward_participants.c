#include "forward_participants.h"
#include "render/renderer_internal.h"
#include "render/views_internal.h"
#include "render/world/render_world.h"
#include <math.h>
#include <string.h>

de_result de_render_add_forward_participant(de_renderer *r,
                                            const de_forward_participant_desc *desc) {
    if (!r || !desc || !desc->gather || !desc->build || !desc->max_items || desc->max_items > 65536)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid forward participant");
    de__render_world_wait_idle(r);
    for (u32 i = 0; i < r->forward_participant_count; ++i)
        if (r->forward_participants[i].gather == desc->gather &&
            r->forward_participants[i].user == desc->user)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "duplicate forward participant");
    if (r->forward_participant_count == 16 || r->forward_item_capacity > 65536 - desc->max_items)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "forward participant capacity exhausted");
    r->forward_participants[r->forward_participant_count++] = *desc;
    r->forward_item_capacity += desc->max_items;
    return DE_OK;
}
de_result de_render_remove_forward_participant(de_renderer *r, de_forward_gather_fn gather,
                                               void *user) {
    if (!r || !gather)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid forward participant removal");
    de__render_world_wait_idle(r);
    for (u32 i = 0; i < r->forward_participant_count; ++i) {
        if (r->forward_participants[i].gather != gather || r->forward_participants[i].user != user)
            continue;
        r->forward_item_capacity -= r->forward_participants[i].max_items;
        --r->forward_participant_count;
        memmove(&r->forward_participants[i], &r->forward_participants[i + 1],
                (r->forward_participant_count - i) * sizeof r->forward_participants[0]);
        de_gpu_wait_idle(r->dev);
        return DE_OK;
    }
    return DE_FAIL(DE_ERR_NOT_FOUND, "forward participant not registered");
}
bool de_render_forward_view_info(const de_forward_context *c, de_view_frame_info *out) {
    if (out)
        memset(out, 0, sizeof *out);
    if (!c || !out)
        return false;
    *out = c->info;
    return true;
}
de_rg_resource de_render_forward_color(const de_forward_context *c) {
    return c ? c->lighting->target : (de_rg_resource){0};
}
de_rg_resource de_render_forward_depth(const de_forward_context *c) {
    return c ? c->vis->depth : (de_rg_resource){0};
}
void de__forward_declare_lighting(de_rg_pass *p, const de__lighting_context *lc) {
    const de_rg_resource reads[] = {
        lc->lights,        lc->dir_lights,    lc->grid_buf,   lc->list,           lc->counters,
        lc->ltc1,          lc->ltc2,          lc->strip,      lc->shadow_records, lc->atlas_records,
        lc->atlas_texture, lc->probe_records, lc->probe_list, lc->gi_records};
    for (u32 i = 0; i < sizeof reads / sizeof reads[0]; ++i)
        if (reads[i].id)
            de_rg_read(p, reads[i], DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < lc->shadow_texture_count; ++i)
        de_rg_read(p, lc->shadow_textures[i], DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < lc->gi_texture_count; ++i) {
        de_rg_read(p, lc->gi_textures[i], DE_ACCESS_SHADER_READ);
        de_rg_write(p, lc->gi_textures[i], DE_ACCESS_SHADER_WRITE);
    }
    for (u32 i = 0; i < lc->gi_buffer_count; ++i)
        de_rg_read(p, lc->gi_buffers[i], DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < lc->probe_texture_count; ++i)
        de_rg_read(p, lc->probe_textures[i], DE_ACCESS_SHADER_READ);
}
void de_render_forward_declare(de_rg_pass *p, const de_forward_context *c, bool depth_test) {
    if (!p || !c)
        return;
    de_rg_read(p, c->lighting->target, DE_ACCESS_RENDER_TARGET);
    de_rg_write(p, c->lighting->target, DE_ACCESS_RENDER_TARGET);
    if (depth_test) {
        de_rg_read(p, c->vis->depth, DE_ACCESS_DEPTH_READ);
        de_rg_write(p, c->vis->depth, DE_ACCESS_DEPTH_READ | DE_ACCESS_DEPTH_WRITE);
    }
    de_rg_read(p, c->vis->scene_views, DE_ACCESS_SHADER_READ);
    const de_rg_resource tables[] = {c->vis->scene_instances, c->vis->scene_meshes,
                                     c->vis->scene_materials};
    for (u32 i = 0; i < 3; ++i)
        if (tables[i].id)
            de_rg_read(p, tables[i], DE_ACCESS_SHADER_READ);
    de__forward_declare_lighting(p, c->lighting);
}
static u32 buffer_index(const de_forward_context *c, const de_rg_pass *p, de_rg_resource b) {
    return b.id ? de_gpu_buffer_bindless(c->renderer->dev, de_rg_buffer(p, b)) : 0;
}
bool de_render_forward_bindings(const de_forward_context *c, const de_rg_pass *p,
                                de_forward_shader_bindings *out) {
    if (out)
        memset(out, 0, sizeof *out);
    if (!c || !p || !out)
        return false;
    const de__lighting_context *lc = c->lighting;
    *out = (de_forward_shader_bindings){.scene_views = buffer_index(c, p, c->vis->scene_views),
                                        .scene_view_slot = c->vis->scene_view_slot,
                                        .lighting_constants = lc->constants_bindless,
                                        .lighting_entry = lc->constants_entry,
                                        .lights = buffer_index(c, p, lc->lights),
                                        .dir_lights = buffer_index(c, p, lc->dir_lights),
                                        .grid = buffer_index(c, p, lc->grid_buf),
                                        .list = buffer_index(c, p, lc->list),
                                        .counters = buffer_index(c, p, lc->counters),
                                        .ltc_sampler = lc->sampler_bindless};
    out->ltc1 = lc->ltc1.id ? de_gpu_texture_bindless(c->renderer->dev, de_rg_texture(p, lc->ltc1),
                                                      0, false)
                            : 0;
    out->ltc2 = lc->ltc2.id ? de_gpu_texture_bindless(c->renderer->dev, de_rg_texture(p, lc->ltc2),
                                                      0, false)
                            : 0;
    return true;
}
de_result de__forward_collect(de_forward_context *c, de__transparent_item **items, u32 *count) {
    de_renderer *r = c->renderer;
    if (!r || !r->forward_participant_count)
        return DE_OK;
    c->participant_count = r->forward_participant_count;
    memcpy(c->participants, r->forward_participants,
           c->participant_count * sizeof c->participants[0]);
    const u32 capacity = r->forward_item_capacity;
    de__transparent_item *merged =
        de_arena_push(c->vis->arena, ((size_t)*count + capacity) * sizeof *merged, 16);
    c->items = de_arena_push(c->vis->arena, (size_t)capacity * sizeof *c->items, 16);
    if (!merged || !c->items)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "forward participant frame arena");
    if (*count)
        memcpy(merged, *items, (size_t)*count * sizeof *merged);
    u32 at = 0;
    for (u32 i = 0; i < c->participant_count; ++i) {
        const de_forward_participant_desc *d = &c->participants[i];
        memset(c->items + at, 0, (size_t)d->max_items * sizeof *c->items);
        const u32 n = d->gather(c, c->items + at, d->max_items, d->user);
        if (n > d->max_items)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "forward participant returned count over capacity");
        for (u32 j = 0; j < n; ++j) {
            const de_forward_item *item = &c->items[at + j];
            if (!isfinite(item->center_relative.x) || !isfinite(item->center_relative.y) ||
                !isfinite(item->center_relative.z))
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "non-finite forward participant center");
            de_vec3 view = de_m4_mul_point(&c->info.view_matrix, item->center_relative);
            merged[*count] = (de__transparent_item){.depth = -view.z,
                                                    .entity = item->entity,
                                                    .index = *count,
                                                    .layer = item->layer,
                                                    .order = item->order,
                                                    .participant = i + 1,
                                                    .participant_item = at + j,
                                                    .always_on_top = item->always_on_top};
            ++*count;
        }
        at += d->max_items;
    }
    *items = merged;
    return DE_OK;
}
