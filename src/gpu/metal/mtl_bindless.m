/* mtl_bindless.m — the Metal bindless table (DENG-287; shaders/common/bindless.slang).
 *
 * One argument buffer (argument buffers tier 2) in shared memory, laid out as Slang emits
 * `DeBindlessHeap`: 65536 sampled-texture IDs, 16384 storage-texture IDs, 2048 sampler IDs
 * (MTLResourceID, 8 bytes each), then 65536 buffer GPU addresses (u64). The front-end's index
 * allocators hand out the slots; this file writes them from the CPU (`gpuResourceID`,
 * `gpuAddress`). Shaders read the buffer at index 0 of every stage (DENG-288 binds it).
 *
 * Null descriptors: slot 0 of every table (DE_GPU_BINDLESS_INVALID), every slot never written and
 * every slot whose resource was destroyed holds a null resource: a 1x1 RGBA8 texture (sampled),
 * a 1x1 RGBA8 read-write texture (storage), a nearest/wrap sampler and a 4 KiB buffer, all zero.
 * They live in the backend's own private heap, which is the first heap of the residency list.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "gpu/metal/mtl_internal.h"

#include <string.h>

_Static_assert(sizeof(MTLResourceID) == DE__MTL_BINDLESS_ENTRY_BYTES,
               "MTLResourceID is one 8-byte table entry");
_Static_assert(DE__MTL_BINDLESS_BYTES == 1196032u, "the DeBindlessHeap size of bindless.slang");

static const u32 k_table_offset[4] = {
    [DE__GPU_BINDLESS_SAMPLED_TEXTURE] = DE__MTL_BINDLESS_OFFSET_TEXTURES,
    [DE__GPU_BINDLESS_STORAGE_TEXTURE] = DE__MTL_BINDLESS_OFFSET_STORAGE_TEXTURES,
    [DE__GPU_BINDLESS_BUFFER] = DE__MTL_BINDLESS_OFFSET_BUFFERS,
    [DE__GPU_BINDLESS_SAMPLER] = DE__MTL_BINDLESS_OFFSET_SAMPLERS,
};
static const u32 k_table_size[4] = {
    [DE__GPU_BINDLESS_SAMPLED_TEXTURE] = DE__MTL_BINDLESS_TEXTURES,
    [DE__GPU_BINDLESS_STORAGE_TEXTURE] = DE__MTL_BINDLESS_STORAGE_TEXTURES,
    [DE__GPU_BINDLESS_BUFFER] = DE__MTL_BINDLESS_BUFFERS,
    [DE__GPU_BINDLESS_SAMPLER] = DE__MTL_BINDLESS_SAMPLERS,
};

/* Writes entry `index` of table `space`. Different slots never share bytes, so concurrent writers
 * (resource creation on several threads) need no lock; an aligned 8-byte store is what the GPU
 * sees whole. */
static void put(mtl_device *m, de__gpu_bindless_space space, u32 index, u64 value) {
    DE_ASSERT((u32)space < 4u && index < k_table_size[space]);
    memcpy(m->bindless_cpu + k_table_offset[space] + (size_t)index * DE__MTL_BINDLESS_ENTRY_BYTES,
           &value, sizeof value);
}

static u64 resource_id(id<MTLTexture> t) {
    MTLResourceID rid = t.gpuResourceID;
    u64 v;
    memcpy(&v, &rid, sizeof v);
    return v;
}

static u64 sampler_id(id<MTLSamplerState> s) {
    MTLResourceID rid = s.gpuResourceID;
    u64 v;
    memcpy(&v, &rid, sizeof v);
    return v;
}

