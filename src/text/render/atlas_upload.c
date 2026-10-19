#include "text_render_internal.h"
#if DE_HAS_RENDER
#include "text/fallback_internal.h"
#include <string.h>
typedef struct atlas_pass {
    struct de__text_render *r;
    u32 pc[8];
    de_rg_resource source, target;
} atlas_pass;
static void upload_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    atlas_pass *p = user;
    (void)pass;
    de_gpu_cmd_set_pipeline(cmd, p->r->upload_pipeline);
    de_gpu_cmd_push_constants(cmd, p->pc, sizeof p->pc);
    de_gpu_cmd_dispatch(cmd, (p->pc[6] + 7) / 8, (p->pc[7] + 7) / 8, 1);
}
void de__text_atlas_collect(struct de__text_render *r) {
    for (u32 i = 0; i < r->page_count;) {
        de__text_page *p = &r->pages[i];
        const de_text_font *font = de_text__font_get(r->text, p->font);
        if (font && font->identity == p->identity) {
            ++i;
            continue;
        }
        /* Unsent UI packets hold texture-generation leases; submitted work is retired by
         * the RHI queue fences. Neither needs this obsolete CPU atlas cache entry. */
        de_gpu_texture_destroy(r->device, p->texture);
        *p = r->pages[--r->page_count];
        memset(&r->pages[r->page_count], 0, sizeof *p);
    }
}
de__text_page *de__text_page_ensure(struct de__text_render *r, de_font handle, u32 page) {
    de__text_atlas_collect(r);
    de_text_font *f = de_text__font_get(r->text, handle);
    if (!f || page >= de_text__page_count(r->text, handle))
        return NULL;
    for (u32 i = 0; i < r->page_count; ++i)
        if (r->pages[i].font.id == handle.id && r->pages[i].identity == f->identity &&
            r->pages[i].page == page)
            return &r->pages[i];
    if (r->page_count == DE_TEXT_PAGE_CAP)
        return NULL;
    de_gpu_texture texture = de_gpu_texture_create(
        r->device,
        &(de_gpu_texture_desc){.dim = DE_TEX_2D,
                               .format = DE_FMT_RGBA8_UNORM,
                               .width = f->width,
                               .height = f->height,
                               .depth_or_layers = 1,
                               .mip_count = 1,
                               .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                               .name = "text atlas"});
    if (!texture.id)
        return NULL;
    de__text_page *p = &r->pages[r->page_count++];
    *p = (de__text_page){.font = handle,
                         .identity = f->identity,
                         .page = page,
                         .texture = texture,
                         .sampled = de_gpu_texture_bindless(r->device, texture, 0, false),
                         .storage = de_gpu_texture_bindless(r->device, texture, 0, true)};
    return p;
}
static bool upload_rect(struct de__text_render *r, de__view_frame *vf, de__text_page *page,
                        const de_text_dirty_rect *rect, const u8 *pixels, u32 font_width) {
    u64 bytes = (u64)rect->w * rect->h * 4;
    if (!bytes)
        return true;
    de_render_uploader *u = r->renderer->uploader;
    de__upload_stage stage = {0};
    u32 slot = (u32)(vf->frame_index % 3);
    if (u) {
        if (de__uploader_stage(u, bytes, 16, &stage) != DE_OK)
            return false;
    } else {
        u32 at = (r->cursor[slot] + 15) & ~15u;
        if (bytes > DE_TEXT_FRAME_BYTES - at)
            return false;
        stage.ptr = r->mapped[slot] + at;
        stage.offset = at;
        stage.ring = r->frames[slot];
        r->cursor[slot] = at + (u32)bytes;
    }
    for (u32 y = 0; y < rect->h; ++y)
        memcpy(stage.ptr + (u64)y * rect->w * 4,
               pixels + 4 * ((u64)(rect->y + y) * font_width + rect->x), rect->w * 4);
    atlas_pass *p = de_arena_push_zero(vf->graph->arena, sizeof *p, 16);
    if (!p) {
        if (u)
            de__uploader_stage_close(u, stage.record, (de_gpu_fence){0}, 0);
        return false;
    }
    p->r = r;
    p->source = de_rg_import_buffer(vf->graph, stage.ring, DE_ACCESS_SHADER_READ);
    p->target = de_rg_import_texture(vf->graph, page->texture,
                                     page->initialized ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE);
    p->pc[0] = de_gpu_buffer_bindless(r->device, stage.ring);
    p->pc[1] = page->storage;
    p->pc[2] = (u32)stage.offset;
    p->pc[3] = rect->w * 4;
    p->pc[4] = rect->x;
    p->pc[5] = rect->y;
    p->pc[6] = rect->w;
    p->pc[7] = rect->h;
    de_rg_pass *pass =
        de_rg_add_pass(vf->graph, "text.atlas_dirty", DE_QUEUE_GRAPHICS, upload_execute, p);
    if (!pass) {
        if (u)
            de__uploader_stage_close(u, stage.record, (de_gpu_fence){0}, 0);
        return false;
    }
    de_rg_read(pass, p->source, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, p->target, DE_ACCESS_SHADER_WRITE);
    de_rg_export(vf->graph, p->target, DE_ACCESS_SHADER_READ);
    if (u)
        de__uploader_stage_close(u, stage.record, r->renderer->frame_fence, vf->frame_index + 1);
    page->initialized = true;
    ++r->stats.uploads;
    r->stats.uploaded_bytes += bytes;
    return true;
}
void de__text_atlas_sync(struct de__text_render *r,
                         de__view_frame *vf) { /* Caller owns runtime+render locks; CPU pages cannot
                                                  change while copied. */
    for (u32 i = 0; i < r->page_count; ++i) {
        de__text_page *page = &r->pages[i];
        de_text_font *f = de_text__font_get(r->text, page->font);
        if (!f || f->identity != page->identity)
            continue;
        const u8 *pixels = de_text__page_data(r->text, page->font, page->page);
        if (!pixels)
            continue;
        if (!page->initialized) {
            de_text_dirty_rect full = {page->page, 0, 0, f->width, f->height};
            if (!upload_rect(r, vf, page, &full, pixels, f->width))
                continue;
            for (u32 j = 0; j < f->dirty_count;) {
                if (f->dirty[j].page == page->page) {
                    memmove(f->dirty + j, f->dirty + j + 1,
                            (f->dirty_count - j - 1) * sizeof *f->dirty);
                    --f->dirty_count;
                } else
                    ++j;
            }
        } else
            for (u32 j = 0; j < f->dirty_count;) {
                de_text_dirty_rect rect = f->dirty[j];
                if (rect.page == page->page && upload_rect(r, vf, page, &rect, pixels, f->width)) {
                    memmove(f->dirty + j, f->dirty + j + 1,
                            (f->dirty_count - j - 1) * sizeof *f->dirty);
                    --f->dirty_count;
                } else
                    ++j;
            }
    }
}
#endif
