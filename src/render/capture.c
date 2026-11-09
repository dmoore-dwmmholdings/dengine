#include "render/capture_internal.h"
#if defined(DE_DEV) || defined(DE_DEBUG)
#include "render/renderer_internal.h"
#include "gpu/gpu_backend.h"
#include <string.h>

de_result de__render_capture_request(de_renderer *r, const char *path, u32 frames) {
    if (!r || !path || !*path || strlen(path) >= sizeof r->capture.path || frames < 1 ||
        frames > 100)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "capture: expected a path and 1..100 frames");
    if (!r->dev->vt->capture_begin || !r->dev->vt->capture_end)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "capture: this backend has no launch capture adapter");
    de_mutex_lock(&r->lock);
    if (de_atomic_load_u32(&r->capture.state, DE_ATOMIC_ACQUIRE)) {
        de_mutex_unlock(&r->lock);
        return DE_FAIL(DE_ERR_BUSY, "capture: a request is already pending or active");
    }
    memcpy(r->capture.path, path, strlen(path) + 1u);
    r->capture.frames = frames;
    r->capture.completed = 0;
    de_atomic_store_u32(&r->capture.state, 1, DE_ATOMIC_RELEASE);
    de_mutex_unlock(&r->lock);
    return DE_OK;
}

void de__render_capture_begin(de_renderer *r) {
    if (de_atomic_load_u32(&r->capture.state, DE_ATOMIC_ACQUIRE) != 1)
        return;
    /* The nonzero mailbox state keeps the copied path immutable until the render thread
     * releases it. No lock is held across native driver calls. */
    if (r->dev->vt->capture_begin(r->dev, r->capture.path) != DE_OK) {
        DE_WARN(r->log, "capture: could not start %s", r->capture.path);
        de_atomic_store_u32(&r->capture.state, 0, DE_ATOMIC_RELEASE);
        return; /* In particular, never end a foreign capture after a failed begin. */
    }
    de_atomic_store_u32(&r->capture.state, 2, DE_ATOMIC_RELEASE);
}

void de__render_capture_abort(de_renderer *r) {
    const u32 state = de_atomic_load_u32(&r->capture.state, DE_ATOMIC_ACQUIRE);
    if (!state)
        return;
    if (state == 2) {
        /* No failing fence wait here: retain the caller's original render error chain. */
        de_gpu_wait_idle(r->dev);
        r->dev->vt->capture_end(r->dev);
    }
    DE_WARN(r->log, "capture: incomplete (%u/%u frames): %s", r->capture.completed,
            r->capture.frames, r->capture.path);
    de_atomic_store_u32(&r->capture.state, 0, DE_ATOMIC_RELEASE);
}

void de__render_capture_frame(de_renderer *r, bool success) {
    if (de_atomic_load_u32(&r->capture.state, DE_ATOMIC_ACQUIRE) != 2)
        return;
    if (!success) {
        de__render_capture_abort(r);
        return;
    }
    if (++r->capture.completed < r->capture.frames)
        return;
    const de_result done =
        de_gpu_fence_wait(r->dev, r->frame_fence, r->frame_index, DE__RENDER_FENCE_TIMEOUT_NS);
    de_gpu_wait_idle(r->dev); /* Includes presents and other queues, not only render CPU work. */
    r->dev->vt->capture_end(r->dev);
    if (done == DE_OK && !de_gpu_device_is_lost(r->dev))
        DE_INFO(r->log, "capture: complete (%u frames): %s", r->capture.completed, r->capture.path);
    else
        DE_WARN(r->log, "capture: GPU completion failed: %s", r->capture.path);
    de_atomic_store_u32(&r->capture.state, 0, DE_ATOMIC_RELEASE);
}
#endif
