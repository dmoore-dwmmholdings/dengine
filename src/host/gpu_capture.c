#define _CRT_SECURE_NO_WARNINGS /* getenv: validated host launch options. */
#include "host/gpu_capture.h"
#if defined(DE_DEV) || defined(DE_DEBUG)
#include "host/host_internal.h"
#include "platform/vfs_internal.h"
#include <dengine/de_input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if DE_HAS_RENDER
#include "render/capture_internal.h"
#endif

de_result de__host_capture_prepare(de__host *h) {
    de__host_capture *c = &h->capture;
    for (int i = 1; h->desc.argv && i < h->desc.argc; ++i)
        if (h->desc.argv[i] && strcmp(h->desc.argv[i], "--gpu-capture") == 0)
            c->launch = true;
    const char *tool = getenv("DE_GPU_CAPTURE");
    if (!tool || !*tool) {
        if (c->launch)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "--gpu-capture requires DE_GPU_CAPTURE=xcode");
        return DE_OK;
    }
    /* Headless has no GPU. An inherited DIDE launch environment must not change that policy. */
    bool headless = h->desc.headless;
#if !DE_HAS_RENDER
    headless = true;
#endif
    if (headless) {
        if (c->launch)
            return DE_FAIL(DE_ERR_UNSUPPORTED, "capture: headless hosts have no GPU capture");
        return DE_OK;
    }
    if (strcmp(tool, "xcode") != 0)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "capture: only xcode is supported by this phase");
#if !defined(__APPLE__)
    return DE_FAIL(DE_ERR_UNSUPPORTED, "capture: xcode requires Metal on macOS");
#else
    const char *dir = getenv("DE_GPU_CAPTURE_DIR");
    bool is_dir = false;
    if (!dir || dir[0] != '/' || strlen(dir) >= sizeof c->directory ||
        de__native_stat(dir, &is_dir) != DE_OK || !is_dir)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "capture: DE_GPU_CAPTURE_DIR must be an existing absolute directory");
    const char *frames = getenv("DE_GPU_CAPTURE_FRAMES");
    c->frames = 1;
    if (frames) {
        if (!*frames)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "capture: frame count must be 1..100");
        u32 n = 0;
        for (const char *p = frames; *p; ++p) {
            if (*p < '0' || *p > '9' || n > 100)
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "capture: frame count must be 1..100");
            n = n * 10 + (u32)(*p - '0');
        }
        if (!n || n > 100)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "capture: frame count must be 1..100");
        c->frames = n;
    }
    const char *enabled = getenv("MTL_CAPTURE_ENABLED");
    if (enabled && strcmp(enabled, "1") != 0)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "capture: existing MTL_CAPTURE_ENABLED must be 1");
    if (!enabled) {
        if (setenv("MTL_CAPTURE_ENABLED", "1", 0) != 0)
            return DE_FAIL(DE_ERR_IO, "capture: could not enable Metal document capture");
        c->enabled_metal_env = true;
    }
    strcpy(c->directory, dir);
    c->armed = true;
    return DE_OK;
#endif
}

bool de__host_capture_pressed(const struct de_input *in) {
    return in && de_key_pressed(in, DE_KEY_F12) &&
           (de_key_down(in, DE_KEY_LCTRL) || de_key_down(in, DE_KEY_RCTRL)) &&
           (de_key_down(in, DE_KEY_LSHIFT) || de_key_down(in, DE_KEY_RSHIFT));
}

void de__host_capture_poll(de__host *h) {
#if DE_HAS_RENDER
    de__host_capture *c = &h->capture;
    if (!c->armed)
        return;
    const bool hotkey = de__host_capture_pressed(h->engine->input);
    if (!c->launch && !hotkey)
        return;
    c->launch = false; /* One explicit request, not one per render-loop iteration. */
    if (!h->engine->renderer) {
        DE_WARN(h->log, "capture: no renderer is available");
        return;
    }
    char path[1024];
    const int n = snprintf(path, sizeof path, "%s/capture-%u-%llu.gputrace", c->directory,
                           de__native_process_id(), (unsigned long long)de_time_now_ns());
    if (n < 0 || (size_t)n >= sizeof path) {
        DE_WARN(h->log, "capture: output path is too long");
        return;
    }
    const de_result result = de__render_capture_request(h->engine->renderer, path, c->frames);
    if (result != DE_OK)
        DE_WARN(h->log, "capture: request rejected (%s)", de_result_str(result));
#else
    (void)h;
#endif
}

void de__host_capture_cleanup(de__host *h) {
#if defined(__APPLE__)
    /* Host runs are sequential; do not erase a later explicit environment change. */
    const char *value = getenv("MTL_CAPTURE_ENABLED");
    if (h->capture.enabled_metal_env && value && strcmp(value, "1") == 0)
        (void)unsetenv("MTL_CAPTURE_ENABLED");
#else
    (void)h;
#endif
}
#endif
