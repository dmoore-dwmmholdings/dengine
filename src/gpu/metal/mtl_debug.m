/* mtl_debug.m — debug groups, labels and programmatic capture on Metal (DENG-326; SPEC §18.3).
 *
 * Debug groups (de_gpu_cmd_marker_push / pop). Encoders are opened lazily (mtl_cmd.m), so a
 * marker cannot simply go on "the current encoder":
 *   - inside a render pass (the render encoder is open and end_render was not seen) the group is
 *     pushed on the render encoder, so captures show it among the draws. The front-end checks
 *     (Debug/Dev) that such a marker is popped before end_render, so it is popped on the same
 *     encoder; marker_encoder[level] records which one (the list's encoder count), and a pop on a
 *     different encoder (misuse in Release, or a render pass split by an indirect-count draw,
 *     mtl_icb.m) pops nothing.
 *   - elsewhere the open blit or compute encoder (or a render encoder whose pass ended) is ended
 *     first, and the group goes on the command buffer, where it holds the encoders made inside it.
 * The colour is not shown by Xcode; Metal has no colour for debug groups.
 *
 * Labels (object_name): buffers, textures and their per-mip storage views, fences
 * (MTLSharedEvent) and query heaps get the name as `label`. Pipeline states and samplers take
 * their label from their descriptor at creation only: pipelines are labelled by mtl_pipeline.m
 * from the record's name (a default "<kind>#<slot>" in Debug/Dev), samplers are not labelled.
 *
 * Capture (test hook, also for tools): MTLCaptureManager into a .gputrace document. Metal allows
 * it only when MTL_CAPTURE_ENABLED=1 was in the environment when the process made its first
 * device (or the app's Info.plist sets MetalCaptureEnabled).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "gpu/metal/mtl_internal.h"

#include <string.h>

static mtl_cmd *cmd_payload(de_gpu_cmd *c) {
    return (mtl_cmd *)de__gpu_cmd_payload(c);
}

/* ---------- Debug groups ---------- */

static NSString *group_name(const char *name) {
    NSString *s = name ? [NSString stringWithUTF8String:name] : nil;
    return s ? s : @"(marker)";
}

void de__mtl_cmd_marker_push(de_gpu_cmd *c, const char *name, u32 color) {
    (void)color;
    mtl_cmd *mc = cmd_payload(c);
    const u32 level = c->marker_depth - 1u; /* the front-end counted this marker already */
    if (!mc->cb)
        return;
    @autoreleasepool {
        if (c->in_render && mc->encoder && mc->kind == DE__MTL_ENC_RENDER && !mc->render_ended) {
            if (level >= DE__GPU_MARKER_LEVELS)
                return; /* too deep to remember for the pop: not shown */
            [(__bridge id<MTLRenderCommandEncoder>)mc->encoder pushDebugGroup:group_name(name)];
            mc->marker_encoder[level] = mc->encoders;
            return;
        }
        de__mtl_encoder_end(c);
        if (level < DE__GPU_MARKER_LEVELS)
            mc->marker_encoder[level] = 0;
        [(__bridge id<MTLCommandBuffer>)mc->cb pushDebugGroup:group_name(name)];
    }
}

void de__mtl_cmd_marker_pop(de_gpu_cmd *c) {
    mtl_cmd *mc = cmd_payload(c);
    const u32 level = c->marker_depth; /* the front-end already uncounted it */
    if (!mc->cb)
        return;
    const u32 on = level < DE__GPU_MARKER_LEVELS ? mc->marker_encoder[level] : 0u;
    @autoreleasepool {
        if (on != 0) {
            if (mc->encoder && mc->kind == DE__MTL_ENC_RENDER && mc->encoders == on)
                [(__bridge id<MTLRenderCommandEncoder>)mc->encoder popDebugGroup];
            return; /* its encoder already ended: the group ended with it */
        }
        if (level >= DE__GPU_MARKER_LEVELS && c->in_render)
            return; /* pushed nowhere (see push) */
        de__mtl_encoder_end(c);
        [(__bridge id<MTLCommandBuffer>)mc->cb popDebugGroup];
    }
}

/* ---------- Labels ---------- */

