/* vk_device.c — Vulkan instance, adapter choice, device, queues and the backend vtable
 * (DENG-283; SPEC §3.3, §12.1).
 *
 * Instance: Vulkan 1.3 application info; VK_EXT_debug_utils when present (names and markers in
 * capture tools); VK_LAYER_KHRONOS_validation and a messenger when de_gpu_device_desc.debug_layer
 * (gpu_validation adds GPU-assisted and synchronization validation); the surface extensions the
 * loader offers (VK_KHR_surface + win32/xlib/xcb/wayland/metal), so DENG-284 can present;
 * VK_KHR_portability_enumeration when offered (the macOS loader hides MoltenVK without it).
 *
 * Adapter: `adapter` 0 picks the best device: Base-tier capable first, then discrete > integrated
 * > virtual > CPU, then the most device-local memory. Device creation fails with
 * DE_ERR_UNSUPPORTED when the best device is not Base-tier capable; `adapter` n (1-based) forces
 * the n-th device anyway, with caps.base_tier_ok false and the reason in caps.base_tier_reason.
 * The device must still be Vulkan 1.3: the backend calls 1.3 core entry points.
 *
 * Queues: graphics from the first graphics family; compute from a compute family without
 * graphics, else another compute family, else a second queue of the graphics family, else shared;
 * copy likewise (a transfer-only family first). de_gpu queues that share one VkQueue share its
 * submit mutex. Each de_gpu queue has a timeline semaphore for the front-end's internal timeline.
 *
 * Submission: until DENG-284, queue_submit only signals the internal timeline (an empty
 * vkQueueSubmit2), so deferred destruction works; command lists (cmd_init) and fences are
 * DE_ERR_UNSUPPORTED. DENG-284 replaces those entries in the vtable below.
 */
#include "gpu/vulkan/vk_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------- Results ---------- */

const char *de__vk_result_name(VkResult r) {
    switch (r) {
#define DE__VK_RESULT_CASE(x)                                                                      \
    case x:                                                                                        \
        return #x;
        DE__VK_RESULT_CASE(VK_SUCCESS)
        DE__VK_RESULT_CASE(VK_NOT_READY)
        DE__VK_RESULT_CASE(VK_TIMEOUT)
        DE__VK_RESULT_CASE(VK_INCOMPLETE)
        DE__VK_RESULT_CASE(VK_ERROR_OUT_OF_HOST_MEMORY)
        DE__VK_RESULT_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        DE__VK_RESULT_CASE(VK_ERROR_INITIALIZATION_FAILED)
        DE__VK_RESULT_CASE(VK_ERROR_DEVICE_LOST)
        DE__VK_RESULT_CASE(VK_ERROR_MEMORY_MAP_FAILED)
        DE__VK_RESULT_CASE(VK_ERROR_LAYER_NOT_PRESENT)
        DE__VK_RESULT_CASE(VK_ERROR_EXTENSION_NOT_PRESENT)
        DE__VK_RESULT_CASE(VK_ERROR_FEATURE_NOT_PRESENT)
        DE__VK_RESULT_CASE(VK_ERROR_INCOMPATIBLE_DRIVER)
        DE__VK_RESULT_CASE(VK_ERROR_TOO_MANY_OBJECTS)
        DE__VK_RESULT_CASE(VK_ERROR_FORMAT_NOT_SUPPORTED)
        DE__VK_RESULT_CASE(VK_ERROR_FRAGMENTED_POOL)
        DE__VK_RESULT_CASE(VK_ERROR_OUT_OF_POOL_MEMORY)
        DE__VK_RESULT_CASE(VK_ERROR_FRAGMENTATION)
        DE__VK_RESULT_CASE(VK_ERROR_SURFACE_LOST_KHR)
        DE__VK_RESULT_CASE(VK_ERROR_OUT_OF_DATE_KHR)
        DE__VK_RESULT_CASE(VK_SUBOPTIMAL_KHR)
#undef DE__VK_RESULT_CASE
    default:
        return "VK_ERROR_UNKNOWN";
    }
}

de_result de__vk_result_code(VkResult r) {
    switch (r) {
    case VK_SUCCESS:
        return DE_OK;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    case VK_ERROR_OUT_OF_POOL_MEMORY:
    case VK_ERROR_FRAGMENTED_POOL:
    case VK_ERROR_FRAGMENTATION:
    case VK_ERROR_TOO_MANY_OBJECTS:
        return DE_ERR_OUT_OF_MEMORY;
    case VK_ERROR_DEVICE_LOST:
        return DE_ERR_DEVICE_LOST;
    case VK_ERROR_LAYER_NOT_PRESENT:
    case VK_ERROR_EXTENSION_NOT_PRESENT:
    case VK_ERROR_FEATURE_NOT_PRESENT:
    case VK_ERROR_INCOMPATIBLE_DRIVER:
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return DE_ERR_UNSUPPORTED;
    case VK_TIMEOUT:
        return DE_ERR_TIMEOUT;
    default:
        return DE_ERR_INTERNAL;
    }
}

