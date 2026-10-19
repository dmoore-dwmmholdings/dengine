#include "renderer_internal.h"
#include "shaders/engine_shaders.h"
#include <math.h>
de_gpu_device *de_renderer_device(de_renderer *r) {
    return r ? r->dev : NULL;
}
de_result de_render_builtin_graphics_pipeline(de_renderer *r, const char *name, u32 permutation,
                                              const char *vs_entry, const char *ps_entry,
                                              const de_gpu_pipeline_desc *state,
                                              de_gpu_pipeline *out) {
    if (out)
        *out = (de_gpu_pipeline){0};
    if (!r || !name || !vs_entry || !ps_entry || !state || !out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "builtin graphics pipeline: invalid argument");
    const char *entries[] = {vs_entry, ps_entry};
    de__engine_shader_code code;
    char why[512];
    DE_TRY(de__engine_shader_load(r->dev, r->alloc, name, permutation, entries, 2, &code, why,
                                  sizeof why));
    de_gpu_pipeline_desc d = *state;
    d.kind = DE_PIPE_GRAPHICS;
    d.vs = code.entries[0].blob;
    d.ps = code.entries[1].blob;
    d.push_constant_bytes = code.push_constant_bytes;
    *out = de_gpu_pipeline_create(r->dev, &d);
    de__engine_shader_code_free(&code);
    return out->id ? DE_OK
                   : DE_FAIL_WRAP(de_error_last()->result, "builtin graphics pipeline: %s", name);
}
de_result de_renderer_set_ui_output(de_renderer *r, const de_ui_output_calibration *value) {
    if (!r || !value || !isfinite(value->ui_white_nits) || !isfinite(value->paper_white_nits) ||
        !(value->ui_white_nits > 0) || !(value->paper_white_nits > 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "UI calibration nits must be finite and positive");
    de_mutex_lock(&r->lock);
    r->settings.ui_output = *value;
    de_mutex_unlock(&r->lock);
    return DE_OK;
}
de_ui_output_calibration de_renderer_get_ui_output(de_renderer *r) {
    de_ui_output_calibration c = {.ui_white_nits = 203, .paper_white_nits = 203};
    if (r) {
        de_mutex_lock(&r->lock);
        c = r->settings.ui_output;
        de_mutex_unlock(&r->lock);
        if (!(c.ui_white_nits > 0))
            c.ui_white_nits = 203;
        if (!(c.paper_white_nits > 0))
            c.paper_white_nits = 203;
    }
    return c;
}
#include "loaders/render_assets.h"
#include "asset/slots_internal.h"
de_gpu_texture de_render_texture_gpu(de_renderer *r, de_texture texture) {
    const de_render_texture *record =
        r && r->assets ? de_render_texture_get(r->assets, texture) : NULL;
    return record ? record->texture : (de_gpu_texture){0};
}
bool de_render_pin_asset(de_renderer *r, u32 handle) {
    return r && r->assets && handle && de__slot_ref_handle(r->assets, handle);
}
void de_render_unpin_asset(de_renderer *r, u32 handle) {
    if (r && r->assets && handle)
        (void)de__slot_unref_handle(r->assets, handle);
}
u64 de_renderer_completed_frame(de_renderer *r) {
    return r ? de_gpu_fence_value(r->dev, r->frame_fence) : 0;
}
de_result de_renderer_wait_frame(de_renderer *r, u64 value) {
    if (!r || value == UINT64_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid renderer fence value");
    if (!value)
        return DE_OK; /* A packet never submitted to the GPU needs no wait. */
    return de_gpu_fence_wait(r->dev, r->frame_fence, value, DE__RENDER_FENCE_TIMEOUT_NS);
}

#include "world/render_world.h"
u64 de_renderer_extract_frame(de_renderer *r) {
    return r && r->render_world && r->render_world->filling ? r->render_world->filling->frame : 0;
}
u64 de_renderer_completed_extract_frame(de_renderer *r) {
    return r && r->render_world
               ? de_atomic_load_u64(&r->render_world->rt.completed_extract, DE_ATOMIC_ACQUIRE)
               : 0;
}
