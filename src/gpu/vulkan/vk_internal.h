/* vk_internal.h — the Vulkan RHI backend's shared declarations (DENG-283; SPEC §3.3, §12.1).
 *
 * The backend fills de__gpu_vulkan_vtable (vk_device.c) behind the de_gpu front-end
 * (gpu/gpu_backend.h). Files:
 *   vk_loader.c     opens libvulkan at run time (de_dylib_open) and fills de__vk_fns from the
 *                   entry-point lists below; no volk, no import library, no Vulkan SDK at build
 * time vk_device.c     instance, adapter choice, device, queues, per-queue timelines, the vtable
 *   vk_caps.c       feature/property queries, the Base-tier check, de_gpu_caps
 *   vk_memory.c     memory classes (one per memory type and linear/optimal kind), device heaps
 *   vk_resources.c  buffers, images and their views, samplers (placed and sub-allocated)
 *   vk_bindless.c   the global descriptor set (shaders/common/bindless.slang) and its writes
 * DENG-284 adds command lists, barriers, sync and the swapchain (vk_cmd.c, vk_barriers.c,
 * vk_sync.c, vk_swapchain.c); it appends to the entry-point lists and to this header.
 *
 * Conventions the other Vulkan files rely on:
 *   - Bindless descriptors (set 0): binding 0 sampled images in
 * VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, binding 1 storage images in VK_IMAGE_LAYOUT_GENERAL,
 * binding 2 samplers, binding 3 storage buffers. A texture must be in that layout when a shader
 * reads it through its index; the barrier code (DENG-284) moves SHADER_READ to
 * SHADER_READ_ONLY_OPTIMAL and SHADER_WRITE to GENERAL. Index 0 of each binding is a zeroed dummy
 * resource (images in GENERAL).
 *   - de__vk_device.pipeline_layout is the one pipeline layout of every pipeline: the bindless set
 *     plus a DE_GPU_MAX_PUSH_CONSTANT_BYTES push-constant range at offset 0 for all stages.
 *   - Images start in VK_IMAGE_LAYOUT_UNDEFINED; the caller's first barrier transitions them.
 */
#ifndef DE_GPU_VK_INTERNAL_H
#define DE_GPU_VK_INTERNAL_H
#include "gpu/gpu_backend.h"

#include <vulkan/vulkan.h>

/* ---------- Entry points ----------
 * Each list is X(name). vk_loader.c resolves them; a missing entry of a required list fails
 * device creation with DE_ERR_UNSUPPORTED naming it, a missing optional one stays NULL.
 * Later tasks append names to these lists (append-only). */

/** From the library (vkGetInstanceProcAddr with a NULL instance). */
#define DE_VK_GLOBAL_FUNCS(X)                                                                      \
    X(vkCreateInstance)                                                                            \
    X(vkEnumerateInstanceExtensionProperties)                                                      \
    X(vkEnumerateInstanceLayerProperties)

/** Instance-level, required. */
#define DE_VK_INSTANCE_FUNCS(X)                                                                    \
    X(vkDestroyInstance)                                                                           \
    X(vkEnumeratePhysicalDevices)                                                                  \
    X(vkGetPhysicalDeviceProperties)                                                               \
    X(vkGetPhysicalDeviceProperties2)                                                              \
    X(vkGetPhysicalDeviceFeatures2)                                                                \
    X(vkGetPhysicalDeviceMemoryProperties)                                                         \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                    \
    X(vkGetPhysicalDeviceFormatProperties)                                                         \
    X(vkGetPhysicalDeviceImageFormatProperties)                                                    \
    X(vkEnumerateDeviceExtensionProperties)                                                        \
    X(vkCreateDevice)                                                                              \
    X(vkGetDeviceProcAddr)

/** Instance-level, optional (extensions): NULL when absent. */
#define DE_VK_INSTANCE_FUNCS_OPT(X)                                                                \
    X(vkCreateDebugUtilsMessengerEXT)                                                              \
    X(vkDestroyDebugUtilsMessengerEXT)                                                             \
    X(vkSetDebugUtilsObjectNameEXT)                                                                \
    X(vkDestroySurfaceKHR)                                                                         \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                        \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                   \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                        \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)                                                   \
    X(vkCmdBeginDebugUtilsLabelEXT) /* DENG-326: markers (vk_debug.c) */                           \
    X(vkCmdEndDebugUtilsLabelEXT)