/* The null resources: placed one after another in a private heap of the backend. */
static de_result create_null_resources(de_gpu_device *d, mtl_device *m, id<MTLDevice> dev) {
    const MTLResourceOptions opts =
        MTLResourceStorageModePrivate | MTLResourceHazardTrackingModeUntracked;
    MTLTextureDescriptor *sampled =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:1
                                                          height:1
                                                       mipmapped:NO];
    sampled.resourceOptions = opts;
    sampled.usage = MTLTextureUsageShaderRead;
    MTLTextureDescriptor *storage = [sampled copy];
    storage.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;

    const MTLSizeAndAlign sa[3] = {
        [dev heapTextureSizeAndAlignWithDescriptor:sampled],
        [dev heapTextureSizeAndAlignWithDescriptor:storage],
        [dev heapBufferSizeAndAlignWithLength:DE__MTL_NULL_BUFFER_BYTES options:opts],
    };
    NSUInteger offset[3], size = 0;
    for (u32 i = 0; i < 3; ++i) {
        const NSUInteger a = sa[i].align ? sa[i].align : 1;
        size = (size + a - 1) / a * a;
        offset[i] = size;
        size += sa[i].size;
    }
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.type = MTLHeapTypePlacement;
    hd.resourceOptions = opts;
    hd.size = size;
    id<MTLHeap> heap = [dev newHeapWithDescriptor:hd];
    if (heap == nil)
        return de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY, "metal: heap of the null descriptors");
    heap.label = @"de null descriptors";
    m->internal.heap = (void *)CFBridgingRetain(heap);
    de_result r = de__mtl_heap_list_add(d, &m->internal);
    if (r != DE_OK)
        return r;

    id<MTLTexture> tex = [heap newTextureWithDescriptor:sampled offset:offset[0]];
    id<MTLTexture> rw = [heap newTextureWithDescriptor:storage offset:offset[1]];
    id<MTLBuffer> buf = [heap newBufferWithLength:DE__MTL_NULL_BUFFER_BYTES
                                          options:opts
                                           offset:offset[2]];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.supportArgumentBuffers = YES;
    sd.label = @"de null sampler";
    id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
    if (tex == nil || rw == nil || buf == nil || smp == nil)
        return de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY, "metal: null descriptor resources");
    tex.label = @"de null texture";
    rw.label = @"de null storage texture";
    buf.label = @"de null buffer";

    /* Zero them: private memory starts undefined. One blit on the copy queue, waited for. */
    id<MTLBuffer> zeros = [dev newBufferWithLength:DE__MTL_NULL_BUFFER_BYTES
                                           options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> q = (__bridge id<MTLCommandQueue>)m->queues[DE_QUEUE_COPY];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    if (zeros == nil || cb == nil)
        return de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY, "metal: clearing the null descriptors");
    memset(zeros.contents, 0, DE__MTL_NULL_BUFFER_BYTES);
    cb.label = @"de clear null descriptors";
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit fillBuffer:buf range:NSMakeRange(0, DE__MTL_NULL_BUFFER_BYTES) value:0];
    for (u32 i = 0; i < 2; ++i)
        [blit copyFromBuffer:zeros
                   sourceOffset:0
              sourceBytesPerRow:4
            sourceBytesPerImage:4
                     sourceSize:MTLSizeMake(1, 1, 1)
                      toTexture:i == 0 ? tex : rw
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted)
        return de__gpu_fail(d, DE_ERR_DEVICE_LOST, "metal: clearing the null descriptors: %s",
                            cb.error ? cb.error.localizedDescription.UTF8String : "failed");

    m->null_texture = (void *)CFBridgingRetain(tex);
    m->null_storage_texture = (void *)CFBridgingRetain(rw);
    m->null_buffer = (void *)CFBridgingRetain(buf);
    m->null_sampler = (void *)CFBridgingRetain(smp);
    m->null_ids[DE__GPU_BINDLESS_SAMPLED_TEXTURE] = resource_id(tex);
    m->null_ids[DE__GPU_BINDLESS_STORAGE_TEXTURE] = resource_id(rw);
    m->null_ids[DE__GPU_BINDLESS_BUFFER] = buf.gpuAddress;
    m->null_ids[DE__GPU_BINDLESS_SAMPLER] = sampler_id(smp);
    return DE_OK;
}

