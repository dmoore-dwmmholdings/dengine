/* mtl_cmd_helpers.h — C interface of mtl_cmd_helpers.m (Metal command, sync and swapchain tests,
 * DENG-288). */
#ifndef DE_TEST_MTL_CMD_HELPERS_H
#define DE_TEST_MTL_CMD_HELPERS_H
#include <dengine/de_gpu.h>

#include <stddef.h>

/** fillBuffer:range:value: on a blit encoder (from de__mtl_cmd_encoder) for a buffer object
 * (from de__mtl_buffer_object). */
void de_test_mtl_fill(void *blit_encoder, void *buffer, u64 offset, u64 size, u8 value);

/**
 * Dispatches one thread of a kernel on a compute encoder that the backend opened (it has bound
 * the bindless table at buffer index 0 and made the heaps resident): it copies `count` float4s
 * from bindless buffer `in_index` to bindless buffer `out_index`. The kernel does no useHeap: of
 * its own. 1 = ok; 0 with the reason in `err`.
 */
int de_test_mtl_dispatch_copy(de_gpu_device *d, void *compute_encoder, u32 in_index, u32 out_index,
                              u32 count, char *err, size_t cap);

/** Whether this OS has MTLResidencySet (macOS 15+). */
bool de_test_mtl_has_residency_sets(void);

/** GPU timestamp counter samples around a blit pass, with the GPU and CPU clocks around it. */
typedef struct de_test_mtl_timestamps {
    u64 start, end;            /* counter samples (MTLCommonCounterSetTimestamp) */
    u64 gpu_before, gpu_after; /* sampleTimestamps:gpuTimestamp: before commit, after completion */
    double gpu_seconds;        /* the command buffer's GPUEndTime - GPUStartTime */
} de_test_mtl_timestamps;
/** 1 = filled; -1 = the device cannot sample timestamps at stage boundaries (skip); 0 = error. */
int de_test_mtl_sample_timestamps(de_gpu_device *d, de_test_mtl_timestamps *out, char *err,
                                  size_t cap);

/** What a CAMetalLayer is set to. */
typedef struct de_test_mtl_layer {
    bool is_metal_layer, display_sync, framebuffer_only, device_set;
    u32 max_drawables;
    bool bgra8_srgb;
    double drawable_w, drawable_h;
} de_test_mtl_layer;
void de_test_mtl_layer_info(void *layer, de_gpu_device *d, de_test_mtl_layer *out);

#endif
