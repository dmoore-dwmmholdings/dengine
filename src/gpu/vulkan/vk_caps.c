/* vk_caps.c — what a Vulkan device supports, the Base-tier check and de_gpu_caps (DENG-283;
 * SPEC §3.3: Vulkan 1.3, descriptor indexing, buffer device address, sync2, dynamic rendering,
 * timeline semaphores; RT tier VK_KHR_acceleration_structure + VK_KHR_ray_query (+ the RT
 * pipeline for the path tracer); mesh shaders VK_EXT_mesh_shader).
 */
#include "gpu/vulkan/vk_internal.h"

#include <stdio.h>
#include <string.h>

static bool has_ext(const VkExtensionProperties *e, u32 n, const char *name) {
    for (u32 i = 0; i < n; ++i)
        if (strcmp(e[i].extensionName, name) == 0)
            return true;
    return false;
}

void de__vk_physical_chain_features(de__vk_physical *p) {
    const bool v12 = p->api_version >= VK_API_VERSION_1_2;
    const bool v13 = p->api_version >= VK_API_VERSION_1_3;
    void *next = NULL;
    /* Built back to front: each structure points at the one added before it. */
    p->fault.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
    p->rt_pipeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    p->ray_query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    p->accel.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    p->mesh.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    p->f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    p->f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    p->f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    p->f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    if (p->ext.device_fault) {
        p->fault.pNext = next;
        next = &p->fault;
    }
    if (p->ext.rt_pipeline) {
        p->rt_pipeline.pNext = next;
        next = &p->rt_pipeline;
    }
    if (p->ext.ray_query) {
        p->ray_query.pNext = next;
        next = &p->ray_query;
    }
    if (p->ext.accel) {
        p->accel.pNext = next;
        next = &p->accel;
    }
    if (p->ext.mesh_shader) {
        p->mesh.pNext = next;
        next = &p->mesh;
    }
    if (v13) {
        p->f13.pNext = next;
        next = &p->f13;
    }
    if (v12) {
        p->f12.pNext = next;
        next = &p->f12;
        p->f11.pNext = next;
        next = &p->f11;
    }
    p->f.pNext = next;
}

