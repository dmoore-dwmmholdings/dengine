/* mtl_test_helpers.m — Objective-C side of the Metal backend tests (DENG-287). Built with ARC.
 *
 * The tests are C (they include mtl_internal.h to prove it is plain C); what needs Metal itself
 * is here: what the device reports independently of the backend, the IDs and addresses of
 * resources, the storage mode of a resource, and a compute kernel that reads the bindless table
 * on the GPU with the layout Slang emits for shaders/common/bindless.slang (DeBindlessHeap).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "gpu/metal/mtl_internal.h"
#include "mtl_test_helpers.h"

#include <stdio.h>
#include <string.h>

int de_test_mtl_device_info(u32 adapter, de_test_mtl_info *out) {
    @autoreleasepool {
        memset(out, 0, sizeof *out);
        id<MTLDevice> dev = nil;
        if (adapter == 0) {
            dev = MTLCreateSystemDefaultDevice();
        } else {
            NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
            if (adapter <= all.count)
                dev = all[adapter - 1];
        }
        if (dev == nil)
            return 0;
        snprintf(out->name, sizeof out->name, "%s", dev.name.UTF8String);
        out->device_count = (u32)MTLCopyAllDevices().count;
        out->metal3 = [dev supportsFamily:MTLGPUFamilyMetal3];
        out->apple7 = [dev supportsFamily:MTLGPUFamilyApple7];
        out->apple9 = [dev supportsFamily:MTLGPUFamilyApple9];
        out->ab_tier2 = dev.argumentBuffersSupport == MTLArgumentBuffersTier2;
        out->raytracing = dev.supportsRaytracing;
        out->unified = dev.hasUnifiedMemory;
        out->working_set = dev.recommendedMaxWorkingSetSize;
        out->rw_tier2 = dev.readWriteTextureSupport == MTLReadWriteTextureTier2;
        out->max_samplers = dev.maxArgumentBufferSamplerCount;
        out->msaa4 = [dev supportsTextureSampleCount:4];
        out->bc = dev.supportsBCTextureCompression;
        return 1;
    }
}

/* Per-invocation forwarding objects avoid swizzling Metal classes or global test state. */
@interface DENSMtlIdleState : NSObject {
  @public
    de_atomic_u32 waits, expired;
}
@property(nonatomic, strong) id<MTLSharedEvent> gate;
@property(nonatomic, strong) dispatch_semaphore_t entered, resume;
@property(nonatomic) bool pause_next;
@end
@implementation DENSMtlIdleState
@end

@interface DENSMtlIdleBuffer : NSObject
@property(nonatomic, strong) id<MTLCommandBuffer> target;
@property(nonatomic, strong) DENSMtlIdleState *state;
@property(nonatomic, strong) id<MTLSharedEvent> gate;
@property(nonatomic) bool pause;
@end
@implementation DENSMtlIdleBuffer
- (id)forwardingTargetForSelector:(SEL)selector {
    (void)selector;
    return self.target;
}
- (void)waitUntilCompleted {
    de_atomic_fetch_add_u32(&self.state->waits, 1, DE_ATOMIC_RELAXED);
    if (self.pause) {
        dispatch_semaphore_signal(self.state.entered);
        if (dispatch_semaphore_wait(self.state.resume,
                                    dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)))
            de_atomic_store_u32(&self.state->expired, 1, DE_ATOMIC_RELAXED);
    }
    self.gate.signaledValue = 1;
    [self.target waitUntilCompleted];
}
@end

@interface DENSMtlIdleQueue : NSObject
@property(nonatomic, strong) id<MTLCommandQueue> target;
@property(nonatomic, strong) DENSMtlIdleState *state;
@property(nonatomic) u32 created;
@property(nonatomic, strong) id<MTLCommandBuffer> last_target;
@end
@implementation DENSMtlIdleQueue
- (id)forwardingTargetForSelector:(SEL)selector {
    (void)selector;
    return self.target;
}
- (id<MTLCommandBuffer>)wrap:(id<MTLCommandBuffer>)target {
    if (!target)
        return nil;
    ++self.created;
    DENSMtlIdleBuffer *buffer = [DENSMtlIdleBuffer new];
    buffer.target = target;
    buffer.state = self.state;
    buffer.gate = self.state.gate;
    buffer.pause = self.state.pause_next;
    self.last_target = target;
    return (id<MTLCommandBuffer>)buffer;
}
- (id<MTLCommandBuffer>)commandBuffer {
    return [self wrap:[self.target commandBuffer]];
}
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences {
    return [self wrap:[self.target commandBufferWithUnretainedReferences]];
}
- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)descriptor {
    return [self wrap:[self.target commandBufferWithDescriptor:descriptor]];
}
@end

