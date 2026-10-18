/* d3d12_caps.c — D3D12 feature support -> de_gpu_caps (DENG-285).
 *
 * A pure function over de__d3d12_features, so the unit test feeds it fake feature tables on every
 * build host (tests/unit/gpu/d3d12/test_d3d12_device.c). d3d12_device.c fills the table from
 * CheckFeatureSupport and the DXGI adapter description.
 */
#include "gpu/d3d12/d3d12_internal.h"

#include <stdio.h>
#include <string.h>

/* Appends "; <text>" (or "<text>" first) to the reason, cut to the buffer. */
static void add_reason(char *reason, size_t cap, const char *text) {
    size_t len = strlen(reason);
    if (len + 1 >= cap)
        return;
    snprintf(reason + len, cap - len, "%s%s", len ? "; " : "", text);
}

void de__d3d12_caps_evaluate(const de__d3d12_features *f, de_gpu_caps *caps) {
    memset(caps, 0, sizeof *caps);
    caps->backend = DE_GPU_D3D12;
    {
        size_t n = 0;
        while (n < sizeof caps->device_name - 1 && n < sizeof f->adapter_name && f->adapter_name[n])
            ++n;
        memcpy(caps->device_name, f->adapter_name, n);
        caps->device_name[n] = '\0';
    }
    caps->vendor_id = f->vendor_id;
    caps->device_id = f->device_id;
    caps->dedicated_vram = f->dedicated_vram;
    caps->shared_memory = f->shared_memory;

    /* Ray tracing: DXR 1.0 gives pipelines, 1.1 inline ray queries (the RT tier, SPEC §3.3), 1.2
     * opacity micromaps. WARP implements DXR in software. */
    if (f->raytracing_tier >= DE__D3D12_RAYTRACING_TIER_1_0) {
        caps->raytracing = f->software ? DE_RT_API_ONLY : DE_RT_HARDWARE;
        caps->rt_pipelines = f->shader_model >= DE__D3D12_SHADER_MODEL_6_6 &&
                             f->resource_binding_tier >= DE__D3D12_BINDING_TIER_3;
        if (caps->rt_pipelines) {
            caps->max_rt_recursion = 31;
            caps->max_rt_payload_bytes = 4096;
            caps->max_rt_attribute_bytes = 32;
        }
        caps->ray_query = f->raytracing_tier >= DE__D3D12_RAYTRACING_TIER_1_1;
        caps->opacity_micromaps = f->raytracing_tier >= DE__D3D12_RAYTRACING_TIER_1_2;
    }
    /* Mesh shaders are a shader model 6.5 feature. */
    caps->mesh_shaders = f->mesh_shader_tier >= DE__D3D12_MESH_SHADER_TIER_1 &&
                         f->shader_model >= DE__D3D12_SHADER_MODEL_6_5;
    if (caps->mesh_shaders) {
        caps->mesh_max_output_vertices = 256; /* D3D12 mesh shader limits */
        caps->mesh_max_output_primitives = 256;
    }
    caps->hdr_output = f->hdr_output;
    caps->vrr = f->tearing;
    caps->enhanced_barriers = f->enhanced_barriers;
    caps->async_compute = true; /* D3D12 always has a compute queue */
    caps->wave_size_min = f->wave_lane_min;
    caps->wave_size_max =
        f->wave_lane_max >= f->wave_lane_min ? f->wave_lane_max : f->wave_lane_min;

    caps->max_bindless_textures = DE__D3D12_BINDLESS_TEXTURES;
    caps->max_bindless_storage_textures = DE__D3D12_BINDLESS_STORAGE_TEXTURES;
    caps->max_bindless_buffers = DE__D3D12_BINDLESS_BUFFERS;
    caps->max_bindless_samplers = DE__D3D12_BINDLESS_SAMPLERS;
    caps->max_bindless_accels = DE__D3D12_BINDLESS_ACCELS;
    caps->max_push_constant_bytes = DE_GPU_MAX_PUSH_CONSTANT_BYTES;
    caps->timestamp_frequency = f->timestamp_frequency;

    /* Base tier (SPEC §3.3): SM 6.6 (ResourceDescriptorHeap indexing) and binding tier 3. */
    char reason[sizeof caps->base_tier_reason];
    reason[0] = '\0';
    char item[96];
    if (f->shader_model < DE__D3D12_SHADER_MODEL_6_6) {
        snprintf(item, sizeof item, "shader model %u.%u (needs 6.6)", (f->shader_model >> 4) & 0xfu,
                 f->shader_model & 0xfu);
        add_reason(reason, sizeof reason, item);
    }
    if (f->resource_binding_tier < DE__D3D12_BINDING_TIER_3) {
        snprintf(item, sizeof item, "resource binding tier %u (needs 3)", f->resource_binding_tier);
        add_reason(reason, sizeof reason, item);
    }
    caps->base_tier_ok = reason[0] == '\0';
    memcpy(caps->base_tier_reason, reason, sizeof reason);
}