/** Device-level, required (Vulkan 1.3 core). */
#define DE_VK_DEVICE_FUNCS(X)                                                                      \
    X(vkDestroyDevice)                                                                             \
    X(vkGetDeviceQueue)                                                                            \
    X(vkDeviceWaitIdle)                                                                            \
    X(vkQueueSubmit2)                                                                              \
    X(vkQueueWaitIdle)                                                                             \
    X(vkAllocateMemory)                                                                            \
    X(vkFreeMemory)                                                                                \
    X(vkMapMemory)                                                                                 \
    X(vkCreateBuffer)                                                                              \
    X(vkDestroyBuffer)                                                                             \
    X(vkGetBufferMemoryRequirements)                                                               \
    X(vkBindBufferMemory)                                                                          \
    X(vkGetBufferDeviceAddress)                                                                    \
    X(vkCreateImage)                                                                               \
    X(vkDestroyImage)                                                                              \
    X(vkGetImageMemoryRequirements)                                                                \
    X(vkBindImageMemory)                                                                           \
    X(vkGetDeviceBufferMemoryRequirements)                                                         \
    X(vkGetDeviceImageMemoryRequirements)                                                          \
    X(vkCreateImageView)                                                                           \
    X(vkDestroyImageView)                                                                          \
    X(vkCreateSampler)                                                                             \
    X(vkDestroySampler)                                                                            \
    X(vkGetDescriptorSetLayoutSupport)                                                             \
    X(vkCreateDescriptorSetLayout)                                                                 \
    X(vkDestroyDescriptorSetLayout)                                                                \
    X(vkCreateDescriptorPool)                                                                      \
    X(vkDestroyDescriptorPool)                                                                     \
    X(vkAllocateDescriptorSets)                                                                    \
    X(vkUpdateDescriptorSets)                                                                      \
    X(vkCreatePipelineLayout)                                                                      \
    X(vkDestroyPipelineLayout)                                                                     \
    X(vkCreateSemaphore)                                                                           \
    X(vkDestroySemaphore)                                                                          \
    X(vkGetSemaphoreCounterValue)                                                                  \
    X(vkWaitSemaphores)                                                                            \
    X(vkCreateCommandPool)                                                                         \
    X(vkDestroyCommandPool)                                                                        \
    X(vkAllocateCommandBuffers)                                                                    \
    X(vkBeginCommandBuffer)                                                                        \
    X(vkEndCommandBuffer)                                                                          \
    X(vkCmdPipelineBarrier2)                                                                       \
    X(vkCmdClearColorImage)                                                                        \
    X(vkCmdFillBuffer)                                                                             \
    X(vkResetCommandPool)                                                                          \
    X(vkCmdBeginRendering)                                                                         \
    X(vkCmdEndRendering)                                                                           \
    X(vkCmdSetViewport)                                                                            \
    X(vkCmdSetScissor)                                                                             \
    X(vkCmdCopyBuffer)                                                                             \
    X(vkCmdCopyBufferToImage)                                                                      \
    X(vkCmdCopyImageToBuffer)                                                                      \
    X(vkCreateShaderModule)                                                                        \
    X(vkDestroyShaderModule)                                                                       \
    X(vkCreateGraphicsPipelines)                                                                   \
    X(vkCreateComputePipelines)                                                                    \
    X(vkDestroyPipeline)                                                                           \
    X(vkCreatePipelineCache)                                                                       \
    X(vkDestroyPipelineCache)                                                                      \
    X(vkGetPipelineCacheData)                                                                      \
    X(vkCmdBindPipeline)                                                                           \
    X(vkCmdBindDescriptorSets)                                                                     \
    X(vkCmdPushConstants)                                                                          \
    X(vkCmdDraw)                                                                                   \
    X(vkCmdDrawIndexed)                                                                            \
    X(vkCmdBindIndexBuffer)                                                                        \
    X(vkCmdDispatch)                                                                               \
    X(vkCmdDispatchIndirect)                                                                       \
    X(vkCmdDrawIndexedIndirect)                                                                    \
    X(vkCmdDrawIndexedIndirectCount)                                                               \
    X(vkCreateQueryPool) /* DENG-326: timestamps (vk_query.c) */                                   \
    X(vkDestroyQueryPool)                                                                          \
    X(vkCmdResetQueryPool)                                                                         \
    X(vkCmdWriteTimestamp2)                                                                        \
    X(vkCmdCopyQueryPoolResults)

/** Device-level, optional (extensions): NULL when absent or not enabled. */
#define DE_VK_DEVICE_FUNCS_OPT(X)                                                                  \
    X(vkGetDeviceFaultInfoEXT)                                                                     \
    X(vkCreateSwapchainKHR)                                                                        \
    X(vkDestroySwapchainKHR)                                                                       \
    X(vkGetSwapchainImagesKHR)                                                                     \
    X(vkAcquireNextImageKHR)                                                                       \
    X(vkQueuePresentKHR)                                                                           \
    X(vkCmdDrawMeshTasksEXT)                                                                       \
    X(vkCmdDrawMeshTasksIndirectEXT)                                                               \
    X(vkCmdDrawMeshTasksIndirectCountEXT)

/** Every entry point the backend calls, resolved by vk_loader.c. */
typedef struct de__vk_fns {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define DE__VK_FN_FIELD(name) PFN_##name name;
    DE_VK_GLOBAL_FUNCS(DE__VK_FN_FIELD)
    DE_VK_INSTANCE_FUNCS(DE__VK_FN_FIELD)
    DE_VK_INSTANCE_FUNCS_OPT(DE__VK_FN_FIELD)
    DE_VK_DEVICE_FUNCS(DE__VK_FN_FIELD)
    DE_VK_DEVICE_FUNCS_OPT(DE__VK_FN_FIELD)
#undef DE__VK_FN_FIELD
} de__vk_fns;

/* ---------- Physical device description (vk_caps.c) ---------- */

/**
 * What a physical device supports: properties, the feature chain and the extensions the backend
 * cares about. de__vk_physical_query fills it and links the pNext chains, so do not copy it by
 * value afterwards (the chains would point into the source).
 */
typedef struct de__vk_physical {
    VkPhysicalDevice handle;
    u32 api_version; /**< the device's Vulkan version (VK_MAKE_API_VERSION) */
    VkPhysicalDeviceProperties2 props;
    VkPhysicalDeviceVulkan11Properties p11;
    VkPhysicalDeviceVulkan12Properties p12;
    VkPhysicalDeviceVulkan13Properties p13;
    VkPhysicalDeviceMeshShaderPropertiesEXT mesh_props;
    VkPhysicalDeviceFeatures2 f;
    VkPhysicalDeviceVulkan11Features f11;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accel;
    VkPhysicalDeviceRayQueryFeaturesKHR ray_query;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt_pipeline;
    VkPhysicalDeviceFaultFeaturesEXT fault;
    VkPhysicalDeviceMemoryProperties mem;
    /** Device extensions present (vkEnumerateDeviceExtensionProperties). */
    struct {
        bool portability_subset; /**< VK_KHR_portability_subset: must be enabled (MoltenVK) */
        bool mesh_shader;        /**< VK_EXT_mesh_shader */
        bool accel;              /**< VK_KHR_acceleration_structure (+ deferred host ops) */
        bool ray_query;          /**< VK_KHR_ray_query */
        bool rt_pipeline;        /**< VK_KHR_ray_tracing_pipeline */
        bool device_fault;       /**< VK_EXT_device_fault */
        bool deferred_host_ops;  /**< VK_KHR_deferred_host_operations */
        bool swapchain;          /**< VK_KHR_swapchain */
    } ext;
} de__vk_physical;

