#include "draw_list.h"
#include "ui/world/world_internal.h"
#include <math.h>
#include <string.h>
#include <xxhash.h>
static de_vec4 full_clip(const de_ui *ui) {
    return (de_vec4){0, 0, (f32)ui->width, (f32)ui->height};
}
de_vec4 de_ui__clip_intersect(de_vec4 a, de_vec4 b) {
    f32 x = fmaxf(a.x, b.x), y = fmaxf(a.y, b.y);
    return (de_vec4){x, y, fmaxf(0, fminf(a.x + a.z, b.x + b.z) - x),
                     fmaxf(0, fminf(a.y + a.w, b.y + b.w) - y)};
}
u32 de_ui__nine_slice(de_vec4 r, de_vec4 uv, de_vec4 in, u32 width, u32 height, f32 scale,
                      de_ui_gpu_quad out[9]) {
    if (!width || !height || !(r.z > 0) || !(r.w > 0) || !(scale > 0))
        return 0;
    f32 sourcew = fabsf(uv.z - uv.x) * width, sourceh = fabsf(uv.w - uv.y) * height;
    f32 left = fmaxf(0, in.x), top = fmaxf(0, in.y), right = fmaxf(0, in.z),
        bottom = fmaxf(0, in.w);
    f32 ratio = fminf(1, sourcew / fmaxf(left + right, 1e-6f));
    left *= ratio;
    right *= ratio;
    ratio = fminf(1, sourceh / fmaxf(top + bottom, 1e-6f));
    top *= ratio;
    bottom *= ratio;
    f32 sx = fminf(scale, r.z / fmaxf(left + right, 1e-6f)),
        sy = fminf(scale, r.w / fmaxf(top + bottom, 1e-6f));
    f32 x[] = {r.x, r.x + left * sx, r.x + r.z - right * sx, r.x + r.z};
    f32 y[] = {r.y, r.y + top * sy, r.y + r.w - bottom * sy, r.y + r.w};
    f32 signx = uv.z >= uv.x ? 1 : -1, signy = uv.w >= uv.y ? 1 : -1;
    f32 u[] = {uv.x, uv.x + signx * left / width, uv.z - signx * right / width, uv.z};
    f32 v[] = {uv.y, uv.y + signy * top / height, uv.w - signy * bottom / height, uv.w};
    u32 count = 0;
    for (u32 row = 0; row < 3; ++row)
        for (u32 col = 0; col < 3; ++col)
            if (x[col + 1] > x[col] && y[row + 1] > y[row])
                out[count++] = (de_ui_gpu_quad){
                    .rect = {x[col], y[row], x[col + 1] - x[col], y[row + 1] - y[row]},
                    .uv = {u[col], v[row], u[col + 1], v[row + 1]},
                    .kind = DE_UI_QUAD_IMAGE};
    return count;
}
static bool less(const de_ui_draw_node *a, const de_ui_draw_node *b) {
    if (a->root != b->root)
        return a->root < b->root;
    if (a->popup != b->popup)
        return a->popup < b->popup;
    if (a->z != b->z)
        return a->z < b->z;
    return a->tree < b->tree;
}
static void sort_nodes(struct de_ui_render *r) {
    for (u32 width = 1; width < r->node_count; width *= 2) {
        for (u32 first = 0; first < r->node_count; first += width * 2) {
            u32 mid = first + width < r->node_count ? first + width : r->node_count;
            u32 end = mid + width < r->node_count ? mid + width : r->node_count, a = first, b = mid;
            for (u32 out = first; out < end; ++out)
                r->sort[out] = a < mid && (b == end || less(&r->nodes[a], &r->nodes[b]))
                                   ? r->nodes[a++]
                                   : r->nodes[b++];
        }
        de_ui_draw_node *tmp = r->nodes;
        r->nodes = r->sort;
        r->sort = tmp;
    }
}
static de_ui_draw_cmd *push(struct de_ui_render *r, const de_ui_draw_node *node) {
    u32 limit = (u32)de_cvar_int(r->max_quads);
    if (limit > r->capacity)
        limit = r->capacity;
    if (r->build.count >= limit) {
        ++r->stats.truncated;
        if (!r->overflow_logged) {
            DE_WARN(r->log, "UI quad capacity reached; truncating at%u", limit);
            r->overflow_logged = true;
        }
        return NULL;
    }
    de_ui_draw_cmd *cmd = &r->build.commands[r->build.count++];
    memset(cmd, 0, sizeof *cmd);
    cmd->root = node->root;
    cmd->clip = r->inherited[node->index].clip;
    ++r->build.roots[node->root].count;
    return cmd;
}
static de_vec4 tint_alpha(de_vec4 color, f32 opacity) {
    color.w *= opacity;
    return color;
}
void de_ui__set_widget_draw(de_ui *ui, de_ui_widget_draw_fn fn, void *user) {
    if (!ui || !ui->render)
        return;
    struct de_ui_render *r = ui->render;
    r->widget_draw = fn;
    r->widget_user = user;
}
void de_ui__emit_rect(de_ui_draw_emitter *emitter, de_vec4 rect, de_vec4 fill, de_vec4 border,
                      f32 border_width, f32 radius) {
    if (!emitter || !(rect.z > 0) || !(rect.w > 0))
        return;
    de_ui_draw_cmd *cmd = push(emitter->owner, emitter->node);
    if (!cmd)
        return;
    cmd->gpu = (de_ui_gpu_quad){.rect = rect,
                                .color = tint_alpha(fill, emitter->opacity),
                                .border = tint_alpha(border, emitter->opacity),
                                .params = {radius, border_width, 0, 0},
                                .kind = DE_UI_QUAD_RECT};
}
static void image(struct de_ui_render *r, const de_ui_draw_node *node, const DeUiImage *image,
                  de_vec4 rect, f32 opacity) {
    de_ui_gpu_quad quads[9];
    u32 count = 1;
    quads[0] = (de_ui_gpu_quad){.rect = rect, .uv = image->uv_rect, .kind = DE_UI_QUAD_IMAGE};
#if DE_HAS_RENDER
    if (image->nine_slice.x > 0 || image->nine_slice.y > 0 || image->nine_slice.z > 0 ||
        image->nine_slice.w > 0) {
        u32 width = 0, height = 0;
        if (de_render_texture_size(r->ui->desc.renderer, image->texture, &width, &height))
            count = de_ui__nine_slice(rect, image->uv_rect, image->nine_slice, width, height,
                                      r->ui->scale, quads);
    }
#endif
    for (u32 i = 0; i < count; ++i) {
        de_ui_draw_cmd *cmd = push(r, node);
        if (!cmd)
            return;
        cmd->gpu = quads[i];
        cmd->gpu.color = tint_alpha(image->tint, opacity);
        cmd->image = image->texture;
    }
}
void de_ui__set_text_geometry(de_ui *ui, de_ui_text_geometry_fn fn, void *user) {
    if (!ui || !ui->render)
        return;
    struct de_ui_render *r = ui->render;
    r->text_geometry = fn;
    r->text_geometry_user = user;
}
void de_ui__text_geometry(de_ui *ui, de_entity entity, f32 default_width, de_vec2 *offset,
                          f32 *wrap_width) {
    *offset = (de_vec2){0};
    *wrap_width = default_width;
    struct de_ui_render *r = ui->render;
    if (r && r->text_geometry)
        r->text_geometry(ui, entity, offset, wrap_width, r->text_geometry_user);
    if (!isfinite(offset->x) || !isfinite(offset->y))
        *offset = (de_vec2){0};
    if (!isfinite(*wrap_width) || *wrap_width < 0)
        *wrap_width = default_width;
}
static void text(struct de_ui_render *r, const de_ui_draw_node *node, const DeUiText *text,
                 const DeUiStyle *resolved, de_vec4 rect, f32 opacity) {
    de_ui *ui = r->ui;
    if (!ui->desc.text || !text->style.font.id)
        return;
    de_vec2 offset;
    f32 wrap_width;
    de_ui__text_geometry(ui, ui->records[node->index].entity, rect.z, &offset, &wrap_width);
    de_ui_text_cache *cache = &r->text_cache[node->index];
    de_str content = de_ui__text_content(ui, text);
    de_text_style style = text->style;
    style.size_px = (style.size_px > 0 ? style.size_px : 16) * ui->scale;
    style.letter_spacing *= ui->scale;
    de_font_metrics metrics = {0};
    (void)de_text_font_metrics(ui->desc.text, style.font, style.size_px, &metrics);
    u64 hash = XXH3_64bits(content.ptr, content.len) ^ XXH3_64bits(text, sizeof *text) ^
               XXH3_64bits(&ui->scale, sizeof ui->scale) ^
               XXH3_64bits(&wrap_width, sizeof wrap_width);
    if (cache->entity.bits != ui->records[node->index].entity.bits || cache->hash != hash ||
        cache->generation != metrics.generation || !cache->layout) {
        de_text_layout *layout = NULL;
        de_text_layout_desc desc = {.utf8 = content,
                                    .style = &style,
                                    .max_width = wrap_width,
                                    .align = text->align,
                                    .direction = DE_DIR_AUTO,
                                    .pixel_snap = ui->desc.pixel_art};
        if (de_text_layout_build(ui->desc.text, &desc, ui->alloc, &layout) != DE_OK)
            return;
        de_text_layout_free(cache->layout);
        *cache =
            (de_ui_text_cache){ui->records[node->index].entity, layout, hash, metrics.generation};
    }
    u32 count = de_text_layout_quads(cache->layout, r->glyphs, r->capacity);
    for (u32 i = 0; i < count; ++i) {
        de_ui_draw_cmd *cmd = push(r, node);
        if (!cmd)
            return;
        const de_glyph_quad *q = &r->glyphs[i];
        cmd->gpu = (de_ui_gpu_quad){
            .rect = {rect.x + offset.x + q->pos.x, rect.y + offset.y + q->pos.y, q->pos.z,
                     q->pos.w},
            .uv = q->uv,
            .color = tint_alpha(resolved ? resolved->text_color : q->color, opacity),
            .kind = DE_UI_QUAD_TEXT,
            .outline = style.outline_color,
            .shadow = style.shadow_color,
            .shadow_offset = {style.shadow_offset[0] * ui->scale,
                              style.shadow_offset[1] * ui->scale, 0, 0}};
        cmd->gpu.params.w = style.outline_width * ui->scale;
        cmd->font = q->font.id ? q->font : style.font;
        cmd->page = q->atlas_page;
    }
}
void de_ui__build_draw_list(de_system_ctx *ctx) {
    de_ui *ui = ctx->user;
    struct de_ui_render *r = ui->render;
    if (!r)
        return;
    const u64 start = de_time_now_ns();
    r->build.count = r->build.root_count = r->node_count = 0;
    r->stats.truncated = 0;
    de_component panel = de_component_find(ui->world, "DeUiWorldPanel");
    for (u32 root = 0; root < ui->record_count; ++root) {
        de_ui_record *record = &ui->records[root];
        if (!record->alive || de_ui__find(ui, record->parent) != DE_UI_NONE_INDEX)
            continue;
        u32 ordinal = r->build.root_count++;
        r->build.roots[ordinal] =
            (de_ui_root_list){.entity = record->entity,
                              .world = panel.id && de_entity_has(ui->world, record->entity, panel)};
        u32 index = root;
        for (;;) {
            de_ui_record *v = &ui->records[index];
            u32 parent = index == root ? DE_UI_NONE_INDEX : de_ui__find(ui, v->parent);
            de_ui_inherited inherited =
                parent == DE_UI_NONE_INDEX
                    ? (de_ui_inherited){r->build.roots[ordinal].world ? record->node.computed_rect
                                                                      : full_clip(ui),
                                        1, 0}
                    : r->inherited[parent];
            const DeUiStyle *style = de_entity_get_const(ui->world, v->entity, ui->ids.style);
            if (style)
                inherited.opacity *= fmaxf(0, fminf(1, style->opacity));
            if (v->node.clip)
                inherited.clip = de_ui__clip_intersect(inherited.clip, v->node.computed_rect);
            if (de_entity_has(ui->world, v->entity, ui->ids.popup))
                ++inherited.popup;
            r->inherited[index] = inherited;
            if (v->node.display != DE_UI_NONE && v->node.computed_rect.z > 0 &&
                v->node.computed_rect.w > 0 && inherited.clip.z > 0 && inherited.clip.w > 0) {
                u32 tree = r->node_count;
                r->nodes[r->node_count++] =
                    (de_ui_draw_node){index, ordinal, inherited.popup, tree, v->node.z_order};
            }
            if (v->first_child != DE_UI_NONE_INDEX) {
                index = v->first_child;
                continue;
            }
            while (index != root && ui->records[index].next_sibling == DE_UI_NONE_INDEX)
                index = de_ui__find(ui, ui->records[index].parent);
            if (index == root)
                break;
            index = ui->records[index].next_sibling;
        }
    }
    sort_nodes(r);
    f32 screen_scale = ui->scale;
    for (u32 i = 0; i < r->node_count; ++i) {
        const de_ui_draw_node *node = &r->nodes[i];
        const de_ui_record *record = &ui->records[node->index];
        de_ui_root_list *root = &r->build.roots[node->root];
        ui->scale = root->world ? 1 : screen_scale;
        if (!root->count)
            root->first = r->build.count;
        de_vec4 rect = record->node.computed_rect;
        f32 opacity = r->inherited[node->index].opacity;
        const DeUiStyle *style = de_entity_get_const(ui->world, record->entity, ui->ids.style);
        if (style &&
            (style->background.w > 0 || (style->border_width > 0 && style->border_color.w > 0))) {
            de_ui_draw_cmd *cmd = push(r, node);
            if (cmd)
                cmd->gpu = (de_ui_gpu_quad){.rect = rect,
                                            .color = tint_alpha(style->background, opacity),
                                            .border = tint_alpha(style->border_color, opacity),
                                            .params = {style->corner_radius * ui->scale,
                                                       style->border_width * ui->scale, 0, 0},
                                            .kind = DE_UI_QUAD_RECT};
        }
        if (style && style->background_image.id) {
            DeUiImage background = {.texture = style->background_image,
                                    .uv_rect = {0, 0, 1, 1},
                                    .nine_slice = style->nine_slice,
                                    .tint = {1, 1, 1, 1}};
            image(r, node, &background, rect, opacity);
        }
        const DeUiImage *im = de_entity_get_const(ui->world, record->entity, ui->ids.image);
        if (im)
            image(r, node, im, rect, opacity);
        if (r->widget_draw) {
            de_ui_draw_emitter emitter = {r, node, opacity};
            r->widget_draw(ui, record->entity, rect, &emitter, r->widget_user);
        }
        const DeUiText *label = de_entity_get_const(ui->world, record->entity, ui->ids.text);
        if (label)
            text(r, node, label, style, rect, opacity);
    }
    ui->scale = screen_scale;
    if (ui->desc.pixel_art) {
        for (u32 i = 0; i < r->build.count; ++i) {
            de_ui__pixel_snap_rect(&r->build.commands[i].gpu.rect);
            de_ui__pixel_snap_rect(&r->build.commands[i].clip);
        }
    }
    ++r->build.generation;
    r->stats.quads = r->build.count;
    r->stats.roots = r->build.root_count;
    r->stats.build_extract_ms = (f32)(de_time_now_ns() - start) / 1e6f;
}

