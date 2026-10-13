/* d3d12_swapchain.c — DXGI flip-model swapchains (DENG-286; SPEC §12.1).
 *
 * Create: IDXGISwapChain4 for the window's HWND (de_window_native, S03), on the graphics queue,
 * FLIP_DISCARD, `image_count` buffers (0 = 3; 2 .. 16), B8G8R8A8_UNORM buffers in the sRGB color
 * space; the images are front-end textures (de__gpu_texture_wrap) with format BGRA8_SRGB, so their
 * render-target views write sRGB (a flip-model buffer cannot have an sRGB format itself, its views
 * can). HDR color spaces come in S18: anything but DE_CS_SRGB is DE_ERR_UNSUPPORTED. Alt+Enter
 * is left to the host (DXGI_MWA_NO_ALT_ENTER). With DXGI tearing support (caps.vrr) the swapchain
 * is created with ALLOW_TEARING, which IMMEDIATE needs. A frame-latency waitable object with a
 * maximum latency of image_count - 1 paces acquire.
 *
 * DXGI allows one flip-model swapchain per window. A destroyed swapchain lives on until the GPU
 * is done with it (deferred destruction), so create, when DXGI refuses the window, waits for the
 * device to go idle (which releases it) and tries once more.
 *
 * Acquire: waits on the waitable object (once per presented frame, at most a second; a hidden or
 * occluded window may never signal it) and returns the image GetCurrentBackBufferIndex names. Until
 * present, acquire returns the same image. No GPU wait is needed: the flip model orders the reuse
 * of a buffer on the queue itself.
 *
 * Present: the caller renders the image on the graphics queue and ends with RENDER_TARGET ->
 * PRESENT; Present() is queued on the graphics queue after every submission made before it. VSYNC
 * presents with sync interval 1; MAILBOX with interval 0 (flip-discard drops frames that a newer
 * one replaces before the next vblank); IMMEDIATE with interval 0 and ALLOW_TEARING when the
 * swapchain has it (else as MAILBOX). DXGI_STATUS_OCCLUDED is success; a removed device is
 * DE_ERR_DEVICE_LOST. DXGI swapchains do not go out of date: the window's new size takes effect at
 * de_gpu_swapchain_resize, which the host calls on DE_EV_WINDOW_RESIZED.
 *
 * Resize (and destroy): wait for the device to go idle, release the images (their handles turn
 * stale at once), ResizeBuffers, wrap the new images. A 0 width or height (a minimized window)
 * keeps the current images.
 */
#include "gpu/d3d12/d3d12_internal.h"

#include <string.h>

#define DEFAULT_IMAGES  3u
#define ACQUIRE_WAIT_MS 1000u

static d3d12_swapchain *payload(de__gpu_swapchain *s) {
    return DE__GPU_PAYLOAD(d3d12_swapchain, s);
}

/* Releases the images' resources now (the GPU is idle) and destroys their handles. */
static void drop_images(de_gpu_device *d, d3d12_swapchain *ds) {
    for (u32 i = 0; i < ds->image_count; ++i) {
        de__gpu_texture *t = de__gpu_get_texture(d, ds->images[i]);
        if (t) {
            d3d12_texture *dt = DE__GPU_PAYLOAD(d3d12_texture, t);
            if (dt->res)
                ID3D12Resource_Release(dt->res);
            dt->res = NULL;
            de_gpu_texture_destroy(d, ds->images[i]);
        }
        ds->images[i].id = 0;
    }
}

