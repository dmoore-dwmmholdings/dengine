#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include "gpu/metal/mtl_accel_internal.h"
#include <string.h>
static id<MTLFunction> function(de_gpu_device *d, de_gpu_shader_blob b) {
    dispatch_data_t data =
        dispatch_data_create(b.code, (size_t)b.size, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *error = nil;
    id<MTLLibrary> library =
        [(__bridge id<MTLDevice>)de__mtl_dev(d)->device newLibraryWithData:data error:&error];
    if (!library) {
        de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "Metal RT library: %s",
                     error.localizedDescription.UTF8String);
        return nil;
    }
    id<MTLFunction> f =
        [library newFunctionWithName:[NSString stringWithUTF8String:b.entry ? b.entry : ""]];
    if (!f)
        de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "Metal RT missing function %s",
                     b.entry ? b.entry : "");
    return f;
}
static void release(void **p) {
    if (*p)
        (void)CFBridgingRelease(*p);
    *p = NULL;
}
de_result de__mtl_rt_pipeline_create(de_gpu_device *d, de__gpu_pipeline *p,
                                     const de_gpu_pipeline_desc *desc) {
    @autoreleasepool {
        de_gpu_rt_desc r;
        de_gpu_rt_hit_group shorthand;
        de__gpu_rt_desc_init(desc, &r, &shorthand);
        mtl_pipeline *out = DE__GPU_PAYLOAD(mtl_pipeline, p);
        id<MTLFunction> raygen = function(d, r.raygen);
        if (!raygen)
            return DE_ERR_INVALID_ARGUMENT;
        NSMutableArray<id<MTLFunction>> *visible = [NSMutableArray new],
                                        *any = [NSMutableArray new], *all = [NSMutableArray new];
        de_gpu_shader_blob fallback = r.raygen;
        fallback.entry = "de_rt_accept";
        id<MTLFunction> accept = function(d, fallback);
        if (!accept)
            return DE_ERR_INVALID_ARGUMENT;
        for (u32 i = 0; i < r.miss_count; ++i) {
            id<MTLFunction> f = function(d, r.miss[i]);
            if (!f)
                return DE_ERR_INVALID_ARGUMENT;
            [visible addObject:f];
        }
        for (u32 i = 0; i < r.hit_group_count; ++i) {
            id<MTLFunction> f = function(d, r.hit_groups[i].closest_hit);
            if (!f)
                return DE_ERR_INVALID_ARGUMENT;
            [visible addObject:f];
            f = r.hit_groups[i].any_hit.code ? function(d, r.hit_groups[i].any_hit) : accept;
            if (!f)
                return DE_ERR_INVALID_ARGUMENT;
            [any addObject:f];
        }
        for (id<MTLFunction> f in visible)
            if (![all containsObject:f])
                [all addObject:f];
        for (id<MTLFunction> f in any)
            if (![all containsObject:f])
                [all addObject:f];
        if (!any.count)
            [all addObject:accept];
        MTLLinkedFunctions *linked = [MTLLinkedFunctions new];
        linked.functions = all;
        MTLComputePipelineDescriptor *pd = [MTLComputePipelineDescriptor new];
        pd.computeFunction = raygen;
        pd.linkedFunctions = linked;
        NSError *error = nil;
        id<MTLComputePipelineState> state = [(__bridge id<MTLDevice>)de__mtl_dev(d)->device
            newComputePipelineStateWithDescriptor:pd
                                          options:MTLPipelineOptionNone
                                       reflection:nil
                                            error:&error];
        if (!state)
            return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "Metal RT pipeline: %s",
                                error.localizedDescription.UTF8String);
        MTLVisibleFunctionTableDescriptor *vd = [MTLVisibleFunctionTableDescriptor new];
        vd.functionCount = visible.count ? visible.count : 1;
        id<MTLVisibleFunctionTable> vt = [state newVisibleFunctionTableWithDescriptor:vd];
        if (!vt)
            return de__gpu_fail(d, DE_ERR_UNSUPPORTED, "Metal visible function table");
        for (NSUInteger i = 0; i < visible.count; ++i) {
            id<MTLFunctionHandle> h = [state functionHandleWithFunction:visible[i]];
            if (!h)
                return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "Metal visible handle");
            [vt setFunction:h atIndex:i];
        }
        void **callbacks =
            DE_ALLOC(d->alloc, (any.count ? any.count : 1) * sizeof *callbacks, 16, DE_MEM_RENDER);
        if (!callbacks)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "Metal RT callbacks");
        for (NSUInteger i = 0; i < any.count; ++i)
            callbacks[i] = (__bridge void *)any[i];
        if (!any.count)
            callbacks[0] = (__bridge void *)accept;
        de_result result =
            de__mtl_intersection_tables(d, (__bridge void *)state, callbacks,
                                        (u32)(any.count ? any.count : 1), &out->intersection_table);
        DE_FREE(d->alloc, callbacks, (any.count ? any.count : 1) * sizeof *callbacks);
        if (result != DE_OK)
            return result;
        const u32 n = 1 + r.miss_count + r.hit_group_count;
        u32 *handles = DE_ALLOC(d->alloc, (size_t)n * 16, 16, DE_MEM_RENDER);
        if (!handles) {
            release(&out->intersection_table);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "Metal SBT handles");
        }
        memset(handles, 0, (size_t)n * 16);
        handles[0] = r.miss_count;
        handles[1] = r.hit_group_count;
        handles[2] = 16;
        handles[3] = (1 + r.miss_count) * 16;
        for (u32 i = 1; i < n; ++i)
            handles[i * 4] = i - 1;
        result = de__gpu_rt_sbt_create(d, p, handles, 16, 16, 16, r.miss_count, r.hit_group_count);
        DE_FREE(d->alloc, handles, (size_t)n * 16);
        if (result != DE_OK) {
            release(&out->intersection_table);
            return result;
        }
        out->state = (void *)CFBridgingRetain(state);
        out->visible_table = (void *)CFBridgingRetain(vt);
        out->push_stages = DE__MTL_PUSH_COMPUTE;
        out->push_bytes = p->push_constant_bytes;
        return DE_OK;
    }
}
void de__mtl_cmd_dispatch_rays(de_gpu_cmd *c, u32 x, u32 y, u32 z) {
    de__gpu_pipeline *p = c->bound_pipeline;
    @autoreleasepool {
        mtl_pipeline *mp = DE__GPU_PAYLOAD(mtl_pipeline, p);
        mtl_cmd *mc = de__gpu_cmd_payload(c);
        id<MTLComputeCommandEncoder> e =
            (__bridge id<MTLComputeCommandEncoder>)de__mtl_cmd_encoder(c, DE__MTL_ENC_COMPUTE);
        [e setComputePipelineState:(__bridge id<MTLComputePipelineState>)mp->state];
        [e setIntersectionFunctionTable:(__bridge id<MTLIntersectionFunctionTable>)
                                            mp->intersection_table
                          atBufferIndex:2];
        [e setVisibleFunctionTable:(__bridge id<MTLVisibleFunctionTable>)mp->visible_table
                     atBufferIndex:3];
        de__gpu_buffer *b = de__gpu_get_buffer(c->device, p->rt_sbt);
        [e setBuffer:(__bridge id<MTLBuffer>)DE__GPU_PAYLOAD(mtl_buffer, b)->buffer
              offset:0
             atIndex:4];
        if (p->push_constant_bytes)
            [e setBytes:mc->draw.push length:p->push_constant_bytes atIndex:1];
        [e dispatchThreads:MTLSizeMake(x, y, z) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        /* Next compute binding must restore its state even on this same encoder. */
        mc->draw.dirty |= DE__MTL_DIRTY_PIPELINE | DE__MTL_DIRTY_PUSH;
    }
}
