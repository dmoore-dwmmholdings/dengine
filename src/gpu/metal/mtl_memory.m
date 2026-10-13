/* mtl_memory.m — device memory for the Metal backend (DENG-287, DENG-282 heap contract).
 *
 * Each memory class is a list of MTLHeaps of type Placement with hazard tracking off: the
 * front-end's TLSF picks the offset, and resources are placed there with
 * newBufferWithLength:options:offset: / newTextureWithDescriptor:offset:. The class's
 * `backend_bits` are the MTLResourceOptions of its heaps (and of every resource placed in them):
 *   GPU_ONLY  private storage                         buffers and textures, 256 MiB blocks
 *   UPLOAD    shared storage, write-combined CPU cache buffers, 64 MiB blocks
 *   READBACK  shared storage, default CPU cache       buffers, 64 MiB blocks
 * Shared storage is the app's own memory on Apple silicon (unified memory), so `contents` gives
 * the CPU address of each buffer; heaps are not mapped as a whole (heap->mapped stays NULL).
 *
 * Residency (R4): macOS 14 has no residency sets, so an encoder makes heaps resident with
 * useHeap:. The backend keeps every live heap in one list (de__mtl_heap_list) for DENG-288.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "gpu/metal/mtl_internal.h"

#include <stdio.h>
#include <string.h>

static const MTLResourceOptions k_untracked = MTLResourceHazardTrackingModeUntracked;

/* Whether the device can make a shared-storage placement heap (Apple-family GPUs can; Mac2-family
 * and paravirtual devices only make private heaps). */
static bool probe_shared_heaps(mtl_device *m, MTLResourceOptions options) {
    @autoreleasepool {
        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.type = MTLHeapTypePlacement;
        hd.resourceOptions = options;
        hd.size = 64u << 10;
        return [(__bridge id<MTLDevice>)m->device newHeapWithDescriptor:hd] != nil;
    }
}

de_result de__mtl_memory_init(de_gpu_device *d) {
    mtl_device *m = de__mtl_dev(d);
    m->resource_options[DE__GPU_MEM_CLASS_GPU_ONLY] =
        (u32)(MTLResourceStorageModePrivate | k_untracked);
    m->resource_options[DE__GPU_MEM_CLASS_UPLOAD] =
        (u32)(MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined | k_untracked);
    m->resource_options[DE__GPU_MEM_CLASS_READBACK] =
        (u32)(MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache | k_untracked);

    m->shared_heaps =
        probe_shared_heaps(m, (MTLResourceOptions)m->resource_options[DE__GPU_MEM_CLASS_UPLOAD]) &&
        probe_shared_heaps(m, (MTLResourceOptions)m->resource_options[DE__GPU_MEM_CLASS_READBACK]);
    if (!m->shared_heaps)
        DE_INFO(d->log, "metal: no shared-storage heaps on this device: UPLOAD and READBACK "
                        "buffers are standalone MTLBuffers");

    for (u32 c = 0; c < DE__GPU_MEM_CLASS_BASE_COUNT; ++c) {
        de__gpu_mem_class_desc cd;
        memset(&cd, 0, sizeof cd);
        cd.memory = (de_gpu_memory)c;
        cd.backend_bits = m->resource_options[c];
        if (c != DE__GPU_MEM_CLASS_GPU_ONLY) {
            cd.block_size = DE__MTL_SHARED_BLOCK_SIZE;
            cd.dedicated_threshold = DE__MTL_SHARED_DEDICATED_SIZE;
        }
        de_result r = de__gpu_mem_configure_class(d, c, &cd);
        if (r != DE_OK)
            return r;
    }
    return DE_OK;
}

/* ---------- The heap list (residency) ---------- */