/**
 * The SPEC §3.3 Base-tier feature requirements of the Vulkan backend: X(struct member of
 * de__vk_physical, feature field). de__vk_base_tier_check reports the first one missing by name.
 */
#define DE_VK_BASE_TIER_FEATURES(X)                                                                \
    X(f12, descriptorIndexing)                                                                     \
    X(f12, runtimeDescriptorArray)                                                                 \
    X(f12, descriptorBindingPartiallyBound)                                                        \
    X(f12, descriptorBindingSampledImageUpdateAfterBind)                                           \
    X(f12, descriptorBindingStorageImageUpdateAfterBind)                                           \
    X(f12, descriptorBindingStorageBufferUpdateAfterBind)                                          \
    X(f12, descriptorBindingUpdateUnusedWhilePending)                                              \
    X(f12, shaderSampledImageArrayNonUniformIndexing)                                              \
    X(f12, shaderStorageImageArrayNonUniformIndexing)                                              \
    X(f12, shaderStorageBufferArrayNonUniformIndexing)                                             \
    X(f12, bufferDeviceAddress)                                                                    \
    X(f12, timelineSemaphore)                                                                      \
    X(f13, synchronization2)                                                                       \
    X(f13, dynamicRendering)                                                                       \
    X(f13, maintenance4)

/* Bindless table sizes (descriptors per binding of set 0). The same numbers as the Metal argument
 * buffer (shaders/common/bindless.slang), so a scene that fits one backend fits all three. A device
 * whose update-after-bind limits are below them is not Base-tier capable. */
#define DE__VK_BINDLESS_TEXTURES         65536u
#define DE__VK_BINDLESS_STORAGE_TEXTURES 16384u
#define DE__VK_BINDLESS_SAMPLERS         2048u
#define DE__VK_BINDLESS_BUFFERS          65536u

/** Descriptor set 0 bindings (shaders/common/bindless.slang). */
enum {
    DE__VK_BINDING_SAMPLED_IMAGES = 0,
    DE__VK_BINDING_STORAGE_IMAGES = 1,
    DE__VK_BINDING_SAMPLERS = 2,
    DE__VK_BINDING_STORAGE_BUFFERS = 3,
    DE__VK_BINDING_COUNT = 4
};

/**
 * Fills `p` for `handle` (properties, features with their pNext chains, extensions, memory).
 * `fns` needs the instance-level entry points; `a` holds the extension list for the call.
 */
void de__vk_physical_query(const de__vk_fns *fns, const de_allocator *a, VkPhysicalDevice handle,
                           de__vk_physical *p);
/** Links the feature chain of `p` (f -> f11 -> f12 -> f13 -> extension structs that `p->ext` says
 * are present). de__vk_physical_query calls it; call it again on a zeroed copy to build an
 * enable-chain. */
void de__vk_physical_chain_features(de__vk_physical *p);
/**
 * The Base-tier check (SPEC §3.3, Vulkan column): Vulkan 1.3, every DE_VK_BASE_TIER_FEATURES
 * feature, and update-after-bind limits that hold the bindless tables. Returns true, or false with
 * the reason ("missing Vulkan feature dynamicRendering", ...) in `reason` (always terminated).
 * Pure: it reads only `p`.
 */
bool de__vk_base_tier_check(const de__vk_physical *p, char *reason, size_t cap);
/** Fills `caps` from the chosen device and what was enabled (vk_caps.c). */
struct de__vk_device;
void de__vk_fill_caps(const struct de__vk_device *vk, de_gpu_caps *caps);

/* ---------- The device payload ---------- */

/** Most swapchain acquires waiting for a graphics submit at once (one per live swapchain). */
#define DE__VK_MAX_PENDING_ACQUIRES 16u

typedef struct de__vk_queue {
    VkQueue queue;
    u32 family;
    u32 slot;             /**< queue index within its family */
    u32 lock;             /**< index into de__vk_device.queue_locks (shared when the VkQueue is) */
    VkSemaphore timeline; /**< the front-end's internal timeline of this de_gpu_queue */
} de__vk_queue;

/** A memory class registered with the heap manager (vk_memory.c). */
typedef struct de__vk_mem_class {
    u32 type;             /**< Vulkan memory type index */
    bool linear;          /**< buffers (true) or optimal-tiling images (false) */
    de_gpu_memory memory; /**< the de_gpu_memory it serves */
    VkMemoryPropertyFlags flags;
} de__vk_mem_class;

