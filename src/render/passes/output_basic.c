/*
 * output_basic.c: the basic output pass (SPEC §12.7; DENG-304). Until the S18 tonemappers
 * (DENG-163) replace it, each view's lit image reaches the screen through this stage: the
 * pre-exposed linear HDR lighting target (lighting.h, the context de__lighting_build published for
 * the view) clamped to [0, 1], the equivalent of DE_TM_NONE (shaders/post/output_basic.slang,
 * engine shader `post_output_basic`). The result is a new RGBA16F texture the stage makes the
 * view's colour; views.c composites it into the view's output, and the sRGB swapchain applies the
 * sRGB OETF when the output graph draws the view (all three backends' swapchains are sRGB).
 *
 * The stage sits at (DE__VIEW_SLOT_POST, DE__VIEW_ORDER_TONEMAP), between DE_HOOK_BEFORE_TONEMAP
 * and DE_HOOK_AFTER_TONEMAP. It adds nothing when the view lit nothing this frame (no G-buffer) or
 * when an earlier stage already replaced the view's colour (a debug view: r.hzb_debug,
 * r.cluster_debug). It is the first pass that reads the lighting target, so with it the render
 * graph keeps lighting, the resolve and classification.
 */
#include "render/lighting/lighting.h"

#include "render/graph/rg_internal.h"
#include "render/renderer_internal.h"
#include "render/passes/vis/gbuffer.h"

#include <string.h>

/** output_basic.slang OutputParams (32 bytes). */
typedef struct ob_params {
    u32 src, dst, width, height, encode, reserved0, reserved1, reserved2;
} ob_params;
DE_STATIC_ASSERT(sizeof(ob_params) == 32, "OutputParams is 32 bytes");

struct de__output_basic {
    de__vis_shared *vis;
    de__lighting *lighting;
    de__vis_kernel kernel;
    de__vis_kernel debug_kernel;
    bool enabled;
};

typedef struct ob_pass {
    de_gpu_device *dev;
    const de__vis_kernel *kernel;
    de_rg_resource src, dst;
    u32 width, height, encode;
} ob_pass;