de_result de__vk_fail(de_gpu_device *d, VkResult vr, const char *fmt, ...) {
    char msg[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    de_result code = de__vk_result_code(vr);
    if (code == DE_OK)
        code = DE_ERR_INTERNAL;
    if (vr == VK_ERROR_DEVICE_LOST)
        de_atomic_store_u32(&de__vk(d)->lost, 1u, DE_ATOMIC_RELEASE);
    return de__gpu_fail(d, code, "%s: %s", msg, de__vk_result_name(vr));
}

/* ---------- Debug messenger ---------- */

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)types;
    de_gpu_device *d = (de_gpu_device *)user;
    de__vk_device *vk = de__vk(d);
    const char *msg = data && data->pMessage ? data->pMessage : "(no message)";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        de_atomic_fetch_add_u32(&vk->validation_errors, 1u, DE_ATOMIC_RELAXED);
        DE_ERROR(d->log, "vulkan validation: %s", msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        de_atomic_fetch_add_u32(&vk->validation_warnings, 1u, DE_ATOMIC_RELAXED);
        DE_WARN(d->log, "vulkan validation: %s", msg);
    }
    return VK_FALSE; /* never abort the call */
}

static void messenger_info(de_gpu_device *d, VkDebugUtilsMessengerCreateInfoEXT *ci) {
    memset(ci, 0, sizeof *ci);
    ci->sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci->messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci->messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci->pfnUserCallback = debug_callback;
    ci->pUserData = d;
}

/* ---------- Instance ---------- */

#define MAX_NAMES 16

typedef struct name_list {
    const char *names[MAX_NAMES];
    u32 count;
} name_list;

static void add_name(name_list *l, const char *name) {
    if (l->count < MAX_NAMES)
        l->names[l->count++] = name;
}

static bool list_has(const VkExtensionProperties *e, u32 n, const char *name) {
    for (u32 i = 0; i < n; ++i)
        if (strcmp(e[i].extensionName, name) == 0)
            return true;
    return false;
}

