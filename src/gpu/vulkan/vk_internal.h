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
    X(vkSetDebugUtilsObjectNameEXT)

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
    X(vkCmdFillBuffer)

/** Device-level, optional (extensions): NULL when absent or not enabled. */
#define DE_VK_DEVICE_FUNCS_OPT(X) X(vkGetDeviceFaultInfoEXT)

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

#endif