de_result de__mtl_heap_list_add(de_gpu_device *d, mtl_heap *h) {
    mtl_device *m = de__mtl_dev(d);
    de_result r = DE_OK;
    de_mutex_lock(&m->heap_lock);
    if (m->heap_count == m->heap_cap) {
        u32 cap = m->heap_cap ? m->heap_cap * 2 : 16;
        mtl_heap **n = (mtl_heap **)DE_REALLOC(d->alloc, m->heaps, sizeof(mtl_heap *) * m->heap_cap,
                                               sizeof(mtl_heap *) * cap, 16, DE_MEM_RENDER);
        if (!n) {
            r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "metal: heap list of %u entries", cap);
        } else {
            m->heaps = n;
            m->heap_cap = cap;
        }
    }
    if (r == DE_OK) {
        h->slot = m->heap_count;
        m->heaps[m->heap_count++] = h;
    }
    de_mutex_unlock(&m->heap_lock);
    return r;
}

void de__mtl_heap_list_remove(de_gpu_device *d, mtl_heap *h) {
    mtl_device *m = de__mtl_dev(d);
    de_mutex_lock(&m->heap_lock);
    DE_ASSERT(h->slot < m->heap_count && m->heaps[h->slot] == h);
    if (h->slot < m->heap_count && m->heaps[h->slot] == h) {
        mtl_heap *last = m->heaps[--m->heap_count];
        m->heaps[h->slot] = last;
        last->slot = h->slot;
    }
    de_mutex_unlock(&m->heap_lock);
}

u32 de__mtl_heap_list(de_gpu_device *d, void **out, u32 cap) {
    if (!de__mtl_device_object(d))
        return 0;
    mtl_device *m = de__mtl_dev(d);
    de_mutex_lock(&m->heap_lock);
    const u32 n = m->heap_count;
    for (u32 i = 0; i < n && i < cap; ++i)
        out[i] = m->heaps[i]->heap;
    de_mutex_unlock(&m->heap_lock);
    return n;
}

/* ---------- Heaps (vtable) ---------- */

de_result de__mtl_heap_create(de_gpu_device *d, de__gpu_heap *h,
                              const de__gpu_mem_class_desc *cls) {
    @autoreleasepool {
        mtl_device *m = de__mtl_dev(d);
        id<MTLDevice> dev = (__bridge id<MTLDevice>)m->device;
        const MTLResourceOptions storage =
            (MTLResourceOptions)cls->backend_bits & MTLResourceStorageModeMask;
        if (storage == MTLResourceStorageModeShared && !m->shared_heaps)
            return DE_FAIL(DE_ERR_UNSUPPORTED,
                           "metal: \"%s\" has no shared-storage heaps (class \"%s\")",
                           dev.name.UTF8String, cls->name ? cls->name : "?");
        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.type = MTLHeapTypePlacement;
        hd.resourceOptions = (MTLResourceOptions)cls->backend_bits;
        hd.size = (NSUInteger)h->size;
        id<MTLHeap> heap = [dev newHeapWithDescriptor:hd];
        if (heap == nil)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "metal: MTLHeap of %llu bytes for class \"%s\"",
                           (unsigned long long)h->size, cls->name ? cls->name : "?");
        heap.label = [NSString stringWithFormat:@"de %s heap %u%s", cls->name ? cls->name : "",
                                                h->index, h->dedicated ? " (dedicated)" : ""];
        mtl_heap *mh = DE__GPU_PAYLOAD(mtl_heap, h);
        mh->heap = (void *)CFBridgingRetain(heap);
        de_result r = de__mtl_heap_list_add(d, mh);
        if (r != DE_OK) {
            (void)CFBridgingRelease(mh->heap);
            mh->heap = NULL;
            return r;
        }
        return DE_OK;
    }
}

void de__mtl_heap_destroy(de_gpu_device *d, de__gpu_heap *h) {
    mtl_heap *mh = DE__GPU_PAYLOAD(mtl_heap, h);
    if (!mh->heap)
        return;
    de__mtl_heap_list_remove(d, mh);
    @autoreleasepool {
        (void)CFBridgingRelease(mh->heap);
    }
    mh->heap = NULL;
}