static i32 idle_wait_worker(void *user) {
    de_gpu_wait_idle(user);
    return 0;
}

int de_test_mtl_idle_wait(de_gpu_device *d, u32 lists, bool native_only, bool concurrent_submit,
                          de_test_mtl_idle_probe *out, char *err, size_t cap) {
    memset(out, 0, sizeof *out);
    if (lists > 2)
        return 0;
    @autoreleasepool {
        de_gpu_fence gate = de_gpu_fence_create(d, 0), done = de_gpu_fence_create(d, 0);
        de_gpu_fence second_gate = {0}, second_done = {0};
        if (concurrent_submit) {
            second_gate = de_gpu_fence_create(d, 0);
            second_done = de_gpu_fence_create(d, 0);
        }
        if (!gate.id || !done.id || (concurrent_submit && (!second_gate.id || !second_done.id))) {
            de_gpu_fence_destroy(d, gate);
            de_gpu_fence_destroy(d, done);
            de_gpu_fence_destroy(d, second_gate);
            de_gpu_fence_destroy(d, second_done);
            snprintf(err, cap, "idle probe fences");
            return 0;
        }
        mtl_device *m = de__mtl_dev(d);
        DENSMtlIdleState *state = [DENSMtlIdleState new];
        state.gate =
            (__bridge id<MTLSharedEvent>)DE__GPU_PAYLOAD(mtl_fence, de__gpu_get_fence(d, gate))
                ->event;
        id<MTLSharedEvent> first = state.gate, second = nil;
        if (concurrent_submit) {
            second = (__bridge id<MTLSharedEvent>)DE__GPU_PAYLOAD(mtl_fence,
                                                                  de__gpu_get_fence(d, second_gate))
                         ->event;
            state.entered = dispatch_semaphore_create(0);
            state.resume = dispatch_semaphore_create(0);
            state.pause_next = true;
        }
        DENSMtlIdleQueue *queue = [DENSMtlIdleQueue new];
        queue.state = state;
        de_mutex_lock(&d->submit_lock[DE_QUEUE_GRAPHICS]);
        void *original = m->queues[DE_QUEUE_GRAPHICS];
        queue.target = (__bridge id<MTLCommandQueue>)original;
        m->queues[DE_QUEUE_GRAPHICS] = (void *)CFBridgingRetain(queue);
        de_mutex_unlock(&d->submit_lock[DE_QUEUE_GRAPHICS]);
        int ok = 1;
        de_thread *worker = NULL;
        id<MTLCommandBuffer> drain = nil;
        @try {
            de_gpu_cmd *commands[2] = {0};
            for (u32 i = 0; i < lists; ++i) {
                commands[i] = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "idle probe");
                if (!commands[i])
                    ok = 0;
            }
            de_gpu_wait wait = {gate, 1};
            de_gpu_submit submit = {.cmds = commands,
                                    .cmd_count = lists,
                                    .waits = &wait,
                                    .wait_count = 1,
                                    .signal = done,
                                    .signal_value = 1};
            if (native_only) {
                /* The present path commits native work without advancing submitted[]. */
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                drain = ((DENSMtlIdleBuffer *)cb).target;
                id<MTLSharedEvent> finished = (__bridge id<MTLSharedEvent>)DE__GPU_PAYLOAD(
                                                  mtl_fence, de__gpu_get_fence(d, done))
                                                  ->event;
                [cb encodeWaitForEvent:state.gate value:1];
                [cb encodeSignalEvent:finished value:1];
                de_mutex_lock(&d->submit_lock[DE_QUEUE_GRAPHICS]);
                [cb commit];
                de__mtl_remember_committed(d, DE_QUEUE_GRAPHICS, (__bridge void *)cb);
                de_mutex_unlock(&d->submit_lock[DE_QUEUE_GRAPHICS]);
            } else if (!ok || de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &submit) != DE_OK) {
                snprintf(err, cap, "idle probe submission");
                ok = 0;
            }
            if (ok) {
                queue.created = 0;
                bool retained_newer = true;
                if (concurrent_submit) {
                    if (de_thread_create(idle_wait_worker, d, "idle probe", DE_THREAD_NORMAL, 0,
                                         &worker) != DE_OK ||
                        dispatch_semaphore_wait(
                            state.entered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) {
                        snprintf(err, cap, "idle probe worker did not enter native wait");
                        ok = 0;
                    } else {
                        /* First wait is paused with its retained snapshot. Queue newer work
                         * behind a different GPU event while that wait holds no submit lock. */
                        state.gate = second;
                        state.pause_next = false;
                        de_gpu_wait later_wait = {second_gate, 1};
                        de_gpu_submit later = {.waits = &later_wait,
                                               .wait_count = 1,
                                               .signal = second_done,
                                               .signal_value = 1};
                        if (de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &later) != DE_OK)
                            ok = 0;
                        queue.created = 0;
                    }
                    dispatch_semaphore_signal(state.resume);
                    if (worker) {
                        de_thread_join(worker);
                        worker = NULL;
                    }
                    de_mutex_lock(&d->submit_lock[DE_QUEUE_GRAPHICS]);
                    retained_newer = m->last_committed[DE_QUEUE_GRAPHICS] != NULL &&
                                     de_gpu_fence_value(d, second_done) == 0;
                    de_mutex_unlock(&d->submit_lock[DE_QUEUE_GRAPHICS]);
                    de_gpu_wait_idle(d); /* Must still find and wait for the later buffer. */
                    retained_newer &= de_gpu_fence_value(d, second_done) >= 1;
                } else
                    de_gpu_wait_idle(d);
                out->created = queue.created;
                out->waits = de_atomic_load_u32(&state->waits, DE_ATOMIC_RELAXED);
                out->completed =
                    retained_newer && !de_atomic_load_u32(&state->expired, DE_ATOMIC_RELAXED) &&
                    de_gpu_fence_value(d, done) >= 1 &&
                    d->vt->queue_completed(d, DE_QUEUE_GRAPHICS) >=
                        de_atomic_load_u64(&d->submitted[DE_QUEUE_GRAPHICS], DE_ATOMIC_ACQUIRE);
            }
        } @finally {
            first.signaledValue = 1;
            second.signaledValue = 1;
            if (concurrent_submit)
                dispatch_semaphore_signal(state.resume);
            if (worker)
                de_thread_join(worker);
            [drain waitUntilCompleted];
            [queue.last_target waitUntilCompleted];
            de_mutex_lock(&d->submit_lock[DE_QUEUE_GRAPHICS]);
            void *probe = m->queues[DE_QUEUE_GRAPHICS];
            m->queues[DE_QUEUE_GRAPHICS] = original;
            (void)CFBridgingRelease(probe);
            de_mutex_unlock(&d->submit_lock[DE_QUEUE_GRAPHICS]);
            de_gpu_wait_idle(d);
            de_gpu_fence_destroy(d, gate);
            de_gpu_fence_destroy(d, done);
            de_gpu_fence_destroy(d, second_gate);
            de_gpu_fence_destroy(d, second_done);
        }
        return ok;
    }
}

