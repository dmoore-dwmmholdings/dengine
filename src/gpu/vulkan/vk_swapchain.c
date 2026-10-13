/* vk_swapchain.c — Vulkan swapchains: surface, VkSwapchainKHR, acquire, present, resize
 * (DENG-284; SPEC §6, §12.1).
 *
 * Surface: from the platform layer (S03, de_window_vulkan_create_surface on a window created with
 * DE_WINDOW_SURFACE_VULKAN). The platform makes it through SDL's Vulkan loader, the device was
 * made through vk_loader.c's: both must be the same library (DE_VULKAN_LIBRARY and
 * SDL_VULKAN_LIBRARY name the same file, or neither is set), or the instance handle means nothing
 * to the surface code; swapchain_create checks this and refuses with DE_ERR_UNSUPPORTED.
 *
 * Swapchain: sRGB only (B8G8R8A8_SRGB or R8G8B8A8_SRGB, SRGB_NONLINEAR; HDR colour spaces are
 * S18 and refused). Image count: the desc's, else 3, clamped to the surface's limits and to
 * DE__VK_MAX_SWAPCHAIN_IMAGES. Present modes: VSYNC = FIFO; MAILBOX = MAILBOX, else FIFO;
 * IMMEDIATE = IMMEDIATE, else MAILBOX, else FIFO. `allow_vrr` is logged: variable refresh is up
 * to the driver and compositor with these modes. Extent: the surface's current extent, or the
 * window's pixel size (resize: the size given) clamped to the surface's limits. Images are
 * wrapped as de_gpu textures (RENDER_TARGET | COPY_SRC | COPY_DST) with de__gpu_texture_wrap.
 *
 * Acquire and present: binary semaphores bridged to the graphics timeline.
 *   - acquire: vkAcquireNextImageKHR signals the next semaphore of a ring of image_count + 1
 *     slots (a slot is reused once the submit that waited on it completed). The slot goes on the
 *     device's pending list; the next graphics-queue submit waits on it (vk_sync.c).
 *   - present: one bridge vkQueueSubmit2 on the graphics queue waits on the graphics internal
 *     timeline at the value last submitted (and on the acquire, if no submit took it) and
 *     signals the image's present semaphore; vkQueuePresentKHR waits on that. So present needs
 *     no fence: it follows every graphics submit made before it.
 *   - VK_ERROR_OUT_OF_DATE_KHR from acquire or present, and VK_SUBOPTIMAL_KHR, give DE_ERR_BUSY:
 *     the caller resizes (de_gpu_swapchain_resize) with the window's pixel size. A suboptimal
 *     acquire still hands out its image.
 * Resize and destroy wait for the device to idle first, so no image or semaphore is in use.
 */
#include "gpu/vulkan/vk_internal.h"

#include <stdio.h>
#include <string.h>

#define NO_IMAGE UINT32_MAX

static de__vk_swapchain *payload(de__gpu_swapchain *s) {
    return DE__GPU_PAYLOAD(de__vk_swapchain, s);
}

static de_mutex *gfx_lock(de__vk_device *vk) {
    return &vk->queue_locks[vk->queues[DE_QUEUE_GRAPHICS].lock];
}

/* Removes `vs`'s acquire slots from the device's pending list (graphics lock held). */
static void unlist_acquires(de__vk_device *vk, de__vk_swapchain *vs) {
    u32 n = 0;
    for (u32 i = 0; i < vk->pending_acquire_count; ++i) {
        de__vk_acquire *a = vk->pending_acquire[i];
        if (a >= vs->acquire && a < vs->acquire + DE__VK_MAX_SWAPCHAIN_IMAGES + 1u)
            continue;
        vk->pending_acquire[n++] = a;
    }
    vk->pending_acquire_count = n;
}

/* ---------- Semaphores ---------- */