static de_result create_instance(de_gpu_device *d, const de_gpu_device_desc *desc) {
    de__vk_device *vk = de__vk(d);
    const de_allocator *a = d->alloc;

    /* Instance extensions offered by the loader (and implicit layers). */
    u32 n = 0;
    vk->fn.vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    const size_t bytes = (size_t)n * sizeof(VkExtensionProperties);
    VkExtensionProperties *ext =
        n ? (VkExtensionProperties *)DE_ALLOC(a, bytes, 8, DE_MEM_RENDER) : NULL;
    if (n && !ext)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "vulkan: instance extension list");
    if (n && vk->fn.vkEnumerateInstanceExtensionProperties(NULL, &n, ext) < 0)
        n = 0;

    name_list exts = {{0}, 0};
    static const char *const k_surface[] = {
        "VK_KHR_surface",
        "VK_KHR_win32_surface",
        "VK_KHR_xlib_surface",
        "VK_KHR_xcb_surface",
        "VK_KHR_wayland_surface",
        "VK_EXT_metal_surface",
        "VK_EXT_swapchain_colorspace",
    };
    for (size_t i = 0; i < sizeof k_surface / sizeof k_surface[0]; ++i)
        if (list_has(ext, n, k_surface[i]))
            add_name(&exts, k_surface[i]);
    VkInstanceCreateFlags flags = 0;
    if (list_has(ext, n, "VK_KHR_portability_enumeration")) {
        add_name(&exts, "VK_KHR_portability_enumeration");
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        vk->on.portability = true;
    }
    if (list_has(ext, n, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        add_name(&exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        vk->on.debug_utils = true;
    }
    if (ext)
        DE_FREE(a, ext, bytes);

    /* Validation layer. */
    name_list layers = {{0}, 0};
    bool validation_features = false;
    if (desc->debug_layer || desc->gpu_validation) {
        u32 ln = 0;
        vk->fn.vkEnumerateInstanceLayerProperties(&ln, NULL);
        const size_t lbytes = (size_t)ln * sizeof(VkLayerProperties);
        VkLayerProperties *lp =
            ln ? (VkLayerProperties *)DE_ALLOC(a, lbytes, 8, DE_MEM_RENDER) : NULL;
        if (lp && vk->fn.vkEnumerateInstanceLayerProperties(&ln, lp) >= 0) {
            for (u32 i = 0; i < ln; ++i)
                if (strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation") == 0)
                    vk->on.validation = true;
        }
        if (lp)
            DE_FREE(a, lp, lbytes);
        if (vk->on.validation) {
            add_name(&layers, "VK_LAYER_KHRONOS_validation");
            if (desc->gpu_validation) {
                u32 vn = 0;
                vk->fn.vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &vn,
                                                              NULL);
                const size_t vbytes = (size_t)vn * sizeof(VkExtensionProperties);
                VkExtensionProperties *ve =
                    vn ? (VkExtensionProperties *)DE_ALLOC(a, vbytes, 8, DE_MEM_RENDER) : NULL;
                if (ve &&
                    vk->fn.vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation",
                                                                  &vn, ve) >= 0 &&
                    list_has(ve, vn, "VK_EXT_validation_features")) {
                    add_name(&exts, "VK_EXT_validation_features");
                    validation_features = true;
                }
                if (ve)
                    DE_FREE(a, ve, vbytes);
            }
        } else {
            DE_WARN(d->log, "vulkan: debug_layer requested, but VK_LAYER_KHRONOS_validation is "
                            "not installed; continuing without validation");
        }
    }

    VkApplicationInfo app;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "dengine";
    app.pEngineName = "dengine";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.flags = flags;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = exts.count;
    ci.ppEnabledExtensionNames = exts.names;
    ci.enabledLayerCount = layers.count;
    ci.ppEnabledLayerNames = layers.names;

    /* The messenger in the chain also reports vkCreateInstance / vkDestroyInstance problems. */
    VkDebugUtilsMessengerCreateInfoEXT mi;
    messenger_info(d, &mi);
    static const VkValidationFeatureEnableEXT k_vf[] = {
        VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT,
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
    };
    VkValidationFeaturesEXT vf;
    memset(&vf, 0, sizeof vf);
    vf.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    vf.enabledValidationFeatureCount = (u32)(sizeof k_vf / sizeof k_vf[0]);
    vf.pEnabledValidationFeatures = k_vf;
    const void *next = NULL;
    if (vk->on.validation && vk->on.debug_utils) {
        mi.pNext = next;
        next = &mi;
    }
    if (validation_features) {
        vf.pNext = next;
        next = &vf;
    }
    ci.pNext = next;

    VkResult vr = vk->fn.vkCreateInstance(&ci, NULL, &vk->instance);
    if (vr != VK_SUCCESS) {
        vk->instance = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: vkCreateInstance (Vulkan 1.3, %s)", vk->lib_name);
    }
    DE_TRY(de__vk_loader_instance(vk));
    if (vk->on.validation && vk->on.debug_utils && vk->fn.vkCreateDebugUtilsMessengerEXT) {
        messenger_info(d, &mi);
        vr = vk->fn.vkCreateDebugUtilsMessengerEXT(vk->instance, &mi, NULL, &vk->messenger);
        if (vr != VK_SUCCESS)
            vk->messenger = VK_NULL_HANDLE;
    }
    return DE_OK;
}

/* ---------- Physical device choice ---------- */

