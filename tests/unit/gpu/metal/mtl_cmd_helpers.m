/* mtl_cmd_helpers.m — Objective-C side of the Metal command, sync and swapchain tests (DENG-288).
 * Built with ARC. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "gpu/metal/mtl_internal.h"
#include "mtl_cmd_helpers.h"
#include "de_test.h"
#include "de_alloc_guard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST(mtl, completed_predecessor_has_no_notification_allocation) {
    dispatch_group_t previous = dispatch_group_create();
    dispatch_group_t following = dispatch_group_create();
    for (u32 i = 0; i < 16; ++i) {
        de_alloc_guard guard = {0};
        if (i >= 8)
            de_alloc_guard_begin(&guard);
        bool linked = de__mtl_chain_predecessor((__bridge void *)following,
                                                (__bridge void *)previous, 10, 10);
        intptr_t ready =
            dispatch_group_wait(following, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC));
        if (i >= 8)
            de_alloc_guard_end(&guard);
        EXPECT_FALSE(linked);
        EXPECT_EQ(ready, 0);
        if (i >= 8)
            DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    }
}

static void pending_predecessor_keeps_dependency(u64 completed_serial) {
    dispatch_group_t previous = dispatch_group_create();
    dispatch_group_t following = dispatch_group_create();
    dispatch_group_enter(previous);
    bool linked = de__mtl_chain_predecessor((__bridge void *)following, (__bridge void *)previous,
                                            10, completed_serial);
    intptr_t while_pending = dispatch_group_wait(following, DISPATCH_TIME_NOW);
    /* Always release held work before assertions, including counterfactual failure paths. */
    dispatch_group_leave(previous);
    intptr_t finished =
        dispatch_group_wait(following, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC));
    EXPECT_TRUE(linked);
    EXPECT_NE(while_pending, 0);
    EXPECT_EQ(finished, 0);
}

TEST(mtl, pending_predecessor_keeps_submission_order) {
    pending_predecessor_keeps_dependency(9);
}

TEST(mtl, visible_serial_does_not_bypass_pending_native_completion) {
    pending_predecessor_keeps_dependency(10);
}

TEST(mtl, completed_native_group_keeps_unpublished_serial_dependency) {
    dispatch_group_t previous = dispatch_group_create();
    dispatch_group_t following = dispatch_group_create();
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    bool linked =
        de__mtl_chain_predecessor((__bridge void *)following, (__bridge void *)previous, 10, 9);
    intptr_t finished =
        dispatch_group_wait(following, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC));
    de_alloc_guard_end(&guard);
    EXPECT_TRUE(linked);
    EXPECT_EQ(finished, 0);
    /* A positive control: the existing notification really remains on this path. */
    if (de_alloc_guard_counts_raw())
        EXPECT_GT(guard.raw_mallocs, 0u);
}

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

int de_test_mtl_dispatch_copy(de_gpu_device *d, void *compute_encoder, u32 in_index, u32 out_index,
                              u32 count, char *err, size_t cap) {
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

bool de_test_mtl_has_residency_sets(de_gpu_device *d) {
    if (@available(macOS 15.0, *)) {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_device_object(d);
        return [dev supportsFamily:MTLGPUFamilyApple6];
    }
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

/* DENG-741: protocol recording double, not a GPU or advertised device capability. Exercise the
 * production initializer with no supported hardware family. Calling newResidencySet on that
 * device is forbidden even when the runtime exposes the selector. Real default/forced-path
 * GPU tests above independently cover actual allocation and bindless reads/writes. */
@interface DEResidencyProbe : NSObject
@property BOOL apple6;
@property BOOL metal3;
@property NSUInteger creates;
@property NSUInteger familyQueries;
@end
@implementation DEResidencyProbe
- (BOOL)supportsFamily:(MTLGPUFamily)family {
    self.familyQueries += 1;
    return (family == MTLGPUFamilyApple6 && self.apple6) ||
           (family == MTLGPUFamilyMetal3 && self.metal3);
}
- (id)newResidencySetWithDescriptor:(id)desc error:(NSError **)error {
    (void)desc;
    (void)error;
    self.creates += 1;
    /* Allocation failure must still preserve useHeaps, rather than losing residency. */
    return nil;
}
@end

static void residency_probe(bool apple6, bool metal3) {
    @autoreleasepool {
        ASSERT_EQ(de_log_init(&(de_log_desc){.no_console_sink = true}), DE_OK);
        const size_t bytes = DE__GPU_ALIGN16(sizeof(de_gpu_device)) + sizeof(mtl_device);
        de_gpu_device *d = (de_gpu_device *)calloc(1, bytes);
        ASSERT_TRUE(d != NULL);
        mtl_device *m = de__mtl_dev(d);
        DEResidencyProbe *probe = [DEResidencyProbe new];
        probe.apple6 = apple6;
        probe.metal3 = metal3;
        m->device = (__bridge void *)probe;
        de_mutex_init(&m->heap_lock);
        EXPECT_EQ(de__mtl_residency_init(d), DE_OK);
        bool available = false;
        if (@available(macOS 15.0, *))
            available = de__mtl_residency_sets_built();
        EXPECT_EQ(probe.familyQueries, available ? 1u : 0u);
        EXPECT_EQ(probe.creates, available && apple6 ? 1u : 0u);
        EXPECT_TRUE(m->use_heaps);
        EXPECT_TRUE(m->residency_set == NULL);
        de__mtl_residency_shutdown(d);
        de_mutex_destroy(&m->heap_lock);
        free(d);
        de_log_shutdown();
    }
}

TEST(mtl, residency_unsupported_device_never_creates_set) {
    residency_probe(false, false);
}

TEST(mtl, residency_metal3_without_apple6_never_creates_set) {
    residency_probe(false, true);
}

TEST(mtl, residency_apple6_attempts_set_and_preserves_allocation_failure_fallback) {
    residency_probe(true, false);
}