de_result de__output_basic_create_ex(de__vis_shared *vis, de__output_basic **out) {
    *out = NULL;
    if (vis == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__output_basic_create_ex: no vis frame");
    de__output_basic *ob = DE_NEW(vis->alloc, de__output_basic, DE_MEM_RENDER);
    if (ob == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__output_basic_create_ex: no memory");
    memset(ob, 0, sizeof *ob);
    ob->vis = vis;
    if (vis->enabled) {
        static const char *const k_entries[2] = {"cs_output_basic", "cs_view_debug"};
        static const u32 g8[3] = {8u, 8u, 1u};
        de__engine_shader_code code;
        de_result r = de__vis_shader_load(vis, "post_output_basic", k_entries, 2, &code);
        if (r == DE_OK) {
            r = de__vis_kernel_create(vis, &code, 0, "output basic", g8, (u32)sizeof(ob_params),
                                      &ob->kernel);
            if (r == DE_OK)
                r = de__vis_kernel_create(vis, &code, 1, "view material debug", g8, 48u,
                                          &ob->debug_kernel);
            de__engine_shader_code_free(&code);
        }
        if (r == DE_OK) {
            ob->enabled = true;
        } else if (r == DE_ERR_UNSUPPORTED) {
            const de_error *e = de_error_last();
            DE_WARN(vis->log, "output: the basic output pass is off: %s",
                    e != NULL ? e->message : "no engine shaders");
            de_error_clear();
        } else {
            de__output_basic_destroy(ob);
            return r;
        }
    }
    *out = ob;
    return DE_OK;
}

void de__output_basic_destroy(de__output_basic *ob) {
    if (ob == NULL)
        return;
    de__vis_kernel_destroy(ob->vis, &ob->kernel);
    de__vis_kernel_destroy(ob->vis, &ob->debug_kernel);
    DE_FREE(ob->vis->alloc, ob, sizeof *ob);
}

bool de__output_basic_enabled(const de__output_basic *ob) {
    return ob != NULL && ob->enabled;
}

static void ob_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const ob_pass *p = (const ob_pass *)user;
    const ob_params pc = {
        .src = de_gpu_texture_bindless(p->dev, de_rg_texture(pass, p->src), 0, false),
        .dst = de_gpu_texture_bindless(p->dev, de_rg_texture(pass, p->dst), 0, true),
        .width = p->width,
        .height = p->height,
        .encode = p->encode};
    de_gpu_cmd_set_pipeline(cmd, p->kernel->pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
    de_gpu_cmd_dispatch(cmd, (p->width + 7u) / 8u, (p->height + 7u) / 8u, 1);
}

de_rg_resource de__output_basic_add(de__output_basic *ob, de_rg *g, de_arena *arena,
                                    de_rg_resource src, u32 width, u32 height, bool encode) {
    if (!de__output_basic_enabled(ob) || g == NULL || arena == NULL || !DE_HANDLE_VALID(src) ||
        width == 0 || height == 0)
        return (de_rg_resource){0};
    const de_rg_resource dst = de_rg_create_texture(
        g, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                  .width = width,
                                  .height = height,
                                  .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC |
                                           DE_USAGE_RENDER_TARGET,
                                  .name = "output.basic"});
    ob_pass *p = (ob_pass *)de_arena_push(arena, sizeof *p, 16);
    if (!DE_HANDLE_VALID(dst) || p == NULL) {
        if (p == NULL)
            (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "output: the frame arena is full");
        return (de_rg_resource){0};
    }
    *p = (ob_pass){.dev = ob->vis->dev,
                   .kernel = &ob->kernel,
                   .src = src,
                   .dst = dst,
                   .width = width,
                   .height = height,
                   .encode = encode ? 1u : 0u};
    de_rg_pass *pass = de_rg_add_pass(g, "output.basic", DE_QUEUE_GRAPHICS, ob_execute, p);
    de_rg_read(pass, src, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, dst, DE_ACCESS_SHADER_WRITE);
    return dst;
}

typedef struct debug_params {
    u32 gb0, gb1, gb2, motion, depth, emission, dst, width, height, mode, reserved0, reserved1;
} debug_params;
DE_STATIC_ASSERT(sizeof(debug_params) == 48, "ViewDebugParams is 48 bytes");
typedef struct debug_pass {
    de__output_basic *ob;
    const de__vis_context *vc;
    de_rg_resource dst;
    de_view_render_mode mode;
} debug_pass;

static u32 debug_texture(const debug_pass *p, const de_rg_pass *pass, de_rg_resource src) {
    return de_gpu_texture_bindless(p->ob->vis->dev, de_rg_texture(pass, src), 0, false);
}
static void debug_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const debug_pass *p = user;
    const de__vis_context *vc = p->vc;
    const debug_params pc = {
        .gb0 = debug_texture(p, pass, vc->gbuffer[DE__GB0]),
        .gb1 = debug_texture(p, pass, vc->gbuffer[DE__GB1]),
        .gb2 = debug_texture(p, pass, vc->gbuffer[DE__GB2]),
        .motion = debug_texture(p, pass, vc->gbuffer[DE__GB_MOTION]),
        .depth = debug_texture(p, pass, vc->depth),
        .emission = debug_texture(p, pass, vc->gbuffer[DE__GB_LIGHTING]),
        .dst = de_gpu_texture_bindless(p->ob->vis->dev, de_rg_texture(pass, p->dst), 0, true),
        .width = vc->width,
        .height = vc->height,
        .mode = (u32)p->mode};
    de_gpu_cmd_set_pipeline(cmd, p->ob->debug_kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, (pc.width + 7u) / 8u, (pc.height + 7u) / 8u, 1);
}