static u64 device_score(const de__vk_physical *p, bool base_ok) {
    u64 type_rank;
    switch (p->props.properties.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        type_rank = 4;
        break;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        type_rank = 3;
        break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        type_rank = 2;
        break;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        type_rank = 1;
        break;
    default:
        type_rank = 0;
        break;
    }
    u64 local_mib = 0;
    for (u32 i = 0; i < p->mem.memoryHeapCount; ++i)
        if (p->mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            local_mib += p->mem.memoryHeaps[i].size >> 20;
    if (local_mib >= ((u64)1 << 36))
        local_mib = ((u64)1 << 36) - 1;
    return ((base_ok ? (u64)1 : 0u) << 40) | (type_rank << 36) | local_mib;
}

#define MAX_PHYSICAL 16

static de_result pick_physical(de_gpu_device *d, const de_gpu_device_desc *desc) {
    de__vk_device *vk = de__vk(d);
    VkPhysicalDevice list[MAX_PHYSICAL];
    u32 count = MAX_PHYSICAL;
    VkResult vr = vk->fn.vkEnumeratePhysicalDevices(vk->instance, &count, list);
    if (vr < 0)
        return de__vk_fail(d, vr, "vulkan: vkEnumeratePhysicalDevices");
    if (count == 0)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "vulkan: the Vulkan loader (%s) lists no device; no Vulkan driver is "
                            "installed",
                            vk->lib_name);

    u32 chosen = 0;
    if (desc->adapter != 0) {
        if (desc->adapter > count)
            return de__gpu_fail(d, DE_ERR_NOT_FOUND,
                                "vulkan: adapter %u requested, but there %s only %u Vulkan "
                                "device%s",
                                desc->adapter, count == 1 ? "is" : "are", count,
                                count == 1 ? "" : "s");
        chosen = desc->adapter - 1u;
        vk->forced = true;
    } else {
        u64 best = 0;
        bool have = false;
        char reason[128];
        for (u32 i = 0; i < count; ++i) {
            de__vk_physical_query(&vk->fn, d->alloc, list[i], &vk->phys);
            const bool ok = de__vk_base_tier_check(&vk->phys, reason, sizeof reason);
            const u64 score = device_score(&vk->phys, ok);
            if (!have || score > best) {
                best = score;
                chosen = i;
                have = true;
            }
        }
    }
    de__vk_physical_query(&vk->fn, d->alloc, list[chosen], &vk->phys);
    vk->base_ok = de__vk_base_tier_check(&vk->phys, vk->base_reason, sizeof vk->base_reason);
    if (!vk->base_ok && !vk->forced)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "vulkan: no Vulkan device meets the Base tier; the best one, \"%s\", "
                            "%s (set de_gpu_device_desc.adapter to use it anyway)",
                            vk->phys.props.properties.deviceName, vk->base_reason);
    if (vk->phys.api_version < VK_API_VERSION_1_3)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED, "vulkan: \"%s\" %s",
                            vk->phys.props.properties.deviceName, vk->base_reason);
    if (!vk->base_ok)
        DE_WARN(d->log,
                "vulkan: adapter %u (\"%s\") is not Base-tier capable (%s); using it as "
                "requested",
                desc->adapter, vk->phys.props.properties.deviceName, vk->base_reason);
    return DE_OK;
}

/* ---------- Queues ---------- */

typedef struct queue_plan {
    u32 family[DE_QUEUE_COUNT_];
    u32 slot[DE_QUEUE_COUNT_];
    u32 used[32]; /* queues taken per family (index < 32) */
    u32 family_count;
} queue_plan;

static bool family_ok(const VkQueueFamilyProperties *f, VkQueueFlags want, VkQueueFlags avoid) {
    return f->queueCount > 0 && (f->queueFlags & want) == want && !(f->queueFlags & avoid);
}

/* A new queue of family `fam` if it has one left, else its last queue (shared). */
static void take(queue_plan *qp, const VkQueueFamilyProperties *props, u32 q, u32 fam) {
    qp->family[q] = fam;
    if (qp->used[fam] < props[fam].queueCount) {
        qp->slot[q] = qp->used[fam]++;
    } else {
        qp->slot[q] = qp->used[fam] - 1u;
    }
}

static de_result plan_queues(de_gpu_device *d, queue_plan *qp) {
    de__vk_device *vk = de__vk(d);
    VkQueueFamilyProperties props[32];
    u32 n = 32;
    vk->fn.vkGetPhysicalDeviceQueueFamilyProperties(vk->phys.handle, &n, props);
    memset(qp, 0, sizeof *qp);
    qp->family_count = n;
    const VkQueueFlags G = VK_QUEUE_GRAPHICS_BIT, C = VK_QUEUE_COMPUTE_BIT,
                       T = VK_QUEUE_TRANSFER_BIT;

    u32 gfx = n;
    for (u32 i = 0; i < n && gfx == n; ++i)
        if (family_ok(&props[i], G | C, 0))
            gfx = i;
    if (gfx == n)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "vulkan: \"%s\" has no queue family with graphics and compute",
                            vk->phys.props.properties.deviceName);
    take(qp, props, DE_QUEUE_GRAPHICS, gfx);

    /* Compute: dedicated family, else another family, else the graphics family. */
    u32 cmp = n;
    for (u32 i = 0; i < n && cmp == n; ++i)
        if (family_ok(&props[i], C, G))
            cmp = i;
    for (u32 i = 0; i < n && cmp == n; ++i)
        if (i != gfx && family_ok(&props[i], C, 0))
            cmp = i;
    take(qp, props, DE_QUEUE_COMPUTE, cmp == n ? gfx : cmp);

    /* Copy: transfer-only family, else a family nobody uses yet, else one with a free queue. */
    u32 cpy = n;
    for (u32 i = 0; i < n && cpy == n; ++i)
        if (family_ok(&props[i], T, G | C))
            cpy = i;
    for (u32 i = 0; i < n && cpy == n; ++i)
        if (qp->used[i] == 0 && props[i].queueCount > 0 && (props[i].queueFlags & (G | C | T)) != 0)
            cpy = i;
    for (u32 i = 0; i < n && cpy == n; ++i)
        if (qp->used[i] < props[i].queueCount && (props[i].queueFlags & (G | C | T)) != 0)
            cpy = i;
    take(qp, props, DE_QUEUE_COPY, cpy == n ? gfx : cpy);
    return DE_OK;
}

