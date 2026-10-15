/*
 * renderer.c: de_renderer create / destroy / attach and the frame skeleton (SPEC §12, §12.8, §8;
 * DENG-174). The frame order and the struct are in renderer_internal.h; settings, presets and
 * cvars in settings.c.
 *
 * Later S12 tasks edit this file in fixed places only:
 *   - DENG-178 and DENG-175 each add one call in de_renderer_attach (marked below);
 *   - DENG-176 replaced the frame graph with the per-view loop in views.c (de__views_render)
 *     and added de__renderer_kick_prepare (the main-thread part of a frame).
 */
#include "render/renderer_internal.h"

#include "render/components_internal.h"
#include "render/graph/rg_internal.h"
#include "render/lighting/lighting.h"
#include "render/loaders/render_assets.h"
#include "render/materials/brdf_luts.h"
#include "render/materials/material_instances.h"
#include "render/materials/material_models.h"
#include "render/views_internal.h"
#include "render/scene/gpu_scene.h"
#include "render/world/render_world.h"

#include "gpu/gpu_backend.h" /* de__gpu_get_texture: the target's size and format */

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------- Create / destroy ---------- */

/* DENG-177: the first stage of every view (right after "view.debug_clear"): the view's scene
 * constants entry of the GPU scene, so the stages after it read the tables through it. */
static void gpu_scene_view_stage(de__view_frame *vf, void *user) {
    de_renderer *r = (de_renderer *)user;
    de__gpu_view_params p = {
        .view = vf->view_matrix, .proj = vf->proj, .width = vf->width, .height = vf->height};
    if (vf->camera != NULL)
        p.camera_position = vf->camera->offset;
    const u32 slot = (vf->view.id & 0xFFu) - 1u; /* views.c: slot + 1 in the low byte */
    /* DENG-304: the view's exposure of this frame (lighting/exposure.c) for every pass that writes
     * radiance, and the previous frame's for temporal passes. */
    p.pre_exposure = de__lighting_view_exposure(
        de__views_lighting(r), slot, vf->view, vf->frame_index,
        vf->camera != NULL ? vf->camera->camera.exposure_ev100 : (f32)NAN, &p.prev_pre_exposure);
    const de_result res = de__gpu_scene_view_pass(r->gpu_scene, vf->graph, slot, &p);
    if (res != DE_OK)
        DE_ERROR(r->log, "renderer: view \"%s\": GPU scene constants: %s", vf->name,
                 de_result_str(res));
}

/* Releases what `r` holds; NULL members are skipped, so it also unwinds a partial create. The
 * GPU must be idle (or never used). */
static void renderer_free(de_renderer *r) {
    de__views_destroy(r); /* DENG-176: view graphs, outputs, the composite */
    de__material_models_destroy(r->material_models); /* DENG-298: after the vis frame */
    de__brdf_luts_destroy(r->brdf_luts);             /* DENG-299 */
    de__gpu_scene_destroy(r->gpu_scene); /* DENG-177: before the uploader whose ring it stages in */
    de__material_instances_destroy(r->material_instances); /* DENG-302: after the GPU scene */
    if (r->uploader != NULL && !r->uploader_orphaned)
        de_render_uploader_destroy(r->uploader);
    if (r->graph != NULL)
        de__rg_destroy(r->graph);
    for (u32 i = 0; i < DE__RENDER_MAX_FRAMES_IN_FLIGHT; ++i)
        de_arena_destroy(r->arenas[i]);
    if (DE_HANDLE_VALID(r->swapchain))
        de_gpu_swapchain_destroy(r->dev, r->swapchain);
    if (DE_HANDLE_VALID(r->offscreen))
        de_gpu_texture_destroy(r->dev, r->offscreen);
    if (DE_HANDLE_VALID(r->frame_fence))
        de_gpu_fence_destroy(r->dev, r->frame_fence);
    de__render_cvars_release(r);
    de_mutex_destroy(&r->lock);
    DE_FREE(r->alloc, r, sizeof *r);
}

/* Appends the tier report's Base reasons to `buf`. */
static void base_reasons(const de_gpu_tier_report *rep, char *buf, size_t cap) {
    size_t n = 0;
    buf[0] = '\0';
    for (u32 i = 0; i < rep->base_reason_count && n + 1 < cap; ++i) {
        const int w = snprintf(buf + n, cap - n, "%s%s", i ? "; " : "", rep->base_reasons[i]);
        if (w < 0)
            break;
        n += (size_t)w < cap - n ? (size_t)w : cap - n - 1;
    }
}