void de__mtl_object_name(de_gpu_device *d, de__gpu_kind kind, void *rec, const char *name) {
    (void)d;
    @autoreleasepool {
        NSString *label = [NSString stringWithUTF8String:name];
        if (label == nil)
            return;
        switch (kind) {
        case DE__GPU_BUFFER: {
            mtl_buffer *b = DE__GPU_PAYLOAD(mtl_buffer, (de__gpu_buffer *)rec);
            if (b->buffer)
                ((__bridge id<MTLBuffer>)b->buffer).label = label;
            break;
        }
        case DE__GPU_TEXTURE: {
            mtl_texture *t = DE__GPU_PAYLOAD(mtl_texture, (de__gpu_texture *)rec);
            if (t->texture)
                ((__bridge id<MTLTexture>)t->texture).label = label;
            for (u32 m = 0; m < DE_GPU_MAX_MIPS; ++m)
                if (t->views[m])
                    ((__bridge id<MTLTexture>)t->views[m]).label = label;
            break;
        }
        case DE__GPU_FENCE: {
            mtl_fence *f = DE__GPU_PAYLOAD(mtl_fence, (de__gpu_fence *)rec);
            if (f->event)
                ((__bridge id<MTLSharedEvent>)f->event).label = label;
            break;
        }
        case DE__GPU_QUERY_HEAP: {
            /* The sample buffer got the name from its descriptor at creation. */
            mtl_query_heap *q = DE__GPU_PAYLOAD(mtl_query_heap, (de__gpu_query_heap *)rec);
            if (q->staging)
                ((__bridge id<MTLBuffer>)q->staging).label = label;
            break;
        }
        default:
            break;
        }
    }
}

/* ---------- Test hooks ---------- */

static bool is_metal(de_gpu_device *d) {
    return d && d->vt == &de__gpu_metal_vtable;
}

de_result de__mtl_capture_begin(de_gpu_device *d, const char *path) {
    if (!is_metal(d) || !path)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__mtl_capture_begin: not a Metal device");
    @autoreleasepool {
        MTLCaptureManager *mgr = [MTLCaptureManager sharedCaptureManager];
        if (![mgr supportsDestination:MTLCaptureDestinationGPUTraceDocument])
            return DE_FAIL(DE_ERR_UNSUPPORTED,
                           "metal: GPU capture to a document is off; set MTL_CAPTURE_ENABLED=1 "
                           "before the first Metal device");
        MTLCaptureDescriptor *cd = [MTLCaptureDescriptor new];
        cd.captureObject = (__bridge id<MTLDevice>)de__mtl_dev(d)->device;
        cd.destination = MTLCaptureDestinationGPUTraceDocument;
        cd.outputURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
        NSError *err = nil;
        if (![mgr startCaptureWithDescriptor:cd error:&err])
            return DE_FAIL(DE_ERR_UNSUPPORTED, "metal: startCaptureWithDescriptor (%s): %s", path,
                           err ? err.localizedDescription.UTF8String : "no reason given");
    }
    return DE_OK;
}

void de__mtl_capture_end(de_gpu_device *d) {
    if (!is_metal(d))
        return;
    @autoreleasepool {
        MTLCaptureManager *mgr = [MTLCaptureManager sharedCaptureManager];
        if (mgr.isCapturing)
            [mgr stopCapture];
    }
}

bool de__mtl_object_label(de_gpu_device *d, de__gpu_kind kind, u32 handle, char *out, size_t cap) {
    if (out && cap)
        out[0] = '\0';
    if (!is_metal(d) || !out || cap == 0)
        return false;
    @autoreleasepool {
        NSString *label = nil;
        switch (kind) {
        case DE__GPU_BUFFER: {
            de__gpu_buffer *b = de__gpu_get_buffer(d, (de_gpu_buffer){handle});
            if (b)
                label = ((__bridge id<MTLBuffer>)DE__GPU_PAYLOAD(mtl_buffer, b)->buffer).label;
            break;
        }
        case DE__GPU_TEXTURE: {
            de__gpu_texture *t = de__gpu_get_texture(d, (de_gpu_texture){handle});
            if (t)
                label = ((__bridge id<MTLTexture>)DE__GPU_PAYLOAD(mtl_texture, t)->texture).label;
            break;
        }
        case DE__GPU_FENCE: {
            de__gpu_fence *f = de__gpu_get_fence(d, (de_gpu_fence){handle});
            if (f)
                label = ((__bridge id<MTLSharedEvent>)DE__GPU_PAYLOAD(mtl_fence, f)->event).label;
            break;
        }
        case DE__GPU_QUERY_HEAP: {
            de__gpu_query_heap *q = de__gpu_get_query_heap(d, (de_gpu_query_heap){handle});
            if (q)
                label = ((__bridge id<MTLCounterSampleBuffer>)DE__GPU_PAYLOAD(mtl_query_heap, q)
                             ->samples)
                            .label;
            break;
        }
        default:
            break;
        }
        if (label == nil)
            return false;
        strncpy(out, label.UTF8String, cap - 1);
        out[cap - 1] = '\0';
        return true;
    }
}