/* ---------- Device ---------- */

/* The features the backend enables: every Base-tier feature and the optional ones it uses, each
 * only if the device supports it (a forced device may lack Base-tier ones). */
static void wanted_features(const de__vk_physical *s, de__vk_physical *e) {
    memset(e, 0, sizeof *e);
    e->api_version = s->api_version;
    e->ext = s->ext;
#define W(st, field) e->st.field = s->st.field;
    W(f.features, samplerAnisotropy)
    W(f.features, textureCompressionBC)
    W(f.features, shaderStorageImageReadWithoutFormat)
    W(f.features, shaderStorageImageWriteWithoutFormat)
    W(f.features, shaderInt64)
    W(f.features, shaderInt16)
    W(f.features, fragmentStoresAndAtomics)
    W(f.features, vertexPipelineStoresAndAtomics)
    W(f.features, independentBlend)
    W(f.features, multiDrawIndirect)
    W(f.features, drawIndirectFirstInstance)
    W(f.features, imageCubeArray)
    W(f.features, depthClamp)
    W(f.features, depthBiasClamp)
    W(f.features, fillModeNonSolid)
    W(f.features, sampleRateShading)
    W(f.features, shaderImageGatherExtended)
    W(f.features, shaderClipDistance)
    W(f.features, fullDrawIndexUint32)
    W(f11, shaderDrawParameters)
    W(f11, storageBuffer16BitAccess)
#define W_BASE(st, field) W(st, field)
    DE_VK_BASE_TIER_FEATURES(W_BASE)
#undef W_BASE
    W(f12, drawIndirectCount)
    W(f12, scalarBlockLayout)
    W(f12, hostQueryReset)
    W(f12, shaderFloat16)
    W(f12, shaderInt8)
    W(f12, storageBuffer8BitAccess)
    W(f12, vulkanMemoryModel)
    W(f12, vulkanMemoryModelDeviceScope)
    W(f12, samplerFilterMinmax)
    W(f12, separateDepthStencilLayouts)
    W(f12, uniformBufferStandardLayout)
    W(f12, shaderSubgroupExtendedTypes)
    W(f13, subgroupSizeControl)
    W(f13, computeFullSubgroups)
    W(f13, shaderDemoteToHelperInvocation)
    W(f13, shaderTerminateInvocation)
    W(mesh, meshShader)
    W(mesh, taskShader)
    W(accel, accelerationStructure)
    W(ray_query, rayQuery)
    W(rt_pipeline, rayTracingPipeline)
    W(fault, deviceFault)
#undef W
    /* Mesh shading without task shaders is not useful to the renderer's amplification path. */
    if (!e->mesh.taskShader)
        e->mesh.meshShader = VK_FALSE;
    /* Ray tracing needs buffer device addresses. */
    if (!e->f12.bufferDeviceAddress) {
        e->accel.accelerationStructure = VK_FALSE;
        e->ray_query.rayQuery = VK_FALSE;
        e->rt_pipeline.rayTracingPipeline = VK_FALSE;
    }
    if (!e->accel.accelerationStructure) {
        e->ray_query.rayQuery = VK_FALSE;
        e->rt_pipeline.rayTracingPipeline = VK_FALSE;
    }
    e->ext.mesh_shader = s->ext.mesh_shader && e->mesh.meshShader;
    e->ext.accel = s->ext.accel && e->accel.accelerationStructure;
    e->ext.ray_query = e->ext.accel && s->ext.ray_query && e->ray_query.rayQuery;
    e->ext.rt_pipeline = e->ext.accel && s->ext.rt_pipeline && e->rt_pipeline.rayTracingPipeline;
    e->ext.device_fault = s->ext.device_fault && e->fault.deviceFault;
    de__vk_physical_chain_features(e);
}

