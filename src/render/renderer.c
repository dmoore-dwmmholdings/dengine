/*
 * renderer.c: de_renderer create / destroy / attach and the frame skeleton (SPEC §12, §12.8, §8;
 * DENG-174). The frame order and the struct are in renderer_internal.h; settings, presets and
 * cvars in settings.c.
 *
 * Later S12 tasks edit this file in fixed places only:
 *   - DENG-178 and DENG-175 each add one call in de_renderer_attach (marked below);
 *   - DENG-176 replaces the body of render_frame_graph (the per-view loop in views.c).
 */
#include "render/renderer_internal.h"

#include "render/graph/rg_internal.h"
#include "render/loaders/render_assets.h"

#include <stdio.h>
#include <string.h>

/* ---------- Create / destroy ---------- */

/* Releases what `r` holds; NULL members are skipped, so it also unwinds a partial create. The
 * GPU must be idle (or never used). */
static void renderer_free(de_renderer *r) {
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

/* Fills the device's graphics command-list pool (zero per-frame allocation, SPEC §5.3): begins
 * DE__RENDER_CMD_PRIME lists at once and submits them empty; de_gpu_wait_idle (the end of create)
 * returns them to the pool. */
static de_result prime_cmd_pool(de_renderer *r) {
    de_gpu_cmd *cmds[DE__RENDER_CMD_PRIME];
    u32 n = 0;
    for (; n < DE__RENDER_CMD_PRIME; ++n) {
        cmds[n] = de_gpu_cmd_begin(r->dev, DE_QUEUE_GRAPHICS, "renderer prime");
        if (cmds[n] == NULL)
            break;
    }
    if (n == 0)
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "de_renderer_create: command lists");
    const de_gpu_submit s = {.cmds = cmds, .cmd_count = n};
    const de_result res = de_gpu_queue_submit(r->dev, DE_QUEUE_GRAPHICS, &s);
    if (res != DE_OK)
        return DE_FAIL_WRAP(res, "de_renderer_create: priming the command lists");
    return n == DE__RENDER_CMD_PRIME
               ? DE_OK
               : DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_renderer_create: command lists");
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

    res = prime_cmd_pool(r);
    if (res != DE_OK)
        goto fail;

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
    }

    de_gpu_wait_idle(r->dev); /* the primed command lists go back to the pool */
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
    /* ---- DENG-175: register the Extract systems here (one call) ---- */
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

static void clear_pass(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    de_renderer *r = (de_renderer *)user;
    de_gpu_attachment color = {
        .texture = de_rg_texture(pass, r->target_res), .clear = true, .store = true};
    memcpy(color.clear_color, r->clear_color, sizeof color.clear_color);
    de_gpu_cmd_begin_render(cmd, &color, 1, NULL);
    de_gpu_cmd_end_render(cmd);
}

/*
 * Steps 5 and 6 of a frame (renderer_internal.h): the frame graph clears `target` and leaves it in
 * `final_access`; `cmd` is open on the graphics queue and already holds the uploader's acquire
 * barriers. Until the render graph compiler and recorder exist (DENG-172, DENG-173), the target's
 * two barriers are recorded here around the live passes. DENG-176 replaces this function's body
 * with the per-view loop (src/render/views.c).
 */
