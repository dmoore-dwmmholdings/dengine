/* mtl_caps.m — de_gpu_caps for a Metal device (DENG-287, SPEC §3.3).
 *
 * Base tier on macOS: Metal 3, Apple GPU family 7 (M1) and argument buffers tier 2. A device
 * below it still creates (de-caps reports why), but the engine does not render on it.
 * Ray tracing: HARDWARE on Apple family 9 (M3 and later), API_ONLY on other devices that support
 * the API (M1/M2 run it in software tiers), NONE otherwise.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "gpu/metal/mtl_internal.h"

#include <string.h>

/* Bounded copy that always terminates. */
static void copy_str(char *dst, size_t cap, const char *src) {
    size_t n = src ? strlen(src) : 0;
    if (n >= cap)
        n = cap - 1;
    if (n)
        memcpy(dst, src, n);
    dst[n] = '\0';
}

void de__mtl_caps_fill(void *device, de_gpu_caps *caps) {
    @autoreleasepool {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)device;
        memset(caps, 0, sizeof *caps);
        copy_str(caps->device_name, sizeof caps->device_name, dev.name.UTF8String);
        /* Apple's PCI vendor id. Metal has no device id; the low half of the IORegistry id keeps
         * two adapters of one machine apart. */
        caps->vendor_id = 0x106B;
        caps->device_id = (u32)(dev.registryID & 0xFFFFFFFFu);

        /* The working set Metal recommends is the memory the GPU can use well. On Apple silicon it
         * is part of the one unified pool, so nothing is reported as shared on top of it. */
        caps->unified_memory = dev.hasUnifiedMemory;
        caps->dedicated_vram = dev.recommendedMaxWorkingSetSize;
        caps->shared_memory = 0;

        const bool metal3 = [dev supportsFamily:MTLGPUFamilyMetal3];
        const bool apple7 = [dev supportsFamily:MTLGPUFamilyApple7];
        const bool apple9 = [dev supportsFamily:MTLGPUFamilyApple9];
        const bool ab_tier2 = dev.argumentBuffersSupport == MTLArgumentBuffersTier2;

        if (dev.supportsRaytracing) {
            caps->raytracing = apple9 ? DE_RT_HARDWARE : DE_RT_API_ONLY;
            caps->ray_query = true; /* intersection queries in compute (and render below) */
        } else {
            caps->raytracing = DE_RT_NONE;
        }
        /* Metal has no ray-tracing pipeline with a shader binding table, nor opacity
         * micromaps; the S16 path tracer maps onto compute + intersection functions. */
        caps->rt_pipelines = false;
        caps->opacity_micromaps = false;

        caps->mesh_shaders = metal3; /* object + mesh stages: Metal 3 on Apple7+ */
        caps->mesh_max_output_vertices = caps->mesh_shaders ? 256u : 0u;
        caps->mesh_max_output_primitives = caps->mesh_shaders ? 512u : 0u;

        /* EDR is the macOS HDR path (CAMetalLayer, DENG-288); whether the display shows it is a
         * swapchain question. VRR (ProMotion / adaptive sync) is driven by the display link. */
        caps->hdr_output = true;
        caps->vrr = false;
        caps->enhanced_barriers = false;
        caps->async_compute = true; /* separate graphics / compute / copy MTLCommandQueues */
        caps->wave_size_min = caps->wave_size_max = 32; /* Apple GPU SIMD-group width */

        caps->max_bindless_textures = DE__MTL_BINDLESS_TEXTURES;
        caps->max_bindless_storage_textures = DE__MTL_BINDLESS_STORAGE_TEXTURES;
        caps->max_bindless_buffers = DE__MTL_BINDLESS_BUFFERS;
        u64 samplers = dev.maxArgumentBufferSamplerCount;
        caps->max_bindless_samplers =
            samplers < DE__MTL_BINDLESS_SAMPLERS ? (u32)samplers : DE__MTL_BINDLESS_SAMPLERS;
        caps->max_bindless_accels = dev.supportsRaytracing ? DE__MTL_BINDLESS_ACCELS : 0; /* S16 */
        caps->max_push_constant_bytes = DE_GPU_MAX_PUSH_CONSTANT_BYTES; /* setBytes, index 1 */

        /* GPU timestamps (counter samples, sampleTimestamps:gpuTimestamp:) count nanoseconds,
         * not mach ticks: DENG-288 measured a 10.043 ms command buffer as 10,043,083 counter
         * ticks on an M5 Pro, and sampleTimestamps: gives its CPU time in ns too (mach ticks
         * times 125/3). gpu_metal.mtl.timestamp_frequency checks this on every run. */
        caps->timestamp_frequency = (u64)1000000000u;

        const char *why = NULL;
        if (!metal3)
            why = "the device does not support Metal 3 (MTLGPUFamilyMetal3)";
        else if (!apple7)
            why = "the device is not Apple GPU family 7 (M1) or later";
        else if (!ab_tier2)
            why = "the device lacks argument buffers tier 2";
        caps->base_tier_ok = why == NULL;
        copy_str(caps->base_tier_reason, sizeof caps->base_tier_reason, why);
    }
}
