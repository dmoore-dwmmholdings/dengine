#ifndef DE_GPU_MTL_ACCEL_INTERNAL_H
#define DE_GPU_MTL_ACCEL_INTERNAL_H
#include "gpu/metal/mtl_internal.h"
#include "gpu/gpu_accel_internal.h"
/* Metal indirect descriptors are 72 bytes: packed column-major float4x3, four u32,
 * then an eight-byte resource ID. Never use the Vulkan/DXR 64-byte record here. */
typedef struct mtl_accel {
    void *object;
    void *descriptor; /* retained native descriptor, reused across recordings */
    void *refs; /* retained NSArray of every BLAS eligible in the build snapshot */
    struct mtl_accel *next;
    de__gpu_buffer *query;
} mtl_accel;
de_result de__mtl_accel_sizes(de_gpu_device *, const de_gpu_accel_desc *, de_gpu_accel_sizes *);
de_result de__mtl_accel_create(de_gpu_device *, de__gpu_accel *, const de_gpu_accel_desc *);
void de__mtl_accel_destroy(de_gpu_device *, de__gpu_accel *);
u64 de__mtl_accel_address(de_gpu_device *, de__gpu_accel *);
void de__mtl_bindless_write_accel(de_gpu_device *, u32, de__gpu_accel *);
void de__mtl_accel_blas(de_gpu_cmd *, de__gpu_accel *, bool);
void de__mtl_accel_tlas(de_gpu_cmd *, de__gpu_accel *, de__gpu_buffer *, u32, bool);
void de__mtl_accel_emit(de_gpu_cmd *, de__gpu_accel *);
bool de__mtl_accel_read(de_gpu_device *, de__gpu_accel *, u64 *);
void de__mtl_accel_compact(de_gpu_cmd *, de__gpu_accel *, de__gpu_accel *);
void de__mtl_accel_residency(de_gpu_device *, void *, de__mtl_encoder_kind);
void de__mtl_accel_shutdown(de_gpu_device *);
/* Link a compute entry point with an intersection function, producing retained Metal objects.
 * Native functions stay opaque outside this private backend interface. */
de_result de__mtl_intersection_table(de_gpu_device *, void *pipeline, void *function, void **table);
de_result de__mtl_intersection_link(de_gpu_device *, void *kernel, void *function,
                                    void **pipeline, void **table);
#endif