u32 de_ui__draw_order(const de_ui *ui, const de_ui_draw_node **nodes,
                      const de_ui_inherited **inherited) {
    if (nodes)
        *nodes = NULL;
    if (inherited)
        *inherited = NULL;
    if (!ui || !ui->render)
        return 0;
    const struct de_ui_render *r = ui->render;
    if (nodes)
        *nodes = r->nodes;
    if (inherited)
        *inherited = r->inherited;
    return r->node_count;
}
bool de_ui__root_info(const de_ui *ui, u32 ordinal, de_entity *entity, de_vec4 *rect, bool *world) {
    if (!ui || !ui->render)
        return false;
    const struct de_ui_render *r = ui->render;
    if (ordinal >= r->build.root_count)
        return false;
    const de_ui_root_list *root = &r->build.roots[ordinal];
    u32 index = de_ui__find(ui, root->entity);
    if (index == DE_UI_NONE_INDEX)
        return false;
    if (entity)
        *entity = root->entity;
    if (rect)
        *rect = ui->records[index].node.computed_rect;
    if (world)
        *world = root->world;
    return true;
}
const de_ui_draw_list *de_ui__packet_for_frame(const struct de_ui_render *r, u64 token) {
    for (u32 i = 0; i < DE_UI_RENDER_SNAPSHOTS; ++i)
        if (r->snapshots[i].valid && r->snapshots[i].generation == token)
            return &r->snapshots[i];
    return NULL;
}