de_result de_renderer_create(const de_renderer_desc *desc, de_renderer **out) {
    if (out != NULL)
        *out = NULL;
    if (desc == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_renderer_create: NULL %s",
                       desc == NULL ? "desc" : "out");
    if (desc->device == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_renderer_create: desc.device is NULL");

    /* The tier first: a device below Base gets a clear error, not a crash later. */
    const de_gpu_caps *caps = de_gpu_get_caps(desc->device);
    de_gpu_tier_report report;
    de_gpu_evaluate_tier(caps, &report);
    if (!report.base_ok) {
        char why[DE_GPU_TIER_MAX_REASONS * DE_GPU_TIER_REASON_BYTES];
        base_reasons(&report, why, sizeof why);
        return DE_FAIL(DE_ERR_UNSUPPORTED,
                       "de_renderer_create: \"%s\" (%s) is below the Base tier (SPEC 3.3): %s",
                       caps != NULL ? caps->device_name : "?",
                       caps != NULL ? de_gpu_backend_name(caps->backend) : "?", why);
    }

    const de_allocator *a = desc->allocator != NULL ? desc->allocator : de_heap_allocator();
    de_renderer *r = DE_NEW(a, de_renderer, DE_MEM_RENDER);
    if (r == NULL)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_renderer_create: renderer");
    memset(r, 0, sizeof *r);
    r->alloc = a;
    r->dev = desc->device;
    r->assets = desc->assets;
    r->types = desc->types;
    r->window = desc->window;
    r->hdr_output = desc->hdr_output;
    r->caps = *caps;
    r->report = report;
    r->log = de_log_register_category("render", DE_LOG_INFO);
    u32 fif = desc->frames_in_flight != 0 ? desc->frames_in_flight : 2u;
    r->frames_in_flight = fif < 2u                                ? 2u
                          : fif > DE__RENDER_MAX_FRAMES_IN_FLIGHT ? DE__RENDER_MAX_FRAMES_IN_FLIGHT
                                                                  : fif;
    r->clear_color[0] = 0.0f;
    r->clear_color[1] = 0.0f;
    r->clear_color[2] = 0.0f;
    r->clear_color[3] = 1.0f;
    de_mutex_init(&r->lock);

    /* Settings: the cvars' selection (or auto), clamped to the device. */
    de__render_cvars_init(r);
    r->settings = de__render_cvars_initial(r);
    de_mutex_lock(&r->lock);
    de__render_cvars_publish(r, 0);
    de_mutex_unlock(&r->lock);
    r->frame = r->settings;

    de_result res = DE_OK;
    /* Per-frame storage: a timeline fence (frame N signals N + 1) and one arena per frame in
     * flight. */
    r->frame_fence = de_gpu_fence_create(r->dev, 0);
    if (!DE_HANDLE_VALID(r->frame_fence)) {
        res = DE_FAIL_WRAP(DE_ERR_INTERNAL, "de_renderer_create: frame fence");
        goto fail;
    }
    for (u32 i = 0; i < r->frames_in_flight; ++i) {
        char name[32];
        snprintf(name, sizeof name, "render.frame%u", i);
        res = de_arena_create(&(de_arena_desc){.reserve_bytes = DE__RENDER_FRAME_ARENA_RESERVE,
                                               .tag = DE_MEM_RENDER,
                                               .name = name},
                              &r->arenas[i]);
        if (res != DE_OK) {
            res = DE_FAIL_WRAP(res, "de_renderer_create: frame arena %u", i);
            goto fail;
        }
    }
    res =
        de__rg_create(&(de__rg_desc){.allocator = a, .device = r->dev, .name = "frame"}, &r->graph);
    if (res != DE_OK) {
        res = DE_FAIL_WRAP(res, "de_renderer_create: frame graph");
        goto fail;
    }

    /* The device's graphics command-list pool (zero per-frame allocation, SPEC §5.3); each graph
     * also reserves its own lists when it first executes (DENG-415). */
    res = de_gpu_cmd_reserve(r->dev, DE_QUEUE_GRAPHICS, DE__RENDER_CMD_PRIME);
    if (res != DE_OK) {
        res = DE_FAIL_WRAP(res, "de_renderer_create: command lists");
        goto fail;
    }

    /* Output: a swapchain for the window, else an offscreen target of our own. */
    if (DE_HANDLE_VALID(r->window)) {
        r->swapchain =
            de_gpu_swapchain_create(r->dev, &(de_gpu_swapchain_desc){.window = r->window});
        if (!DE_HANDLE_VALID(r->swapchain)) {
            const de_error *e = de_error_last();
            res = DE_FAIL_WRAP(e != NULL ? e->result : DE_ERR_UNSUPPORTED,
                               "de_renderer_create: swapchain on %s",
                               de_gpu_backend_name(r->caps.backend));
            goto fail;
        }
        de_window_size_pixels(r->window, &r->window_w, &r->window_h);
        r->swapchain_w = r->window_w;
        r->swapchain_h = r->window_h;
    } else {
        r->offscreen = de_gpu_texture_create(
            r->dev, &(de_gpu_texture_desc){.format = DE__RENDER_OFFSCREEN_FORMAT,
                                           .width = DE__RENDER_OFFSCREEN_WIDTH,
                                           .height = DE__RENDER_OFFSCREEN_HEIGHT,
                                           .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_SRC,
                                           .name = "renderer offscreen target"});
        if (!DE_HANDLE_VALID(r->offscreen)) {
            res = DE_FAIL_WRAP(DE_ERR_INTERNAL, "de_renderer_create: offscreen target");
            goto fail;
        }
    }

    /* DENG-298: the material models, before the views (the vis frame's resolve reads them). */
    res = de__material_models_create(r->dev, r->alloc, &r->material_models);
    if (res != DE_OK) {
        res = DE_FAIL_WRAP(res, "de_renderer_create: material models");
        goto fail;
    }

    /* DENG-299: the energy LUT, generated on the graphics queue before any frame. */
    res = de__brdf_luts_create(r->dev, r->alloc, &r->brdf_luts);
    if (res != DE_OK) {
        res = DE_FAIL_WRAP(res, "de_renderer_create: brdf LUT");
        goto fail;
    }

    /* DENG-176: the views (the main view, the pass-registration list, the composite). */
    res = de__views_create(r);
    if (res != DE_OK) {
        res = DE_FAIL_WRAP(res, "de_renderer_create: views");
        goto fail;
    }

    /* Last, because the asset registry cannot unregister: the runtime texture, mesh, material
     * and model types and the uploader they share (DENG-105). */
    if (r->assets != NULL) {
        res = de_render_register_asset_types(r->assets, r->dev, a, &r->uploader);
        if (res != DE_OK) {
            if (r->uploader != NULL) {
                /* Some types were registered before the failure: their loaders refuse, but they
                 * point at the uploader until the asset system goes, so it must outlive this
                 * failed renderer. It is leaked (a create that fails half way through
                 * registration only fails for lack of memory). */
                r->uploader_orphaned = true;
                DE_ERROR(r->log, "de_renderer_create: asset type registration failed half way; "
                                 "the uploader is kept for the asset system");
            }
            res = DE_FAIL_WRAP(res, "de_renderer_create: registering the render asset types");
            goto fail;
        }
        /* DENG-422: unloaded assets stay alive until the frames that could name them complete. */
        de__render_uploader_set_frame_fence(r->uploader, r->frame_fence);
    }

    /* ---- DENG-302: material instances (asset records and runtime instances) ---- */
    res =
        de__material_instances_create(&(de__material_instances_desc){.allocator = a,
                                                                     .assets = r->assets,
                                                                     .uploader = r->uploader,
                                                                     .models = r->material_models},
                                      &r->material_instances);
    if (res != DE_OK) {
        res = DE_FAIL_WRAP(res, "de_renderer_create: material instances");
        goto fail;
    }

    /* ---- DENG-177: the GPU scene, staging in the uploader's ring, and its view stage ---- */
    res = de__gpu_scene_create(&(de__gpu_scene_desc){.device = r->dev,
                                                     .allocator = a,
                                                     .assets = r->assets,
                                                     .uploader = r->uploader,
                                                     .initial_capacity[DE__GS_VIEWS] = DE__VIEW_MAX,
                                                     .builtin_scatter = true,
                                                     .views_in_view_graphs = true,
                                                     .materials = r->material_instances,
                                                     .material_models = r->material_models},
                               &r->gpu_scene);
    if (res == DE_OK)
        res = de__view_register_stage(r, DE__VIEW_SLOT_VISIBILITY, INT32_MIN + 1, "gpu_scene.view",
                                      gpu_scene_view_stage, r);
    if (res != DE_OK) {
        res = DE_FAIL_WRAP(res, "de_renderer_create: GPU scene");
        goto fail;
    }
    DE_INFO(r->log, "renderer: %s (%s), tier %s, %u frames in flight%s", r->caps.device_name,
            de_gpu_backend_name(r->caps.backend), r->settings.tier == DE_TIER_RT ? "rt" : "base",
            r->frames_in_flight, DE_HANDLE_VALID(r->swapchain) ? "" : ", offscreen");
    *out = r;
    return DE_OK;

fail:
    de_gpu_wait_idle(r->dev);
    renderer_free(r);
    return res;
}

void de_renderer_destroy(de_renderer *r) {
    if (r == NULL)
        return;
    de__render_world_destroy(r); /* DENG-175: render thread, Extract systems */
    if (r->frame_index > 0)
        (void)de_gpu_fence_wait(r->dev, r->frame_fence, r->frame_index,
                                DE__RENDER_FENCE_TIMEOUT_NS);
    de_gpu_wait_idle(r->dev); /* also the copy queue and every deferred destruction */
    renderer_free(r);
}

void de_renderer_attach(de_renderer *r, de_world *w) {
    if (r == NULL || w == NULL)
        return;
    r->world = w;
    /* ---- DENG-178: register the render components here (one call) ---- */
    de__render_components_attach(r, w);
    /* ---- DENG-175: register the Extract systems here (one call) ---- */
    de__render_world_attach(r, w);
}

/* ---------- Settings ---------- */

void de_renderer_set_settings(de_renderer *r, const de_render_settings *s) {
    if (r == NULL || s == NULL)
        return;
    de_render_settings c = *s;
    de__render_settings_clamp(&r->caps, &r->report, &c);
    de_mutex_lock(&r->lock);
    r->settings = c;
    de__render_cvars_publish(r, DE__RCV_PUBLISH_TIER | DE__RCV_PUBLISH_QUALITY);
    de_mutex_unlock(&r->lock);
}

de_render_settings de_renderer_get_settings(const de_renderer *r) {
    if (r == NULL)
        return de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_MEDIUM);
    de_renderer *m = (de_renderer *)r; /* the lock is the only thing written */
    de_mutex_lock(&m->lock);
    const de_render_settings s = m->settings;
    de_mutex_unlock(&m->lock);
    return s;
}