static void destroy_semaphores(de__vk_device *vk, de__vk_swapchain *vs) {
    for (u32 i = 0; i < DE__VK_MAX_SWAPCHAIN_IMAGES; ++i) {
        if (vs->present[i])
            vk->fn.vkDestroySemaphore(vk->device, vs->present[i], NULL);
        vs->present[i] = VK_NULL_HANDLE;
    }
    for (u32 i = 0; i < DE__VK_MAX_SWAPCHAIN_IMAGES + 1u; ++i) {
        if (vs->acquire[i].semaphore)
            vk->fn.vkDestroySemaphore(vk->device, vs->acquire[i].semaphore, NULL);
        memset(&vs->acquire[i], 0, sizeof vs->acquire[i]);
    }
}

static de_result create_semaphores(de_gpu_device *d, de__vk_swapchain *vs) {
    for (u32 i = 0; i < vs->image_count; ++i) {
        vs->present[i] = de__vk_binary_semaphore(d, "swapchain present");
        if (!vs->present[i])
            return DE_ERR_OUT_OF_MEMORY;
    }
    for (u32 i = 0; i < vs->image_count + 1u; ++i) {
        vs->acquire[i].semaphore = de__vk_binary_semaphore(d, "swapchain acquire");
        if (!vs->acquire[i].semaphore)
            return DE_ERR_OUT_OF_MEMORY;
    }
    vs->acquire_next = 0;
    return DE_OK;
}

/* ---------- Images ---------- */

static void release_images(de_gpu_device *d, de__vk_swapchain *vs) {
    for (u32 i = 0; i < DE__VK_MAX_SWAPCHAIN_IMAGES; ++i) {
        if (vs->textures[i].id)
            de_gpu_texture_destroy(d, vs->textures[i]); /* external: the image stays ours */
        vs->textures[i].id = 0;
        vs->images[i] = VK_NULL_HANDLE;
    }
}

static de_gpu_format de_format_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_SRGB:
        return DE_FMT_BGRA8_SRGB;
    case VK_FORMAT_R8G8B8A8_SRGB:
        return DE_FMT_RGBA8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM:
        return DE_FMT_BGRA8_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return DE_FMT_RGBA8_UNORM;
    default:
        return DE_FMT_UNKNOWN;
    }
}

static de_result wrap_images(de_gpu_device *d, de__gpu_swapchain *s) {
    de__vk_device *vk = de__vk(d);
    de__vk_swapchain *vs = payload(s);
    u32 n = DE__VK_MAX_SWAPCHAIN_IMAGES;
    VkResult vr = vk->fn.vkGetSwapchainImagesKHR(vk->device, vs->swapchain, &n, vs->images);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE)
        return de__vk_fail(d, vr, "de_gpu_swapchain_create: vkGetSwapchainImagesKHR");
    vs->image_count = n;
    de_gpu_texture_desc td;
    memset(&td, 0, sizeof td);
    td.dim = DE_TEX_2D;
    td.format = de_format_of(vs->format);
    td.width = vs->extent.width;
    td.height = vs->extent.height;
    td.usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST;
    td.name = "swapchain image";
    for (u32 i = 0; i < n; ++i) {
        de__gpu_texture *t = NULL;
        de_gpu_texture h = de__gpu_texture_wrap(d, &td, &t);
        if (!h.id)
            return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_gpu_swapchain_create: image %u", i);
        de__vk_texture *vt = DE__GPU_PAYLOAD(de__vk_texture, t);
        vt->image = vs->images[i];
        vt->format = vs->format;
        vt->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        vs->textures[i] = h;
        DE_TRY(de__vk_texture_create_views(d, t)); /* none: no SAMPLED or STORAGE usage */
        de__gpu_texture_wrap_finish(d, t);
    }
    return DE_OK;
}

/* ---------- Building the swapchain ---------- */