/* Wraps buffer i of the swapchain as a front-end texture, for every buffer. */
static de_result wrap_images(de_gpu_device *d, d3d12_swapchain *ds) {
    DXGI_SWAP_CHAIN_DESC1 sd;
    HRESULT hr = IDXGISwapChain4_GetDesc1(ds->sc, &sd);
    if (FAILED(hr))
        return de__d3d12_fail(hr, "de_gpu_swapchain: IDXGISwapChain::GetDesc1");
    ds->width = sd.Width;
    ds->height = sd.Height;
    for (u32 i = 0; i < ds->image_count; ++i) {
        ID3D12Resource *res = NULL;
        hr = IDXGISwapChain4_GetBuffer(ds->sc, i, &IID_ID3D12Resource, (void **)&res);
        if (FAILED(hr)) {
            drop_images(d, ds);
            return de__d3d12_fail(hr, "de_gpu_swapchain: IDXGISwapChain::GetBuffer(%u)", i);
        }
        de_gpu_texture_desc td;
        memset(&td, 0, sizeof td);
        td.dim = DE_TEX_2D;
        td.format = DE_FMT_BGRA8_SRGB;
        td.width = sd.Width;
        td.height = sd.Height;
        td.depth_or_layers = 1;
        td.mip_count = 1;
        td.sample_count = 1;
        td.usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST;
        td.name = "swapchain image";
        de__gpu_texture *t = NULL;
        de_gpu_texture h = de__gpu_texture_wrap(d, &td, &t);
        if (!h.id) {
            ID3D12Resource_Release(res);
            drop_images(d, ds);
            return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_gpu_swapchain: image %u", i);
        }
        DE__GPU_PAYLOAD(d3d12_texture, t)->res = res; /* the texture owns this reference */
        de__d3d12_set_name((ID3D12Object *)res, "de swapchain image");
        de__gpu_texture_wrap_finish(d, t);
        ds->images[i] = h;
    }
    return DE_OK;
}

static void release(d3d12_swapchain *ds) {
    if (ds->waitable)
        CloseHandle(ds->waitable);
    if (ds->sc)
        IDXGISwapChain4_Release(ds->sc);
    ds->waitable = NULL;
    ds->sc = NULL;
}

