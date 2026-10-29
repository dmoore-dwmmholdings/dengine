/* Explicit diagnostic command: copy the actual UI composite, using public render-graph APIs.
 * One bounded readback exists only while requested; ordinary frames allocate nothing here. */
#include "capture.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

static struct {
    de_engine *engine;
    bool active;
    _Atomic(u32) recorded;
    de_gpu_buffer buffer;
    de_rg_resource color, destination;
    u32 width, height, pitch;
    u64 after_frame;
    char path[DE_FS_PATH_MAX];
} capture;

static void copy_pixels(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    (void)user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, de_rg_buffer(pass, capture.destination), 0,
                                      capture.pitch, de_rg_texture(pass, capture.color), 0, 0);
    atomic_store_explicit(&capture.recorded, 1, memory_order_release);
}
static void hook(de_rg *graph, de_view view, void *user) {
    (void)user;
    de_renderer *renderer = capture.engine->renderer;
    if (view.id != de_renderer_main_view(renderer).id ||
        atomic_load_explicit(&capture.recorded, memory_order_acquire))
        return;
    de_render_hook_info info;
    if (de_view_hook_info(renderer, view, &info) && info.frame_index <= capture.after_frame)
        return;
    if (!de_view_hook_info(renderer, view, &info) || info.color_format != DE_FMT_RGBA16_FLOAT ||
        !info.width || !info.height || info.width > 2048 || info.height > 2048) {
        atomic_store_explicit(&capture.recorded, 2, memory_order_release);
        return;
    }
    capture.width = info.width;
    capture.height = info.height;
    capture.pitch = (info.width * 8 + 255u) & ~255u;
    capture.buffer = de_gpu_buffer_create(
        capture.engine->gpu, &(de_gpu_buffer_desc){.size = (u64)capture.pitch * info.height,
                                                   .usage = DE_USAGE_COPY_DST,
                                                   .memory = DE_MEM_READBACK});
    if (!capture.buffer.id) {
        atomic_store_explicit(&capture.recorded, 2, memory_order_release);
        return;
    }
    capture.color = de_view_color(renderer, view);
    capture.destination = de_rg_import_buffer(graph, capture.buffer, DE_ACCESS_NONE);
    de_rg_pass *pass =
        de_rg_add_pass(graph, "aseprite_ui.capture", DE_QUEUE_GRAPHICS, copy_pixels, NULL);
    if (!pass) {
        atomic_store_explicit(&capture.recorded, 2, memory_order_release);
        return;
    }
    de_rg_read(pass, capture.color, DE_ACCESS_COPY_SRC);
    de_rg_write(pass, capture.destination, DE_ACCESS_COPY_DST);
    de_rg_pass_side_effect(pass);
}

static de_result request(int argc, const char *const *argv, void *user) {
    (void)user;
    if (argc != 2 || strlen(argv[1]) >= sizeof capture.path)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "aseprite_ui.capture <writable-vpath>");
    if (capture.active)
        return DE_FAIL(DE_ERR_BUSY, "a UI capture is already pending");
    strcpy(capture.path, argv[1]);
    capture.after_frame = de_renderer_completed_frame(capture.engine->renderer) + 4;
    atomic_store_explicit(&capture.recorded, 0, memory_order_release);
    capture.active = true;
    de_render_add_hook(capture.engine->renderer, DE_HOOK_UI, hook, NULL);
    return DE_OK;
}
de_result sprite_ui_capture_init(de_engine *engine) {
    memset(&capture, 0, sizeof capture);
    capture.engine = engine;
    atomic_init(&capture.recorded, 0);
    return de_command_register("aseprite_ui.capture", "Capture main-view UI to a writable vpath",
                               request, NULL);
}
void sprite_ui_capture_poll(void) {
    if (!capture.active)
        return;
    u32 recorded = atomic_load_explicit(&capture.recorded, memory_order_acquire);
    if (!recorded)
        return;
    /* Removing the hook drains render/GPU work before touching or freeing its storage. */
    (void)de_render_remove_hook(capture.engine->renderer, DE_HOOK_UI, hook, NULL);
    if (recorded == 1) {
        size_t size = (size_t)capture.pitch * capture.height;
        u8 *bytes = malloc(12 + size);
        const void *pixels = de_gpu_buffer_map(capture.engine->gpu, capture.buffer);
        if (bytes && pixels) {
            /* Supported platforms are little-endian. Three u32s then padded RGBA16F rows. */
            memcpy(bytes, &capture.width, 4);
            memcpy(bytes + 4, &capture.height, 4);
            memcpy(bytes + 8, &capture.pitch, 4);
            memcpy(bytes + 12, pixels, size);
            de_result result = de_file_write_all_atomic(capture.path, bytes, 12 + size);
            if (result != DE_OK)
                DE_ERROR(DE_LOG_CAT_GENERAL, "UI capture failed: %s", de_result_str(result));
        }
        free(bytes);
    } else {
        DE_ERROR(DE_LOG_CAT_GENERAL, "UI capture failed: unsupported size/format or allocation");
    }
    if (capture.buffer.id)
        de_gpu_buffer_destroy(capture.engine->gpu, capture.buffer);
    capture.buffer = (de_gpu_buffer){0};
    capture.active = false;
}
void sprite_ui_capture_shutdown(void) {
    (void)de_command_unregister("aseprite_ui.capture");
    if (capture.active)
        (void)de_render_remove_hook(capture.engine->renderer, DE_HOOK_UI, hook, NULL);
    if (capture.buffer.id)
        de_gpu_buffer_destroy(capture.engine->gpu, capture.buffer);
}
