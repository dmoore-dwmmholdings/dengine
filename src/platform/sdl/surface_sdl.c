/* surface_sdl.c: native window handles and graphics surfaces on SDL3 (SPEC §6, §12.1; DENG-250).
 *
 * - D3D12: the Win32 window handle, from SDL's window properties.
 * - Metal: an SDL Metal view in the window, made by the first de_window_native() call and
 *   destroyed just before the window (de__platform_sdl_surface_release). Its layer is the
 *   CAMetalLayer the RHI renders to.
 * - Vulkan: SDL opens the Vulkan loader at run time (the engine never links it) and creates the
 *   surface. The engine holds one loader reference from the first use until shutdown, so the
 *   function pointers handed to the RHI stay valid while windows come and go.
 *
 * Only the SDL window flags differ per surface kind at creation; everything else happens here. */
#include "platform/sdl/sdl_internal.h"

#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_vulkan.h>

#include <string.h>

/* The public API passes Vulkan surfaces as u64. VkSurfaceKHR is a non-dispatchable handle: a
 * pointer on 64-bit targets and a uint64_t on 32-bit ones, 64 bits wide on the targets we ship. */
DE_STATIC_ASSERT(sizeof(VkSurfaceKHR) <= sizeof(u64), "a Vulkan surface fits in a u64");

/* Takes the engine's loader reference once. SDL looks for the loader in the standard places
 * (libvulkan / vulkan-1.dll / the macOS Vulkan SDK or MoltenVK); SDL_VULKAN_LIBRARY overrides. */
static de_result de__vulkan_load(de__platform_sdl_state *st) {
    if (st->vulkan_loaded)
        return DE_OK;
    if (st->headless)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "vulkan: headless platform");
    if (!SDL_Vulkan_LoadLibrary(NULL))
        return DE_FAIL(DE_ERR_UNSUPPORTED, "vulkan: no Vulkan loader: %s", SDL_GetError());
    st->vulkan_loaded = true;
    return DE_OK;
}

/* The live slot of `w`, or NULL. */
static de__platform_sdl_window *de__surface_slot(de__platform_sdl_state *st, de_window w) {
    SDL_Window *win = de__platform_sdl_window_get(w);
    return win ? de__platform_sdl_slot_from_id(st, SDL_GetWindowID(win)) : NULL;
}

de_result de__platform_sdl_surface_flags(de__platform_sdl_state *st, de_window_surface surface,
                                         SDL_WindowFlags *flags) {
    switch (surface) {
    case DE_WINDOW_SURFACE_NONE:
        return DE_OK;
    case DE_WINDOW_SURFACE_VULKAN: {
        /* SDL_CreateWindow would open the loader too, but it fails with a generic error. */
        de_result r = de__vulkan_load(st);
        if (r != DE_OK)
            return r;
        *flags |= SDL_WINDOW_VULKAN;
        break;
    }
    case DE_WINDOW_SURFACE_METAL:
#if defined(SDL_PLATFORM_MACOS)
        *flags |= SDL_WINDOW_METAL;
        break;
#else
        return DE_FAIL(DE_ERR_UNSUPPORTED, "surface: Metal is macOS only");
#endif
    default:
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "surface: bad surface kind %d", (int)surface);
    }
    /* SDL's Metal view (also the one inside a MoltenVK surface) would resize the layer's
     * drawable on every window size event, from the main thread. The RHI swapchain owns the
     * drawable size instead (de_platform.h), so turn that off before any view exists. */
    SDL_SetHint(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE, "0");
    return DE_OK;
}

void de__platform_sdl_surface_release(de__platform_sdl_window *s) {
    if (s->metal_view) {
        SDL_Metal_DestroyView(s->metal_view);
        s->metal_view = NULL;
    }
}

void de__platform_sdl_surface_shutdown(de__platform_sdl_state *st) {
    if (st->vulkan_loaded) {
        SDL_Vulkan_UnloadLibrary();
        st->vulkan_loaded = false;
    }
}