typedef struct de__vk_device {
    de__vk_fns fn;
    de_dylib *lib;
    char lib_name[256]; /**< the library that was opened */
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkDevice device;
    de__vk_physical phys; /**< the chosen device's support (pNext chains point into it) */
    /** Enabled features and extensions (a subset of `phys`). */
    struct {
        bool debug_utils, validation, portability;
        bool mesh_shader, accel, ray_query, rt_pipeline, device_fault, swapchain;
        bool sampler_anisotropy, bc, cube_array, storage_without_format;
    } on;
    bool forced;  /**< chosen through de_gpu_device_desc.adapter */
    bool base_ok; /**< de__vk_base_tier_check result */
    char base_reason[128];

    de__vk_queue queues[DE_QUEUE_COUNT_];
    de_mutex queue_locks[DE_QUEUE_COUNT_];
    u32 queue_lock_count;
    de_atomic_u32 lost; /**< device lost: every queue value counts as complete */

    de_atomic_u32 validation_errors, validation_warnings;

    /* Memory classes: index = heap-manager class id. */
    de__vk_mem_class classes[DE__GPU_MEM_CLASS_MAX];
    u32 class_count;

    /* Bindless (vk_bindless.c). */
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet set;
    VkPipelineLayout pipeline_layout;
    u32 table_size[DE__VK_BINDING_COUNT];
    VkDeviceSize max_storage_range;
    VkImage null_image;
    VkImageView null_sampled_view, null_storage_view;
    VkBuffer null_buffer;
    VkSampler null_sampler;
    VkDeviceMemory null_memory[2];
    /** Debug/Dev: the handle written at each index of each binding (test hook, rewrite checks). */
    u64 *shadow[DE__VK_BINDING_COUNT];

    de_gpu_device *owner;

    /* DENG-284: submission state, guarded by the queue's lock (queue_locks[queues[q].lock]). */
    /** Internal timeline value last handed to vkQueueSubmit2, per de_gpu queue. */
    u64 last_value[DE_QUEUE_COUNT_];
    /** Swapchain acquires (vk_swapchain.c) that the next graphics-queue submit waits on; guarded
     * by the graphics queue's lock. */
    struct de__vk_acquire *pending_acquire[DE__VK_MAX_PENDING_ACQUIRES];
    u32 pending_acquire_count;

    /* DENG-323: the pipeline cache (vk_pipeline_cache.c). */
    VkPipelineCache pipeline_cache;
    char pipeline_cache_path[512];   /**< the cache file; "" = no persistent cache */
    u32 pipeline_cache_state;        /**< de__vk_pipeline_cache_state */
    u64 pipeline_cache_loaded;       /**< Vulkan cache bytes loaded at device creation */
    u64 pipeline_cache_saved;        /**< Vulkan cache bytes last written (device destroy) */
    de_atomic_u32 pipelines_created; /**< since device creation: no new pipeline, no save */
    /** DENG-323: GPU-assisted validation is on (gpu_av), or was turned off for a portability
     * driver (gpu_av_off: MoltenVK cannot compile instrumented shaders; vk_device.c). */
    bool gpu_av, gpu_av_off;
    /** DENG-326: the test hook's capturing messenger (vk_debug.c), or NULL. */
    struct de__vk_debug_capture *capture;
} de__vk_device;

static inline de__vk_device *de__vk(de_gpu_device *d) {
    return (de__vk_device *)de__gpu_device_payload(d);
}

/* ---------- Object payloads ---------- */

typedef struct de__vk_buffer {
    VkBuffer buffer;
    de__gpu_alloc alloc;     /**< zero for a placed buffer (its region owns the memory) */
    VkDeviceAddress address; /**< buffer device address, or 0 */
} de__vk_buffer;

typedef struct de__vk_texture {
    VkImage image;
    VkFormat format;
    VkImageAspectFlags aspect;                  /**< every aspect of the format (depth | stencil) */
    VkImageView sampled_view;                   /**< whole texture, SAMPLED usage */
    VkImageView storage_views[DE_GPU_MAX_MIPS]; /**< one per mip, STORAGE usage */
    de__gpu_alloc alloc; /**< zero for placed and external (swapchain) images */
} de__vk_texture;

typedef struct de__vk_sampler {
    VkSampler sampler;
} de__vk_sampler;

typedef struct de__vk_heap {
    VkDeviceMemory memory;
} de__vk_heap;

/* ---------- Loader (vk_loader.c) ---------- */

/** Opens libvulkan (DE_VULKAN_LIBRARY, then the platform names) and resolves the global entry
 * points. DE_ERR_UNSUPPORTED naming every library tried when none loads. */
de_result de__vk_loader_open(de__vk_device *vk);
/** Resolves the instance-level entry points of vk->instance. */
de_result de__vk_loader_instance(de__vk_device *vk);
/** Resolves the device-level entry points of vk->device. */
de_result de__vk_loader_device(de__vk_device *vk);
/** Closes the library. Safe on a partly opened loader. */
void de__vk_loader_close(de__vk_device *vk);

/* ---------- Helpers ---------- */

/** "VK_ERROR_DEVICE_LOST" etc. */
const char *de__vk_result_name(VkResult r);
/** The de_result for a VkResult: out-of-memory, device lost, unsupported, else internal. */
de_result de__vk_result_code(VkResult r);
/** Fails with de__vk_result_code(vr) and "<message>: <VkResult name>", logged to `gpu`. */
de_result de__vk_fail(de_gpu_device *d, VkResult vr, const char *fmt, ...) DE_PRINTF_FMT(3, 4);
/** VkFormat of a de_gpu_format (VK_FORMAT_UNDEFINED for UNKNOWN). */
VkFormat de__vk_format(de_gpu_format f);
/** Every aspect of a format: DEPTH, DEPTH | STENCIL or COLOR. */
VkImageAspectFlags de__vk_format_aspect(de_gpu_format f);

/* ---------- Device-create stages and vtable entries ---------- */

/* vk_memory.c */
de_result de__vk_memory_init(de_gpu_device *d);
de_result de__vk_heap_create(de_gpu_device *d, de__gpu_heap *h, const de__gpu_mem_class_desc *cls);
void de__vk_heap_destroy(de_gpu_device *d, de__gpu_heap *h);
de_result de__vk_buffer_requirements(de_gpu_device *d, const de_gpu_buffer_desc *desc,
                                     de__gpu_mem_req *out);