static u32 clamp_u32(u32 v, u32 lo, u32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static bool has_mode(const VkPresentModeKHR *m, u32 n, VkPresentModeKHR want) {
    for (u32 i = 0; i < n; ++i)
        if (m[i] == want)
            return true;
    return false;
}

/* Creates the VkSwapchainKHR of `s` at about w x h (replacing vs->swapchain if any), then its
 * images and semaphores. DE_ERR_BUSY for a zero-size (minimised) surface. */
static de_result build(de_gpu_device *d, de__gpu_swapchain *s, u32 w, u32 h) {
    de__vk_device *vk = de__vk(d);
    de__vk_swapchain *vs = payload(s);
    const de_gpu_swapchain_desc *desc = &s->desc;
    VkSurfaceKHR surface;
    memcpy(&surface, &vs->surface, sizeof surface); /* u64 <-> handle, as the platform does */

    VkSurfaceCapabilitiesKHR caps;
    VkResult vr = vk->fn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->phys.handle, surface, &caps);
    if (vr != VK_SUCCESS)
        return de__vk_fail(d, vr, "de_gpu_swapchain: vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    /* Format: sRGB, BGRA first (the usual native order), then RGBA. */
    VkSurfaceFormatKHR formats[64];
    u32 nf = 64;
    vr = vk->fn.vkGetPhysicalDeviceSurfaceFormatsKHR(vk->phys.handle, surface, &nf, formats);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE)
        return de__vk_fail(d, vr, "de_gpu_swapchain: vkGetPhysicalDeviceSurfaceFormatsKHR");
    static const VkFormat k_pref[] = {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB};
    VkSurfaceFormatKHR chosen = {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    bool hdr10 = false, scrgb = false;
    for (u32 p = 0; p < 2 && chosen.format == VK_FORMAT_UNDEFINED; ++p)
        for (u32 i = 0; i < nf; ++i)
            if (formats[i].format == k_pref[p] &&
                formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                chosen = formats[i];
    for (u32 i = 0; i < nf; ++i) {
        hdr10 = hdr10 || formats[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
        scrgb = scrgb || formats[i].colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
    }
    if (chosen.format == VK_FORMAT_UNDEFINED)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "de_gpu_swapchain_create: the surface offers no BGRA8/RGBA8 sRGB "
                            "format (%u formats)",
                            nf);

    VkPresentModeKHR modes[16];
    u32 nm = 16;
    vr = vk->fn.vkGetPhysicalDeviceSurfacePresentModesKHR(vk->phys.handle, surface, &nm, modes);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE)
        nm = 0;
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR; /* always supported */
    if (desc->present == DE_PRESENT_MAILBOX && has_mode(modes, nm, VK_PRESENT_MODE_MAILBOX_KHR))
        mode = VK_PRESENT_MODE_MAILBOX_KHR;
    else if (desc->present == DE_PRESENT_IMMEDIATE) {
        if (has_mode(modes, nm, VK_PRESENT_MODE_IMMEDIATE_KHR))
            mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (has_mode(modes, nm, VK_PRESENT_MODE_MAILBOX_KHR))
            mode = VK_PRESENT_MODE_MAILBOX_KHR;
    }

    VkExtent2D extent;
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        extent.width = clamp_u32(w, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = clamp_u32(h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        /* Nothing to present to: drop the old swapchain (the device is idle); acquire says BUSY
         * until a resize finds an area again. */
        if (vs->swapchain)
            vk->fn.vkDestroySwapchainKHR(vk->device, vs->swapchain, NULL);
        vs->swapchain = VK_NULL_HANDLE;
        return DE_FAIL(DE_ERR_BUSY, "de_gpu_swapchain: the window has no area (minimised)");
    }

    u32 count = desc->image_count ? desc->image_count : 3u;
    const u32 max_count = caps.maxImageCount ? caps.maxImageCount : DE__VK_MAX_SWAPCHAIN_IMAGES;
    count = clamp_u32(count, caps.minImageCount, max_count);
    if (count > DE__VK_MAX_SWAPCHAIN_IMAGES)
        count = DE__VK_MAX_SWAPCHAIN_IMAGES;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    usage |= caps.supportedUsageFlags &
             (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        static const VkCompositeAlphaFlagBitsKHR k_alpha[] = {
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR};
        for (u32 i = 0; i < 3; ++i)
            if (caps.supportedCompositeAlpha & k_alpha[i]) {
                alpha = k_alpha[i];
                break;
            }
    }

    VkSwapchainCreateInfoKHR ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface;
    ci.minImageCount = count;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = usage;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = vs->swapchain;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    vr = vk->fn.vkCreateSwapchainKHR(vk->device, &ci, NULL, &sc);
    if (vs->swapchain) {
        /* Retired: the device is idle, so it can go now (also when the new one failed). */
        vk->fn.vkDestroySwapchainKHR(vk->device, vs->swapchain, NULL);
        vs->swapchain = VK_NULL_HANDLE;
    }
    if (vr != VK_SUCCESS)
        return de__vk_fail(d, vr, "de_gpu_swapchain: vkCreateSwapchainKHR (%ux%u, %u images)",
                           extent.width, extent.height, count);
    vs->swapchain = sc;
    vs->format = chosen.format;
    vs->mode = mode;
    vs->extent = extent;
    vs->current = NO_IMAGE;
    vs->suboptimal = false;
    DE_TRY(wrap_images(d, s));
    DE_TRY(create_semaphores(d, vs));
    DE_INFO(d->log,
            "vulkan swapchain: %ux%u, %u images, %s, present mode %d (asked %d)%s; HDR10 %s, "
            "scRGB %s (HDR swapchains: S18)",
            extent.width, extent.height, vs->image_count,
            chosen.format == VK_FORMAT_B8G8R8A8_SRGB ? "B8G8R8A8_SRGB" : "R8G8B8A8_SRGB", (int)mode,
            (int)desc->present, desc->allow_vrr ? ", VRR allowed (driver/compositor)" : "",
            hdr10 ? "offered" : "not offered", scrgb ? "offered" : "not offered");
    return DE_OK;
}

/* Waits until no queue uses the swapchain's images or semaphores. */
static void idle(de_gpu_device *d) {
    d->vt->wait_idle(d);
}

/* ---------- Vtable entries ---------- */

de_result de__vk_swapchain_create(de_gpu_device *d, de__gpu_swapchain *s,
                                  const de_gpu_swapchain_desc *desc) {
    de__vk_device *vk = de__vk(d);
    de__vk_swapchain *vs = payload(s);
    vs->current = NO_IMAGE;
    if (desc->colorspace != DE_CS_SRGB)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "de_gpu_swapchain_create: colour space %d; the Vulkan backend has sRGB "
                            "swapchains only until S18",
                            (int)desc->colorspace);
    if (!vk->on.swapchain || !vk->fn.vkCreateSwapchainKHR || !vk->fn.vkQueuePresentKHR ||
        !vk->fn.vkGetPhysicalDeviceSurfaceSupportKHR ||
        !vk->fn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR || !vk->fn.vkDestroySurfaceKHR)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "de_gpu_swapchain_create: this Vulkan device or instance has no "
                            "VK_KHR_swapchain / VK_KHR_surface");

    /* The surface comes from SDL's loader: it must be the device's loader. */
    de_window_vulkan_fn platform_gipa = de_window_vulkan_get_instance_proc_addr();
    if (!platform_gipa)
        return DE_FAIL_WRAP(DE_ERR_UNSUPPORTED,
                            "de_gpu_swapchain_create: the platform has no Vulkan loader");
    if (platform_gipa != (de_window_vulkan_fn)vk->fn.vkGetInstanceProcAddr)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "de_gpu_swapchain_create: the window's Vulkan loader (SDL) is not the "
                            "device's (%s); set SDL_VULKAN_LIBRARY and DE_VULKAN_LIBRARY to the "
                            "same library",
                            vk->lib_name);
    de_result r =
        de_window_vulkan_create_surface(desc->window, (void *)vk->instance, NULL, &vs->surface);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "de_gpu_swapchain_create: surface");
    VkSurfaceKHR surface;
    memcpy(&surface, &vs->surface, sizeof surface);

    VkBool32 supported = VK_FALSE;
    VkResult vr = vk->fn.vkGetPhysicalDeviceSurfaceSupportKHR(
        vk->phys.handle, vk->queues[DE_QUEUE_GRAPHICS].family, surface, &supported);
    if (vr != VK_SUCCESS || !supported) {
        r = de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                         "de_gpu_swapchain_create: the graphics queue family %u cannot present "
                         "to this window (%s)",
                         vk->queues[DE_QUEUE_GRAPHICS].family, de__vk_result_name(vr));
    }
    if (r == DE_OK) {
        u32 w = 0, h = 0;
        de_window_size_pixels(desc->window, &w, &h);
        r = build(d, s, w, h);
    }
    if (r != DE_OK)
        de__vk_swapchain_destroy(d, s);
    return r;
}

