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
        de__mtl_residency_encoder(d, (__bridge void *)enc, DE__MTL_ENC_COMPUTE);
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