de_result de__vk_texture_requirements(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                      de__gpu_mem_req *out);
/** The first class of `mask` that serves `memory` (classes are ordered by preference), or
 * DE__GPU_MEM_CLASS_MAX. */
u32 de__vk_pick_class(const de__vk_device *vk, u32 mask, de_gpu_memory memory);
/** Mask of the classes of kind `linear` serving `memory` whose memory type is in `type_bits`. */
u32 de__vk_class_mask(const de__vk_device *vk, bool linear, de_gpu_memory memory, u32 type_bits);

/* vk_resources.c */
/** VkBufferCreateInfo / VkImageCreateInfo for a desc (shared with the requirements queries). */
void de__vk_buffer_info(const de__vk_device *vk, const de_gpu_buffer_desc *desc,
                        VkBufferCreateInfo *out);
void de__vk_image_info(const de_gpu_texture_desc *desc, VkImageCreateInfo *out);
/** Checks that the device supports an image (format, usage, size, samples): DE_ERR_UNSUPPORTED. */
de_result de__vk_image_supported(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                 const VkImageCreateInfo *ci);
/** Creates the sampled and per-mip storage views of `t` (by its usage) for the image in its
 * payload. Also for wrapped (swapchain) images, before de__gpu_texture_wrap_finish. */
de_result de__vk_texture_create_views(de_gpu_device *d, de__gpu_texture *t);
de_result de__vk_buffer_create(de_gpu_device *d, de__gpu_buffer *b, const de_gpu_buffer_desc *desc);
de_result de__vk_buffer_create_placed(de_gpu_device *d, de__gpu_buffer *b,
                                      const de_gpu_buffer_desc *desc, de__gpu_heap *heap,
                                      u64 offset);
void de__vk_buffer_destroy(de_gpu_device *d, de__gpu_buffer *b);
de_result de__vk_texture_create(de_gpu_device *d, de__gpu_texture *t,
                                const de_gpu_texture_desc *desc);
de_result de__vk_texture_create_placed(de_gpu_device *d, de__gpu_texture *t,
                                       const de_gpu_texture_desc *desc, de__gpu_heap *heap,
                                       u64 offset);
void de__vk_texture_destroy(de_gpu_device *d, de__gpu_texture *t);
de_result de__vk_sampler_create(de_gpu_device *d, de__gpu_sampler *s,
                                const de_gpu_sampler_desc *desc);
void de__vk_sampler_destroy(de_gpu_device *d, de__gpu_sampler *s);

/* vk_bindless.c */
de_result de__vk_bindless_init(de_gpu_device *d);
void de__vk_bindless_shutdown(de_gpu_device *d);
void de__vk_bindless_write_buffer(de_gpu_device *d, u32 index, de__gpu_buffer *b);
void de__vk_bindless_write_texture(de_gpu_device *d, u32 index, de__gpu_texture *t, u32 mip,
                                   bool storage);
void de__vk_bindless_write_sampler(de_gpu_device *d, u32 index, de__gpu_sampler *s);
/* Points a destroyed texture's indices at the index-0 dummies (vk_bindless.c). */
void de__vk_bindless_clear_texture(de_gpu_device *d, const de__gpu_texture *t);

/* ---------- Test hooks ---------- */

/** Validation-layer messages counted since device creation (errors; warnings in `*warnings`). */
u32 de__gpu_vk_validation_messages(de_gpu_device *d, u32 *warnings);
/** True when VK_LAYER_KHRONOS_validation is active on the device. */
bool de__gpu_vk_validation_enabled(de_gpu_device *d);
/**
 * Debug/Dev builds: the Vulkan handle (VkImageView, VkSampler or VkBuffer, as u64) last written
 * at `index` of bindless space `space`; 0 when nothing was. Release builds keep no record and
 * return false.
 */
bool de__gpu_vk_bindless_written(de_gpu_device *d, de__gpu_bindless_space space, u32 index,
                                 u64 *handle);
/** The Vulkan handles behind objects (as u64), for tests: VkBuffer, VkImageView (sampled view, or
 * storage view of `mip`), VkSampler. 0 for another backend's device or a stale handle. */
u64 de__gpu_vk_buffer_handle(de_gpu_device *d, de_gpu_buffer b);
u64 de__gpu_vk_texture_view(de_gpu_device *d, de_gpu_texture t, u32 mip, bool storage);
u64 de__gpu_vk_sampler_handle(de_gpu_device *d, de_gpu_sampler s);

/* ---------- Command lists, barriers, sync, swapchain (DENG-284) ---------- */

/** A de_gpu_fence: a timeline semaphore. */
typedef struct de__vk_fence {
    VkSemaphore semaphore;
} de__vk_fence;

/** One swapchain acquire slot: the binary semaphore vkAcquireNextImageKHR signals, waited on by
 * the next graphics submit (or by present when nothing was submitted). */
typedef struct de__vk_acquire {
    VkSemaphore semaphore;
    u64 consumed_value; /**< graphics internal timeline value of the submit that waited on it */
    bool pending;       /**< signalled by an acquire, not yet waited on */
} de__vk_acquire;

#define DE__VK_MAX_SWAPCHAIN_IMAGES 8u

typedef struct de__vk_swapchain {
    u64 surface; /**< VkSurfaceKHR */
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkPresentModeKHR mode;
    VkExtent2D extent;
    u32 image_count;
    VkImage images[DE__VK_MAX_SWAPCHAIN_IMAGES];
    de_gpu_texture textures[DE__VK_MAX_SWAPCHAIN_IMAGES];
    /** Signalled by the timeline bridge submit, waited on by vkQueuePresentKHR (one per image). */
    VkSemaphore present[DE__VK_MAX_SWAPCHAIN_IMAGES];
    de__vk_acquire acquire[DE__VK_MAX_SWAPCHAIN_IMAGES + 1u];
    u32 acquire_next; /**< next acquire slot (ring) */
    u32 current;      /**< acquired image index, or UINT32_MAX */
    u32 current_slot; /**< acquire slot of the acquired image */
    bool suboptimal;  /**< the last acquire or present said VK_SUBOPTIMAL_KHR */
} de__vk_swapchain;

