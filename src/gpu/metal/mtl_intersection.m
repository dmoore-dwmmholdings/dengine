/* Native intersection-function linking for Metal ray-query compute entry points (DENG-43). */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "gpu/metal/mtl_accel_internal.h"
/* Private shader ABI: optional de_metal_intersection export in the compute metallib;
 * one linked callback at offset0, kernel IFT buffer2, callback argument heap buffer0. */
de_result de__mtl_intersection_table(de_gpu_device *d, void *pipeline, void *function, void **table) {
    *table = NULL;
    @autoreleasepool {
        id<MTLComputePipelineState> p = (__bridge id<MTLComputePipelineState>)pipeline;
        MTLIntersectionFunctionTableDescriptor *td = [MTLIntersectionFunctionTableDescriptor new];
        td.functionCount = 1;
        id<MTLIntersectionFunctionTable> t = [p newIntersectionFunctionTableWithDescriptor:td];
        id<MTLFunctionHandle> h = [p functionHandleWithFunction:(__bridge id<MTLFunction>)function];
        if (!t || !h) return de__gpu_fail(d, DE_ERR_UNSUPPORTED, "metal: intersection table");
        [t setFunction:h atIndex:0];
        [t setBuffer:(__bridge id<MTLBuffer>)de__mtl_dev(d)->bindless offset:0 atIndex:0];
        *table = (void *)CFBridgingRetain(t);
        return DE_OK;
    }
}
de_result de__mtl_intersection_link(de_gpu_device *d, void *kernel, void *function,
                                    void **pipeline, void **table) {
    *pipeline = NULL;
    *table = NULL;
    @autoreleasepool {
        MTLComputePipelineDescriptor *desc = [MTLComputePipelineDescriptor new];
        desc.computeFunction = (__bridge id<MTLFunction>)kernel;
        MTLLinkedFunctions *linked = [MTLLinkedFunctions new];
        linked.functions = @[(__bridge id<MTLFunction>)function];
        desc.linkedFunctions = linked;
        NSError *err = nil;
        id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_dev(d)->device;
        id<MTLComputePipelineState> p = [dev newComputePipelineStateWithDescriptor:desc
            options:MTLPipelineOptionNone reflection:nil error:&err];
        if (!p) return de__gpu_fail(d, DE_ERR_UNSUPPORTED, "metal: intersection pipeline: %s",
                                    err.localizedDescription.UTF8String);
        DE_TRY(de__mtl_intersection_table(d, (__bridge void *)p, function, table));
        *pipeline = (void *)CFBridgingRetain(p);
        return DE_OK;
    }
}
