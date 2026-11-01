#include "host/splash.h"
#include "host/host_internal.h"
#include <string.h>
#if DE_HAS_RENDER
#include <dengine/de_gpu.h>
#include "render/shaders/engine_shaders.h"
#include "host/branding/splash_atlas.h"
#include <stb_image.h>
#endif

static f32 ease(f32 x) {
    if (x <= 0)
        return 0;
    if (x >= 1)
        return 1;
    return x * x * (3 - 2 * x);
}
de__splash_sample de__splash_at(u64 ns, bool still) {
    if (still)
        return (de__splash_sample){1, .64f, 1, 1, 1, ns >= 650000000ull};
    f32 seconds = (f32)((f64)ns / 1000000000.0);
    f32 enter = ease(seconds / .3f), leave = 1 - ease((seconds - 2.55f) / .45f);
    return (de__splash_sample){enter * leave,
                               (f32)((f64)ns / DE__SPLASH_DURATION_NS),
                               ease(seconds / 2.16f),
                               ease((seconds - 1.14f) / .54f),
                               leave,
                               ns >= DE__SPLASH_DURATION_NS};
}
#if DE_HAS_RENDER
static bool arg_is(const de__host *h, const char *arg) {
    for (int i = 1; h->desc.argv && i < h->desc.argc; ++i)
        if (h->desc.argv[i] && !strcmp(h->desc.argv[i], arg))
            return true;
    return false;
}
#endif
bool de__splash_quit_event(const de_app_event *event, de_window window) {
    return event && (event->kind == DE_EV_QUIT ||
                     (event->kind == DE_EV_WINDOW_CLOSE && event->window.id == window.id));
}
bool de__splash_enabled(const de__host *h) {
    if (!h || !h->engine || !DE_HANDLE_VALID(h->engine->window) || h->desc.headless ||
        h->desc.offscreen || de_engine_editor_session(h->engine))
        return false;
    bool enabled = !h->desc.skip_splash;
    bool bounded = h->loop_max_frames != 0;
    for (int i = 1; h->desc.argv && i < h->desc.argc; ++i) {
        const char *s = h->desc.argv[i];
        if (!s)
            continue;
        if (!strcmp(s, "--frames") || !strncmp(s, "--frames=", 9))
            bounded = true;
    }
    if (bounded)
        enabled = false;
    for (int i = 1; h->desc.argv && i < h->desc.argc; ++i) {
        const char *s = h->desc.argv[i];
        if (s && !strcmp(s, "--splash"))
            enabled = true;
        if (s && !strcmp(s, "--no-splash"))
            enabled = false;
    }
    return enabled;
}