/** What one de_gpu_access bit (or a combination) means to Vulkan: the stages and accesses of that
 * use and the image layout it needs. */
typedef struct de__vk_access_info {
    VkPipelineStageFlags2 stages;
    VkAccessFlags2 access;
    VkImageLayout layout; /**< images only */
} de__vk_access_info;

/** Device facts the barrier translation needs (vk_barriers.c). Pure data, so tests build it. */
typedef struct de__vk_barrier_ctx {
    VkPipelineStageFlags2 queue_stages; /**< stages the recording queue's family supports */
    bool separate_depth_layouts;        /**< separateDepthStencilLayouts is enabled */
    bool accel, rt_pipeline, mesh;      /**< ray tracing / mesh shader stages exist */
    u32 cmd_family;                     /**< queue family the list is recorded for */
    u32 family[DE_QUEUE_COUNT_];        /**< queue family of each de_gpu queue */
    de_gpu_queue cmd_queue;             /**< queue the list is recorded for */
} de__vk_barrier_ctx;

/** One command list (de__gpu_cmd_payload): its own transient pool and primary command buffer. The
 * front-end reuses a list only after its submission completed, so cmd_begin resets the pool, and
 * a pool is only ever used by the one thread recording the list. */
typedef struct de__vk_cmd {
    VkCommandPool pool;
    VkCommandBuffer cb;
    /** Attachment views made by begin_render, destroyed when the list is reused or freed. */
    VkImageView *views;
    u32 view_count, view_cap;
    VkExtent2D render_extent;       /**< of the open render pass (viewport flip) */
    de__vk_barrier_ctx barrier_ctx; /**< made once at cmd_init (the queue never changes) */
    /** DENG-323 (vk_draw.c): the bindless set is bound at [0] graphics, [1] compute; cleared by
     * cmd_begin, so the set is bound once per command buffer and bind point. */
    bool set_bound[2];
} de__vk_cmd;

static inline de__vk_cmd *de__vk_cmd_of(de_gpu_cmd *c) {
    return (de__vk_cmd *)de__gpu_cmd_payload(c);
}

/** The texture facts a barrier needs (from the record and its payload). */
typedef struct de__vk_barrier_texture {
    VkImage image;
    VkImageAspectFlags aspect;
    bool depth, stencil;
    u32 mip_count, layers; /**< layers: 1 for 3D textures */
} de__vk_barrier_texture;

/**
 * The stages, accesses and layout of `access` (de_gpu_access bits) for a texture (`image`) or a
 * buffer, on a queue that supports `ctx->queue_stages`. Each bit comes from one table row; the
 * layout of several bits is their common layout, the read-only depth layout for depth reads with
 * shader reads, or GENERAL. `before` is the source side: NONE and PRESENT then wait for all prior
 * commands (ALL_COMMANDS with no access), so the barrier chains after a swapchain acquire wait.
 * Pure.
 */
de__vk_access_info de__vk_access_info_of(const de__vk_barrier_ctx *ctx, u32 access, bool image,
                                         bool depth, bool stencil, bool before);
/**
 * Translates one de_gpu_barrier: fills `*img` for a texture (`tex` non-NULL) or `*buf` for a
 * buffer (`buf_handle`), with the subresource range (0 counts = the rest) and, when the two
 * queues are in different families, the queue-family ownership transfer half that the recording
 * queue performs (release on src_queue, acquire on dst_queue). Pure.
 */
void de__vk_translate_barrier(const de__vk_barrier_ctx *ctx, const de_gpu_barrier *b,
                              const de__vk_barrier_texture *tex, VkBuffer buf_handle,
                              VkImageMemoryBarrier2 *img, VkBufferMemoryBarrier2 *buf);
/** The barrier context of a device for a list recorded on queue `q`. */
void de__vk_barrier_ctx_init(de_gpu_device *d, de_gpu_queue q, de__vk_barrier_ctx *ctx);
/** The layout of a depth attachment: DEPTH_ATTACHMENT_OPTIMAL for depth-only formats when
 * separate depth/stencil layouts are on, DEPTH_STENCIL_ATTACHMENT_OPTIMAL otherwise. */
VkImageLayout de__vk_depth_write_layout(bool separate_depth_layouts, bool stencil);

/* vk_barriers.c, vk_cmd.c */
void de__vk_cmd_barriers(de_gpu_cmd *c, const de_gpu_barrier *b, u32 count);
de_result de__vk_cmd_init(de_gpu_device *d, de_gpu_cmd *c);
void de__vk_cmd_fini(de_gpu_device *d, de_gpu_cmd *c);
de_result de__vk_cmd_begin(de_gpu_cmd *c, const char *name);
void de__vk_cmd_begin_render(de_gpu_cmd *c, const de_gpu_attachment *colors, u32 color_count,
                             const de_gpu_attachment *depth);
void de__vk_cmd_end_render(de_gpu_cmd *c);
void de__vk_cmd_viewport(de_gpu_cmd *c, f32 x, f32 y, f32 w, f32 h);
void de__vk_cmd_copy_buffer(de_gpu_cmd *c, de__gpu_buffer *dst, u64 dst_off, de__gpu_buffer *src,
                            u64 src_off, u64 bytes);
void de__vk_cmd_copy_buffer_to_texture(de_gpu_cmd *c, de__gpu_texture *dst, u32 mip, u32 layer,
                                       de__gpu_buffer *src, u64 src_off, u32 row_pitch);