void de__vk_physical_query(const de__vk_fns *fns, const de_allocator *a, VkPhysicalDevice handle,
                           de__vk_physical *p) {
    memset(p, 0, sizeof *p);
    p->handle = handle;

    VkPhysicalDeviceProperties base;
    fns->vkGetPhysicalDeviceProperties(handle, &base);
    p->api_version = base.apiVersion;

    /* Extensions: a device lists 100-300 (260 bytes each), so they go on the heap. Without
     * memory the device simply looks like it has none of the optional ones. */
    u32 n = 0;
    fns->vkEnumerateDeviceExtensionProperties(handle, NULL, &n, NULL);
    const size_t bytes = (size_t)n * sizeof(VkExtensionProperties);
    VkExtensionProperties *ext =
        n ? (VkExtensionProperties *)DE_ALLOC(a, bytes, 8, DE_MEM_RENDER) : NULL;
    if (!ext)
        n = 0;
    if (n && fns->vkEnumerateDeviceExtensionProperties(handle, NULL, &n, ext) < 0)
        n = 0;
    p->ext.portability_subset = has_ext(ext, n, "VK_KHR_portability_subset");
    p->ext.mesh_shader = has_ext(ext, n, VK_EXT_MESH_SHADER_EXTENSION_NAME);
    p->ext.deferred_host_ops = has_ext(ext, n, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    p->ext.accel =
        has_ext(ext, n, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && p->ext.deferred_host_ops;
    p->ext.ray_query = p->ext.accel && has_ext(ext, n, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    p->ext.rt_pipeline =
        p->ext.accel && has_ext(ext, n, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    p->ext.device_fault = has_ext(ext, n, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    p->ext.swapchain = has_ext(ext, n, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (ext)
        DE_FREE(a, ext, bytes);

    /* Properties: the 1.1/1.2/1.3 structures are only valid on devices of that version. */
    p->props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p->p11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
    p->p12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    p->p13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES;
    p->mesh_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
    void *next = NULL;
    if (p->ext.mesh_shader) {
        p->mesh_props.pNext = next;
        next = &p->mesh_props;
    }
    if (p->api_version >= VK_API_VERSION_1_3) {
        p->p13.pNext = next;
        next = &p->p13;
    }
    if (p->api_version >= VK_API_VERSION_1_2) {
        p->p12.pNext = next;
        next = &p->p12;
        p->p11.pNext = next;
        next = &p->p11;
    }
    p->props.pNext = next;
    fns->vkGetPhysicalDeviceProperties2(handle, &p->props);

    de__vk_physical_chain_features(p);
    fns->vkGetPhysicalDeviceFeatures2(handle, &p->f);
    fns->vkGetPhysicalDeviceMemoryProperties(handle, &p->mem);
}

bool de__vk_base_tier_check(const de__vk_physical *p, char *reason, size_t cap) {
    if (cap)
        reason[0] = '\0';
    if (p->api_version < VK_API_VERSION_1_3) {
        snprintf(reason, cap, "needs Vulkan 1.3; the device has %u.%u",
                 VK_API_VERSION_MAJOR(p->api_version), VK_API_VERSION_MINOR(p->api_version));
        return false;
    }
#define DE__VK_REQUIRE(s, field)                                                                   \
    if (!p->s.field) {                                                                             \
        snprintf(reason, cap, "missing Vulkan feature %s", #field);                                \
        return false;                                                                              \
    }
    DE_VK_BASE_TIER_FEATURES(DE__VK_REQUIRE)
#undef DE__VK_REQUIRE
    const struct {
        u32 have, need;
        const char *name;
    } limits[] = {
        {p->p12.maxPerStageDescriptorUpdateAfterBindSampledImages, DE__VK_BINDLESS_TEXTURES,
         "maxPerStageDescriptorUpdateAfterBindSampledImages"},
        {p->p12.maxDescriptorSetUpdateAfterBindSampledImages, DE__VK_BINDLESS_TEXTURES,
         "maxDescriptorSetUpdateAfterBindSampledImages"},
        {p->p12.maxPerStageDescriptorUpdateAfterBindStorageImages, DE__VK_BINDLESS_STORAGE_TEXTURES,
         "maxPerStageDescriptorUpdateAfterBindStorageImages"},
        {p->p12.maxDescriptorSetUpdateAfterBindStorageImages, DE__VK_BINDLESS_STORAGE_TEXTURES,
         "maxDescriptorSetUpdateAfterBindStorageImages"},
        {p->p12.maxPerStageDescriptorUpdateAfterBindSamplers, DE__VK_BINDLESS_SAMPLERS,
         "maxPerStageDescriptorUpdateAfterBindSamplers"},
        {p->p12.maxDescriptorSetUpdateAfterBindSamplers, DE__VK_BINDLESS_SAMPLERS,
         "maxDescriptorSetUpdateAfterBindSamplers"},
        {p->p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers, DE__VK_BINDLESS_BUFFERS,
         "maxPerStageDescriptorUpdateAfterBindStorageBuffers"},
        {p->p12.maxDescriptorSetUpdateAfterBindStorageBuffers, DE__VK_BINDLESS_BUFFERS,
         "maxDescriptorSetUpdateAfterBindStorageBuffers"},
        {p->p12.maxPerStageUpdateAfterBindResources,
         DE__VK_BINDLESS_TEXTURES + DE__VK_BINDLESS_STORAGE_TEXTURES + DE__VK_BINDLESS_BUFFERS,
         "maxPerStageUpdateAfterBindResources"},
        {p->props.properties.limits.maxPushConstantsSize, DE_GPU_MAX_PUSH_CONSTANT_BYTES,
         "maxPushConstantsSize"},
    };
    for (size_t i = 0; i < sizeof limits / sizeof limits[0]; ++i)
        if (limits[i].have < limits[i].need) {
            snprintf(reason, cap, "limit %s is %u, below %u", limits[i].name, limits[i].have,
                     limits[i].need);
            return false;
        }
    return true;
}

static u64 round_double(double v) {
    return v <= 0.0 ? 0u : (u64)(v + 0.5);
}

void de__vk_fill_caps(const de__vk_device *vk, de_gpu_caps *caps) {
    const de__vk_physical *p = &vk->phys;
    const VkPhysicalDeviceProperties *pr = &p->props.properties;
    memset(caps, 0, sizeof *caps);
    caps->backend = DE_GPU_VULKAN;
    /* deviceName is 256 bytes, caps 128: copy and cut by hand (no -Wformat-truncation). */
    size_t n = 0;
    while (n + 1u < sizeof caps->device_name && pr->deviceName[n])
        ++n;
    memcpy(caps->device_name, pr->deviceName, n);
    caps->device_name[n] = '\0';
    caps->vendor_id = pr->vendorID;
    caps->device_id = pr->deviceID;

    /* Memory: a discrete GPU's device-local heaps are its VRAM, the others system memory it can
     * reach. Integrated and CPU devices share system memory: everything counts as shared. */
    const bool discrete = pr->deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    for (u32 i = 0; i < p->mem.memoryHeapCount; ++i) {
        const VkMemoryHeap *h = &p->mem.memoryHeaps[i];
        if (discrete && (h->flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
            caps->dedicated_vram += h->size;
        else
            caps->shared_memory += h->size;
    }

    if (vk->on.accel && (vk->on.ray_query || vk->on.rt_pipeline))
        caps->raytracing =
            pr->deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? DE_RT_API_ONLY : DE_RT_HARDWARE;
    else
        caps->raytracing = DE_RT_NONE;
    caps->ray_query = vk->on.ray_query;
    caps->rt_pipelines = vk->on.rt_pipeline;
    caps->opacity_micromaps = false;
    caps->mesh_shaders = vk->on.mesh_shader;
    if (vk->on.mesh_shader) {
        caps->mesh_max_output_vertices = p->mesh_props.maxMeshOutputVertices;
        caps->mesh_max_output_primitives = p->mesh_props.maxMeshOutputPrimitives;
    }
    caps->hdr_output = false; /* swapchain colour spaces: DENG-284 / S18 */
    caps->vrr = false;
    caps->enhanced_barriers = false;
    caps->async_compute = vk->queues[DE_QUEUE_COMPUTE].lock != vk->queues[DE_QUEUE_GRAPHICS].lock;

    if (p->api_version >= VK_API_VERSION_1_3 && p->f13.subgroupSizeControl &&
        p->p13.minSubgroupSize) {
        caps->wave_size_min = p->p13.minSubgroupSize;
        caps->wave_size_max = p->p13.maxSubgroupSize;
    } else {
        caps->wave_size_min = caps->wave_size_max = p->p11.subgroupSize;
    }

    caps->max_bindless_textures = vk->table_size[DE__VK_BINDING_SAMPLED_IMAGES];
    caps->max_bindless_storage_textures = vk->table_size[DE__VK_BINDING_STORAGE_IMAGES];
    caps->max_bindless_samplers = vk->table_size[DE__VK_BINDING_SAMPLERS];
    caps->max_bindless_buffers = vk->table_size[DE__VK_BINDING_STORAGE_BUFFERS];
    caps->max_bindless_accels = vk->on.accel ? vk->table_size[DE__VK_BINDING_ACCELS] : 0;
    u32 push = pr->limits.maxPushConstantsSize;
    caps->max_push_constant_bytes =
        push < DE_GPU_MAX_PUSH_CONSTANT_BYTES ? push : DE_GPU_MAX_PUSH_CONSTANT_BYTES;
    /* timestampPeriod is nanoseconds per tick. */
    caps->timestamp_frequency = pr->limits.timestampPeriod > 0.0f
                                    ? round_double(1e9 / (double)pr->limits.timestampPeriod)
                                    : 0u;
    caps->base_tier_ok = vk->base_ok;
    snprintf(caps->base_tier_reason, sizeof caps->base_tier_reason, "%s",
             vk->base_ok ? "" : vk->base_reason);
}