static de_result create_device(de_gpu_device *d, const queue_plan *qp) {
    de__vk_device *vk = de__vk(d);
    de__vk_physical *en = (de__vk_physical *)DE_ALLOC(d->alloc, sizeof(de__vk_physical),
                                                      DE_ALIGNOF(de__vk_physical), DE_MEM_RENDER);
    if (!en)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "vulkan: device create info");
    wanted_features(&vk->phys, en);

    /* Queue create infos: one per family used, as many queues as were taken. */
    static const float k_priorities[DE_QUEUE_COUNT_] = {1.0f, 1.0f, 1.0f};
    VkDeviceQueueCreateInfo qci[DE_QUEUE_COUNT_];
    u32 qcount = 0;
    for (u32 fam = 0; fam < qp->family_count && fam < 32; ++fam) {
        if (qp->used[fam] == 0)
            continue;
        memset(&qci[qcount], 0, sizeof qci[qcount]);
        qci[qcount].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci[qcount].queueFamilyIndex = fam;
        qci[qcount].queueCount = qp->used[fam];
        qci[qcount].pQueuePriorities = k_priorities;
        ++qcount;
    }

    name_list exts = {{0}, 0};
    if (vk->phys.ext.portability_subset)
        add_name(&exts, "VK_KHR_portability_subset");
    if (vk->phys.ext.swapchain) {
        add_name(&exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        vk->on.swapchain = true;
    }
    if (en->ext.mesh_shader)
        add_name(&exts, VK_EXT_MESH_SHADER_EXTENSION_NAME);
    if (en->ext.accel) {
        add_name(&exts, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        add_name(&exts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    }
    if (en->ext.ray_query)
        add_name(&exts, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    if (en->ext.rt_pipeline)
        add_name(&exts, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    if (en->ext.device_fault)
        add_name(&exts, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);

    VkDeviceCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext = &en->f; /* features come through VkPhysicalDeviceFeatures2 */
    ci.queueCreateInfoCount = qcount;
    ci.pQueueCreateInfos = qci;
    ci.enabledExtensionCount = exts.count;
    ci.ppEnabledExtensionNames = exts.names;
    VkResult vr = vk->fn.vkCreateDevice(vk->phys.handle, &ci, NULL, &vk->device);

    vk->on.mesh_shader = en->ext.mesh_shader;
    vk->on.accel = en->ext.accel;
    vk->on.ray_query = en->ext.ray_query;
    vk->on.rt_pipeline = en->ext.rt_pipeline;
    vk->on.device_fault = en->ext.device_fault;
    vk->on.sampler_anisotropy = en->f.features.samplerAnisotropy != VK_FALSE;
    vk->on.bc = en->f.features.textureCompressionBC != VK_FALSE;
    vk->on.cube_array = en->f.features.imageCubeArray != VK_FALSE;
    vk->on.storage_without_format = en->f.features.shaderStorageImageReadWithoutFormat &&
                                    en->f.features.shaderStorageImageWriteWithoutFormat;
    DE_FREE(d->alloc, en, sizeof(de__vk_physical));
    if (vr != VK_SUCCESS) {
        vk->device = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: vkCreateDevice (\"%s\")",
                           vk->phys.props.properties.deviceName);
    }
    return de__vk_loader_device(vk);
}

static de_result setup_queues(de_gpu_device *d, const queue_plan *qp) {
    de__vk_device *vk = de__vk(d);
    vk->queue_lock_count = 0;
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        de__vk_queue *vq = &vk->queues[q];
        vq->family = qp->family[q];
        vq->slot = qp->slot[q];
        vk->fn.vkGetDeviceQueue(vk->device, vq->family, vq->slot, &vq->queue);
        vq->lock = vk->queue_lock_count;
        for (u32 p = 0; p < q; ++p)
            if (vk->queues[p].family == vq->family && vk->queues[p].slot == vq->slot)
                vq->lock = vk->queues[p].lock;
        if (vq->lock == vk->queue_lock_count)
            ++vk->queue_lock_count;

        VkSemaphoreTypeCreateInfo ti;
        memset(&ti, 0, sizeof ti);
        ti.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        ti.initialValue = 0;
        VkSemaphoreCreateInfo si;
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        si.pNext = &ti;
        VkResult vr = vk->fn.vkCreateSemaphore(vk->device, &si, NULL, &vq->timeline);
        if (vr != VK_SUCCESS) {
            vq->timeline = VK_NULL_HANDLE;
            return de__vk_fail(d, vr, "vulkan: timeline semaphore of queue %u", q);
        }
    }
    return DE_OK;
}

static bool is_lost(de__vk_device *vk) {
    return de_atomic_load_u32(&vk->lost, DE_ATOMIC_ACQUIRE) != 0;
}

/* Releases everything device_create made; safe on a partly created device. */
static void teardown(de_gpu_device *d) {
    de__vk_device *vk = de__vk(d);
    /* Entry points may be missing when the loader stage itself failed: check each one. */
    if (vk->device) {
        if (vk->fn.vkDeviceWaitIdle && !is_lost(vk))
            vk->fn.vkDeviceWaitIdle(vk->device);
        de__vk_bindless_shutdown(d);
        for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
            if (vk->queues[q].timeline && vk->fn.vkDestroySemaphore)
                vk->fn.vkDestroySemaphore(vk->device, vk->queues[q].timeline, NULL);
        if (vk->fn.vkDestroyDevice)
            vk->fn.vkDestroyDevice(vk->device, NULL);
        vk->device = VK_NULL_HANDLE;
    }
    if (vk->messenger && vk->fn.vkDestroyDebugUtilsMessengerEXT)
        vk->fn.vkDestroyDebugUtilsMessengerEXT(vk->instance, vk->messenger, NULL);
    vk->messenger = VK_NULL_HANDLE;
    if (vk->instance && vk->fn.vkDestroyInstance)
        vk->fn.vkDestroyInstance(vk->instance, NULL);
    vk->instance = VK_NULL_HANDLE;
    de__vk_loader_close(vk);
    for (u32 i = 0; i < DE_QUEUE_COUNT_; ++i)
        de_mutex_destroy(&vk->queue_locks[i]);
}

static de_result vk_device_create(de_gpu_device *d, const de_gpu_device_desc *desc,
                                  de_gpu_caps *caps) {
    de__vk_device *vk = de__vk(d);
    vk->owner = d;
    for (u32 i = 0; i < DE_QUEUE_COUNT_; ++i)
        de_mutex_init(&vk->queue_locks[i]);
    de_atomic_init_u32(&vk->lost, 0);
    de_atomic_init_u32(&vk->validation_errors, 0);
    de_atomic_init_u32(&vk->validation_warnings, 0);

    queue_plan qp;
    de_result r = de__vk_loader_open(vk);
    if (r == DE_OK)
        r = create_instance(d, desc);
    if (r == DE_OK)
        r = pick_physical(d, desc);
    if (r == DE_OK)
        r = plan_queues(d, &qp);
    if (r == DE_OK)
        r = create_device(d, &qp);
    if (r == DE_OK)
        r = setup_queues(d, &qp);
    if (r == DE_OK)
        r = de__vk_memory_init(d);
    if (r == DE_OK)
        r = de__vk_bindless_init(d);
    if (r != DE_OK) {
        teardown(d);
        return r;
    }
    de__vk_fill_caps(vk, caps);
    DE_INFO(d->log,
            "vulkan: \"%s\" (Vulkan %u.%u.%u, driver 0x%x) via %s; queues: graphics %u.%u, "
            "compute %u.%u, copy %u.%u; validation %s",
            caps->device_name, VK_API_VERSION_MAJOR(vk->phys.api_version),
            VK_API_VERSION_MINOR(vk->phys.api_version), VK_API_VERSION_PATCH(vk->phys.api_version),
            vk->phys.props.properties.driverVersion, vk->lib_name,
            vk->queues[DE_QUEUE_GRAPHICS].family, vk->queues[DE_QUEUE_GRAPHICS].slot,
            vk->queues[DE_QUEUE_COMPUTE].family, vk->queues[DE_QUEUE_COMPUTE].slot,
            vk->queues[DE_QUEUE_COPY].family, vk->queues[DE_QUEUE_COPY].slot,
            vk->on.validation ? "on" : "off");
    return DE_OK;
}

static void vk_device_destroy(de_gpu_device *d) {
    teardown(d);
}

/* ---------- Internal timelines (placeholder submission until DENG-284) ---------- */

static void vk_wait_idle(de_gpu_device *d) {
    de__vk_device *vk = de__vk(d);
    if (is_lost(vk))
        return;
    /* vkDeviceWaitIdle needs every queue externally synchronised. */
    for (u32 i = 0; i < vk->queue_lock_count; ++i)
        de_mutex_lock(&vk->queue_locks[i]);
    VkResult vr = vk->fn.vkDeviceWaitIdle(vk->device);
    for (u32 i = vk->queue_lock_count; i-- > 0;)
        de_mutex_unlock(&vk->queue_locks[i]);
    if (vr == VK_ERROR_DEVICE_LOST)
        de_atomic_store_u32(&vk->lost, 1u, DE_ATOMIC_RELEASE);
}

static de_result vk_queue_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s,
                                 u64 queue_value) {
    de__vk_device *vk = de__vk(d);
    de__vk_queue *vq = &vk->queues[q];
    /* Signal the internal timeline first in every case: the front-end has published the value. */
    VkResult vr = VK_ERROR_DEVICE_LOST;
    if (!is_lost(vk)) {
        VkSemaphoreSubmitInfo sig;
        memset(&sig, 0, sizeof sig);
        sig.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        sig.semaphore = vq->timeline;
        sig.value = queue_value;
        sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 si;
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &sig;
        de_mutex_lock(&vk->queue_locks[vq->lock]);
        vr = vk->fn.vkQueueSubmit2(vq->queue, 1, &si, VK_NULL_HANDLE);
        de_mutex_unlock(&vk->queue_locks[vq->lock]);
    }
    if (vr != VK_SUCCESS) {
        /* The value can no longer be signalled in order: report every value as complete. */
        de_atomic_store_u32(&vk->lost, 1u, DE_ATOMIC_RELEASE);
        return de__vk_fail(d, vr, "vulkan: vkQueueSubmit2 on the %s queue",
                           q == DE_QUEUE_GRAPHICS  ? "graphics"
                           : q == DE_QUEUE_COMPUTE ? "compute"
                                                   : "copy");
    }
    if (s->cmd_count || s->wait_count || s->signal.id)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "de_gpu_queue_submit: the Vulkan backend has no command lists or "
                            "fences yet (DENG-284)");
    return DE_OK;
}

static u64 vk_queue_completed(de_gpu_device *d, de_gpu_queue q) {
    de__vk_device *vk = de__vk(d);
    if (is_lost(vk))
        return UINT64_MAX;
    u64 value = 0;
    VkResult vr = vk->fn.vkGetSemaphoreCounterValue(vk->device, vk->queues[q].timeline, &value);
    if (vr != VK_SUCCESS) {
        de_atomic_store_u32(&vk->lost, 1u, DE_ATOMIC_RELEASE);
        return UINT64_MAX;
    }
    return value;
}

static de_result vk_cmd_init(de_gpu_device *d, de_gpu_cmd *c) {
    (void)c;
    return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                        "de_gpu_cmd_begin: the Vulkan backend has no command lists yet (DENG-284)");
}