de_native_window de_window_native(de_window w) {
    de_native_window n;
    memset(&n, 0, sizeof n);
    de__platform_sdl_state *st = de__platform_sdl_get();
    de__platform_sdl_window *s = de__surface_slot(st, w);
    if (!s)
        return n;
    n.sdl_window = s->sdl;
#if defined(SDL_PLATFORM_WINDOWS)
    n.hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(s->sdl),
                                    SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
#endif
#if defined(SDL_PLATFORM_MACOS)
    if (s->surface == DE_WINDOW_SURFACE_METAL) {
        if (!s->metal_view) {
            s->metal_view = SDL_Metal_CreateView(s->sdl);
            if (!s->metal_view) {
                (void)DE_FAIL(DE_ERR_INTERNAL, "de_window_native: Metal view: %s", SDL_GetError());
                return n;
            }
        }
        n.metal_layer = SDL_Metal_GetLayer(s->metal_view);
        /* Refreshed on every call: the density changes when the window moves to a display
         * with another scale (DE_DISPLAY_SCALE), and the RHI asks again then. */
        float density = SDL_GetWindowPixelDensity(s->sdl);
        if (n.metal_layer && density > 0.0f)
            de__platform_sdl_metal_layer_set_scale(n.metal_layer, density);
    }
#endif
    return n;
}

u32 de_window_vulkan_instance_extensions(const char **out, u32 max) {
    de__platform_sdl_state *st = de__platform_sdl_get();
    if (!st) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "vulkan: de_platform_init not called");
        return 0;
    }
    DE__PLATFORM_SDL_ASSERT_MAIN(st);
    if (de__vulkan_load(st) != DE_OK)
        return 0;
    Uint32 count = 0;
    const char *const *names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!names) {
        (void)DE_FAIL(DE_ERR_INTERNAL, "vulkan: instance extensions: %s", SDL_GetError());
        return 0;
    }
    for (u32 i = 0; out && i < count && i < max; i++)
        out[i] = names[i];
    return count;
}

de_window_vulkan_fn de_window_vulkan_get_instance_proc_addr(void) {
    de__platform_sdl_state *st = de__platform_sdl_get();
    if (!st) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "vulkan: de_platform_init not called");
        return NULL;
    }
    DE__PLATFORM_SDL_ASSERT_MAIN(st);
    if (de__vulkan_load(st) != DE_OK)
        return NULL;
    SDL_FunctionPointer fn = SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!fn)
        (void)DE_FAIL(DE_ERR_INTERNAL, "vulkan: vkGetInstanceProcAddr: %s", SDL_GetError());
    return (de_window_vulkan_fn)fn;
}

de_result de_window_vulkan_create_surface(de_window w, void *vk_instance,
                                          const void *vk_allocation_callbacks,
                                          u64 *out_vk_surface) {
    if (!out_vk_surface)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_window_vulkan_create_surface: NULL out");
    *out_vk_surface = 0;
    de__platform_sdl_state *st = de__platform_sdl_get();
    de__platform_sdl_window *s = de__surface_slot(st, w);
    if (!s)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_window_vulkan_create_surface: stale window %#x",
                       w.id);
    if (!vk_instance)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_window_vulkan_create_surface: NULL instance");
    /* SDL would switch any window to Vulkan on the fly; the engine asks for it up front, so a
     * window never changes API under a Metal view or a D3D12 swapchain. */
    if (s->surface != DE_WINDOW_SURFACE_VULKAN)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_window_vulkan_create_surface: window %#x was not created with "
                       "DE_WINDOW_SURFACE_VULKAN",
                       w.id);
    de_result r = de__vulkan_load(st); /* already open: the window took the loader */
    if (r != DE_OK)
        return r;
    VkSurfaceKHR surface = 0;
    if (!SDL_Vulkan_CreateSurface(s->sdl, (VkInstance)vk_instance,
                                  (const struct VkAllocationCallbacks *)vk_allocation_callbacks,
                                  &surface))
        return DE_FAIL(DE_ERR_INTERNAL, "de_window_vulkan_create_surface: %s", SDL_GetError());
    memcpy(out_vk_surface, &surface, sizeof surface);
    return DE_OK;
}

void de_window_vulkan_destroy_surface(void *vk_instance, u64 vk_surface,
                                      const void *vk_allocation_callbacks) {
    de__platform_sdl_state *st = de__platform_sdl_get();
    if (!st || !st->vulkan_loaded || !vk_instance || !vk_surface)
        return;
    DE__PLATFORM_SDL_ASSERT_MAIN(st);
    VkSurfaceKHR surface = 0;
    memcpy(&surface, &vk_surface, sizeof surface);
    SDL_Vulkan_DestroySurface((VkInstance)vk_instance, surface,
                              (const struct VkAllocationCallbacks *)vk_allocation_callbacks);
}
