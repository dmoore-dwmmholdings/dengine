#ifndef DE_GPU_ACCEL_INTERNAL_H
#define DE_GPU_ACCEL_INTERNAL_H
#include "gpu/gpu_backend.h"

/* Shared by Vulkan, DXR and the forthcoming Metal implementation. */
typedef struct de__gpu_native_instance {
    f32 transform[12];
    u32 custom_mask, sbt_flags;
    u64 address;
} de__gpu_native_instance;
typedef struct de__gpu_accel_address {
    u32 handle, reserved;
    u64 address;
} de__gpu_accel_address;
_Static_assert(sizeof(de_gpu_tlas_instance) == 60, "public instance ABI");
_Static_assert(sizeof(de__gpu_native_instance) == 64, "native instance ABI");
_Static_assert(offsetof(de__gpu_native_instance, address) == 56, "native address ABI");

/* Internal resources bypass public pools, but use the same TLSF heap manager and bindless
 * allocator. Each recording takes another block; blocks are reused only after its fence. */
typedef struct de__gpu_accel_scratch {
    struct de__gpu_accel_scratch *next;
    de__gpu_buffer *scratch, *instances, *addresses;
} de__gpu_accel_scratch;
de__gpu_buffer *de__gpu_accel_buffer(de_gpu_device *d, u64 bytes, u32 usage, de_gpu_memory memory);
void de__gpu_accel_buffer_free(de_gpu_device *d, de__gpu_buffer *b);
de__gpu_accel_scratch *de__gpu_accel_scratch_get(de_gpu_cmd *c, u64 bytes, u32 instances);
bool de__gpu_accel_translate_gpu(de_gpu_cmd *, de__gpu_buffer *, de__gpu_accel_scratch *, u32);
void de__gpu_accel_cmd_fini(de_gpu_cmd *c);
void de__gpu_accel_submitted(de_gpu_cmd *c, u64 value);
de_result de__gpu_accel_validate(de_gpu_device *d, const de_gpu_accel_desc *desc);
de_gpu_accel de__gpu_accel_create(de_gpu_device *d, const de_gpu_accel_desc *desc, u64 bytes);
/* CPU reference; a missing/stale/non-BLAS handle produces mask zero and address zero. */
void de__gpu_accel_translate(const de_gpu_tlas_instance *in, de__gpu_native_instance *out,
                             const de__gpu_accel_address *table, u32 table_count);

/* Deterministic null model: no native GPU support is advertised. */
de_result de__null_accel_sizes(de_gpu_device *, const de_gpu_accel_desc *, de_gpu_accel_sizes *);
de_result de__null_accel_create(de_gpu_device *, de__gpu_accel *, const de_gpu_accel_desc *);
void de__null_accel_build_blas(de_gpu_cmd *, de__gpu_accel *, bool);
void de__null_accel_build_tlas(de_gpu_cmd *, de__gpu_accel *, de__gpu_buffer *, u32, bool);
void de__null_accel_emit(de_gpu_cmd *, de__gpu_accel *);
bool de__null_accel_read(de_gpu_device *, de__gpu_accel *, u64 *);
void de__null_accel_compact(de_gpu_cmd *, de__gpu_accel *, de__gpu_accel *);
#endif