/* ---------- Test hooks ---------- */

u32 de__gpu_vk_validation_messages(de_gpu_device *d, u32 *warnings) {
    const bool ok = d && d->vt == &de__gpu_vulkan_vtable;
    if (warnings)
        *warnings =
            ok ? de_atomic_load_u32(&de__vk(d)->validation_warnings, DE_ATOMIC_RELAXED) : 0u;
    return ok ? de_atomic_load_u32(&de__vk(d)->validation_errors, DE_ATOMIC_RELAXED) : 0u;
}

bool de__gpu_vk_validation_enabled(de_gpu_device *d) {
    return d && d->vt == &de__gpu_vulkan_vtable && de__vk(d)->on.validation;
}

/* ---------- The vtable ---------- */

const de__gpu_backend_vtable de__gpu_vulkan_vtable = {
    .name = "vulkan",
    .device_payload_size = sizeof(de__vk_device),
    .cmd_payload_size = 0,
    .payload_size =
        {
            [DE__GPU_BUFFER] = sizeof(de__vk_buffer),
            [DE__GPU_TEXTURE] = sizeof(de__vk_texture),
            [DE__GPU_SAMPLER] = sizeof(de__vk_sampler),
        },
    .device_create = vk_device_create,
    .device_destroy = vk_device_destroy,
    .wait_idle = vk_wait_idle,
    .buffer_create = de__vk_buffer_create,
    .buffer_destroy = de__vk_buffer_destroy,
    .texture_create = de__vk_texture_create,
    .texture_destroy = de__vk_texture_destroy,
    .sampler_create = de__vk_sampler_create,
    .sampler_destroy = de__vk_sampler_destroy,
    .bindless_write_buffer = de__vk_bindless_write_buffer,
    .bindless_write_texture = de__vk_bindless_write_texture,
    .bindless_write_sampler = de__vk_bindless_write_sampler,
    .cmd_init = vk_cmd_init,
    .queue_submit = vk_queue_submit,
    .queue_completed = vk_queue_completed,
    .heap_payload_size = sizeof(de__vk_heap),
    .heap_create = de__vk_heap_create,
    .heap_destroy = de__vk_heap_destroy,
    .buffer_requirements = de__vk_buffer_requirements,
    .texture_requirements = de__vk_texture_requirements,
    .buffer_create_placed = de__vk_buffer_create_placed,
    .texture_create_placed = de__vk_texture_create_placed,
};
