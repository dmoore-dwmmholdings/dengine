/* mtl_cmd_helpers.m — Objective-C side of the Metal command, sync and swapchain tests (DENG-288).
 * Built with ARC. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "gpu/metal/mtl_internal.h"
#include "mtl_cmd_helpers.h"
#include "de_test.h"
#include "de_alloc_guard.h"
#include "mtl_test_helpers.h"

#include <stdio.h>
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

/* Observe real fence publication while forwarding every call to the native encoder.
 * Install only after recording: the backend still ends and submits actual GPU work. */
typedef struct finish_fence_probe {
    u32 queue_updates;
    bool ended, published_before_end;
} finish_fence_probe;

@interface DEMtlFinishFenceProbe : NSObject
@property(nonatomic, strong) id<MTLComputeCommandEncoder> native;
@property(nonatomic, strong) id<MTLFence> queue_fence;
@property(nonatomic, assign) finish_fence_probe *probe;
@end

@implementation DEMtlFinishFenceProbe
- (void)updateFence:(id<MTLFence>)fence {
    if (fence == self.queue_fence)
        ++self.probe->queue_updates;
    [self.native updateFence:fence];
}
- (void)endEncoding {
    self.probe->ended = true;
    self.probe->published_before_end = self.probe->queue_updates != 0;
    [self.native endEncoding];
}
- (id)forwardingTargetForSelector:(SEL)selector {
    (void)selector;
    return self.native;
}
@end