void de__vk_swapchain_destroy(de_gpu_device *d, de__gpu_swapchain *s) {
    de__vk_device *vk = de__vk(d);
    de__vk_swapchain *vs = payload(s);
    idle(d);
    de_mutex_lock(gfx_lock(vk));
    unlist_acquires(vk, vs);
    de_mutex_unlock(gfx_lock(vk));
    release_images(d, vs);
    destroy_semaphores(vk, vs);
    if (vs->swapchain)
        vk->fn.vkDestroySwapchainKHR(vk->device, vs->swapchain, NULL);
    vs->swapchain = VK_NULL_HANDLE;
    if (vs->surface) {
        VkSurfaceKHR surface;
        memcpy(&surface, &vs->surface, sizeof surface);
        vk->fn.vkDestroySurfaceKHR(vk->instance, surface, NULL);
    }
    vs->surface = 0;
}

de_result de__vk_swapchain_acquire(de_gpu_device *d, de__gpu_swapchain *s, de_gpu_texture *image) {
    de__vk_device *vk = de__vk(d);
    de__vk_swapchain *vs = payload(s);
    if (!vs->swapchain)
        return DE_FAIL(DE_ERR_BUSY, "de_gpu_swapchain_acquire: no swapchain; resize it");
    if (vs->current != NO_IMAGE) { /* acquired and not presented yet: the same image */
        *image = vs->textures[vs->current];
        return DE_OK;
    }
    if (de__vk_lost(vk))
        return de__gpu_fail(d, DE_ERR_DEVICE_LOST, "de_gpu_swapchain_acquire: device lost");
    const u32 slot_index = vs->acquire_next;
    de__vk_acquire *slot = &vs->acquire[slot_index];
    /* The submit that waited on this semaphore must be done before it is signalled again. */
    if (!de__vk_wait_queue_value(vk, DE_QUEUE_GRAPHICS, slot->consumed_value))
        return de__gpu_fail(d, DE_ERR_DEVICE_LOST, "de_gpu_swapchain_acquire: device lost");
    u32 index = 0;
    VkResult vr = vk->fn.vkAcquireNextImageKHR(vk->device, vs->swapchain, UINT64_MAX,
                                               slot->semaphore, VK_NULL_HANDLE, &index);
    if (vr == VK_ERROR_OUT_OF_DATE_KHR)
        return DE_FAIL(DE_ERR_BUSY, "de_gpu_swapchain_acquire: out of date; resize the swapchain");
    if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR)
        return de__vk_fail(d, vr, "de_gpu_swapchain_acquire: vkAcquireNextImageKHR");
    if (vr == VK_SUBOPTIMAL_KHR)
        vs->suboptimal = true;
    vs->acquire_next = (slot_index + 1u) % (vs->image_count + 1u);
    slot->pending = true;
    slot->consumed_value = 0;
    de_mutex_lock(gfx_lock(vk));
    if (vk->pending_acquire_count < DE__VK_MAX_PENDING_ACQUIRES)
        vk->pending_acquire[vk->pending_acquire_count++] = slot;
    de_mutex_unlock(gfx_lock(vk));
    vs->current = index;
    vs->current_slot = slot_index;
    *image = vs->textures[index];
    return DE_OK;
}

