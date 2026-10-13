/* mtl_cmd_helpers.m — Objective-C side of the Metal command, sync and swapchain tests (DENG-288).
 * Built with ARC. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "gpu/metal/mtl_internal.h"
#include "mtl_cmd_helpers.h"

#include <stdio.h>
#include <string.h>

void de_test_mtl_fill(void *blit_encoder, void *buffer, u64 offset, u64 size, u8 value) {
    [(__bridge id<MTLBlitCommandEncoder>)blit_encoder
        fillBuffer:(__bridge id<MTLBuffer>)buffer
             range:NSMakeRange((NSUInteger)offset, (NSUInteger)size)
             value:value];
}

static int fail(char *err, size_t cap, const char *what, NSError *e) {
    snprintf(err, cap, "%s%s%s", what, e ? ": " : "", e ? e.localizedDescription.UTF8String : "");
    return 0;
}

/* The DeBindlessHeap layout of bindless.slang as Slang emits it for Metal (DENG-287), buffer
 * index 0; push constants at buffer index 1. */
static NSString *const k_copy_kernel =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct Heap {\n"
     "    array<texture2d<float, access::sample>, 65536> textures;\n"
     "    array<texture2d<float, access::read_write>, 16384> rw;\n"
     "    array<sampler, 2048> samplers;\n"
     "    array<ulong, 65536> buffers;\n"
     "};\n"
     "struct Push { uint in_buf; uint out_buf; uint count; };\n"
     "kernel void copy4(Heap constant* heap [[buffer(0)]],\n"
     "                  Push constant* pc [[buffer(1)]]) {\n"
     "    device const float4 *in = (device const float4 *)heap->buffers[pc->in_buf];\n"
     "    device float4 *out = (device float4 *)heap->buffers[pc->out_buf];\n"
     "    for (uint i = 0; i < pc->count; ++i) out[i] = in[i] * 2.0;\n"
     "}\n";

int de_test_mtl_dispatch_copy(de_gpu_device *d, void *compute_encoder, u32 in_index,
                              u32 out_index, u32 count, char *err, size_t cap) {
    @autoreleasepool {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_device_object(d);
        MTLCompileOptions *opts = [MTLCompileOptions new];
        opts.languageVersion = MTLLanguageVersion3_0;
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:k_copy_kernel options:opts error:&e];
        if (lib == nil)
            return fail(err, cap, "newLibraryWithSource", e);
        id<MTLFunction> fn = [lib newFunctionWithName:@"copy4"];
        id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&e];
        if (pso == nil)
            return fail(err, cap, "newComputePipelineStateWithFunction", e);
        /* Kept for the life of the process: the backend's command buffers do not retain what
         * they use (commandBufferWithUnretainedReferences), and a pipeline released before the
         * command buffer completes crashed the driver in the next memoryBarrierWithScope:. */
        static NSMutableArray *keep;
        if (keep == nil)
            keep = [NSMutableArray new];
        [keep addObject:pso];
        id<MTLComputeCommandEncoder> enc = (__bridge id<MTLComputeCommandEncoder>)compute_encoder;
        const u32 push[3] = {in_index, out_index, count};
        [enc setComputePipelineState:pso];
        [enc setBytes:push length:sizeof push atIndex:1];
        [enc dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        return 1;
    }
}

bool de_test_mtl_has_residency_sets(void) {
    if (@available(macOS 15.0, *))
        return true;
    return false;
}

int de_test_mtl_sample_timestamps(de_gpu_device *d, de_test_mtl_timestamps *out, char *err,
                                  size_t cap) {
    @autoreleasepool {
        memset(out, 0, sizeof *out);
        id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_device_object(d);
        id<MTLCounterSet> set = nil;
        for (id<MTLCounterSet> s in dev.counterSets)
            if ([s.name isEqualToString:MTLCommonCounterSetTimestamp])
                set = s;
        if (set == nil || ![dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
            return -1;
        MTLCounterSampleBufferDescriptor *sd = [MTLCounterSampleBufferDescriptor new];
        sd.counterSet = set;
        sd.storageMode = MTLStorageModeShared;
        sd.sampleCount = 2;
        NSError *e = nil;
        id<MTLCounterSampleBuffer> samples = [dev newCounterSampleBufferWithDescriptor:sd error:&e];
        if (samples == nil)
            return fail(err, cap, "newCounterSampleBufferWithDescriptor", e);
        /* Enough work to take a few milliseconds: fill 256 MiB eight times. */
        id<MTLBuffer> big = [dev newBufferWithLength:(NSUInteger)256 << 20
                                             options:MTLResourceStorageModePrivate];
        if (big == nil)
            return fail(err, cap, "256 MiB buffer", nil);
        MTLBlitPassDescriptor *bp = [MTLBlitPassDescriptor blitPassDescriptor];
        bp.sampleBufferAttachments[0].sampleBuffer = samples;
        bp.sampleBufferAttachments[0].startOfEncoderSampleIndex = 0;
        bp.sampleBufferAttachments[0].endOfEncoderSampleIndex = 1;
        id<MTLCommandQueue> q =
            (__bridge id<MTLCommandQueue>)de__mtl_queue_object(d, DE_QUEUE_GRAPHICS);
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoderWithDescriptor:bp];
        for (u32 i = 0; i < 8; ++i)
            [blit fillBuffer:big range:NSMakeRange(0, big.length) value:(uint8_t)i];
        [blit endEncoding];
        MTLTimestamp cpu = 0, gpu = 0;
        [dev sampleTimestamps:&cpu gpuTimestamp:&gpu];
        out->gpu_before = gpu;
        [cb commit];
        [cb waitUntilCompleted];
        [dev sampleTimestamps:&cpu gpuTimestamp:&gpu];
        out->gpu_after = gpu;
        if (cb.status != MTLCommandBufferStatusCompleted)
            return fail(err, cap, "command buffer", cb.error);
        out->gpu_seconds = cb.GPUEndTime - cb.GPUStartTime;
        NSData *data = [samples resolveCounterRange:NSMakeRange(0, 2)];
        if (data == nil || data.length < 2 * sizeof(MTLCounterResultTimestamp))
            return fail(err, cap, "resolveCounterRange", nil);
        const MTLCounterResultTimestamp *ts = (const MTLCounterResultTimestamp *)data.bytes;
        out->start = ts[0].timestamp;
        out->end = ts[1].timestamp;
        return 1;
    }
}

void de_test_mtl_layer_info(void *layer, de_gpu_device *d, de_test_mtl_layer *out) {
    memset(out, 0, sizeof *out);
    id obj = (__bridge id)layer;
    if (![obj isKindOfClass:[CAMetalLayer class]])
        return;
    CAMetalLayer *l = (CAMetalLayer *)obj;
    out->is_metal_layer = true;
    out->display_sync = l.displaySyncEnabled;
    out->framebuffer_only = l.framebufferOnly;
    out->device_set = l.device == (__bridge id<MTLDevice>)de__mtl_device_object(d);
    out->max_drawables = (u32)l.maximumDrawableCount;
    out->bgra8_srgb = l.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
    out->drawable_w = l.drawableSize.width;
    out->drawable_h = l.drawableSize.height;
}
