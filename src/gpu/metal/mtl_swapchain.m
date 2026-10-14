/* mtl_swapchain.m — the CAMetalLayer swapchain (DENG-288, SPEC §12.1).
 *
 * The layer comes from the platform layer (de_window_native(w).metal_layer, S03 DENG-250): the
 * window must be created with DE_WINDOW_SURFACE_METAL, and swapchain create runs on the main
 * thread like every window call. The RHI owns the drawable size (the platform turns SDL's
 * automatic resize off): create sets it to the window's pixel size, resize sets it again.
 *
 *   - Format BGRA8 sRGB, colour space sRGB only (HDR/EDR is S18: other colour spaces fail with
 *     DE_ERR_UNSUPPORTED, as on Vulkan). framebufferOnly is off so images can be copied
 *     (screenshots, golden images).
 *   - image_count 0 means 3; Metal allows 2 or 3 drawables (maximumDrawableCount), larger values
 *     become 3, 1 becomes 2.
 *   - DE_PRESENT_VSYNC: displaySyncEnabled; DE_PRESENT_IMMEDIATE: off. Metal has no mailbox
 *     mode: DE_PRESENT_MAILBOX uses VSYNC. allow_vrr changes nothing (macOS drives adaptive sync
 *     itself).
 *   - acquire: nextDrawable (waits up to one second for a free drawable, then DE_ERR_TIMEOUT).
 *     Its texture is wrapped in a transient de_gpu_texture (RENDER_TARGET | COPY_SRC | COPY_DST:
 *     no bindless index, so no residency). Until present, acquire returns the same image. A
 *     zero-size drawable (minimised window) gives DE_ERR_BUSY.
 *   - present: presentDrawable: in a command buffer of the graphics queue, committed after the
 *     lists that render the image. Drawable textures are hazard-tracked by Metal, so Core
 *     Animation shows the image when that work is done. The wrapper is destroyed (deferred).
 *   - resize: sets drawableSize; an image acquired and not presented is dropped.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "gpu/metal/mtl_internal.h"

#include <dengine/de_platform.h>

static mtl_swapchain *payload(de__gpu_swapchain *s) {
    return DE__GPU_PAYLOAD(mtl_swapchain, s);
}

static CAMetalLayer *layer_of(mtl_swapchain *ms) {
    return (__bridge CAMetalLayer *)ms->layer;
}

/* Drops the acquired image (its wrapper goes through deferred destruction). */
static void drop_image(de_gpu_device *d, mtl_swapchain *ms) {
    if (ms->drawable) {
        @autoreleasepool {
            (void)CFBridgingRelease(ms->drawable);
        }
    }
    ms->drawable = NULL;
    if (ms->image.id)
        de_gpu_texture_destroy(d, ms->image);
    ms->image.id = 0;
}

de_result de__mtl_swapchain_create(de_gpu_device *d, de__gpu_swapchain *s,
                                   const de_gpu_swapchain_desc *desc) {
    static const char F[] = "de_gpu_swapchain_create";
    mtl_swapchain *ms = payload(s);
    if (desc->colorspace != DE_CS_SRGB)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "%s: metal: colour space %d; only DE_CS_SRGB until S18 (HDR)", F,
                            (int)desc->colorspace);
    const de_native_window nw = de_window_native(desc->window);
    if (!nw.metal_layer)
        return DE_FAIL_WRAP(DE_ERR_UNSUPPORTED,
                            "%s: metal: the window has no Metal layer (create it with "
                            "DE_WINDOW_SURFACE_METAL)",
                            F);
    u32 w = 0, h = 0;
    de_window_size_pixels(desc->window, &w, &h);
    u32 count = desc->image_count ? desc->image_count : 3u;
    count = count < 2u ? 2u : count > 3u ? 3u : count;
    @autoreleasepool {
        id obj = (__bridge id)nw.metal_layer;
        if (![obj isKindOfClass:[CAMetalLayer class]])
            return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                                "%s: metal: the window's layer is not a "
                                "CAMetalLayer",
                                F);
        CAMetalLayer *layer = (CAMetalLayer *)obj;
        layer.device = (__bridge id<MTLDevice>)de__mtl_dev(d)->device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
        layer.framebufferOnly = NO;
        layer.maximumDrawableCount = count;
        layer.allowsNextDrawableTimeout = YES;
        ms->present =
            desc->present == DE_PRESENT_IMMEDIATE ? DE_PRESENT_IMMEDIATE : DE_PRESENT_VSYNC;
        layer.displaySyncEnabled = ms->present != DE_PRESENT_IMMEDIATE;
        if (w && h)
            layer.drawableSize = CGSizeMake(w, h);
        ms->layer = (void *)CFBridgingRetain(layer);
    }
    ms->width = w;
    ms->height = h;
    if (desc->present == DE_PRESENT_MAILBOX)
        DE_INFO(d->log, "metal: no mailbox present mode; the swapchain uses VSYNC");
    return DE_OK;
}

