/*
 * composite.c: the view composite (SPEC §12.4, §12.9; DENG-176). A graphics pass that draws
 * sampled textures (view outputs) into pixel rectangles of a destination with bilinear filtering:
 * the output graph composites every swapchain view, and a render-to-texture view composites its
 * output into its target. Declared in views_internal.h.
 *
 * Shaders: the engine shader "composite" (shaders/common/composite.slang: vs_main, ps_main),
 * loaded at renderer create through de__engine_shader_load (DENG-421, shaders/engine_shaders.h):
 * cooked in-process through the cache in Dev builds, the embedded build-time cook otherwise, a
 * placeholder on the null backend. Without code for the backend (a cross build without the
 * build-time cook, DXIL without DXC) the composite is not ready: composite passes only clear their
 * destination, and a warning says so.
 *
 * Pipelines: one per destination colour format, made at first use while a graph is built (render
 * thread), at most DE__COMPOSITE_MAX_FORMATS.
 */
#include "render/views_internal.h"

#include "render/renderer_internal.h"
#include "render/graph/rg_internal.h"
#include "render/shaders/engine_shaders.h"

#include <stdio.h>
#include <string.h>

/** Destination formats the composite keeps a pipeline for. */
#define DE__COMPOSITE_MAX_FORMATS 8u
/** Push constants of composite.slang: the source's and the sampler's bindless indices. */
#define DE__COMPOSITE_PUSH_BYTES 8u

struct de__composite {
    de_renderer *r;
    de_gpu_device *dev;
    const de_allocator *alloc;
    bool ready;
    de__engine_shader_code code; /**< vs_main, ps_main (DENG-421) */
    de_gpu_shader_blob vs, ps;
    u32 push_bytes;
    de_gpu_sampler sampler;
    u32 sampler_index;
    struct {
        de_gpu_format format;
        de_gpu_pipeline pipeline; /**< invalid when creation failed (not retried) */
    } pipes[DE__COMPOSITE_MAX_FORMATS];
    u32 pipe_count;
    bool warned_pipe; /**< a pipeline failed or the table is full: logged once */
};

de_result de__composite_create(de_renderer *r, de__composite **out) {
    *out = NULL;
    de__composite *c = DE_NEW(r->alloc, de__composite, DE_MEM_RENDER);
    if (c == NULL)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "composite");
    memset(c, 0, sizeof *c);
    c->r = r;
    c->dev = r->dev;
    c->alloc = r->alloc;
    c->sampler = de_gpu_sampler_create(r->dev, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                                      .mag = DE_FILTER_LINEAR,
                                                                      .mip = DE_FILTER_NEAREST,
                                                                      .u = DE_ADDR_CLAMP,
                                                                      .v = DE_ADDR_CLAMP,
                                                                      .w = DE_ADDR_CLAMP});
    if (!DE_HANDLE_VALID(c->sampler)) {
        de__composite_destroy(r, c);
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "composite: sampler");
    }
    c->sampler_index = de_gpu_sampler_bindless(r->dev, c->sampler);

    char why[768] = "";
    static const char *const k_entries[2] = {"vs_main", "ps_main"};
    if (de__engine_shader_load(r->dev, r->alloc, "composite", 0, k_entries, 2, &c->code, why,
                               sizeof why) == DE_OK) {
        c->vs = c->code.entries[0].blob;
        c->ps = c->code.entries[1].blob;
        c->push_bytes = c->code.push_constant_bytes != 0 ? c->code.push_constant_bytes
                                                         : DE__COMPOSITE_PUSH_BYTES;
        c->ready = true;
    }
    if (!c->ready)
        DE_WARN(r->log,
                "renderer: no view composite (%s): views render, but the output only clears and "
                "render-to-texture targets stay unwritten",
                why);
    *out = c;
    return DE_OK;
}

void de__composite_destroy(de_renderer *r, de__composite *c) {
    if (c == NULL)
        return;
    for (u32 i = 0; i < c->pipe_count; ++i)
        if (DE_HANDLE_VALID(c->pipes[i].pipeline))
            de_gpu_pipeline_destroy(r->dev, c->pipes[i].pipeline);
    if (DE_HANDLE_VALID(c->sampler))
        de_gpu_sampler_destroy(r->dev, c->sampler);
    de__engine_shader_code_free(&c->code);
    DE_FREE(c->alloc, c, sizeof *c);
}

bool de__composite_ready(const de__composite *c) {
    return c != NULL && c->ready;
}

u32 de__composite_shader_origin(const de__composite *c) {
    return c != NULL ? (u32)c->code.origin : 0u;
}

