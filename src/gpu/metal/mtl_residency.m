/* mtl_residency.m — making the bindless heaps resident (DENG-288, SPEC §24 R4).
 *
 * Shaders reach resources through the bindless argument buffer, so Metal cannot see which ones a
 * pass uses: every heap that the table can point into must be made resident (Decision: Metal
 * bindless resources live in heaps named by one residency list). Two paths, picked at device
 * creation:
 *   - macOS 15+: one MTLResidencySet per device holding every heap of the heap list, added to all
 *     three queues. Heaps come and go with allocations (never per frame); each submit first
 *     rebuilds the set when the list changed (heap_gen) and commits it.
 *   - macOS 14 (no residency sets), or the cvar gpu.metal.force_use_heap set at device creation:
 *     every render and compute encoder calls useHeaps: for the whole list when it opens. The list
 *     is read under its lock, so a heap cannot be released while an encoder names it.
 * Storage textures (DENG-407): shaders write the textures the storage table names, and Metal
 * wants write usage named for each (shader validation drops the write and logs "MTLResourceUsage
 * flags mismatch or missing for texture" otherwise). useHeaps: names a heap's resources for
 * reading only, and a residency set of heaps does not cover texture views. So every render and
 * compute encoder also calls useResources: with read-write usage for the storage-texture list
 * (mtl_bindless.m): on the useHeaps: path every storage texture and the null one, on the
 * residency-set path only the per-mip views (heap textures are covered by the set). The views are
 * not added to the residency set: with textures in the set, Metal's GPU validation crashed in
 * -[MTLGPUDebugCommandBuffer preCommit] while other threads released textures.
 * Either way, render and compute encoders get the argument buffer at buffer index 0 of every
 * stage. Blit encoders need neither: resources named by a command are resident by themselves.
 * Swapchain drawables are not in the table (no SAMPLED or STORAGE usage), so they need nothing.
 *
 * The residency-set path needs the macOS 15 SDK at build time (DE__MTL_RESIDENCY_SETS); a build
 * with an older SDK (CI's Xcode 15.4) always takes the useHeaps: path.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "gpu/metal/mtl_internal.h"

#include <dengine/de_debug.h>

#if defined(__MAC_15_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_15_0
#define DE__MTL_RESIDENCY_SETS 1
#else
#define DE__MTL_RESIDENCY_SETS 0
#endif

/* Heaps passed to one useHeaps: call. */
#define HEAP_CHUNK 32u

de_result de__mtl_residency_init(de_gpu_device *d) {
    mtl_device *m = de__mtl_dev(d);
    de_cvar_desc cd = {.name = "gpu.metal.force_use_heap",
                       .help = "Metal: make the bindless heaps resident with useHeaps: in every "
                               "encoder (the macOS 14 path) even where residency sets exist; read "
                               "when a device is created",
                       .type = DE_CVAR_BOOL};
    const bool force = de_cvar_bool(de_cvar_register(&cd));
    m->use_heaps = true;
#if DE__MTL_RESIDENCY_SETS
    if (!force) {
        if (@available(macOS 15.0, *)) {
            @autoreleasepool {
                id<MTLDevice> dev = (__bridge id<MTLDevice>)m->device;
                MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new];
                rd.label = @"de bindless heaps";
                rd.initialCapacity = 16;
                NSError *err = nil;
                id<MTLResidencySet> set = [dev newResidencySetWithDescriptor:rd error:&err];
                if (set != nil) {
                    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
                        [(__bridge id<MTLCommandQueue>)m->queues[q] addResidencySet:set];
                    m->residency_set = (void *)CFBridgingRetain(set);
                    m->use_heaps = false;
                } else {
                    DE_WARN(d->log, "metal: no residency set (%s); using useHeaps: per encoder",
                            err ? err.localizedDescription.UTF8String : "no reason given");
                }
            }
        }
    }
#endif
    de_mutex_lock(&m->heap_lock);
    m->residency_gen = m->heap_gen - 1u; /* the first sync builds the set */
    de_mutex_unlock(&m->heap_lock);
    de__mtl_residency_sync(d);
    DE_INFO(d->log, "metal: residency through %s%s",
            m->use_heaps ? "useHeaps: in every encoder" : "one MTLResidencySet",
            force ? " (gpu.metal.force_use_heap)" : "");
    return DE_OK;
}