#if DE_HAS_RENDER
typedef struct splash_push {
    f32 width, height, time, alpha, text_alpha, pad;
    u32 image, sampler;
} splash_push;
static de_result splash_pipeline(de_gpu_device *d, const de_allocator *allocator,
                                 de_gpu_pipeline *out) {
    *out = (de_gpu_pipeline){0};
    de__engine_shader_code code = {0};
    static const char *const entries[] = {"vs_main", "ps_main"};
    char why[256] = {0};
    de_result r = de__engine_shader_load(d, allocator, "startup_splash", 0, entries, 2, &code, why,
                                         sizeof why);
    if (r == DE_OK) {
        de_gpu_pipeline_desc pd = {.kind = DE_PIPE_GRAPHICS,
                                   .vs = code.entries[0].blob,
                                   .ps = code.entries[1].blob,
                                   .color_formats = {DE_FMT_BGRA8_SRGB},
                                   .color_count = 1,
                                   .cull = DE_CULL_NONE,
                                   .push_constant_bytes = sizeof(splash_push),
                                   .name = "Startup splash"};
        *out = de_gpu_pipeline_create(d, &pd);
        if (!DE_HANDLE_VALID(*out))
            r = DE_ERR_INTERNAL;
    }
    de__engine_shader_code_free(&code);
    return r;
}
/* Fixed atlas upload; neither font decoding nor allocations occur in a splash frame. */
void de__splash_destroy(de_gpu_device *d, de__splash_gpu *gpu) {
    if (DE_HANDLE_VALID(gpu->pipeline))
        de_gpu_pipeline_destroy(d, gpu->pipeline);
    if (DE_HANDLE_VALID(gpu->atlas))
        de_gpu_texture_destroy(d, gpu->atlas);
    if (DE_HANDLE_VALID(gpu->sampler))
        de_gpu_sampler_destroy(d, gpu->sampler);
    memset(gpu, 0, sizeof *gpu);
}
de_result de__splash_create(de_gpu_device *d, const de_allocator *allocator, de__splash_gpu *out) {
    memset(out, 0, sizeof *out);
    int width = 0, height = 0, channels = 0;
    unsigned char *pixels = NULL;
    de_gpu_buffer staging = {0};
    de_gpu_fence fence = {0};
    de_result r = DE_ERR_INTERNAL;
    if (!stbi_info_from_memory(de__splash_atlas_png, sizeof de__splash_atlas_png, &width, &height,
                               &channels) ||
        width != 512 || height != 768)
        goto cleanup;
    pixels = stbi_load_from_memory(de__splash_atlas_png, sizeof de__splash_atlas_png, &width,
                                   &height, &channels, 4);
    if (!pixels)
        goto cleanup;
    out->atlas = de_gpu_texture_create(
        d, &(de_gpu_texture_desc){.width = 512,
                                  .height = 768,
                                  .format = DE_FMT_RGBA8_UNORM,
                                  .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST,
                                  .name = "Startup brand atlas"});
    out->sampler = de_gpu_sampler_create(d, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                                   .mag = DE_FILTER_LINEAR,
                                                                   .mip = DE_FILTER_LINEAR,
                                                                   .u = DE_ADDR_CLAMP,
                                                                   .v = DE_ADDR_CLAMP,
                                                                   .w = DE_ADDR_CLAMP});
    staging = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = 512 * 768 * 4,
                                                            .usage = DE_USAGE_COPY_SRC,
                                                            .memory = DE_MEM_UPLOAD,
                                                            .name = "Startup atlas upload"});
    fence = de_gpu_fence_create(d, 0);
    if (!DE_HANDLE_VALID(out->atlas) || !DE_HANDLE_VALID(out->sampler) ||
        !DE_HANDLE_VALID(staging) || !DE_HANDLE_VALID(fence)) {
        r = DE_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    void *mapped = de_gpu_buffer_map(d, staging);
    if (!mapped)
        goto cleanup;
    memcpy(mapped, pixels, 512 * 768 * 4);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "Startup atlas upload");
    if (!cmd)
        goto cleanup;
    de_gpu_barrier barrier = {.texture = out->atlas, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_buffer_to_texture(cmd, out->atlas, 0, 0, staging, 0, 512 * 4);
    barrier.before = DE_ACCESS_COPY_DST;
    barrier.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    r = de_gpu_queue_submit(
        d, DE_QUEUE_GRAPHICS,
        &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1, .signal = fence, .signal_value = 1});
    if (r != DE_OK)
        goto cleanup;
    r = de_gpu_fence_wait(d, fence, 1, 2000000000ull);
    if (r != DE_OK)
        goto cleanup;
    out->image_index = de_gpu_texture_bindless(d, out->atlas, 0, false);
    out->sampler_index = de_gpu_sampler_bindless(d, out->sampler);
    r = splash_pipeline(d, allocator, &out->pipeline);
cleanup:
    stbi_image_free(pixels);
    if (DE_HANDLE_VALID(staging))
        de_gpu_buffer_destroy(d, staging);
    if (DE_HANDLE_VALID(fence))
        de_gpu_fence_destroy(d, fence);
    if (r != DE_OK)
        de__splash_destroy(d, out);
    return r;
}
void de__splash_record(de_gpu_cmd *cmd, de_gpu_texture target, const de__splash_gpu *gpu, u32 width,
                       u32 height, de__splash_sample sample) {
    if (sample.done)
        sample = (de__splash_sample){0};
    de_gpu_attachment color = {.texture = target, .clear = true, .store = true};
    de_gpu_cmd_begin_render(cmd, &color, 1, NULL);
    de_gpu_cmd_set_pipeline(cmd, gpu->pipeline);
    de_gpu_cmd_viewport(cmd, 0, 0, (f32)width, (f32)height);
    de_gpu_cmd_scissor(cmd, 0, 0, width, height);
    splash_push p = {(f32)width,        (f32)height, sample.reveal,    sample.alpha,
                     sample.text_alpha, 0,           gpu->image_index, gpu->sampler_index};
    de_gpu_cmd_push_constants(cmd, &p, sizeof p);
    de_gpu_cmd_draw(cmd, 3, 1, 0, 0);
    de_gpu_cmd_end_render(cmd);
}
/* One frame in flight keeps this short startup path simple and bounded. */
static de_result splash_frame(de_gpu_device *d, de_gpu_swapchain sc, const de__splash_gpu *gpu,
                              de_gpu_fence fence, u64 *serial, u32 width, u32 height,
                              de__splash_sample sample) {
    if (*serial)
        DE_TRY(de_gpu_fence_wait(d, fence, *serial, 2000000000ull));
    de_gpu_texture target = {0};
    DE_TRY(de_gpu_swapchain_acquire(d, sc, &target));
    de_gpu_cmd *cmd = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "Startup splash");
    if (!cmd)
        return DE_ERR_INTERNAL;
    de_gpu_barrier barrier = {
        .texture = target, .before = DE_ACCESS_NONE, .after = DE_ACCESS_RENDER_TARGET};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de__splash_record(cmd, target, gpu, width, height, sample);
    barrier.before = DE_ACCESS_RENDER_TARGET;
    barrier.after = DE_ACCESS_PRESENT;
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_submit submit = {
        .cmds = &cmd, .cmd_count = 1, .signal = fence, .signal_value = ++*serial};
    DE_TRY(de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &submit));
    return de_gpu_swapchain_present(d, sc);
}
#endif