void de__mtl_swapchain_destroy(de_gpu_device *d, de__gpu_swapchain *s) {
    mtl_swapchain *ms = payload(s);
    drop_image(d, ms);
    if (ms->layer) {
        @autoreleasepool {
            (void)CFBridgingRelease(ms->layer);
        }
    }
    ms->layer = NULL;
}

de_result de__mtl_swapchain_acquire(de_gpu_device *d, de__gpu_swapchain *s, de_gpu_texture *image) {
    static const char F[] = "de_gpu_swapchain_acquire";
    mtl_swapchain *ms = payload(s);
    if (ms->image.id) { /* acquired and not presented yet: the same image */
        *image = ms->image;
        return DE_OK;
    }
    if (ms->width == 0 || ms->height == 0)
        return DE_FAIL(DE_ERR_BUSY, "%s: the drawable has no area (minimised); resize it", F);
    @autoreleasepool {
        id<CAMetalDrawable> dr = [layer_of(ms) nextDrawable];
        if (dr == nil)
            return DE_FAIL(DE_ERR_TIMEOUT, "%s: metal: no drawable became free within a second", F);
        id<MTLTexture> tex = dr.texture;
        de_gpu_texture_desc td = {.dim = DE_TEX_2D,
                                  .format = DE_FMT_BGRA8_SRGB,
                                  .width = (u32)tex.width,
                                  .height = (u32)tex.height,
                                  .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_SRC |
                                           DE_USAGE_COPY_DST,
                                  .name = "swapchain image"};
        de__gpu_texture *t = NULL;
        de_gpu_texture h = de__gpu_texture_wrap(d, &td, &t);
        if (!h.id || !t)
            return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "%s: metal: drawable texture handle", F);
        DE__GPU_PAYLOAD(mtl_texture, t)->texture = (void *)CFBridgingRetain(tex);
        de__gpu_texture_wrap_finish(d, t);
        ms->drawable = (void *)CFBridgingRetain(dr);
        ms->image = h;
        *image = h;
    }
    return DE_OK;
}

de_result de__mtl_swapchain_present(de_gpu_device *d, de__gpu_swapchain *s) {
    static const char F[] = "de_gpu_swapchain_present";
    mtl_swapchain *ms = payload(s);
    if (!ms->drawable)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s: no image is acquired", F);
    de_result r = DE_OK;
    @autoreleasepool {
        id<MTLCommandQueue> q =
            (__bridge id<MTLCommandQueue>)de__mtl_dev(d)->queues[DE_QUEUE_GRAPHICS];
        id<MTLCommandBuffer> cb = [q commandBuffer]; /* retains the drawable until shown */
        if (cb == nil) {
            r = de__gpu_fail(d, DE_ERR_DEVICE_LOST, "%s: metal: no command buffer", F);
        } else {
            cb.label = @"de present";
            [cb presentDrawable:(__bridge id<CAMetalDrawable>)ms->drawable];
            [cb commit];
        }
    }
    drop_image(d, ms);
    return r;
}

de_result de__mtl_swapchain_resize(de_gpu_device *d, de__gpu_swapchain *s, u32 w, u32 h) {
    mtl_swapchain *ms = payload(s);
    drop_image(d, ms); /* its size is the old one */
    ms->width = w;
    ms->height = h;
    if (w && h) {
        @autoreleasepool {
            layer_of(ms).drawableSize = CGSizeMake(w, h);
        }
    }
    return DE_OK;
}

de_gpu_colorspace de__mtl_swapchain_colorspace(de_gpu_device *d, de__gpu_swapchain *s) {
    (void)d;
    (void)s;
    return DE_CS_SRGB;
}