de_result de__mtl_bindless_init(de_gpu_device *d) {
    @autoreleasepool {
        mtl_device *m = de__mtl_dev(d);
        id<MTLDevice> dev = (__bridge id<MTLDevice>)m->device;
        id<MTLBuffer> table =
            [dev newBufferWithLength:DE__MTL_BINDLESS_BYTES
                             options:MTLResourceStorageModeShared |
                                     MTLResourceHazardTrackingModeUntracked];
        if (table == nil)
            return de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY,
                                "metal: bindless argument buffer (%u bytes)",
                                DE__MTL_BINDLESS_BYTES);
        table.label = @"de bindless heap";
        m->bindless = (void *)CFBridgingRetain(table);
        m->bindless_cpu = (u8 *)table.contents;

        de_result r = create_null_resources(d, m, dev);
        if (r != DE_OK)
            return r;
        for (u32 s = 0; s < 4; ++s)
            for (u32 i = 0; i < k_table_size[s]; ++i)
                put(m, (de__gpu_bindless_space)s, i, m->null_ids[s]);
        return DE_OK;
    }
}

static void release(void **p) {
    if (*p)
        (void)CFBridgingRelease(*p);
    *p = NULL;
}

void de__mtl_bindless_shutdown(de_gpu_device *d) {
    mtl_device *m = de__mtl_dev(d);
    @autoreleasepool {
        release(&m->null_texture);
        release(&m->null_storage_texture);
        release(&m->null_buffer);
        release(&m->null_sampler);
        if (m->internal.heap) {
            /* Not in the list when de__mtl_heap_list_add failed. */
            if (m->internal.slot < m->heap_count && m->heaps[m->internal.slot] == &m->internal)
                de__mtl_heap_list_remove(d, &m->internal);
            release(&m->internal.heap);
        }
        release(&m->bindless);
        m->bindless_cpu = NULL;
    }
}

/* ---------- Writes (vtable) ---------- */

void de__mtl_bindless_write_buffer(de_gpu_device *d, u32 index, de__gpu_buffer *b) {
    put(de__mtl_dev(d), DE__GPU_BINDLESS_BUFFER, index,
        DE__GPU_PAYLOAD(mtl_buffer, b)->gpu_address);
}

void de__mtl_bindless_write_texture(de_gpu_device *d, u32 index, de__gpu_texture *t, u32 mip,
                                    bool storage) {
    mtl_texture *mt = DE__GPU_PAYLOAD(mtl_texture, t);
    void *obj = storage && mt->views[mip] ? mt->views[mip] : mt->texture;
    if (!obj)
        return; /* an external texture whose backend object is not set yet */
    put(de__mtl_dev(d),
        storage ? DE__GPU_BINDLESS_STORAGE_TEXTURE : DE__GPU_BINDLESS_SAMPLED_TEXTURE, index,
        resource_id((__bridge id<MTLTexture>)obj));
}

void de__mtl_bindless_write_sampler(de_gpu_device *d, u32 index, de__gpu_sampler *s) {
    put(de__mtl_dev(d), DE__GPU_BINDLESS_SAMPLER, index,
        sampler_id((__bridge id<MTLSamplerState>)DE__GPU_PAYLOAD(mtl_sampler, s)->sampler));
}

void de__mtl_bindless_clear(de_gpu_device *d, de__gpu_bindless_space space, u32 index) {
    mtl_device *m = de__mtl_dev(d);
    if ((u32)space < 4u && m->bindless_cpu)
        put(m, space, index, m->null_ids[space]);
}

void *de__mtl_bindless_buffer(de_gpu_device *d, const u8 **cpu) {
    if (!de__mtl_device_object(d)) {
        if (cpu)
            *cpu = NULL;
        return NULL;
    }
    mtl_device *m = de__mtl_dev(d);
    if (cpu)
        *cpu = m->bindless_cpu;
    return m->bindless;
}
