/* Native intersection-function linking for Metal ray-query compute entry points (DENG-43). */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "gpu/metal/mtl_accel_internal.h"
/* Private shader ABI: optional de_metal_intersection export in the compute metallib;
 * one linked callback at offset0, kernel IFT buffer2, callback argument heap buffer0. */
de_result de__mtl_intersection_table(de_gpu_device *d, void *pipeline, void *function,
                                     void **table) {
    return de__mtl_intersection_tables(d, pipeline, &function, 1, table);
}
de_result de__mtl_intersection_tables(de_gpu_device *d, void *pipeline, void *const *functions,
                                      u32 count, void **table) {
    *table = NULL;
    @autoreleasepool {
        id<MTLComputePipelineState> p = (__bridge id<MTLComputePipelineState>)pipeline;
        MTLIntersectionFunctionTableDescriptor *td = [MTLIntersectionFunctionTableDescriptor new];
        td.functionCount = count;
        id<MTLIntersectionFunctionTable> t = [p newIntersectionFunctionTableWithDescriptor:td];
        if (!t)
            return de__gpu_fail(d, DE_ERR_UNSUPPORTED, "metal: intersection table");
        for (u32 i = 0; i < count; ++i) {
            id<MTLFunctionHandle> h =
                [p functionHandleWithFunction:(__bridge id<MTLFunction>)functions[i]];
            if (!h)
                return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                    "metal: intersection function handle");
            [t setFunction:h atIndex:i];
        }
        [t setBuffer:(__bridge id<MTLBuffer>)de__mtl_dev(d)->bindless offset:0 atIndex:0];
        *table = (void *)CFBridgingRetain(t);
        return DE_OK;
    }
}
