#include "text_render_internal.h"
#if DE_HAS_RENDER
#include "text/shape/shaper_internal.h"
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
/* DENG-261 replaces only this single-line function with paragraph layout. */
static void world_layout(struct de__text_render *r, const DeWorldText *text,
                         const DeGlobalTransform *transform) {
    de_str str = text_string(r, text);
    if (!str.len)
        return;
    f32 size = text->size_m > 0 ? text->size_m : 1, x = 0, width = 0;
    u32 at = 0;
    while (at < str.len) {
        u32 cp = de__text_utf8(str.ptr, (u32)str.len, &at), glyph;
        de_font font;
        (void)de_text__resolve(r->text, text->style.font, cp, &font, &glyph);
        de_text_glyph_entry e;
        (void)de_text__glyph(r->text, font, glyph, &e);
        de_text_font *f = de_text__font_get(r->text, font);
        f32 scale = f && f->mode != DE_FONT_MSDF ? size / f->em : size;
        width += e.advance * scale;
    }
    if (text->align == DE_ALIGN_CENTER)
        x = -width * .5f;
    else if (text->align == DE_ALIGN_END)
        x = -width;
    de_affine basis = de_transform_to_affine_relative(transform, (de_dvec3){0});
    at = 0;
    while (at < str.len) {
        u32 cp = de__text_utf8(str.ptr, (u32)str.len, &at), glyph;
        de_font font;
        (void)de_text__resolve(r->text, text->style.font, cp, &font, &glyph);
        de_text_glyph_entry e;
        bool ready = de_text__glyph(r->text, font, glyph, &e);
        de_text_font *f = de_text__font_get(r->text, font);
        f32 scale = f && f->mode != DE_FONT_MSDF ? size / f->em : size;
        if (ready && e.w && e.h) {
            de_mutex_lock(&r->mutex);
            de__text_queue *q = &r->queues[r->current_frame % 3];
            if (q->world_count == DE_TEXT_DRAW_CAP) {
                de_mutex_unlock(&r->mutex);
                break;
            }
            de__text_item *it = &q->world[q->world_count++];
            memset(it, 0, sizeof *it);
            it->font = font;
            it->page = e.page;
            it->position = transform->position;
            it->gpu.rect =
                (de_vec4){x + e.plane.x * scale, e.plane.y * scale, (e.plane.z - e.plane.x) * scale,
                          (e.plane.w - e.plane.y) * scale};
            it->gpu.uv = e.uv;
            it->gpu.color = text->style.color;
            it->gpu.outline = text->style.outline_color;
            it->gpu.shadow = text->style.shadow_color;
            it->gpu.outline_width = text->style.outline_width;
            it->gpu.shadow_x = text->style.shadow_offset[0];
            it->gpu.shadow_y = text->style.shadow_offset[1];
            it->gpu.flags =
                (text->billboard ? 1u : 0u) | (text->lit ? 2u : 0u) | (text->depth_test ? 4u : 0u);
            it->gpu.right = (de_vec4){basis.m[0], basis.m[1], basis.m[2], 0};
            it->gpu.up = (de_vec4){basis.m[3], basis.m[4], basis.m[5], 0};
            de_mutex_unlock(&r->mutex);
        }
        x += e.advance * scale;
    }
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