de_result de__vk_swapchain_present(de_gpu_device *d, de__gpu_swapchain *s) {
    de__vk_device *vk = de__vk(d);
    de__vk_swapchain *vs = payload(s);
    if (vs->current == NO_IMAGE)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "de_gpu_swapchain_present: no image is acquired");
    if (de__vk_lost(vk))
        return de__gpu_fail(d, DE_ERR_DEVICE_LOST, "de_gpu_swapchain_present: device lost");
    const u32 index = vs->current;
    de__vk_acquire *slot = &vs->acquire[vs->current_slot];
    de__vk_queue *gq = &vk->queues[DE_QUEUE_GRAPHICS];

    de_mutex_lock(gfx_lock(vk));
    /* Bridge: the graphics timeline (everything submitted so far) -> the present semaphore. */
    VkSemaphoreSubmitInfo waits[2];
    memset(waits, 0, sizeof waits);
    u32 nwait = 0;
    waits[nwait].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waits[nwait].semaphore = gq->timeline;
    waits[nwait].value = vk->last_value[DE_QUEUE_GRAPHICS];
    waits[nwait].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ++nwait;
    const bool acquire_unused = slot->pending; /* nothing was submitted after the acquire */
    if (acquire_unused) {
        waits[nwait].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waits[nwait].semaphore = slot->semaphore;
        waits[nwait].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ++nwait;
        unlist_acquires(vk, vs);
        slot->pending = false;
    }
    VkSemaphoreSubmitInfo sig;
    memset(&sig, 0, sizeof sig);
    sig.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    sig.semaphore = vs->present[index];
    sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.waitSemaphoreInfoCount = nwait;
    si.pWaitSemaphoreInfos = waits;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &sig;
    VkResult vr = vk->fn.vkQueueSubmit2(gq->queue, 1, &si, VK_NULL_HANDLE);
    VkResult pr = VK_ERROR_DEVICE_LOST;
    if (vr == VK_SUCCESS) {
        VkPresentInfoKHR pi;
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &vs->present[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &vs->swapchain;
        pi.pImageIndices = &index;
        pr = vk->fn.vkQueuePresentKHR(gq->queue, &pi);
        /* An acquire semaphore waited on by the bridge (not a timeline submit) is only known to
         * be free once the queue drains: rare (nothing was rendered), so wait here. */
        if (acquire_unused)
            vk->fn.vkQueueWaitIdle(gq->queue);
    }
    de_mutex_unlock(gfx_lock(vk));
    vs->current = NO_IMAGE;

    if (vr != VK_SUCCESS)
        return de__vk_fail(d, vr, "de_gpu_swapchain_present: bridge vkQueueSubmit2");
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR || vs->suboptimal) {
        vs->suboptimal = false;
        return DE_FAIL(DE_ERR_BUSY, "de_gpu_swapchain_present: %s; resize the swapchain",
                       pr == VK_ERROR_OUT_OF_DATE_KHR ? "out of date" : "suboptimal");
    }
    if (pr != VK_SUCCESS)
        return de__vk_fail(d, pr, "de_gpu_swapchain_present: vkQueuePresentKHR");
    return DE_OK;
}

de_result de__vk_swapchain_resize(de_gpu_device *d, de__gpu_swapchain *s, u32 w, u32 h) {
    de__vk_device *vk = de__vk(d);
    de__vk_swapchain *vs = payload(s);
    idle(d);
    de_mutex_lock(gfx_lock(vk));
    unlist_acquires(vk, vs);
    de_mutex_unlock(gfx_lock(vk));
    /* Idle: every image and semaphore is free. A signalled, unwaited acquire semaphore cannot be
     * reused, so all semaphores are made again. */
    release_images(d, vs);
    destroy_semaphores(vk, vs);
    vs->current = NO_IMAGE;
    return build(d, s, w, h);
}

de_gpu_colorspace de__vk_swapchain_colorspace(de_gpu_device *d, de__gpu_swapchain *s) {
    (void)d;
    (void)s;
    return DE_CS_SRGB;
}