u64 de_test_mtl_texture_id(void *texture) {
    MTLResourceID rid = ((__bridge id<MTLTexture>)texture).gpuResourceID;
    u64 v;
    memcpy(&v, &rid, sizeof v);
    return v;
}

u64 de_test_mtl_sampler_id(void *sampler) {
    MTLResourceID rid = ((__bridge id<MTLSamplerState>)sampler).gpuResourceID;
    u64 v;
    memcpy(&v, &rid, sizeof v);
    return v;
}

u64 de_test_mtl_buffer_address(void *buffer) {
    return ((__bridge id<MTLBuffer>)buffer).gpuAddress;
}

void de_test_mtl_resource_modes(void *resource, u32 *storage, u32 *hazard, u32 *cpu_cache) {
    id<MTLResource> r = (__bridge id<MTLResource>)resource;
    *storage = (u32)r.storageMode;
    *hazard = (u32)r.hazardTrackingMode;
    *cpu_cache = (u32)r.cpuCacheMode;
}

u32 de_test_mtl_storage_private(void) {
    return (u32)MTLStorageModePrivate;
}
u32 de_test_mtl_storage_shared(void) {
    return (u32)MTLStorageModeShared;
}
u32 de_test_mtl_hazard_untracked(void) {
    return (u32)MTLHazardTrackingModeUntracked;
}
u32 de_test_mtl_cpu_write_combined(void) {
    return (u32)MTLCPUCacheModeWriteCombined;
}

