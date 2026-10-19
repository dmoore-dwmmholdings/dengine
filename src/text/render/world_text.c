#include "text_render_internal.h"
#if DE_HAS_RENDER
#include "text/layout/layout_internal.h"
#include "text/fallback_internal.h"
#include <string.h>
static de_str text_string(struct de__text_render *r, const DeWorldText *t) {
    if (t->loc_id.hash && r->text->localize) {
        de_str s = r->text->localize(r->text, t->loc_id);
        if (s.ptr)
            return s;
    }
    const char *end = memchr(t->literal, 0, sizeof t->literal);
    return (de_str){t->literal, end ? (size_t)(end - t->literal) : sizeof t->literal};
}
/* Shared immutable paragraph layout supplies fallback-aware quads. */
static void world_layout(struct de__text_render *r, const DeWorldText *text,
                         const DeGlobalTransform *transform) {
    de_str str = text_string(r, text);
    if (!str.len)
        return;
    de_text_style style = text->style;
    style.size_px = text->size_m > 0 ? text->size_m : 1;
    de_text_layout *layout = NULL;
    de_text_layout_desc desc = {
        .utf8 = str, .style = &style, .align = text->align, .direction = DE_DIR_AUTO};
    if (de_text_layout_build(r->text, &desc, r->text->alloc, &layout) != DE_OK)
        return;
    f32 x = text->align == DE_ALIGN_CENTER ? -layout->size.x * .5f
            : text->align == DE_ALIGN_END  ? -layout->size.x
                                           : 0;
    de_affine basis = de_transform_to_affine_relative(transform, (de_dvec3){0});
    de_mutex_lock(&r->mutex);
    de__text_queue *queue = &r->queues[r->current_frame % 3];
    for (u32 i = 0; i < layout->quad_count && queue->world_count < DE_TEXT_DRAW_CAP; ++i) {
        const de_glyph_quad *quad = &layout->quads[i];
        de__text_item *item = &queue->world[queue->world_count++];
        memset(item, 0, sizeof *item);
        item->font = quad->font;
        item->page = quad->atlas_page;
        item->position = transform->position;
        item->gpu.rect = (de_vec4){x + quad->pos.x, layout->ascent - quad->pos.y - quad->pos.w,
                                   quad->pos.z, quad->pos.w};
        item->gpu.uv = quad->uv;
        item->gpu.color = quad->color;
        item->gpu.outline = style.outline_color;
        item->gpu.shadow = style.shadow_color;
        item->gpu.outline_width = style.outline_width;
        item->gpu.shadow_x = style.shadow_offset[0];
        item->gpu.shadow_y = style.shadow_offset[1];
        item->gpu.flags =
            (text->billboard ? 1u : 0u) | (text->lit ? 2u : 0u) | (text->depth_test ? 4u : 0u);
        item->gpu.right = (de_vec4){basis.m[0], basis.m[1], basis.m[2], 0};
        item->gpu.up = (de_vec4){basis.m[3], basis.m[4], basis.m[5], 0};
    }
    de_mutex_unlock(&r->mutex);
    de_text_layout_free(layout);
}
static void extract_chunk(const de_query_chunk *c, void *user) {
    struct de__text_render *r = user;
    const DeGlobalTransform *g = c->columns[0];
    const DeWorldText *t = c->columns[1];
    for (u32 i = 0; i < c->count; ++i)
        world_layout(r, &t[i], &g[i]);
}
static void extract(de_system_ctx *ctx) {
    struct de__text_render *r = ctx->user;
    de_query_each(ctx->world, ctx->queries[0], extract_chunk, r);
}
de_result de__text_world_attach(struct de__text_render *r, de_world *w) {
    if (!w)
        return DE_OK;
    r->world = w;
    if (r->renderer->world != w)
        de_renderer_attach(r->renderer, w);
    de_type_registry *types = de_world_types(w);
    de_type_id type = de_type_find(types, "DeWorldText");
    if (!type.id) {
        de_type_desc d = {.name = "DeWorldText",
                          .size = sizeof(DeWorldText),
                          .align = _Alignof(DeWorldText),
                          .version = 1,
                          .is_pod = true};
        DE_TRY(de_type_register_ex(types, &d, &type, NULL));
    }
    r->component = de_component_register(w, type, 0);
    if (!r->component.id)
        return DE_ERR_INTERNAL;
    de_component transform = de_component_find(w, "DeGlobalTransform");
    if (!transform.id)
        return DE_ERR_NOT_FOUND;
    de_query_term terms[] = {{transform, DE_READ}, {r->component, DE_READ}};
    de_query_desc query = {.terms = terms, .term_count = 2};
    static const char *const after[] = {"de.render.extract_views"};
    r->extract = de_system_add(w, &(de_system_desc){.name = "de.text.extract_world",
                                                    .schedule = DE_SCHEDULE_EXTRACT,
                                                    .fn = extract,
                                                    .user = r,
                                                    .queries = &query,
                                                    .query_count = 1,
                                                    .after = after,
                                                    .after_count = 1});
    return r->extract.id ? DE_OK : DE_ERR_INTERNAL;
}
void de__text_world_detach(struct de__text_render *r) {
    if (r->world && r->extract.id)
        de_system_remove(r->world, r->extract);
    r->extract = (de_system){0};
    r->world = NULL;
}
#endif