static de_result render_frame_graph(de_renderer *r, de_arena *arena, de_gpu_cmd *cmd,
                                    de_gpu_texture target, u32 final_access) {
    de_rg *g = r->graph;
    de_result res = de__rg_begin(g, arena);
    if (res != DE_OK) {
        const de_gpu_barrier b = {
            .texture = target, .before = DE_ACCESS_NONE, .after = final_access};
        de_gpu_cmd_barriers(cmd, &b, 1);
        return res;
    }
    r->target_res = de_rg_import_texture(g, target, DE_ACCESS_NONE);
    de_rg_set_name(g, r->target_res, DE_HANDLE_VALID(r->swapchain) ? "swapchain" : "offscreen");
    de_rg_pass *clear = de_rg_add_pass(g, "clear", DE_QUEUE_GRAPHICS, clear_pass, r);
    de_rg_write(clear, r->target_res, DE_ACCESS_RENDER_TARGET);
    de_rg_export(g, r->target_res, final_access);
    res = de__rg_finalize(g);
    if (res == DE_OK) {
        de_gpu_barrier b = {.texture = target,
                            .before = DE_ACCESS_NONE, /* cleared: the old contents go */
                            .after = DE_ACCESS_RENDER_TARGET};
        de_gpu_cmd_barriers(cmd, &b, 1);
        for (u32 i = 0; i < g->pass_count; ++i)
            de__rg_execute_pass(g, i, cmd);
        b.before = DE_ACCESS_RENDER_TARGET;
        b.after = final_access;
        de_gpu_cmd_barriers(cmd, &b, 1);
        r->stats.pass_count = g->pass_count - g->culled_count;
    } else {
        /* Nothing drawn; the target still ends in its final state (a swapchain image must be
         * presentable). */
        const de_gpu_barrier b = {
            .texture = target, .before = DE_ACCESS_NONE, .after = final_access};
        de_gpu_cmd_barriers(cmd, &b, 1);
        r->stats.pass_count = 0;
    }
    de__rg_reset(g);
    return res;
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

    /* 3. This frame's copies. */
    if (r->uploader != NULL)
        de_render_uploader_flush(r->uploader);

    /* 4. The target. */
    de_gpu_texture target = r->offscreen;
    u32 final_access = DE__RENDER_OFFSCREEN_FINAL_ACCESS;
    if (DE_HANDLE_VALID(r->swapchain)) {
        const de_result a = de_gpu_swapchain_acquire(r->dev, r->swapchain, &target);
        if (a == DE_ERR_BUSY) {
            /* No image (minimised, resizing): nothing to draw, but the uploads still need their
             * acquire halves, or the uploader stalls. */
            if (r->uploader != NULL)
                (void)de_render_uploader_acquire_submit(r->uploader);
            return DE_OK;
        }
        if (a != DE_OK)
            return DE_FAIL_WRAP(a, "de_renderer_render_frame: swapchain image");
        final_access = DE_ACCESS_PRESENT;
    }

    /* 5 + 6. One graphics list: acquire barriers of the uploads, then the frame graph. */
    de_gpu_cmd *cmd = de_gpu_cmd_begin(r->dev, DE_QUEUE_GRAPHICS, "frame");
    if (cmd == NULL)
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "de_renderer_render_frame: command list");
    de_gpu_wait waits[1];
    u32 wait_count = 0;
    if (r->uploader != NULL) {
        de_gpu_wait w = {0};
        (void)de_render_uploader_acquire(r->uploader, cmd, &w);
        if (DE_HANDLE_VALID(w.fence) && w.value != 0)
            waits[wait_count++] = w;
    }
    const de_result gr = render_frame_graph(r, arena, cmd, target, final_access);
    if (gr != DE_OK) {
        /* Still submitted: the list holds the uploads' acquire barriers and must not leak. */
        DE_ERROR(r->log, "renderer: frame %llu: the frame graph failed", (unsigned long long)n);
    }
    const de_gpu_submit s = {.cmds = &cmd,
                             .cmd_count = 1,
                             .waits = waits,
                             .wait_count = wait_count,
                             .signal = r->frame_fence,
                             .signal_value = n + 1u};
    const de_result sr = de_gpu_queue_submit(r->dev, DE_QUEUE_GRAPHICS, &s);
    if (sr != DE_OK)
        return DE_FAIL_WRAP(sr, "de_renderer_render_frame: submit of frame %llu",
                            (unsigned long long)n);
    r->frame_index = n + 1u;
    if (DE_HANDLE_VALID(r->swapchain)) {
        const de_result p = de_gpu_swapchain_present(r->dev, r->swapchain);
        if (p != DE_OK && p != DE_ERR_BUSY)
            return DE_FAIL_WRAP(p, "de_renderer_render_frame: present");
    }
    return gr == DE_OK ? DE_OK : DE_FAIL_WRAP(gr, "de_renderer_render_frame: frame graph");
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