const de_render_stats *de_renderer_stats(const de_renderer *r) {
    return r != NULL ? &r->stats : NULL;
}

/* ---------- Frame ---------- */

/* Step 3 (renderer_internal.h): the main-thread part of a frame. */
void de__renderer_kick_prepare(de_renderer *r) {
    if (r == NULL)
        return;
    if (r->uploader != NULL) {
        de_render_uploader_flush(r->uploader); /* also frees unloads whose frames completed */
        (void)de_render_uploader_acquire_submit(r->uploader);
        /* DENG-422: from here on an unload may be named by frame `frame_index`. */
        de__render_uploader_frame_prepared(r->uploader, r->frame_index);
    }
    /* DENG-422: the GPU scene reads asset records here, on the main thread between asset updates,
     * never on the render thread. The buffer is the one this kick hands over (or the last one). */
    const de_result gr = de__gpu_scene_prepare(r->gpu_scene, de__render_world_last(r));
    r->material_instances_live = de__material_instances_live(r->material_instances); /* DENG-302 */
    /* DENG-304: the environment lighting uses this frame (its HDRI's record is read here only). */
    de__lighting_prepare_environment(de__views_lighting(r), r->assets, de__render_world_last(r));
    if (gr != DE_OK)
        DE_ERROR(r->log, "renderer: frame %llu: GPU scene: %s", (unsigned long long)r->frame_index,
                 de_error_last() != NULL ? de_error_last()->message : de_result_str(gr));
    if (DE_HANDLE_VALID(r->window)) {
        u32 w = 0, h = 0;
        de_window_size_pixels(r->window, &w, &h);
        de_mutex_lock(&r->lock);
        r->window_w = w;
        r->window_h = h;
        de_mutex_unlock(&r->lock);
    }
}

