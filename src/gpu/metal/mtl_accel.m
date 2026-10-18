/* DENG-43: Metal 3 acceleration structures, indirect GPU instance translation and residency. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "gpu/metal/mtl_accel_internal.h"
#include <string.h>
#include "mtl_accel_shader.h"

_Static_assert(sizeof(MTLIndirectAccelerationStructureInstanceDescriptor) == 72, "Metal ABI");
_Static_assert(offsetof(MTLIndirectAccelerationStructureInstanceDescriptor,
                        accelerationStructureID) == 64,
               "Metal resource ID offset");
static id<MTLBuffer> buffer(de__gpu_buffer *b) {
    return b ? (__bridge id<MTLBuffer>)DE__GPU_PAYLOAD(mtl_buffer, b)->buffer : nil;
}
static id<MTLAccelerationStructure> object(de__gpu_accel *a) {
    return (__bridge id<MTLAccelerationStructure>)DE__GPU_PAYLOAD(mtl_accel, a)->object;
}
static void release(void **p) {
    if (*p)
        (void)CFBridgingRelease(*p);
    *p = NULL;
}
static MTLAccelerationStructureDescriptor *descriptor(de_gpu_device *d,
                                                      const de_gpu_accel_desc *x) {
    MTLAccelerationStructureDescriptor *result;
    if (x->is_tlas) {
        MTLInstanceAccelerationStructureDescriptor *t =
            [MTLInstanceAccelerationStructureDescriptor descriptor];
        t.instanceCount = x->max_instances;
        t.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeIndirect;
        t.instanceDescriptorStride = sizeof(MTLIndirectAccelerationStructureInstanceDescriptor);
        result = t;
    } else {
        NSMutableArray *geometries = [NSMutableArray arrayWithCapacity:x->geometry_count];
        for (u32 i = 0; i < x->geometry_count; ++i) {
            const de_gpu_blas_geometry *g = &x->geometries[i];
            MTLAccelerationStructureTriangleGeometryDescriptor *v =
                [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
            v.vertexBuffer = buffer(de__gpu_get_buffer(d, g->vertices));
            v.vertexBufferOffset = g->vertex_offset;
            v.vertexStride = g->vertex_stride;
            v.vertexFormat = MTLAttributeFormatFloat3;
            v.indexBuffer = buffer(de__gpu_get_buffer(d, g->indices));
            v.indexBufferOffset = g->index_offset;
            v.indexType = g->index_u16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
            v.triangleCount = (g->indices.id ? g->index_count : g->vertex_count) / 3;
            v.opaque = g->opaque;
            v.intersectionFunctionTableOffset = 0; /* DENG-44 supplies material alpha logic */
            [geometries addObject:v];
        }
        MTLPrimitiveAccelerationStructureDescriptor *p =
            [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        p.geometryDescriptors = geometries;
        result = p;
    }
    result.usage = (x->allow_update ? MTLAccelerationStructureUsageRefit : 0) |
                   (x->prefer_fast_trace ? 0 : MTLAccelerationStructureUsagePreferFastBuild);
    return result;
}
de_result de__mtl_accel_sizes(de_gpu_device *d, const de_gpu_accel_desc *x, de_gpu_accel_sizes *s) {
    @autoreleasepool {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_dev(d)->device;
        if (!dev.supportsRaytracing)
            return de__gpu_fail(d, DE_ERR_UNSUPPORTED, "metal: ray tracing unavailable");
        MTLAccelerationStructureSizes sizes =
            [dev accelerationStructureSizesWithDescriptor:descriptor(d, x)];
        s->accel_bytes = sizes.accelerationStructureSize;
        s->build_scratch_bytes = sizes.buildScratchBufferSize;
        s->update_scratch_bytes = sizes.refitScratchBufferSize;
        return s->accel_bytes ? DE_OK
                              : de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                             "metal: invalid acceleration structure size");
    }
}
de_result de__mtl_accel_create(de_gpu_device *d, de__gpu_accel *a, const de_gpu_accel_desc *x) {
    @autoreleasepool {
        mtl_device *m = de__mtl_dev(d);
        mtl_accel *v = DE__GPU_PAYLOAD(mtl_accel, a);
        id<MTLAccelerationStructure> as = [(__bridge id<MTLDevice>)m->device
            newAccelerationStructureWithSize:a->sizes.accel_bytes];
        if (!as)
            return de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY, "metal: acceleration structure");
        if (x->name && x->name[0])
            as.label = [NSString stringWithUTF8String:x->name];
        if (x->allow_compaction) {
            v->query = de__gpu_accel_buffer(d, 8, DE_USAGE_COPY_DST, DE_MEM_READBACK);
            if (!v->query)
                return DE_ERR_OUT_OF_MEMORY;
        }
        v->object = (void *)CFBridgingRetain(as);
        v->descriptor = (void *)CFBridgingRetain(descriptor(d, x));
        if (x->is_tlas)
            v->refs = (void *)CFBridgingRetain([NSMutableArray new]);
        de_mutex_lock(&m->heap_lock);
        v->next = m->accels;
        m->accels = v;
        de_mutex_unlock(&m->heap_lock);
        return DE_OK;
    }
}
void de__mtl_accel_destroy(de_gpu_device *d, de__gpu_accel *a) {
    @autoreleasepool {
        mtl_device *m = de__mtl_dev(d);
        mtl_accel *v = DE__GPU_PAYLOAD(mtl_accel, a);
        de_mutex_lock(&m->heap_lock);
        mtl_accel **p = &m->accels;
        while (*p && *p != v)
            p = &(*p)->next;
        if (*p)
            *p = v->next;
        release(&v->refs);
        release(&v->descriptor);
        release(&v->object);
        de_mutex_unlock(&m->heap_lock);
        de__gpu_accel_buffer_free(d, v->query);
        memset(v, 0, sizeof *v);
    }
}
u64 de__mtl_accel_address(de_gpu_device *d, de__gpu_accel *a) {
    (void)d;
    MTLResourceID rid = object(a).gpuResourceID;
    u64 value;
    memcpy(&value, &rid, sizeof value);
    return value;
}
void de__mtl_bindless_write_accel(de_gpu_device *d, u32 index, de__gpu_accel *a) {
    const u64 id = a ? de__mtl_accel_address(d, a) : 0;
    memcpy(de__mtl_dev(d)->bindless_cpu + DE__MTL_BINDLESS_OFFSET_ACCELS + (size_t)index * 8, &id,
           sizeof id);
}
/* All AS operations are ordered across encoders, including the translation dispatch. This
 * gives native builds the same internal ordering as Vulkan/DXR without relying on tracking. */