static void single_compute_fence_chain(mtl_fx *f, de_gpu_queue queue, bool mixed_first,
                                       bool submission_wait) {
    mtl_require_base_tier(f);
    enum { FLOAT4S = 4096, BYTES = FLOAT4S * 16 };
    de_gpu_buffer buffers[3] = {0};
    for (u32 i = 0; i < 3; ++i) {
        buffers[i] = de_gpu_buffer_create(
            f->d, &(de_gpu_buffer_desc){.size = BYTES,
                                        .usage = i == 0 ? DE_USAGE_STORAGE
                                                        : DE_USAGE_STORAGE | DE_USAGE_COPY_SRC |
                                                              DE_USAGE_COPY_DST,
                                        .memory = i == 0 ? DE_MEM_UPLOAD : DE_MEM_GPU_ONLY});
        ASSERT_TRUE(buffers[i].id);
    }
    de_gpu_buffer readback = de_gpu_buffer_create(
        f->d, &(de_gpu_buffer_desc){
                  .size = BYTES, .usage = DE_USAGE_COPY_DST, .memory = DE_MEM_READBACK});
    ASSERT_TRUE(readback.id);
    f32 *input = de_gpu_buffer_map(f->d, buffers[0]);
    f32 *output = de_gpu_buffer_map(f->d, readback);
    ASSERT_TRUE(input && output);
    for (u32 i = 0; i < FLOAT4S * 4; ++i)
        input[i] = (f32)(i % 37) * .25f;
    memset(output, 0xa5, BYTES);
    de_gpu_fence fence = de_gpu_fence_create(f->d, 0);
    ASSERT_TRUE(fence.id);
    de_gpu_fence gate = {0};
    if (submission_wait) {
        gate = de_gpu_fence_create(f->d, 0);
        ASSERT_TRUE(gate.id);
    }
    finish_fence_probe probes[2] = {0};
    de_gpu_cmd *commands[3] = {0};
    for (u32 i = 0; i < 2; ++i) {
        commands[i] = de_gpu_cmd_begin(f->d, queue, "Single compute fence chain");
        ASSERT_TRUE(commands[i]);
        if (mixed_first && i == 0) {
            void *blit = de__mtl_cmd_encoder(commands[i], DE__MTL_ENC_BLIT);
            ASSERT_TRUE(blit);
            de_test_mtl_fill(blit, de__mtl_buffer_object(f->d, buffers[1]), 0, BYTES, 0xa5);
            de_gpu_barrier before = {.buffer = buffers[1],
                                     .before = DE_ACCESS_COPY_DST,
                                     .after = DE_ACCESS_SHADER_WRITE};
            de_gpu_cmd_barriers(commands[i], &before, 1);
        }
        void *compute = de__mtl_cmd_encoder(commands[i], DE__MTL_ENC_COMPUTE);
        ASSERT_TRUE(compute);
        char why[256] = {0};
        int copied = de_test_mtl_dispatch_copy(
            f->d, compute, de_gpu_buffer_bindless(f->d, buffers[i]),
            de_gpu_buffer_bindless(f->d, buffers[i + 1]), FLOAT4S, why, sizeof why);
        if (!copied)
            fprintf(stderr, "%s\n", why);
        ASSERT_EQ(copied, 1);
        de_gpu_barrier after = {.buffer = buffers[i + 1],
                                .before = DE_ACCESS_SHADER_WRITE,
                                .after = i == 0 ? DE_ACCESS_SHADER_READ : DE_ACCESS_COPY_SRC};
        de_gpu_cmd_barriers(commands[i], &after, 1);
        mtl_cmd *mc = de__gpu_cmd_payload(commands[i]);
        DEMtlFinishFenceProbe *observer = [DEMtlFinishFenceProbe new];
        observer.native = (id<MTLComputeCommandEncoder>)CFBridgingRelease(mc->encoder);
        observer.queue_fence = (__bridge id<MTLFence>)de__mtl_dev(f->d)->queue_fence[queue];
        observer.probe = &probes[i];
        mc->encoder = (void *)CFBridgingRetain(observer);
    }
    commands[2] = de_gpu_cmd_begin(f->d, queue, "Fence chain readback");
    ASSERT_TRUE(commands[2]);
    de_gpu_cmd_copy_buffer(commands[2], readback, 0, buffers[2], 0, BYTES);
    /* One submission without CPU waits: compute -> compute -> readback.
     * Each command buffer reads its predecessor's untracked output. */
    de_gpu_wait wait = {.fence = gate, .value = 1};
    ASSERT_EQ(de_gpu_queue_submit(f->d, queue,
                                  &(de_gpu_submit){.cmds = commands,
                                                   .cmd_count = 3,
                                                   .waits = submission_wait ? &wait : NULL,
                                                   .wait_count = submission_wait ? 1u : 0u,
                                                   .signal = fence,
                                                   .signal_value = 1}),
              DE_OK);
    if (submission_wait) {
        EXPECT_EQ(de_gpu_fence_value(f->d, fence), 0u);
        ASSERT_EQ(de_gpu_queue_submit(f->d, DE_QUEUE_COPY,
                                      &(de_gpu_submit){.signal = gate, .signal_value = 1}),
                  DE_OK);
    }
    ASSERT_EQ(de_gpu_fence_wait(f->d, fence, 1, 60000000000ull), DE_OK);
    de_gpu_wait_idle(f->d);
    for (u32 i = 0; i < FLOAT4S * 4; ++i)
        EXPECT_FLOAT_EQ(output[i], input[i] * 4, .000001);
    for (u32 i = 0; i < 2; ++i) {
        const bool direct = !(mixed_first && i == 0);
        EXPECT_TRUE(probes[i].ended);
        EXPECT_EQ(probes[i].queue_updates, direct ? 1u : 0u);
        EXPECT_EQ(probes[i].published_before_end, direct);
    }
    de_gpu_fence_destroy(f->d, fence);
    de_gpu_fence_destroy(f->d, gate);
    de_gpu_buffer_destroy(f->d, readback);
    for (u32 i = 0; i < 3; ++i)
        de_gpu_buffer_destroy(f->d, buffers[i]);
}

TEST_F(mtl, single_compute_lists_publish_graphics_queue_fence, mtl_fx) {
    single_compute_fence_chain(f, DE_QUEUE_GRAPHICS, false, false);
}
TEST_F(mtl, single_compute_lists_publish_compute_queue_fence, mtl_fx) {
    single_compute_fence_chain(f, DE_QUEUE_COMPUTE, false, false);
}
TEST_F(mtl, mixed_encoders_keep_queue_fence_join, mtl_fx) {
    single_compute_fence_chain(f, DE_QUEUE_GRAPHICS, true, false);
}
TEST_F(mtl, single_compute_submission_wait_keeps_queue_fence_chain, mtl_fx) {
    single_compute_fence_chain(f, DE_QUEUE_GRAPHICS, false, true);
}