de_result de__d3d12_swapchain_create(de_gpu_device *d, de__gpu_swapchain *s,
                                     const de_gpu_swapchain_desc *desc) {
    d3d12_device *dd = de__d3d12(d);
    d3d12_swapchain *ds = payload(s);
    if (desc->colorspace != DE_CS_SRGB)
        return DE_FAIL(DE_ERR_UNSUPPORTED,
                       "de_gpu_swapchain_create: d3d12 swapchains are sRGB until S18 (HDR)");
    const de_native_window nw = de_window_native(desc->window);
    if (!nw.hwnd)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_gpu_swapchain_create: the window has no HWND (stale handle, or the "
                       "platform layer is not initialised)");
    ds->hwnd = (HWND)nw.hwnd;
    u32 count = desc->image_count ? desc->image_count : DEFAULT_IMAGES;
    if (count < 2)
        count = 2;
    if (count > DE__D3D12_SWAPCHAIN_MAX_IMAGES)
        count = DE__D3D12_SWAPCHAIN_MAX_IMAGES;
    ds->image_count = count;
    ds->tearing = dd->features.tearing;
    ds->flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                (ds->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);

    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof sd);
    sd.Width = 0; /* the window's client size */
    sd.Height = 0;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = count;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = ds->flags;
    IDXGISwapChain1 *sc1 = NULL;
    IUnknown *queue = (IUnknown *)dd->queues[DE_QUEUE_GRAPHICS].queue;
    HRESULT hr =
        IDXGIFactory4_CreateSwapChainForHwnd(dd->factory, queue, ds->hwnd, &sd, NULL, NULL, &sc1);
    if (hr == E_ACCESSDENIED || hr == DXGI_ERROR_ACCESS_DENIED) {
        /* The window still has a destroyed swapchain waiting for the GPU: release it. */
        de_gpu_wait_idle(d);
        hr = IDXGIFactory4_CreateSwapChainForHwnd(dd->factory, queue, ds->hwnd, &sd, NULL, NULL,
                                                  &sc1);
    }
    if (FAILED(hr))
        return de__d3d12_fail(hr, "de_gpu_swapchain_create: CreateSwapChainForHwnd");
    hr = IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain4, (void **)&ds->sc);
    IDXGISwapChain1_Release(sc1);
    if (FAILED(hr)) {
        ds->sc = NULL;
        return de__d3d12_fail(hr, "de_gpu_swapchain_create: no IDXGISwapChain4 (Windows 10 1607+)");
    }
    IDXGIFactory4_MakeWindowAssociation(dd->factory, ds->hwnd, DXGI_MWA_NO_ALT_ENTER);
    UINT support = 0;
    if (SUCCEEDED(IDXGISwapChain4_CheckColorSpaceSupport(
            ds->sc, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &support)) &&
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
        IDXGISwapChain4_SetColorSpace1(ds->sc, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    IDXGISwapChain4_SetMaximumFrameLatency(ds->sc, count - 1u);
    ds->waitable = IDXGISwapChain4_GetFrameLatencyWaitableObject(ds->sc);
    de_result r = wrap_images(d, ds);
    if (r != DE_OK) {
        release(ds);
        return r;
    }
    DE_INFO(d->log, "d3d12: swapchain %ux%u, %u images, %s%s", ds->width, ds->height, count,
            desc->present == DE_PRESENT_VSYNC     ? "vsync"
            : desc->present == DE_PRESENT_MAILBOX ? "mailbox"
                                                  : "immediate",
            ds->tearing ? ", tearing allowed" : "");
    return DE_OK;
}

void de__d3d12_swapchain_destroy(de_gpu_device *d, de__gpu_swapchain *s) {
    d3d12_swapchain *ds = payload(s);
    d->vt->wait_idle(d);
    drop_images(d, ds);
    release(ds);
}

de_result de__d3d12_swapchain_acquire(de_gpu_device *d, de__gpu_swapchain *s,
                                      de_gpu_texture *image) {
    d3d12_swapchain *ds = payload(s);
    if (!ds->waited && ds->waitable) {
        const DWORD w = WaitForSingleObjectEx(ds->waitable, ACQUIRE_WAIT_MS, TRUE);
        if (w == WAIT_TIMEOUT)
            DE_LOG(DE_LOG_DEBUG, d->log,
                   "d3d12: swapchain waitable not signalled after %u ms (occluded?)",
                   ACQUIRE_WAIT_MS);
        ds->waited = true;
    }
    const UINT index = IDXGISwapChain4_GetCurrentBackBufferIndex(ds->sc);
    if (index >= ds->image_count || !ds->images[index].id)
        return DE_FAIL(DE_ERR_INTERNAL, "de_gpu_swapchain_acquire: back buffer %u of %u", index,
                       ds->image_count);
    *image = ds->images[index];
    return DE_OK;
}

de_result de__d3d12_swapchain_present(de_gpu_device *d, de__gpu_swapchain *s) {
    d3d12_swapchain *ds = payload(s);
    UINT interval = 1, flags = 0;
    if (s->desc.present != DE_PRESENT_VSYNC) {
        interval = 0;
        if (s->desc.present == DE_PRESENT_IMMEDIATE && ds->tearing)
            flags = DXGI_PRESENT_ALLOW_TEARING;
    }
    HRESULT hr = IDXGISwapChain4_Present(ds->sc, interval, flags);
    ds->waited = false;
    de__d3d12_drain_messages(de__d3d12(d));
    if (hr == DXGI_STATUS_OCCLUDED || SUCCEEDED(hr))
        return DE_OK;
    return de__d3d12_fail(hr, "de_gpu_swapchain_present: IDXGISwapChain::Present");
}

de_result de__d3d12_swapchain_resize(de_gpu_device *d, de__gpu_swapchain *s, u32 w, u32 h) {
    d3d12_swapchain *ds = payload(s);
    if (w == 0 || h == 0)
        return DE_OK; /* minimized: keep the images until the window has a size again */
    d->vt->wait_idle(d);
    drop_images(d, ds);
    HRESULT hr = IDXGISwapChain4_ResizeBuffers(ds->sc, ds->image_count, w, h, DXGI_FORMAT_UNKNOWN,
                                               ds->flags);
    ds->waited = false;
    de_result r = FAILED(hr)
                      ? de__d3d12_fail(hr, "de_gpu_swapchain_resize: ResizeBuffers(%u, %u)", w, h)
                      : DE_OK;
    /* After a failure the old buffers are still there: wrap whatever the swapchain has. */
    de_result wr = wrap_images(d, ds);
    return r != DE_OK ? r : wr;
}

de_gpu_colorspace de__d3d12_swapchain_colorspace(de_gpu_device *d, de__gpu_swapchain *s) {
    (void)d;
    (void)s;
    return DE_CS_SRGB;
}
