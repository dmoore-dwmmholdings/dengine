#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "gpu/metal/mtl_accel_internal.h"
#include <stdio.h>
#include <string.h>
/* R2 permits handwritten MSL for native intersection tests. This exercises the real bindless
 * AS slot and linked callback table; it does not stand in for production shader compilation. */
typedef struct helper { de_gpu_pipeline pipeline; void *generate; } helper;
void *de_test_mtl_accel_helper(de_gpu_device *d) {
    @autoreleasepool {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_dev(d)->device;
        NSError *err = nil;
        NSData *data=[NSData dataWithContentsOfFile:@DE_MTL_ACCEL_TEST_LIBRARY];
        if (!data) return NULL;
        dispatch_data_t bytes=dispatch_data_create(data.bytes,data.length,NULL,DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        id<MTLLibrary> lib=[dev newLibraryWithData:bytes error:&err];
        if (!lib) { fprintf(stderr,"metallib: %s\n",err.localizedDescription.UTF8String); return NULL; }
        helper *h=DE_ALLOC(d->alloc,sizeof *h,16,DE_MEM_RENDER);
        if (!h) return NULL;
        memset(h,0,sizeof *h);
        h->pipeline=de_gpu_pipeline_create(d,&(de_gpu_pipeline_desc){.kind=DE_PIPE_COMPUTE,
            .cs={data.bytes,data.length,"trace"},.push_constant_bytes=12,.threadgroup_size={1,1,1}});
        if (!h->pipeline.id) { DE_FREE(d->alloc,h,sizeof *h); return NULL; }
        id<MTLComputePipelineState> gen = [dev newComputePipelineStateWithFunction:
            [lib newFunctionWithName:@"generate"] error:&err];
        if (gen) h->generate = (void *)CFBridgingRetain(gen);
        return h;
    }
}
void de_test_mtl_accel_release(de_gpu_device *d, void *value) {
    helper *h = value;
    if (!h) return;
    @autoreleasepool {
        de_gpu_pipeline_destroy(d,h->pipeline);
        if (h->generate) (void)CFBridgingRelease(h->generate);
    }
    DE_FREE(d->alloc,h,sizeof *h);
}
bool de_test_mtl_accel_trace(de_gpu_cmd *c, void *value, u32 slot, de_gpu_buffer output,
                            de_gpu_buffer calls) {
    @autoreleasepool {
        helper *h = value;
        de__gpu_buffer *o = de__gpu_get_buffer(c->device,output);
        de__gpu_buffer *b = de__gpu_get_buffer(c->device,calls);
        if (!h || !o || !b) return false;
        id<MTLComputeCommandEncoder> e = (__bridge id<MTLComputeCommandEncoder>)
            de__mtl_cmd_encoder(c,DE__MTL_ENC_COMPUTE);
        id<MTLBuffer> cb = (__bridge id<MTLBuffer>)DE__GPU_PAYLOAD(mtl_buffer,b)->buffer;
        mtl_pipeline *p=DE__GPU_PAYLOAD(mtl_pipeline,de__gpu_get_pipeline(c->device,h->pipeline));
        id<MTLIntersectionFunctionTable> t=(__bridge id<MTLIntersectionFunctionTable>)p->intersection_table;
        if (!t) return false;
        [t setBuffer:cb offset:0 atIndex:1]; /* diagnostic counter; production callback heap is slot0 */
        [e useResource:cb usage:MTLResourceUsageRead|MTLResourceUsageWrite];
        de_gpu_cmd_set_pipeline(c,h->pipeline);
        const u32 pc[3]={slot,o->bindless,b->bindless};
        de_gpu_cmd_push_constants(c,pc,sizeof pc);
        de_gpu_cmd_dispatch(c,1,1,1);
        return true;
    }
}
bool de_test_mtl_accel_generate(de_gpu_cmd *c, void *value, de_gpu_buffer output,
                               const de_gpu_tlas_instance *instance) {
    @autoreleasepool {
        helper *h=value;
        de__gpu_buffer *b=de__gpu_get_buffer(c->device,output);
        if (!h || !h->generate || !b) return false;
        id<MTLComputeCommandEncoder> e=(__bridge id<MTLComputeCommandEncoder>)
            de__mtl_cmd_encoder(c,DE__MTL_ENC_COMPUTE);
        [e setComputePipelineState:(__bridge id<MTLComputePipelineState>)h->generate];
        [e setBuffer:(__bridge id<MTLBuffer>)DE__GPU_PAYLOAD(mtl_buffer,b)->buffer offset:0 atIndex:2];
        [e setBytes:instance length:sizeof *instance atIndex:3];
        [e dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(16,1,1)];
        return true;
    }
}