void de__host_splash(de__host *h) {
#if DE_HAS_RENDER
    if (!de__splash_enabled(h))
        return;
    de_gpu_device *d = NULL;
    de_gpu_swapchain sc = {0};
    de__splash_gpu gpu = {0};
    de_gpu_fence fence = {0};
    de_gpu_device_desc device = {.allocator = h->engine->allocator};
    de_result r = de_gpu_device_create(&device, &d);
    if (r != DE_OK)
        goto cleanup;
    sc = de_gpu_swapchain_create(d, &(de_gpu_swapchain_desc){.window = h->engine->window});
    if (!DE_HANDLE_VALID(sc)) {
        r = DE_ERR_UNSUPPORTED;
        goto cleanup;
    }
    r = de__splash_create(d, h->engine->allocator, &gpu);
    if (r != DE_OK)
        goto cleanup;
    fence = de_gpu_fence_create(d, 0);
    if (!DE_HANDLE_VALID(fence)) {
        r = DE_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    u32 width = 0, height = 0;
    de_window_size_pixels(h->engine->window, &width, &height);
    if (!width || !height)
        goto cleanup;
    r = de_gpu_swapchain_resize(d, sc, width, height);
    if (r != DE_OK)
        goto cleanup;
    bool still = arg_is(h, "--splash-still");
    u64 start = de_time_now_ns(), serial = 0;
    DE_INFO(h->log, "DEngine startup splash");
    for (;;) {
        u64 frame_start = de_time_now_ns();
        de_app_event events[16];
        u32 count;
        do {
            count = de_platform_pump(events, 16);
            for (u32 i = 0; i < count; ++i)
                if (de__splash_quit_event(&events[i], h->engine->window))
                    de_engine_request_quit(h->engine);
        } while (count == 16);
        if (de_engine_quit_requested(h->engine))
            break;
        u32 w = 0, hh = 0;
        de_window_size_pixels(h->engine->window, &w, &hh);
        if (w && hh && (w != width || hh != height)) {
            if (serial)
                r = de_gpu_fence_wait(d, fence, serial, 2000000000ull);
            if (r != DE_OK)
                break;
            r = de_gpu_swapchain_resize(d, sc, w, hh);
            if (r != DE_OK)
                break;
            width = w;
            height = hh;
        }
        de__splash_sample sample = de__splash_at(frame_start - start, still);
        if (sample.done)
            sample = (de__splash_sample){.done = true};
        if (w && hh) {
            r = splash_frame(d, sc, &gpu, fence, &serial, width, height, sample);
            if (r == DE_ERR_BUSY) {
                de_error_clear();
                r = DE_OK;
            } else if (r != DE_OK)
                break;
        }
        if (sample.done)
            break;
        de_sleep_until_ns(frame_start + 16666667ull);
    }
    if (serial && r == DE_OK)
        r = de_gpu_fence_wait(d, fence, serial, 2000000000ull);
cleanup:
    if (d) {
        if (DE_HANDLE_VALID(fence))
            de_gpu_fence_destroy(d, fence);
        de__splash_destroy(d, &gpu);
        if (DE_HANDLE_VALID(sc))
            de_gpu_swapchain_destroy(d, sc);
        de_gpu_device_destroy(d);
    }
    if (r != DE_OK) {
        DE_WARN(h->log, "startup splash unavailable (%s); continuing startup", de_result_str(r));
        de_error_clear();
    }
#else
    (void)h;
#endif
}