void de__mtl_residency_shutdown(de_gpu_device *d) {
    mtl_device *m = de__mtl_dev(d);
    if (!m->residency_set)
        return;
#if DE__MTL_RESIDENCY_SETS
    if (@available(macOS 15.0, *)) {
        @autoreleasepool {
            id<MTLResidencySet> set = (id<MTLResidencySet>)CFBridgingRelease(m->residency_set);
            for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
                if (m->queues[q])
                    [(__bridge id<MTLCommandQueue>)m->queues[q] removeResidencySet:set];
            [set removeAllAllocations];
            [set commit];
        }
    }
#endif
    m->residency_set = NULL;
}

void de__mtl_residency_sync(de_gpu_device *d) {
    mtl_device *m = de__mtl_dev(d);
    if (!m->residency_set)
        return;
#if DE__MTL_RESIDENCY_SETS
    if (@available(macOS 15.0, *)) {
        de_mutex_lock(&m->heap_lock);
        if (m->residency_gen != m->heap_gen) {
            @autoreleasepool {
                id<MTLResidencySet> set = (__bridge id<MTLResidencySet>)m->residency_set;
                [set removeAllAllocations];
                for (u32 i = 0; i < m->heap_count; ++i)
                    [set addAllocation:(__bridge id<MTLHeap>)m->heaps[i]->heap];
                [set commit];
            }
            m->residency_gen = m->heap_gen;
        }
        de_mutex_unlock(&m->heap_lock);
    }
#endif
}

void de__mtl_residency_encoder(de_gpu_device *d, void *encoder, de__mtl_encoder_kind kind) {
    mtl_device *m = de__mtl_dev(d);
    id<MTLBuffer> table = (__bridge id<MTLBuffer>)m->bindless;
    const bool mesh = (m->render_stages & DE__MTL_STAGE_MESH) != 0;
    if (kind == DE__MTL_ENC_RENDER) {
        id<MTLRenderCommandEncoder> r = (__bridge id<MTLRenderCommandEncoder>)encoder;
        [r setVertexBuffer:table offset:0 atIndex:0];
        [r setFragmentBuffer:table offset:0 atIndex:0];
        if (mesh) {
            [r setObjectBuffer:table offset:0 atIndex:0];
            [r setMeshBuffer:table offset:0 atIndex:0];
        }
    } else if (kind == DE__MTL_ENC_COMPUTE) {
        [(__bridge id<MTLComputeCommandEncoder>)encoder setBuffer:table offset:0 atIndex:0];
    } else {
        return;
    }
    de_mutex_lock(&m->heap_lock);
    for (u32 first = 0; m->use_heaps && first < m->heap_count; first += HEAP_CHUNK) {
        __unsafe_unretained id<MTLHeap> heaps[HEAP_CHUNK];
        u32 n = 0;
        for (; n < HEAP_CHUNK && first + n < m->heap_count; ++n)
            heaps[n] = (__bridge id<MTLHeap>)m->heaps[first + n]->heap;
        if (kind == DE__MTL_ENC_RENDER)
            [(__bridge id<MTLRenderCommandEncoder>)encoder
                useHeaps:heaps
                   count:n
                  stages:(MTLRenderStages)m->render_stages];
        else
            [(__bridge id<MTLComputeCommandEncoder>)encoder useHeaps:heaps count:n];
    }
    /* DENG-407: write usage for the storage-texture list (with useHeaps:, the null storage
     * texture first). */
    const u32 extra = m->use_heaps ? 1u : 0u;
    const u32 total = m->storage_count + extra;
    const MTLResourceUsage rw = MTLResourceUsageRead | MTLResourceUsageWrite;
    for (u32 first = 0; first < total; first += HEAP_CHUNK) {
        __unsafe_unretained id<MTLResource> tex[HEAP_CHUNK];
        u32 n = 0;
        for (; n < HEAP_CHUNK && first + n < total; ++n) {
            const u32 i = first + n;
            tex[n] = i < extra ? (__bridge id<MTLTexture>)m->null_storage_texture
                               : (__bridge id<MTLTexture>)m->storage_objs[i - extra];
        }
        if (kind == DE__MTL_ENC_RENDER)
            [(__bridge id<MTLRenderCommandEncoder>)encoder
                useResources:tex
                       count:n
                       usage:rw
                      stages:(MTLRenderStages)m->render_stages];
        else
            [(__bridge id<MTLComputeCommandEncoder>)encoder useResources:tex count:n usage:rw];
    }
    de_mutex_unlock(&m->heap_lock);
}

bool de__mtl_residency_sets_built(void) {
    return DE__MTL_RESIDENCY_SETS != 0;
}

bool de__mtl_residency_uses_heaps(de_gpu_device *d) {
    return de__mtl_device_object(d) ? de__mtl_dev(d)->use_heaps : false;
}
