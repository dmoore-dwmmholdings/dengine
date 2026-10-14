/*
 * composite.c: the view composite (SPEC §12.4, §12.9; DENG-176). A graphics pass that draws
 * sampled textures (view outputs) into pixel rectangles of a destination with bilinear filtering:
 * the output graph composites every swapchain view, and a render-to-texture view composites its
 * output into its target. Declared in views_internal.h.
 *
 * Shaders: shaders/common/composite.slang (vs_main, ps_main). The renderer has no shader assets of
 * its own yet, so in builds with the cooker (DE_HAS_COOKER: Debug and Dev with the tools) it cooks
 * the file in-process at renderer create through the cooker's built-in shader importer, for the
 * device's target only, and keeps the two entry points' code. The source directory is mounted as
 * `derendershd:/` only for the cook. The null backend gets a placeholder blob (it compiles
 * nothing). Release and Shipping have no cooker: there the composite is not ready, composite
 * passes only clear their destination, and a warning says so (see the Decision page "Renderer
 * built-in shaders are cooked in-process in Dev builds").
 *
 * Pipelines: one per destination colour format, made at first use while a graph is built (render
 * thread), at most DE__COMPOSITE_MAX_FORMATS.
 */
#include "render/views_internal.h"

#include "render/renderer_internal.h"
#include "render/graph/rg_internal.h"

#if DE_HAS_COOKER
#include "asset/manifest_internal.h"
#include "cook/lib/de_cooker.h"
#include "gpu/shader/shader_internal.h"
#endif

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
    /** One block holding the vertex code then the pixel code (cooked builds). */
    void *code;
    size_t code_size;
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

/* ---------- Shaders ---------- */

#if DE_HAS_COOKER

static const char *target_name(de__shader_target t) {
    return t == DE__SHADER_TARGET_DXIL       ? "dxil"
           : t == DE__SHADER_TARGET_METALLIB ? "metallib"
                                             : "spirv";
}

/* The record of entry `entry` for `target` in a checked view, or false. */
static bool find_entry(const de__shader_view *v, u32 target, const char *entry,
                       de__shader_record *out) {
    for (u32 e = 0; e < v->header.entry_count; ++e) {
        const char *name = NULL;
        (void)de__shader_view_entry(v, e, &name);
        if (name == NULL || strcmp(name, entry) != 0)
            continue;
        const i32 ri = de__shader_view_find(v, target, 0, e);
        if (ri < 0)
            return false;
        *out = de__shader_view_record(v, (u32)ri);
        return true;
    }
    return false;
}

/* Cooks composite.slang for the device's target and copies the two entry points' code into
 * c->code. Returns false with the reason in `why`. */
static bool cook_shaders(de__composite *c, char *why, size_t cap) {
    de_asset_type_desc t;
    if (!de_cooker_builtin_importer("shader", &t)) {
        snprintf(why, cap, "this build has no shader importer (no Slang)");
        return false;
    }
    const de__shader_target target = de__shader_target_for_backend(c->r->caps.backend);
    if (de_fs_mount("derendershd", DE_RENDER_SHADER_DIR, 0, 0) != DE_OK) {
        snprintf(why, cap, "cannot mount %s", DE_RENDER_SHADER_DIR);
        return false;
    }
    de_cooker *ck = NULL;
    bool ok = false;
    de_cook_result res;
    memset(&res, 0, sizeof res);
    bool have_res = false;
    if (de_cooker_create(
            &(de_cooker_desc){.allocator = c->alloc, .verify_determinism = DE_COOK_VERIFY_OFF},
            &ck) != DE_OK ||
        de_cooker_register_type(ck, &t) != DE_OK) {
        const de_error *e = de_error_last();
        snprintf(why, cap, "cannot create the cooker: %.300s", e != NULL ? e->message : "?");
        goto done;
    }
    const char *targets[1] = {target_name(target)};
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = "targets";
    kv.value.type = DE__MANIFEST_STRING_ARRAY;
    kv.value.u.arr.items = targets;
    kv.value.u.arr.count = 1;
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = "engine/shaders/composite";
    a.type = "shader";
    a.source = "composite.slang";
    a.file = "derendershd:/assets.toml";
    a.settings = &kv;
    a.setting_count = 1;
    const de_cook_input in = {.asset = &a, .asset_root = "derendershd:/", .source = NULL};
    if (de_cooker_cook(ck, &in, &res) != DE_OK) {
        const de_error *e = de_error_last();
        snprintf(why, cap, "cooking composite.slang for %s failed: %.300s", targets[0],
                 e != NULL ? e->message : "?");
        goto done;
    }
    have_res = true;
    de__shader_view v;
    de__shader_record vs, ps;
    if (de__shader_view_init(res.data, res.size, &v) != DE_OK ||
        !find_entry(&v, (u32)target, "vs_main", &vs) ||
        !find_entry(&v, (u32)target, "ps_main", &ps)) {
        snprintf(why, cap, "the cooked composite.slang has no vs_main/ps_main for %s", targets[0]);
        goto done;
    }
    c->code_size = (size_t)vs.code_size + (size_t)ps.code_size;
    c->code = DE_ALLOC(c->alloc, c->code_size, 16, DE_MEM_RENDER);
    if (c->code == NULL) {
        snprintf(why, cap, "out of memory");
        goto done;
    }
    memcpy(c->code, v.base + vs.code_offset, (size_t)vs.code_size);
    memcpy((u8 *)c->code + vs.code_size, v.base + ps.code_offset, (size_t)ps.code_size);
    c->vs = (de_gpu_shader_blob){.code = c->code, .size = vs.code_size, .entry = "vs_main"};
    c->ps = (de_gpu_shader_blob){
        .code = (const u8 *)c->code + vs.code_size, .size = ps.code_size, .entry = "ps_main"};
    c->push_bytes = vs.push_constant_bytes > ps.push_constant_bytes ? vs.push_constant_bytes
                                                                    : ps.push_constant_bytes;
    ok = true;
done:
    if (have_res)
        de_cook_result_free(&res);
    de_cooker_destroy(ck);
    (void)de_fs_unmount("derendershd", DE_RENDER_SHADER_DIR);
    return ok;
}

#endif /* DE_HAS_COOKER */

/* The null backend compiles nothing: any non-empty code will do. */
static const u32 k_placeholder_code[4] = {0x07230203u, 0x00010600u, 0u, 0u};

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

    char why[512] = "";
    if (r->caps.backend == DE_GPU_NULL) {
        c->vs = (de_gpu_shader_blob){
            .code = k_placeholder_code, .size = sizeof k_placeholder_code, .entry = "vs_main"};
        c->ps = (de_gpu_shader_blob){
            .code = k_placeholder_code, .size = sizeof k_placeholder_code, .entry = "ps_main"};
        c->push_bytes = DE__COMPOSITE_PUSH_BYTES;
        c->ready = true;
    } else {
#if DE_HAS_COOKER
        c->ready = cook_shaders(c, why, sizeof why);
#else
        snprintf(why, sizeof why,
                 "this build has no cooker (Release, Shipping or no tools), and no cooked engine "
                 "shaders exist yet");
#endif
    }
    if (!c->ready)
        DE_WARN(r->log,
                "renderer: no view composite on %s (%s): views render, but the output only "
                "clears and render-to-texture targets need render_scale 1 and a full viewport",
                de_gpu_backend_name(r->caps.backend), why);
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
    if (c->code != NULL)
        DE_FREE(c->alloc, c->code, c->code_size);
    DE_FREE(c->alloc, c, sizeof *c);
}

bool de__composite_ready(const de__composite *c) {
    return c != NULL && c->ready;
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