de_result de_renderer_render_frame(de_renderer *r) {
    if (r == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_renderer_render_frame: NULL renderer");
    const u64 n = r->frame_index;

    /* 1. Wait for the frame that last used this slot, then reset its arena. */
    if (n >= r->frames_in_flight) {
        const de_result w = de_gpu_fence_wait(r->dev, r->frame_fence, n + 1u - r->frames_in_flight,
                                              DE__RENDER_FENCE_TIMEOUT_NS);
        if (w != DE_OK)
            return DE_FAIL_WRAP(w, "de_renderer_render_frame: waiting for frame %llu",
                                (unsigned long long)(n - r->frames_in_flight));
    }
    de_arena *arena = r->arenas[n % r->frames_in_flight];
    de_arena_reset(arena);

    /* 2. Settings: cvar changes, then the copy this frame renders with. */
    de_mutex_lock(&r->lock);
    (void)de__render_cvars_sync(r);
    r->frame = r->settings;
    de_mutex_unlock(&r->lock);

    /* 3. The main-thread part, unless the kick ran it on the main thread already. */
    if (de__render_world_current(r) == NULL)
        de__renderer_kick_prepare(r);

    /* 4. The target: the swapchain (resized to the window first) or the offscreen texture. */
    de_gpu_texture target = r->offscreen;
    u32 final_access = DE__RENDER_OFFSCREEN_FINAL_ACCESS;
    if (DE_HANDLE_VALID(r->swapchain)) {
        de_mutex_lock(&r->lock);
        const u32 ww = r->window_w, wh = r->window_h;
        de_mutex_unlock(&r->lock);
        if (ww == 0 || wh == 0)
            return DE_OK; /* minimised: nothing to draw */
        if (ww != r->swapchain_w || wh != r->swapchain_h) {
            const de_result rs = de_gpu_swapchain_resize(r->dev, r->swapchain, ww, wh);
            if (rs != DE_OK)
                return DE_FAIL_WRAP(rs, "de_renderer_render_frame: swapchain resize to %ux%u", ww,
                                    wh);
            r->swapchain_w = ww;
            r->swapchain_h = wh;
        }
        const de_result a = de_gpu_swapchain_acquire(r->dev, r->swapchain, &target);
        if (a == DE_ERR_BUSY)
            return DE_OK; /* no image (minimised, resizing): nothing to draw */
        if (a != DE_OK)
            return DE_FAIL_WRAP(a, "de_renderer_render_frame: swapchain image");
        final_access = DE_ACCESS_PRESENT;
    }
    const de__gpu_texture *t = de__gpu_get_texture(r->dev, target);
    de__views_set_output_size(r, t != NULL ? t->width : 0, t != NULL ? t->height : 0,
                              t != NULL ? t->format : DE__RENDER_OFFSCREEN_FORMAT);

    /* DENG-177: the GPU scene's uploads, once per frame in its own graph; queue order makes
     * them visible to every view graph. */
    const de_result sr = de__gpu_scene_frame(r->gpu_scene, arena, n);
    if (sr != DE_OK)
        DE_ERROR(r->log, "renderer: frame %llu: GPU scene: %s", (unsigned long long)n,
                 de_error_last() != NULL ? de_error_last()->message : de_result_str(sr));

    /* 5 + 6. The views, then the output graph; the frame fence is signalled with N + 1. */
    const de_result vr = de__views_render(r, arena, target, final_access, n);
    de__gpu_scene_views_end(r->gpu_scene, r->frame_fence, n + 1u); /* DENG-177 */
    de__gpu_scene_fill_stats(r->gpu_scene, &r->stats);
    de__material_fill_stats(r, &r->stats); /* DENG-302 */
    r->frame_index = n + 1u;
    if (DE_HANDLE_VALID(r->swapchain)) {
        const de_result p = de_gpu_swapchain_present(r->dev, r->swapchain);
        if (p != DE_OK && p != DE_ERR_BUSY)
            return DE_FAIL_WRAP(p, "de_renderer_render_frame: present");
    }
    return vr;
}

/* ---------- Test hooks ---------- */

de_gpu_texture de__renderer_offscreen(const de_renderer *r) {
    return r != NULL ? r->offscreen : (de_gpu_texture){0};
}

de_gpu_fence de__renderer_frame_fence(const de_renderer *r, u64 *frames) {
    if (frames != NULL)
        *frames = r != NULL ? r->frame_index : 0;
    return r != NULL ? r->frame_fence : (de_gpu_fence){0};
}

de_render_uploader *de__renderer_uploader(const de_renderer *r) {
    return r != NULL ? r->uploader : NULL;
}
