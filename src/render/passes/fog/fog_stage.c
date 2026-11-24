/*
 * fog_stage.c: the renderer's froxel fog stage (DENG-861). See fog_stage.h.
 */
#include "render/passes/fog/fog_stage.h"

#if DE_HAS_RENDER

#include "render/lighting/lighting.h"
#include "render/passes/vis/vis_stages.h"
#include "render/renderer_internal.h"
#include "render/views_internal.h"
#include "render/world/render_world.h"

#include <math.h>
#include <string.h>

struct de__fog_stage {
    de_renderer *renderer;
    const de_allocator *alloc;
    de_gpu_device *dev;
    de__fog *fog;
    de_gpu_sampler sampler; /* linear clamp */
    u32 sampler_bindless;
    bool registered;
};

void de__fog_stage_add(de__fog_stage *fs, const de__view_frame *vf, de__vis_context *vc,
                       de__lighting_context *lc, f32 pre, const de__fog_aerial *aerial) {
    if (fs == NULL || vf == NULL || vf->world == NULL || vf->graph == NULL)
        return;
    const bool with_aerial = aerial != NULL && DE_HANDLE_VALID(aerial->texture);
    const de__fog_inputs in = {
        .settings = vf->settings,
        .volumes = vf->world->fog,
        .volume_count = vf->world->fog_count,
        .slot = (vf->view.id & 0xFFu) - 1u,
        .frame_index = vf->frame_index,
        .width = vf->width,
        .height = vf->height,
        .view_matrix = vf->view_matrix,
        .proj = vf->proj,
        .prev_view_proj = vf->prev_view_proj,
        .history_ok = !vf->world->origin_changed && !vf->probe_capture,
        .pre_exposure = pre,
        .depth = vc->depth,
        .target = lc->target,
        .lc = lc,
        .ambient = 1u,
        /* DENG-849: with no sky drawn there is no aerial perspective behind the fog. */
        .aerial = with_aerial ? aerial->texture : (de_rg_resource){0},
        .aerial_sampler = with_aerial ? aerial->sampler : 0u,
        .aerial_range_km = with_aerial ? aerial->range_km : 0.0f,
        .sampler = fs->sampler_bindless,
    };
    de__fog_output out;
    de__fog_add_passes(fs->fog, vf->graph, vc->arena, &in, &out);
    lc->fog = out.integrated;
    lc->fog_header = out.header;
}

/* The fog of a view the sky stage did not light with fog (no sky attached, an HDRI environment,
 * the sky's LUTs or pipelines missing or failed): no aerial perspective, the fog alone over the lit
 * target (DENG-849, now the renderer's own stage). */
static void fog_view_stage(de__view_frame *vf, void *user) {
    de__fog_stage *fs = (de__fog_stage *)user;
    if (vf->graph == NULL || vf->world == NULL || !de__fog_ready(fs->fog) ||
        fs->sampler_bindless == DE_GPU_BINDLESS_INVALID ||
        !de__fog_enabled(vf->settings, vf->world->fog, vf->world->fog_count,
                         de__fog_max_distance(fs->fog)))
        return;
    de_renderer *r = vf->renderer;
    de__vis_context *vc =
        de__vis_frame_context(de__views_vis(r), vf->view, vf->graph, vf->frame_index);
    de__lighting *lt = de__views_lighting(r);
    de__lighting_context *lc =
        vc != NULL ? de__lighting_frame_context(lt, vf->view, vf->graph, vf->frame_index) : NULL;
    if (vc == NULL || lc == NULL || !lc->lit || !DE_HANDLE_VALID(lc->target) ||
        !DE_HANDLE_VALID(vc->depth))
        return; /* nothing lit */
    if (DE_HANDLE_VALID(lc->fog))
        return; /* the sky stage added it, with the aerial perspective */
    const u32 slot = (vf->view.id & 0xFFu) - 1u;
    const f32 pre = de__lighting_view_exposure(
        lt, slot, vf->view, vf->frame_index,
        vf->camera != NULL ? vf->camera->camera.exposure_ev100 : (f32)NAN, NULL);
    de__fog_stage_add(fs, vf, vc, lc, pre, NULL);
}

de__fog *de__fog_stage_fog(const de__fog_stage *fs) {
    return fs != NULL ? fs->fog : NULL;
}

void de__fog_stage_destroy(de_renderer *r, de__fog_stage *fs) {
    if (fs == NULL)
        return;
    if (fs->registered)
        de__view_unregister_stage(r, fog_view_stage, fs);
    de__fog_destroy(fs->fog);
    if (DE_HANDLE_VALID(fs->sampler))
        de_gpu_sampler_destroy(fs->dev, fs->sampler);
    DE_FREE(fs->alloc, fs, sizeof *fs);
}

de_result de__fog_stage_create(de_renderer *r, de__fog_stage **out) {
    if (out != NULL)
        *out = NULL;
    if (r == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__fog_stage_create: NULL argument");
    de__fog_stage *fs = DE_NEW(r->alloc, de__fog_stage, DE_MEM_RENDER);
    if (fs == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "fog: no memory");
    memset(fs, 0, sizeof *fs);
    fs->renderer = r;
    fs->alloc = r->alloc;
    fs->dev = r->dev;
    de_result res = de__fog_create(r->dev, r->alloc, &fs->fog);
    if (res == DE_OK) {
        fs->sampler = de_gpu_sampler_create(r->dev, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                                            .mag = DE_FILTER_LINEAR,
                                                                            .mip = DE_FILTER_NEAREST,
                                                                            .u = DE_ADDR_CLAMP,
                                                                            .v = DE_ADDR_CLAMP,
                                                                            .w = DE_ADDR_CLAMP});
        if (!DE_HANDLE_VALID(fs->sampler))
            res = DE_FAIL_WRAP(DE_ERR_INTERNAL, "fog: the sampler");
        else
            fs->sampler_bindless = de_gpu_sampler_bindless(r->dev, fs->sampler);
    }
    if (res == DE_OK) {
        res = de__view_register_stage(r, DE__VIEW_SLOT_TRANSPARENT, DE__FOG_STAGE_ORDER, "fog",
                                      fog_view_stage, fs);
        fs->registered = res == DE_OK;
    }
    if (res != DE_OK) {
        de__fog_stage_destroy(r, fs);
        return res;
    }
    *out = fs;
    return DE_OK;
}

#endif /* DE_HAS_RENDER */