void de__vk_cmd_copy_texture_to_buffer(de_gpu_cmd *c, de__gpu_buffer *dst, u64 dst_off,
                                       u32 row_pitch, de__gpu_texture *src, u32 mip, u32 layer);
/** The VkBufferImageCopy of one mip/layer, rows `row_pitch` bytes apart (BC: rows of blocks).
 * Pure. */
void de__vk_buffer_image_copy(const de__gpu_texture *t, VkImageAspectFlags aspect, u32 mip,
                              u32 layer, u64 offset, u32 row_pitch, VkBufferImageCopy *out);

/* vk_sync.c */
de_result de__vk_queue_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s,
                              u64 queue_value);
de_result de__vk_fence_create(de_gpu_device *d, de__gpu_fence *f, u64 initial);
void de__vk_fence_destroy(de_gpu_device *d, de__gpu_fence *f);
u64 de__vk_fence_value(de_gpu_device *d, de__gpu_fence *f);
de_result de__vk_fence_wait(de_gpu_device *d, de__gpu_fence *f, u64 value, u64 timeout_ns);
/** True once the device is lost (every queue value then counts as complete). */
bool de__vk_lost(de__vk_device *vk);
/** Waits until queue `q`'s internal timeline reaches `value` (no timeout; false when lost). */
bool de__vk_wait_queue_value(de__vk_device *vk, de_gpu_queue q, u64 value);
/** A binary semaphore, or VK_NULL_HANDLE with the error set. */
VkSemaphore de__vk_binary_semaphore(de_gpu_device *d, const char *what);

/* vk_swapchain.c */
de_result de__vk_swapchain_create(de_gpu_device *d, de__gpu_swapchain *s,
                                  const de_gpu_swapchain_desc *desc);
void de__vk_swapchain_destroy(de_gpu_device *d, de__gpu_swapchain *s);
de_result de__vk_swapchain_acquire(de_gpu_device *d, de__gpu_swapchain *s, de_gpu_texture *image);
de_result de__vk_swapchain_present(de_gpu_device *d, de__gpu_swapchain *s);
de_result de__vk_swapchain_resize(de_gpu_device *d, de__gpu_swapchain *s, u32 w, u32 h);
de_gpu_colorspace de__vk_swapchain_colorspace(de_gpu_device *d, de__gpu_swapchain *s);

/* ---------- Pipelines, draws, pipeline cache (DENG-323) ---------- */

/** A de_gpu_pipeline: the VkPipeline and its bind point. */
typedef struct de__vk_pipeline {
    VkPipeline pipeline;
    VkPipelineBindPoint bind_point;
} de__vk_pipeline;

/** What de__vk_spirv_scan reads from one entry point of a SPIR-V module. */
typedef struct de__vk_spirv_info {
    u32 model;               /**< SpvExecutionModel of the entry point */
    u32 local_size[3];       /**< LocalSize / LocalSizeId; 0 when not known (spec constants) */
    u32 push_constant_bytes; /**< end of the last member of its push-constant block; 0 = none */
    u32 output_locations;    /**< bit n: an Output variable at Location n (fragment outputs) */
    char entry[64];          /**< the entry point's name in the module (cut to 63 bytes) */
} de__vk_spirv_info;

/** SPIR-V execution models the backend uses (spirv.h values). */
enum {
    DE__VK_SPIRV_VERTEX = 0,
    DE__VK_SPIRV_FRAGMENT = 4,
    DE__VK_SPIRV_GLCOMPUTE = 5,
    DE__VK_SPIRV_TASK_EXT = 5364,
    DE__VK_SPIRV_MESH_EXT = 5365
};

/**
 * Reads entry point `entry` of a SPIR-V module (NULL or "" = the first one; a module with a
 * single entry point matches any name, as Slang names it "main"): execution model,
 * local size, push-constant bytes and output locations. `scratch` holds one u32 per SPIR-V id
 * (allocated from `a` and freed before returning). Pure apart from the allocation. DE_ERR_PARSE
 * with the reason in `why` for a module that is not SPIR-V, is truncated or lacks the entry point.
 */
de_result de__vk_spirv_scan(const void *code, u64 size, const char *entry, const de_allocator *a,
                            de__vk_spirv_info *out, char *why, size_t why_cap);

/* vk_pipeline.c */
de_result de__vk_pipeline_create(de_gpu_device *d, de__gpu_pipeline *p,
                                 const de_gpu_pipeline_desc *desc);
void de__vk_pipeline_destroy(de_gpu_device *d, de__gpu_pipeline *p);

/* vk_draw.c */
void de__vk_cmd_set_pipeline(de_gpu_cmd *c, de__gpu_pipeline *p);
void de__vk_cmd_push_constants(de_gpu_cmd *c, const void *data, u32 bytes);
void de__vk_cmd_draw(de_gpu_cmd *c, u32 vertex_count, u32 instance_count, u32 first_vertex,
                     u32 first_instance);
void de__vk_cmd_draw_indexed(de_gpu_cmd *c, u32 index_count, u32 instance_count, u32 first_index,
                             i32 vertex_offset, u32 first_instance);
void de__vk_cmd_set_index_buffer(de_gpu_cmd *c, de__gpu_buffer *b, u64 offset, bool index_u16);
void de__vk_cmd_scissor(de_gpu_cmd *c, i32 x, i32 y, u32 w, u32 h);
void de__vk_cmd_dispatch(de_gpu_cmd *c, u32 x, u32 y, u32 z);
void de__vk_cmd_dispatch_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset);
void de__vk_cmd_draw_indexed_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset,
                                      de__gpu_buffer *count, u64 count_offset, u32 max_draws);