static void dependency(de_gpu_cmd *c) {
    de__mtl_encoder_end(c);
    de__mtl_cmd_barriers(c, NULL, 0);
}
static id<MTLAccelerationStructureCommandEncoder> encoder(de_gpu_cmd *c) {
    dependency(c);
    return (__bridge id<MTLAccelerationStructureCommandEncoder>)de__mtl_cmd_encoder(
        c, DE__MTL_ENC_ACCEL);
}
static bool build(de_gpu_cmd *c, de__gpu_accel *a, bool update,
                  MTLAccelerationStructureDescriptor *desc, de__gpu_accel_scratch *s) {
    id<MTLAccelerationStructureCommandEncoder> enc = encoder(c);
    if (!enc)
        return false;
    if (update)
        [enc refitAccelerationStructure:object(a)
                             descriptor:desc
                            destination:object(a)
                          scratchBuffer:buffer(s->scratch)
                    scratchBufferOffset:0];
    else
        [enc buildAccelerationStructure:object(a)
                             descriptor:desc
                          scratchBuffer:buffer(s->scratch)
                    scratchBufferOffset:0];
    dependency(c);
    a->built = true;
    return true;
}
void de__mtl_accel_blas(de_gpu_cmd *c, de__gpu_accel *a, bool update) {
    a->built = false;
    @autoreleasepool {
        de__gpu_accel_scratch *s = de__gpu_accel_scratch_get(
            c, update ? a->sizes.update_scratch_bytes : a->sizes.build_scratch_bytes, 0);
        if (s)
            build(c, a, update,
                  (__bridge MTLAccelerationStructureDescriptor *)DE__GPU_PAYLOAD(mtl_accel, a)
                      ->descriptor,
                  s);
    }
}
static bool translate(de_gpu_cmd *c, de__gpu_buffer *input, de__gpu_accel_scratch *s, u32 count) {
    mtl_device *m = de__mtl_dev(c->device);
    de_mutex_lock(&m->library_lock);
    if (!m->accel_translate) {
        NSError *err = nil;
        id<MTLDevice> dev = (__bridge id<MTLDevice>)m->device;
        id<MTLLibrary> lib = [dev
            newLibraryWithSource:[NSString
                                     stringWithUTF8String:(const char *)de__mtl_accel_translation]
                         options:nil
                           error:&err];
        id<MTLComputePipelineState> p =
            lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"translate"]
                                                     error:&err]
                : nil;
        if (p)
            m->accel_translate = (void *)CFBridgingRetain(p);
        else
            de__gpu_fail(c->device, DE_ERR_UNSUPPORTED, "metal: instance translation: %s",
                         err.localizedDescription.UTF8String);
    }
    void *pipeline = m->accel_translate;
    de_mutex_unlock(&m->library_lock);
    if (!pipeline)
        return false;
    dependency(c);
    id<MTLComputeCommandEncoder> e =
        (__bridge id<MTLComputeCommandEncoder>)de__mtl_cmd_encoder(c, DE__MTL_ENC_COMPUTE);
    if (!e)
        return false;
    [e setComputePipelineState:(__bridge id<MTLComputePipelineState>)pipeline];
    [e setBuffer:buffer(input) offset:0 atIndex:2];
    [e setBuffer:buffer(s->instances) offset:0 atIndex:3];
    [e setBuffer:buffer(s->addresses) offset:0 atIndex:4];
    const u32 p[2] = {count, de__gpu_pool_get_stats(&c->device->pools[DE__GPU_ACCEL]).used};
    [e setBytes:p length:sizeof p atIndex:5];
    if (count)
        [e dispatchThreadgroups:MTLSizeMake(((u64)count + 63) / 64 > 65535 ? 65535
                                                                           : ((u64)count + 63) / 64,
                                            1, 1)
            threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    c->has_pipeline = false;
    ((mtl_cmd *)de__gpu_cmd_payload(c))->draw.dirty = DE__MTL_DIRTY_ALL;
    dependency(c);
    return true;
}
void de__mtl_accel_tlas(de_gpu_cmd *c, de__gpu_accel *a, de__gpu_buffer *input, u32 count,
                        bool update) {
    a->built = false;
    @autoreleasepool {
        /* Shared allocator owns the block lifetime; request enough 64-byte units for 72-byte
         * native records. It also writes the immutable full-generation resource ID snapshot. */
        const u64 units = ((u64)(count ? count : 1) * 72 + 63) / 64;
        if (units > UINT32_MAX)
            return;
        de__gpu_accel_scratch *s = de__gpu_accel_scratch_get(
            c, update ? a->sizes.update_scratch_bytes : a->sizes.build_scratch_bytes, (u32)units);
        if (!s || !translate(c, input, s, count))
            return;
        mtl_accel *v = DE__GPU_PAYLOAD(mtl_accel, a);
        mtl_cmd *mc = de__gpu_cmd_payload(c);
        if (!mc->accel_keep)
            mc->accel_keep = (void *)CFBridgingRetain([NSMutableSet new]);
        NSMutableSet *keep = (__bridge NSMutableSet *)mc->accel_keep;
        NSMutableArray *refs = (__bridge NSMutableArray *)v->refs;
        mtl_device *m = de__mtl_dev(c->device);
        de_mutex_lock(&m->heap_lock);
        /* Keep every prior dependency until this recording completes; reuse the arrays. */
        [keep addObjectsFromArray:refs];
        [refs removeAllObjects];
        de__gpu_pool *pool = &c->device->pools[DE__GPU_ACCEL];
        const u32 n = de__gpu_pool_get_stats(pool).used;
        const de__gpu_accel_address *table = s->addresses->mapped;
        for (u32 i = 0; i < n; ++i) {
            if (!table[i].address || !de__gpu_pool_is_live(pool, i))
                continue;
            de__gpu_accel *b = de__gpu_pool_at(pool, i);
            if (de__gpu_pool_id_at(pool, i) == table[i].handle)
                [refs addObject:object(b)];
        }
        de_mutex_unlock(&m->heap_lock);
        MTLInstanceAccelerationStructureDescriptor *t =
            (__bridge MTLInstanceAccelerationStructureDescriptor *)v->descriptor;
        t.instanceCount = count;
        t.instanceDescriptorBuffer = buffer(s->instances);
        id<MTLAccelerationStructureCommandEncoder> e = encoder(c);
        if (!e)
            return;
        for (id<MTLAccelerationStructure> b in refs)
            [e useResource:b usage:MTLResourceUsageRead];
        if (update)
            [e refitAccelerationStructure:object(a)
                               descriptor:t
                              destination:object(a)
                            scratchBuffer:buffer(s->scratch)
                      scratchBufferOffset:0];
        else
            [e buildAccelerationStructure:object(a)
                               descriptor:t
                            scratchBuffer:buffer(s->scratch)
                      scratchBufferOffset:0];
        [keep addObjectsFromArray:refs];
        dependency(c);
        a->built = true;
    }
}
void de__mtl_accel_emit(de_gpu_cmd *c, de__gpu_accel *a) {
    @autoreleasepool {
        id<MTLAccelerationStructureCommandEncoder> e = encoder(c);
        if (!e)
            return;
        [e writeCompactedAccelerationStructureSize:object(a)
                                          toBuffer:buffer(DE__GPU_PAYLOAD(mtl_accel, a)->query)
                                            offset:0
                                      sizeDataType:MTLDataTypeULong];
        dependency(c);
    }
}
bool de__mtl_accel_read(de_gpu_device *d, de__gpu_accel *a, u64 *bytes) {
    (void)d;
    de__gpu_buffer *q = DE__GPU_PAYLOAD(mtl_accel, a)->query;
    if (!q || !q->mapped)
        return false;
    memcpy(bytes, q->mapped, sizeof *bytes);
    return *bytes > 0 && *bytes <= a->sizes.accel_bytes;
}
void de__mtl_accel_compact(de_gpu_cmd *c, de__gpu_accel *src, de__gpu_accel *dst) {
    @autoreleasepool {
        id<MTLAccelerationStructureCommandEncoder> e = encoder(c);
        if (!e)
            return;
        [e copyAndCompactAccelerationStructure:object(src) toAccelerationStructure:object(dst)];
        dependency(c);
        dst->built = true;
    }
}
void de__mtl_accel_residency(de_gpu_cmd *c, void *encoder, de__mtl_encoder_kind kind) {
    mtl_device *m = de__mtl_dev(c->device);
    mtl_cmd *mc = de__gpu_cmd_payload(c);
    de_mutex_lock(&m->heap_lock);
    for (mtl_accel *a = m->accels; a; a = a->next) {
        NSArray *resources = (__bridge NSArray *)a->refs;
        id<MTLResource> obj = (__bridge id<MTLAccelerationStructure>)a->object;
        /* A retired record can still be on this list, with a retire fence older than this
         * recording. Every object named by useResource needs its own command ownership;
         * front-end deferred destruction alone cannot protect that newer use. Retain while
         * holding the list lock, before another thread can unlink/release the native object. */
        if (!mc->accel_keep)
            mc->accel_keep = (void *)CFBridgingRetain([NSMutableSet new]);
        NSMutableSet *keep = (__bridge NSMutableSet *)mc->accel_keep;
        [keep addObject:obj];
        if (resources)
            [keep addObjectsFromArray:resources];
        if (kind == DE__MTL_ENC_COMPUTE) {
            id<MTLComputeCommandEncoder> e = (__bridge id<MTLComputeCommandEncoder>)encoder;
            [e useResource:obj usage:MTLResourceUsageRead];
            for (id<MTLResource> b in resources)
                [e useResource:b usage:MTLResourceUsageRead];
        } else if (kind == DE__MTL_ENC_RENDER) {
            id<MTLRenderCommandEncoder> e = (__bridge id<MTLRenderCommandEncoder>)encoder;
            [e useResource:obj usage:MTLResourceUsageRead stages:(MTLRenderStages)m->render_stages];
            for (id<MTLResource> b in resources)
                [e useResource:b
                         usage:MTLResourceUsageRead
                        stages:(MTLRenderStages)m->render_stages];
        }
    }
    de_mutex_unlock(&m->heap_lock);
}
void de__mtl_accel_shutdown(de_gpu_device *d) {
    release(&de__mtl_dev(d)->accel_translate);
}