u32 de_test_mtl_texture_type(void *texture, u32 *mip_levels, u32 *array_length) {
    id<MTLTexture> t = (__bridge id<MTLTexture>)texture;
    *mip_levels = (u32)t.mipmapLevelCount;
    *array_length = (u32)t.arrayLength;
    return (u32)t.textureType;
}

u32 de_test_mtl_type_2d(void) {
    return (u32)MTLTextureType2D;
}
u32 de_test_mtl_type_2d_array(void) {
    return (u32)MTLTextureType2DArray;
}

/* ---------- The GPU check ---------- */

/* The DeBindlessHeap struct as Slang 2026.19 emits it for Metal (plain arrays, constant address
 * space, buffer index 0; push constants at buffer index 1), reduced to what the check reads. */
static NSString *const k_kernel =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct Heap {\n"
     "    array<texture2d<float, access::sample>, 65536> textures;\n"
     "    array<texture2d<float, access::read_write>, 16384> rw;\n"
     "    array<sampler, 2048> samplers;\n"
     "    array<ulong, 65536> buffers;\n"
     "};\n"
     "struct Push { uint tex; uint rw; uint smp; uint in_buf; uint out_buf; uint view; };\n"
     "kernel void check(Heap constant* heap [[buffer(0)]],\n"
     "                  Push constant* pc [[buffer(1)]]) {\n"
     "    float4 a = heap->textures[pc->tex].read(uint2(0, 0));\n"
     "    float4 b = heap->rw[pc->rw].read(uint2(0, 0));\n"
     "    float4 c = heap->textures[pc->tex].sample(\n"
     "        heap->samplers[pc->smp], float2(0.5, 0.5), level(0.0));\n"
     "    float4 d = ((device float4 *)heap->buffers[pc->in_buf])[0];\n"
     "    float4 n = heap->textures[0].read(uint2(0, 0));\n"
     "    float4 nb = ((device float4 *)heap->buffers[0])[0];\n"
     "    device float4 *out = (device float4 *)heap->buffers[pc->out_buf];\n"
     "    out[0] = a; out[1] = b; out[2] = c;\n"
     "    out[3] = d; out[4] = n; out[5] = nb;\n"
     "    heap->rw[pc->rw].write(float4(0.25, 0.5, 0.75, 1.0), uint2(0, 0));\n"
     "    heap->rw[pc->view].write(float4(1.0, 0.75, 0.5, 0.25), uint2(0, 0));\n"
     "}\n";

static int fail(char *err, size_t cap, const char *what, NSError *e) {
    snprintf(err, cap, "%s%s%s", what, e ? ": " : "", e ? e.localizedDescription.UTF8String : "");
    return 0;
}

static int run_and_wait(id<MTLCommandBuffer> cb, char *err, size_t cap) {
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted)
        return fail(err, cap, "command buffer failed", cb.error);
    return 1;
}