/* The pipeline for `format`, made at first use; invalid when it cannot be made. */
static de_gpu_pipeline pipeline_for(de__composite *c, de_gpu_format format) {
    for (u32 i = 0; i < c->pipe_count; ++i)
        if (c->pipes[i].format == format)
            return c->pipes[i].pipeline;
    if (c->pipe_count == DE__COMPOSITE_MAX_FORMATS) {
        if (!c->warned_pipe)
            DE_ERROR(c->r->log, "composite: more than %u destination formats",
                     DE__COMPOSITE_MAX_FORMATS);
        c->warned_pipe = true;
        return (de_gpu_pipeline){0};
    }
    de_gpu_pipeline_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.kind = DE_PIPE_GRAPHICS;
    pd.vs = c->vs;
    pd.ps = c->ps;
    pd.color_formats[0] = format;
    pd.color_count = 1;
    pd.blend[0] = DE_BLEND_OPAQUE;
    pd.cull = DE_CULL_NONE;
    pd.push_constant_bytes = c->push_bytes;
    pd.name = "view composite";
    const de_gpu_pipeline p = de_gpu_pipeline_create(c->dev, &pd);
    if (!DE_HANDLE_VALID(p) && !c->warned_pipe) {
        const de_error *e = de_error_last();
        DE_ERROR(c->r->log, "composite: no pipeline for format %u: %s", (unsigned)format,
                 e != NULL ? e->message : "?");
        c->warned_pipe = true;
    }
    c->pipes[c->pipe_count].format = format;
    c->pipes[c->pipe_count].pipeline = p;
    ++c->pipe_count;
    return p;
}

/* ---------- The pass ---------- */

typedef struct composite_pass {
    de__composite *c;
    de_rg_resource dst;
    de_gpu_pipeline pipeline; /**< invalid: clear (or load) only */
    bool clear;
    f32 clear_color[4];
    u32 item_count;
    de__composite_item *items;
} composite_pass;

static void composite_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const composite_pass *cp = (const composite_pass *)user;
    de_gpu_attachment color = {
        .texture = de_rg_texture(pass, cp->dst), .clear = cp->clear, .store = true};
    memcpy(color.clear_color, cp->clear_color, sizeof color.clear_color);
    de_gpu_cmd_begin_render(cmd, &color, 1, NULL);
    if (DE_HANDLE_VALID(cp->pipeline) && cp->item_count > 0) {
        de_gpu_cmd_set_pipeline(cmd, cp->pipeline);
        for (u32 i = 0; i < cp->item_count; ++i) {
            const de__composite_item *it = &cp->items[i];
            const de_gpu_texture src = de_rg_texture(pass, it->src);
            const u32 push[2] = {de_gpu_texture_bindless(cp->c->dev, src, 0, false),
                                 cp->c->sampler_index};
            if (push[0] == DE_GPU_BINDLESS_INVALID)
                continue;
            de_gpu_cmd_viewport(cmd, it->rect[0], it->rect[1], it->rect[2], it->rect[3]);
            de_gpu_cmd_scissor(cmd, (i32)it->rect[0], (i32)it->rect[1], (u32)it->rect[2],
                               (u32)it->rect[3]);
            de_gpu_cmd_push_constants(cmd, push, sizeof push);
            de_gpu_cmd_draw(cmd, 3, 1, 0, 0);
        }
    }
    de_gpu_cmd_end_render(cmd);
}

de_rg_pass *de__composite_add(de__composite *c, de_rg *g, de_arena *arena, const char *name,
                              de_rg_resource dst, de_gpu_format dst_format, const f32 *clear,
                              const de__composite_item *items, u32 item_count) {
    composite_pass *cp = (composite_pass *)de_arena_push(arena, sizeof *cp, 16);
    de__composite_item *copy =
        item_count > 0 ? (de__composite_item *)de_arena_push(arena, sizeof *copy * item_count, 16)
                       : NULL;
    if (cp == NULL || (item_count > 0 && copy == NULL)) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "composite \"%s\": frame arena", name);
        return NULL;
    }
    memset(cp, 0, sizeof *cp);
    cp->c = c;
    cp->dst = dst;
    cp->clear = clear != NULL;
    if (clear != NULL)
        memcpy(cp->clear_color, clear, sizeof cp->clear_color);
    if (item_count > 0)
        memcpy(copy, items, sizeof *copy * item_count);
    cp->items = copy;
    cp->item_count = item_count;
    if (c->ready && item_count > 0)
        cp->pipeline = pipeline_for(c, dst_format);
    de_rg_pass *p = de_rg_add_pass(g, name, DE_QUEUE_GRAPHICS, composite_execute, cp);
    if (p == NULL)
        return NULL;
    for (u32 i = 0; i < item_count; ++i)
        de_rg_read(p, copy[i].src, DE_ACCESS_SHADER_READ);
    if (clear == NULL)
        de_rg_read(p, dst, DE_ACCESS_RENDER_TARGET); /* loaded: keeps what is outside the rects */
    de_rg_write(p, dst, DE_ACCESS_RENDER_TARGET);
    return p;
}