void de__vk_cmd_dispatch_mesh(de_gpu_cmd *c, u32 x, u32 y, u32 z);
void de__vk_cmd_dispatch_mesh_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset,
                                       de__gpu_buffer *count, u64 count_offset, u32 max);
/** The scissor rectangle of (x, y, w, h) cut to x, y >= 0 (Vulkan needs non-negative offsets).
 * Pure. */
VkRect2D de__vk_scissor_rect(i32 x, i32 y, u32 w, u32 h);

/* vk_pipeline_cache.c */
typedef enum de__vk_pipeline_cache_state {
    DE__VK_PIPELINE_CACHE_OFF,      /**< no persistent cache ("" directory, or not set up) */
    DE__VK_PIPELINE_CACHE_EMPTY,    /**< no file yet: an empty cache */
    DE__VK_PIPELINE_CACHE_LOADED,   /**< the file was loaded */
    DE__VK_PIPELINE_CACHE_REJECTED, /**< the file was unreadable, corrupt or foreign: ignored */
} de__vk_pipeline_cache_state;
/** The file header in front of the Vulkan cache data (little-endian, 32 bytes). */
typedef struct de__vk_pipeline_cache_file {
    u32 magic;   /**< DE__VK_PIPELINE_CACHE_MAGIC */
    u32 version; /**< DE__VK_PIPELINE_CACHE_VERSION */
    u64 data_size;
    u64 data_hash; /**< de_hash64 of the data, seed DE__VK_PIPELINE_CACHE_MAGIC */
    u32 vendor_id, device_id;
} de__vk_pipeline_cache_file;
#define DE__VK_PIPELINE_CACHE_MAGIC   0x43505644u /* "DVPC" */
#define DE__VK_PIPELINE_CACHE_VERSION 1u
/** Creates vk->pipeline_cache at device creation, from the file in `dir` (NULL = the default
 * "cache:/pipelines", "" = none) when it is valid for this device. Fails only when Vulkan cannot
 * create an empty cache. */
de_result de__vk_pipeline_cache_init(de_gpu_device *d, const char *dir);
/** Saves the cache (atomically) and destroys it. Before vkDestroyDevice; safe when not created. */
void de__vk_pipeline_cache_shutdown(de_gpu_device *d);
/**
 * Checks a cache file for this device: the file header and checksum, then the Vulkan header
 * (VkPipelineCacheHeaderVersionOne: size, version, vendor, device, pipelineCacheUUID). Returns
 * the Vulkan data and its size, or NULL with the reason in `why`. Pure.
 */
const void *de__vk_pipeline_cache_check(const void *file, u64 size,
                                        const VkPhysicalDeviceProperties *props, u64 *data_size,
                                        char *why, size_t why_cap);

/** Test hooks: the cache state, the bytes loaded and saved, and the cache file path. */
u32 de__gpu_vk_pipeline_cache_state(de_gpu_device *d, u64 *loaded_bytes, const char **path);

/* ---------- Timestamps, markers, names (DENG-326) ---------- */

/** A de_gpu_query_heap: a VkQueryPool of timestamps. */
typedef struct de__vk_query_heap {
    VkQueryPool pool;
    /** timestampValidBits of each de_gpu queue's family (0: that queue cannot write them). */
    u32 valid_bits[DE_QUEUE_COUNT_];
} de__vk_query_heap;

/* vk_query.c */
de_result de__vk_query_heap_create(de_gpu_device *d, de__gpu_query_heap *h,
                                   const de_gpu_query_heap_desc *desc);
void de__vk_query_heap_destroy(de_gpu_device *d, de__gpu_query_heap *h);
void de__vk_cmd_timestamp(de_gpu_cmd *c, de__gpu_query_heap *h, u32 index);
void de__vk_cmd_resolve_queries(de_gpu_cmd *c, de__gpu_query_heap *h, u32 first, u32 count,
                                de__gpu_buffer *dst, u64 dst_off);

/* vk_debug.c */
void de__vk_cmd_marker_push(de_gpu_cmd *c, const char *name, u32 color);
void de__vk_cmd_marker_pop(de_gpu_cmd *c);
void de__vk_object_name(de_gpu_device *d, de__gpu_kind kind, void *rec, const char *name);
/** Names one Vulkan object through VK_EXT_debug_utils (nothing without it). */
void de__vk_set_name(de__vk_device *vk, VkObjectType type, u64 handle, const char *name);
/** The label colour of a 0xRRGGBB marker colour: all zero (the tool's default) for 0. Pure. */
void de__vk_label_color(u32 rgb, float out[4]);

/* Test hooks (vk_debug.c). */
/** Starts (`on`) or stops a second debug messenger that keeps the last validation error: its
 * message, its objects' names and the command-buffer labels open at it. False when the device has
 * no validation layer (or is not a Vulkan device). Stop it before destroying the device. */
bool de__gpu_vk_debug_capture(de_gpu_device *d, bool on);
/** Errors the capturing messenger saw; copies the last one's message, its objects' names
 * (";"-separated) and its command-buffer labels ("outer/inner"). */
u32 de__gpu_vk_debug_captured(de_gpu_device *d, char *message, size_t message_cap, char *objects,
                              size_t objects_cap, char *labels, size_t labels_cap);
/** The VkCommandBuffer of a list being recorded (as u64), 0 for another backend's list. */
u64 de__gpu_vk_cmd_buffer(de_gpu_cmd *c);

/* vk_fault.c (DENG-328): breadcrumbs, the device-lost poll and VK_EXT_device_fault. */
void de__vk_cmd_breadcrumb(de_gpu_cmd *c, de__gpu_buffer *ring, u64 offset, u32 value);
bool de__vk_device_lost(de_gpu_device *d);
size_t de__vk_device_lost_report(de_gpu_device *d, char *out, size_t cap);

#endif