int de_test_mtl_upload_rgba8(de_gpu_device *d, de_gpu_texture t, const u8 rgba[4], char *err,
                             size_t cap) {
    @autoreleasepool {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_device_object(d);
        id<MTLTexture> tex = (__bridge id<MTLTexture>)de__mtl_texture_object(d, t, -1);
        id<MTLCommandQueue> q =
            (__bridge id<MTLCommandQueue>)de__mtl_queue_object(d, DE_QUEUE_COPY);
        id<MTLBuffer> staging = [dev newBufferWithBytes:rgba
                                                 length:4
                                                options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromBuffer:staging
                   sourceOffset:0
              sourceBytesPerRow:4
            sourceBytesPerImage:4
                     sourceSize:MTLSizeMake(1, 1, 1)
                      toTexture:tex
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        return run_and_wait(cb, err, cap);
    }
}

/* Copies texel (0, 0) of `level` of an RGBA8 texture into `dst` at `offset`. */
static void copy_texel(id<MTLBlitCommandEncoder> blit, id<MTLTexture> tex, u32 level,
                       id<MTLBuffer> dst, u32 offset) {
    [blit copyFromTexture:tex
                     sourceSlice:0
                     sourceLevel:level
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(1, 1, 1)
                        toBuffer:dst
               destinationOffset:offset
          destinationBytesPerRow:4
        destinationBytesPerImage:4];
}

int de_test_mtl_bindless_kernel(de_gpu_device *d, const u32 push[6], de_gpu_buffer out_gpu,
                                de_gpu_buffer out_readback, u64 bytes, de_gpu_texture rw,
                                de_gpu_texture rw_mips, u8 rw_after[4], u8 rw_mip1_after[4],
                                char *err, size_t cap) {
    @autoreleasepool {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_device_object(d);
        MTLCompileOptions *opts = [MTLCompileOptions new];
        opts.languageVersion = MTLLanguageVersion3_0;
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:k_kernel options:opts error:&e];
        if (lib == nil)
            return fail(err, cap, "newLibraryWithSource", e);
        id<MTLFunction> fn = [lib newFunctionWithName:@"check"];
        id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&e];
        if (pso == nil)
            return fail(err, cap, "newComputePipelineStateWithFunction", e);

        id<MTLCommandQueue> q =
            (__bridge id<MTLCommandQueue>)de__mtl_queue_object(d, DE_QUEUE_COMPUTE);
        /* Shader validation logs what it rejects (and skips the access) in the command buffer's
         * logs; the encoder status makes the log name the kernel. */
        MTLCommandBufferDescriptor *cbd = [MTLCommandBufferDescriptor new];
        cbd.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
        id<MTLCommandBuffer> cb = [q commandBufferWithDescriptor:cbd];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pso];
        [enc setBytes:push length:6 * sizeof(u32) atIndex:1];
        /* Residency and the table at buffer index 0, as the backend's own compute encoders get
         * them. This command buffer bypasses de_gpu_queue_submit, which brings the residency set
         * up to date with the heaps created since the last submit, so sync it here. */
        de__mtl_residency_sync(d);
        de__mtl_residency_encoder(d, (__bridge void *)enc, DE__MTL_ENC_COMPUTE, NULL);
        [enc dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [enc endEncoding];
        if (!run_and_wait(cb, err, cap))
            return 0;
        for (id<MTLFunctionLog> log in cb.logs) {
            snprintf(err, cap, "Metal shader validation: %s", log.description.UTF8String);
            return 0;
        }

        /* Copy the results and the written storage texels to CPU-visible memory. */
        id<MTLBuffer> texels = [dev newBufferWithLength:8 options:MTLResourceStorageModeShared];
        cb = [(__bridge id<MTLCommandQueue>)de__mtl_queue_object(d, DE_QUEUE_COPY) commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromBuffer:(__bridge id<MTLBuffer>)de__mtl_buffer_object(d, out_gpu)
                 sourceOffset:0
                     toBuffer:(__bridge id<MTLBuffer>)de__mtl_buffer_object(d, out_readback)
            destinationOffset:0
                         size:bytes];
        copy_texel(blit, (__bridge id<MTLTexture>)de__mtl_texture_object(d, rw, -1), 0, texels, 0);
        copy_texel(blit, (__bridge id<MTLTexture>)de__mtl_texture_object(d, rw_mips, -1), 1, texels,
                   4);
        [blit endEncoding];
        if (!run_and_wait(cb, err, cap))
            return 0;
        memcpy(rw_after, texels.contents, 4);
        memcpy(rw_mip1_after, (const u8 *)texels.contents + 4, 4);
        return 1;
    }
}
