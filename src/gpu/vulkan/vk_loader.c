/* vk_loader.c — opens the Vulkan loader at run time and resolves the backend's entry points
 * (DENG-283). No volk and no import library: the engine builds without a Vulkan SDK, and a machine
 * without a Vulkan driver gets DE_ERR_UNSUPPORTED from de_gpu_device_create instead of failing to
 * start.
 *
 * Library search order: the DE_VULKAN_LIBRARY environment variable (a path or a library name; for
 * tests, or MoltenVK without the loader), then the platform names: vulkan-1.dll (Windows);
 * libvulkan.so.1, libvulkan.so (Linux); libvulkan.1.dylib, libvulkan.dylib, the Homebrew and
 * /usr/local loader, then libMoltenVK.dylib (macOS, where MoltenVK can also be loaded directly: it
 * exports vkGetInstanceProcAddr).
 */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#endif
#include "gpu/vulkan/vk_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_library_names[] = {
#if defined(_WIN32)
    "vulkan-1.dll",
#elif defined(__APPLE__)
    "libvulkan.1.dylib",
    "libvulkan.dylib",
    "/opt/homebrew/lib/libvulkan.1.dylib",
    "/usr/local/lib/libvulkan.1.dylib",
    "libMoltenVK.dylib",
#else
    "libvulkan.so.1",
    "libvulkan.so",
#endif
};

de_result de__vk_loader_open(de__vk_device *vk) {
    de_gpu_device *d = vk->owner;
    char tried[512];
    size_t used = 0;
    tried[0] = '\0';

    const char *env = getenv("DE_VULKAN_LIBRARY");
    const size_t n_names = sizeof k_library_names / sizeof k_library_names[0];
    for (size_t i = 0; i <= n_names && !vk->lib; ++i) {
        const char *name = i == 0 ? env : k_library_names[i - 1];
        if (!name || !name[0])
            continue;
        de_dylib *lib = NULL;
        if (de_dylib_open(name, &lib) == DE_OK) {
            vk->lib = lib;
            snprintf(vk->lib_name, sizeof vk->lib_name, "%s", name);
            break;
        }
        int w = snprintf(tried + used, sizeof tried - used, "%s%s", used ? ", " : "", name);
        if (w > 0 && (size_t)w < sizeof tried - used)
            used += (size_t)w;
    }
    de_error_clear(); /* the failed opens above are not this call's error */
    if (!vk->lib)
        return de__gpu_fail(
            d, DE_ERR_UNSUPPORTED,
            "vulkan: no Vulkan loader found (tried %s); install a Vulkan driver, or "
            "set DE_VULKAN_LIBRARY to the loader or ICD library",
            tried);

    vk->fn.vkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)de_dylib_function(vk->lib, "vkGetInstanceProcAddr");
    if (!vk->fn.vkGetInstanceProcAddr) {
        de_error_clear();
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "vulkan: '%s' does not export vkGetInstanceProcAddr", vk->lib_name);
    }
#define DE__VK_LOAD_GLOBAL(name)                                                                   \
    vk->fn.name = (PFN_##name)vk->fn.vkGetInstanceProcAddr(VK_NULL_HANDLE, #name);                 \
    if (!vk->fn.name)                                                                              \
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED, "vulkan: '%s' lacks %s", vk->lib_name, #name);
    DE_VK_GLOBAL_FUNCS(DE__VK_LOAD_GLOBAL)
#undef DE__VK_LOAD_GLOBAL
    return DE_OK;
}

de_result de__vk_loader_instance(de__vk_device *vk) {
    de_gpu_device *d = vk->owner;
#define DE__VK_LOAD_INSTANCE(name)                                                                 \
    vk->fn.name = (PFN_##name)vk->fn.vkGetInstanceProcAddr(vk->instance, #name);                   \
    if (!vk->fn.name)                                                                              \
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED, "vulkan: instance entry point %s is missing",   \
                            #name);
    DE_VK_INSTANCE_FUNCS(DE__VK_LOAD_INSTANCE)
#undef DE__VK_LOAD_INSTANCE
#define DE__VK_LOAD_INSTANCE_OPT(name)                                                             \
    vk->fn.name = (PFN_##name)vk->fn.vkGetInstanceProcAddr(vk->instance, #name);
    DE_VK_INSTANCE_FUNCS_OPT(DE__VK_LOAD_INSTANCE_OPT)
#undef DE__VK_LOAD_INSTANCE_OPT
    return DE_OK;
}

de_result de__vk_loader_device(de__vk_device *vk) {
    de_gpu_device *d = vk->owner;
#define DE__VK_LOAD_DEVICE(name)                                                                   \
    vk->fn.name = (PFN_##name)vk->fn.vkGetDeviceProcAddr(vk->device, #name);                       \
    if (!vk->fn.name)                                                                              \
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,                                                 \
                            "vulkan: device entry point %s is missing (the backend needs Vulkan "  \
                            "1.3)",                                                                \
                            #name);
    DE_VK_DEVICE_FUNCS(DE__VK_LOAD_DEVICE)
#undef DE__VK_LOAD_DEVICE
#define DE__VK_LOAD_DEVICE_OPT(name)                                                               \
    vk->fn.name = (PFN_##name)vk->fn.vkGetDeviceProcAddr(vk->device, #name);
    DE_VK_DEVICE_FUNCS_OPT(DE__VK_LOAD_DEVICE_OPT)
#undef DE__VK_LOAD_DEVICE_OPT
    return DE_OK;
}

void de__vk_loader_close(de__vk_device *vk) {
    if (vk->lib)
        de_dylib_close(vk->lib);
    vk->lib = NULL;
}