de_rg_resource de__output_debug_add(de__output_basic *ob, de_rg *g, de_arena *arena,
                                    const de__vis_context *vc, de_view_render_mode mode) {
    if (!de__output_basic_enabled(ob) || !vc || !g || !arena || mode <= DE_VIEW_RENDER_LIT ||
        mode >= DE_VIEW_RENDER_MODE_COUNT || !vc->depth.id)
        return (de_rg_resource){0};
    for (u32 i = 0; i < DE__GB_COUNT; ++i)
        if (!vc->gbuffer[i].id)
            return (de_rg_resource){0};
    de_rg_resource dst = de_rg_create_texture(
        g, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                  .width = vc->width,
                                  .height = vc->height,
                                  .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC |
                                           DE_USAGE_RENDER_TARGET,
                                  .name = "view.material_debug"});
    debug_pass *p = de_arena_push(arena, sizeof *p, 16);
    if (!dst.id || !p)
        return (de_rg_resource){0};
    *p = (debug_pass){.ob = ob, .vc = vc, .dst = dst, .mode = mode};
    de_rg_pass *pass =
        de_rg_add_pass(g, "view.material_debug", DE_QUEUE_GRAPHICS, debug_execute, p);
    if (!pass)
        return (de_rg_resource){0};
    de_rg_read(pass, vc->depth, DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < DE__GB_COUNT; ++i)
        de_rg_read(pass, vc->gbuffer[i], DE_ACCESS_SHADER_READ);
    de_rg_write(pass, dst, DE_ACCESS_SHADER_WRITE);
    return dst;
}

/* The view stage. */
static void output_stage(de__view_frame *vf, void *user) {
    de__output_basic *ob = (de__output_basic *)user;
    if (!de__output_basic_enabled(ob))
        return;
    if (vf->display.mode != DE_VIEW_RENDER_LIT) {
        const de__vis_context *vc =
            de__vis_frame_context(ob->vis, vf->view, vf->graph, vf->frame_index);
        const de_rg_resource out =
            de__output_debug_add(ob, vf->graph, vf->graph->arena, vc, vf->display.mode);
        if (out.id) {
            vf->color = out;
            vf->color_format = DE_FMT_RGBA16_FLOAT;
        }
        return;
    }
    if (vf->color.id != vf->initial_color.id)
        return; /* off, or a debug view replaced the colour */
    const de__lighting_context *lc =
        de__lighting_frame_context(ob->lighting, vf->view, vf->graph, vf->frame_index);
    if (lc == NULL || !lc->lit || !DE_HANDLE_VALID(lc->target))
        return;
    if (vf->probe_capture) {
        vf->color = lc->target;
        vf->color_format = DE_FMT_RGBA16_FLOAT;
        return;
    }
    const de_rg_resource out = de__output_basic_add(ob, vf->graph, vf->graph->arena, lc->target,
                                                    lc->width, lc->height, false);
    if (DE_HANDLE_VALID(out)) {
        vf->color = out;
        vf->color_format = DE_FMT_RGBA16_FLOAT;
    }
}

de_result de__output_basic_create(de_renderer *r, de__lighting *lt, de__output_basic **out) {
    *out = NULL;
    de__output_basic *ob = NULL;
    de_result res = de__output_basic_create_ex(lt != NULL ? lt->vis : NULL, &ob);
    if (res == DE_ERR_INVALID_ARGUMENT && (lt == NULL || lt->vis == NULL)) {
        de_error_clear(); /* no vis frame: nothing to output */
        return DE_OK;
    }
    if (res != DE_OK)
        return res;
    ob->lighting = lt;
    res = de__view_register_stage(r, DE__VIEW_SLOT_POST, DE__VIEW_ORDER_TONEMAP, "output.basic",
                                  output_stage, ob);
    if (res != DE_OK) {
        de__output_basic_destroy(ob);
        return res;
    }
    *out = ob;
    return DE_OK;
}
