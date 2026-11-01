/* DENG-703: app-owned native GPU frames, not a desktop/screen-recording capture.
 * Run with an existing output directory. Uses the exact host pipeline/recording seams. */
#include "host/splash.h"
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <stb_image_write.h>

#define WIDTH  960u
#define HEIGHT 600u
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: splash-preview <existing-output-directory>\n");
        return 2;
    }
    de_gpu_device *d = NULL;
    de__splash_gpu gpu = {0};
    de_gpu_texture target = {0};
    de_gpu_buffer readback = {0};
    de_gpu_fence fence = {0};
    unsigned char *pixels = NULL;
    de_result r = de_gpu_device_create(&(de_gpu_device_desc){0}, &d);
    if (r != DE_OK)
        goto cleanup;
    r = de__splash_create(d, de_heap_allocator(), &gpu);
    if (r != DE_OK)
        goto cleanup;
    target = de_gpu_texture_create(
        d, &(de_gpu_texture_desc){.width = WIDTH,
                                  .height = HEIGHT,
                                  .format = DE_FMT_BGRA8_SRGB,
                                  .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_SRC,
                                  .name = "Splash preview"});
    readback = de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = WIDTH * HEIGHT * 4,
                                                             .usage = DE_USAGE_COPY_DST,
                                                             .memory = DE_MEM_READBACK,
                                                             .name = "Splash readback"});
    fence = de_gpu_fence_create(d, 0);
    pixels = malloc(WIDTH * HEIGHT * 4);
    if (!DE_HANDLE_VALID(target) || !DE_HANDLE_VALID(readback) || !DE_HANDLE_VALID(fence) ||
        !pixels) {
        r = DE_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (u32 frame = 0; frame <= 90; ++frame) {
        de__splash_sample sample = de__splash_at((u64)frame * 1000000000ull / 30, false);
        de_gpu_cmd *cmd = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "Splash preview");
        if (!cmd) {
            r = DE_ERR_INTERNAL;
            goto cleanup;
        }
        de_gpu_barrier barrier = {.texture = target,
                                  .before = frame ? DE_ACCESS_COPY_SRC : DE_ACCESS_NONE,
                                  .after = DE_ACCESS_RENDER_TARGET};
        de_gpu_cmd_barriers(cmd, &barrier, 1);
        de__splash_record(cmd, target, &gpu, WIDTH, HEIGHT, sample);
        barrier.before = DE_ACCESS_RENDER_TARGET;
        barrier.after = DE_ACCESS_COPY_SRC;
        de_gpu_cmd_barriers(cmd, &barrier, 1);
        de_gpu_cmd_copy_texture_to_buffer(cmd, readback, 0, WIDTH * 4, target, 0, 0);
        de_gpu_submit submit = {
            .cmds = &cmd, .cmd_count = 1, .signal = fence, .signal_value = (u64)frame + 1};
        r = de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &submit);
        if (r != DE_OK)
            goto cleanup;
        r = de_gpu_fence_wait(d, fence, (u64)frame + 1, 2000000000ull);
        if (r != DE_OK)
            goto cleanup;
        const unsigned char *mapped = de_gpu_buffer_map(d, readback);
        if (!mapped) {
            r = DE_ERR_INTERNAL;
            goto cleanup;
        }
        u32 bright = 0, nonblack = 0;
        for (u32 i = 0; i < WIDTH * HEIGHT; ++i) {
            pixels[i * 4] = mapped[i * 4 + 2];
            pixels[i * 4 + 1] = mapped[i * 4 + 1];
            pixels[i * 4 + 2] = mapped[i * 4];
            pixels[i * 4 + 3] = 255;
            bright += pixels[i * 4] > 150;
            nonblack += pixels[i * 4] || pixels[i * 4 + 1] || pixels[i * 4 + 2];
        }
        if ((frame == 0 && bright != 0) ||
            (frame == 54 && (bright < 100 || bright > WIDTH * HEIGHT / 8)) ||
            (frame == 90 && nonblack != 0)) {
            fprintf(stderr, "pixel oracle failed frame%u bright%u nonblack%u\n", frame, bright,
                    nonblack);
            r = DE_ERR_INTERNAL;
            goto cleanup;
        }
        char file[2048];
        if (snprintf(file, sizeof file, "%s/frame-%03u.png", argv[1], frame) < 0 ||
            !stbi_write_png(file, WIDTH, HEIGHT, 4, pixels, WIDTH * 4)) {
            r = DE_ERR_IO;
            goto cleanup;
        }
    }
    puts("91 native GPU frames; initial/hold/final pixel checks passed");
cleanup:
    free(pixels);
    if (d) {
        if (DE_HANDLE_VALID(fence))
            de_gpu_fence_destroy(d, fence);
        if (DE_HANDLE_VALID(readback))
            de_gpu_buffer_destroy(d, readback);
        if (DE_HANDLE_VALID(target))
            de_gpu_texture_destroy(d, target);
        de__splash_destroy(d, &gpu);
        de_gpu_device_destroy(d);
    }
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        fprintf(stderr, "splash preview failed: %s: %s\n", de_result_str(r), e ? e->message : "");
        return 1;
    }
    return 0;
}
