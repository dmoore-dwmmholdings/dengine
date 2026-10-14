/* d3d12_device.c — the D3D12 backend: factory, adapters, debug layer, device, queues (DENG-285).
 *
 * Runtime: the D3D12 Agility SDK when the executable exports D3D12SDKVersion / D3D12SDKPath
 * (d3d12_agility_exports.h) and has the DLLs in <exe dir>/D3D12/; the OS runtime otherwise. The
 * backend works on both and reports what the runtime and the adapter support (d3d12_caps.c).
 *
 * Adapters: the DXGI adapters in high-performance order (IDXGIFactory6), hardware adapters first,
 * then software adapters: WARP, as DXGI lists it (the "Microsoft Basic Render Driver", flagged
 * SOFTWARE or not) or else from EnumWarpAdapter.
 * de_gpu_device_desc.adapter n >= 1 picks the n-th of that list, so a test reaches WARP as the
 * last one; 0 picks the best: among hardware adapters that create a device, the Base-tier capable
 * one (SM 6.6, binding tier 3) with the most dedicated VRAM, else the hardware adapter with the
 * most VRAM; WARP only when no hardware adapter creates a device.
 *
 * Debug: debug_layer enables the D3D12 debug layer (process-wide, and before the device: the
 * runtime removes devices that exist when it is enabled) and routes its messages to the `gpu` log
 * category through ID3D12InfoQueue1 (or, on runtimes without it, by draining the stored messages);
 * gpu_validation adds GPU-based validation. The counts are test hooks.
 *
 * Queues: direct (DE_QUEUE_GRAPHICS), compute, copy, each with an ID3D12Fence that carries the
 * front-end's internal timeline (gpu_backend.h). Submission, fences, command lists, barriers and
 * swapchains are in d3d12_sync.c, d3d12_cmd.c, d3d12_barriers.c and d3d12_swapchain.c (DENG-286).
 *
 * The IIDs of d3d12.h and dxgi1_6.h are defined in this file (initguid.h); nothing links
 * dxguid.lib, whose copy may be older than the Agility headers.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>

#include "gpu/d3d12/d3d12_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DE__D3D12_MAX_ADAPTERS 16u

/* ---------- Errors, names, debug messages ---------- */

static de_result map_hresult(HRESULT hr) {
    switch (hr) {
    case E_OUTOFMEMORY:
        return DE_ERR_OUT_OF_MEMORY;
    case E_INVALIDARG:
        return DE_ERR_INVALID_ARGUMENT;
    case DXGI_ERROR_DEVICE_REMOVED:
    case DXGI_ERROR_DEVICE_RESET:
    case DXGI_ERROR_DEVICE_HUNG:
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
        return DE_ERR_DEVICE_LOST;
    case DXGI_ERROR_UNSUPPORTED:
    case DXGI_ERROR_NOT_FOUND:
    case E_NOINTERFACE:
        return DE_ERR_UNSUPPORTED;
    default:
        return DE_ERR_INTERNAL;
    }
}

de_result de__d3d12_fail(HRESULT hr, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    return DE_FAIL(map_hresult(hr), "%s: HRESULT 0x%08lx", msg, (unsigned long)hr);
}

void de__d3d12_set_name(ID3D12Object *obj, const char *name) {
    if (!obj || !name || !name[0])
        return;
    wchar_t wide[128];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, (int)(sizeof wide / sizeof wide[0])) > 0)
        ID3D12Object_SetName(obj, wide);
}

static void count_message(d3d12_device *dd, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID id,
                          const char *text) {
    if (sev == D3D12_MESSAGE_SEVERITY_CORRUPTION || sev == D3D12_MESSAGE_SEVERITY_ERROR) {
        de_atomic_fetch_add_u32(&dd->debug_errors, 1, DE_ATOMIC_RELAXED);
        DE_ERROR(dd->log, "d3d12 debug layer: %s (message %d)", text ? text : "?", (int)id);
    } else if (sev == D3D12_MESSAGE_SEVERITY_WARNING) {
        de_atomic_fetch_add_u32(&dd->debug_warnings, 1, DE_ATOMIC_RELAXED);
        DE_WARN(dd->log, "d3d12 debug layer: %s (message %d)", text ? text : "?", (int)id);
    }
}

static void __stdcall on_message(D3D12_MESSAGE_CATEGORY cat, D3D12_MESSAGE_SEVERITY sev,
                                 D3D12_MESSAGE_ID id, LPCSTR text, void *ctx) {
    (void)cat;
    count_message((d3d12_device *)ctx, sev, id, text);
}

void de__d3d12_drain_messages(d3d12_device *dd) {
    if (!dd->info || dd->info_cookie)
        return; /* no debug layer, or the callback already saw every message */
    de_mutex_lock(&dd->info_lock);
    const UINT64 n = ID3D12InfoQueue_GetNumStoredMessages(dd->info);
    for (UINT64 i = 0; i < n; ++i) {
        _Alignas(8) unsigned char buf[2048];
        SIZE_T len = 0;
        if (FAILED(ID3D12InfoQueue_GetMessage(dd->info, i, NULL, &len)))
            continue;
        if (len <= sizeof buf) {
            D3D12_MESSAGE *m = (D3D12_MESSAGE *)(void *)buf;
            if (SUCCEEDED(ID3D12InfoQueue_GetMessage(dd->info, i, m, &len)))
                count_message(dd, m->Severity, m->ID, m->pDescription);
        } else {
            /* Too long for the buffer: still count it, by severity unknown -> as an error. */
            count_message(dd, D3D12_MESSAGE_SEVERITY_ERROR, D3D12_MESSAGE_ID_UNKNOWN,
                          "(message too long to show)");
        }
    }
    ID3D12InfoQueue_ClearStoredMessages(dd->info);
    de_mutex_unlock(&dd->info_lock);
}

/* ---------- Adapters and features ---------- */

/* A software adapter: the SOFTWARE flag, or the Microsoft Basic Render Driver (WARP's vendor and
 * device ids), which DXGI lists without that flag on some systems (Windows Server 2022). */
static bool is_software(const DXGI_ADAPTER_DESC1 *ad) {
    return (ad->Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
           (ad->VendorId == 0x1414u && ad->DeviceId == 0x8cu);
}

/* The adapter list (see the file comment). Returns the count; the caller releases them. */
static u32 list_adapters(IDXGIFactory4 *f, IDXGIAdapter1 **out, u32 cap) {
    IDXGIAdapter1 *all[DE__D3D12_MAX_ADAPTERS];
    u32 n = 0;
    IDXGIFactory6 *f6 = NULL;
    if (FAILED(IDXGIFactory4_QueryInterface(f, &IID_IDXGIFactory6, (void **)&f6)))
        f6 = NULL;
    for (UINT i = 0; n < DE__D3D12_MAX_ADAPTERS; ++i) {
        IDXGIAdapter1 *a = NULL;
        HRESULT hr =
            f6 ? IDXGIFactory6_EnumAdapterByGpuPreference(
                     f6, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1, (void **)&a)
               : IDXGIFactory4_EnumAdapters1(f, i, &a);
        if (FAILED(hr))
            break; /* DXGI_ERROR_NOT_FOUND: the end of the list */
        all[n++] = a;
    }
    if (f6)
        IDXGIFactory6_Release(f6);
    /* WARP, unless DXGI already listed it (as the Basic Render Driver, under another LUID). */
    bool listed = false;
    for (u32 i = 0; i < n && !listed; ++i) {
        DXGI_ADAPTER_DESC1 ad;
        listed = SUCCEEDED(IDXGIAdapter1_GetDesc1(all[i], &ad)) && is_software(&ad);
    }
    IDXGIAdapter1 *warp = NULL;
    if (!listed && n < DE__D3D12_MAX_ADAPTERS &&
        SUCCEEDED(IDXGIFactory4_EnumWarpAdapter(f, &IID_IDXGIAdapter1, (void **)&warp)))
        all[n++] = warp;
    /* Hardware adapters first, then software ones, each in DXGI's order. */
    u32 count = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (u32 i = 0; i < n; ++i) {
            if (!all[i])
                continue;
            DXGI_ADAPTER_DESC1 ad;
            const bool software =
                SUCCEEDED(IDXGIAdapter1_GetDesc1(all[i], &ad)) && is_software(&ad);
            if (software != (pass == 1))
                continue;
            if (count < cap)
                out[count++] = all[i];
            else
                IDXGIAdapter1_Release(all[i]);
            all[i] = NULL;
        }
    }
    return count;
}

static bool feature(ID3D12Device *dev, D3D12_FEATURE f, void *data, UINT size) {
    return SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, f, data, size));
}

static bool hdr_output(IDXGIAdapter1 *a) {
    bool hdr = false;
    IDXGIOutput *out = NULL;
    for (UINT i = 0; !hdr && SUCCEEDED(IDXGIAdapter1_EnumOutputs(a, i, &out)); ++i) {
        IDXGIOutput6 *o6 = NULL;
        if (SUCCEEDED(IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput6, (void **)&o6))) {
            DXGI_OUTPUT_DESC1 od;
            if (SUCCEEDED(IDXGIOutput6_GetDesc1(o6, &od)))
                hdr = od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            IDXGIOutput6_Release(o6);
        }
        IDXGIOutput_Release(out);
        out = NULL;
    }
    return hdr;
}

/* Fills everything but timestamp_frequency (a queue property). */
static void query_features(IDXGIFactory4 *factory, IDXGIAdapter1 *a, ID3D12Device *dev,
                           de__d3d12_features *f) {
    memset(f, 0, sizeof *f);
    DXGI_ADAPTER_DESC1 ad;
    if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &ad))) {
        if (WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, f->adapter_name,
                                (int)sizeof f->adapter_name, NULL, NULL) <= 0)
            f->adapter_name[0] = '\0';
        f->vendor_id = ad.VendorId;
        f->device_id = ad.DeviceId;
        f->dedicated_vram = ad.DedicatedVideoMemory;
        f->shared_memory = ad.SharedSystemMemory;
        f->software = is_software(&ad);
    }

    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
                                               D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                               D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl;
    memset(&fl, 0, sizeof fl);
    fl.NumFeatureLevels = (UINT)(sizeof levels / sizeof levels[0]);
    fl.pFeatureLevelsRequested = levels;
    if (feature(dev, D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl))
        f->feature_level = (u32)fl.MaxSupportedFeatureLevel;

    /* An older runtime refuses a shader model it does not know (E_INVALIDARG): ask from the newest
     * the headers know down to 6.0, then 5.1. */
    D3D12_FEATURE_DATA_SHADER_MODEL sm;
    for (u32 v = (u32)D3D_HIGHEST_SHADER_MODEL; v >= 0x60u; --v) {
        sm.HighestShaderModel = (D3D_SHADER_MODEL)v;
        if (feature(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)) {
            f->shader_model = (u32)sm.HighestShaderModel;
            break;
        }
    }
    if (!f->shader_model) {
        sm.HighestShaderModel = D3D_SHADER_MODEL_5_1;
        if (feature(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm))
            f->shader_model = (u32)sm.HighestShaderModel;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS o0;
    if (feature(dev, D3D12_FEATURE_D3D12_OPTIONS, &o0, sizeof o0)) {
        f->resource_binding_tier = (u32)o0.ResourceBindingTier;
        f->resource_heap_tier = (u32)o0.ResourceHeapTier;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1;
    if (feature(dev, D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof o1) && o1.WaveOps) {
        f->wave_lane_min = o1.WaveLaneCountMin;
        f->wave_lane_max = o1.WaveLaneCountMax;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5;
    if (feature(dev, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5))
        f->raytracing_tier = (u32)o5.RaytracingTier;
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7;
    if (feature(dev, D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7))
        f->mesh_shader_tier = (u32)o7.MeshShaderTier;
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12;
    if (feature(dev, D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof o12))
        f->enhanced_barriers = o12.EnhancedBarriersSupported != FALSE;

    IDXGIFactory5 *f5 = NULL;
    if (SUCCEEDED(IDXGIFactory4_QueryInterface(factory, &IID_IDXGIFactory5, (void **)&f5))) {
        BOOL tearing = FALSE;
        if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                        &tearing, sizeof tearing)))
            f->tearing = tearing != FALSE;
        IDXGIFactory5_Release(f5);
    }
    f->hdr_output = hdr_output(a);
}

/* Creates a device on `a` and queries its features. */
static HRESULT open_adapter(IDXGIFactory4 *factory, IDXGIAdapter1 *a, ID3D12Device **dev,
                            de__d3d12_features *f) {
    *dev = NULL;
    HRESULT hr =
        D3D12CreateDevice((IUnknown *)a, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)dev);
    if (SUCCEEDED(hr))
        query_features(factory, a, *dev, f);
    return hr;
}

/* Picks and opens the adapter (see the file comment). On success dd->adapter, dd->dev and
 * dd->features are set. */
static de_result select_adapter(d3d12_device *dd, u32 wanted) {
    IDXGIAdapter1 *list[DE__D3D12_MAX_ADAPTERS];
    const u32 n = list_adapters(dd->factory, list, DE__D3D12_MAX_ADAPTERS);
    de_result r = DE_OK;
    if (n == 0) {
        r = DE_FAIL(DE_ERR_UNSUPPORTED, "d3d12: DXGI lists no adapter (not even WARP)");
    } else if (wanted > n) {
        r = DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                    "d3d12: adapter %u requested; this machine has %u (1 .. %u, the last are "
                    "software adapters such as WARP)",
                    wanted, n, n);
    } else if (wanted > 0) {
        HRESULT hr = open_adapter(dd->factory, list[wanted - 1], &dd->dev, &dd->features);
        if (FAILED(hr)) {
            r = de__d3d12_fail(hr, "d3d12: D3D12CreateDevice on adapter %u", wanted);
        } else {
            dd->adapter = list[wanted - 1];
            list[wanted - 1] = NULL;
        }
    } else {
        /* Best: hardware first (the list is ordered), scored by Base tier, then VRAM. */
        u32 best = n;
        bool best_base = false, best_hw = false;
        u64 best_vram = 0;
        HRESULT last = E_FAIL;
        for (u32 i = 0; i < n; ++i) {
            ID3D12Device *dev = NULL;
            de__d3d12_features f;
            HRESULT hr = open_adapter(dd->factory, list[i], &dev, &f);
            if (FAILED(hr)) {
                last = hr;
                continue;
            }
            de_gpu_caps c;
            de__d3d12_caps_evaluate(&f, &c);
            const bool hw = !f.software;
            if (best_hw && !hw) {
                ID3D12Device_Release(dev);
                break; /* software adapters only matter when no hardware one works */
            }
            const bool better = best == n || (c.base_tier_ok && !best_base) ||
                                (c.base_tier_ok == best_base && f.dedicated_vram > best_vram);
            if (better) {
                if (dd->dev)
                    ID3D12Device_Release(dd->dev);
                dd->dev = dev;
                dd->features = f;
                best = i;
                best_base = c.base_tier_ok;
                best_hw = hw;
                best_vram = f.dedicated_vram;
            } else {
                ID3D12Device_Release(dev);
            }
        }
        if (best == n) {
            r = de__d3d12_fail(last, "d3d12: no adapter creates a device (%u tried)", n);
        } else {
            dd->adapter = list[best];
            list[best] = NULL;
        }
    }
    for (u32 i = 0; i < n; ++i)
        if (list[i])
            IDXGIAdapter1_Release(list[i]);
    return r;
}

/* ---------- Device ---------- */

static void enable_debug_layer(d3d12_device *dd, bool gpu_validation) {
    ID3D12Debug *dbg = NULL;
    HRESULT hr = D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&dbg);
    if (FAILED(hr)) {
        DE_WARN(dd->log,
                "d3d12: the debug layer is not available (HRESULT 0x%08lx): run with the Agility "
                "d3d12SDKLayers.dll in <exe dir>/D3D12/, or install Graphics Tools",
                (unsigned long)hr);
        return;
    }
    ID3D12Debug_EnableDebugLayer(dbg);
    if (gpu_validation) {
        ID3D12Debug1 *dbg1 = NULL;
        if (SUCCEEDED(ID3D12Debug_QueryInterface(dbg, &IID_ID3D12Debug1, (void **)&dbg1))) {
            ID3D12Debug1_SetEnableGPUBasedValidation(dbg1, TRUE);
            ID3D12Debug1_Release(dbg1);
        } else {
            DE_WARN(dd->log, "d3d12: GPU-based validation is not available");
        }
    }
    ID3D12Debug_Release(dbg);
}

static void setup_info_queue(d3d12_device *dd) {
    if (FAILED(ID3D12Device_QueryInterface(dd->dev, &IID_ID3D12InfoQueue, (void **)&dd->info))) {
        dd->info = NULL;
        return;
    }
    if (SUCCEEDED(
            ID3D12Device_QueryInterface(dd->dev, &IID_ID3D12InfoQueue1, (void **)&dd->info1))) {
        DWORD cookie = 0;
        if (SUCCEEDED(ID3D12InfoQueue1_RegisterMessageCallback(
                dd->info1, on_message, D3D12_MESSAGE_CALLBACK_FLAG_NONE, dd, &cookie)))
            dd->info_cookie = cookie;
    } else {
        dd->info1 = NULL;
    }
}

static de_result create_queues(d3d12_device *dd) {
    static const D3D12_COMMAND_LIST_TYPE types[DE_QUEUE_COUNT_] = {
        [DE_QUEUE_GRAPHICS] = D3D12_COMMAND_LIST_TYPE_DIRECT,
        [DE_QUEUE_COMPUTE] = D3D12_COMMAND_LIST_TYPE_COMPUTE,
        [DE_QUEUE_COPY] = D3D12_COMMAND_LIST_TYPE_COPY,
    };
    static const char *const names[DE_QUEUE_COUNT_] = {"de graphics queue", "de compute queue",
                                                       "de copy queue"};
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        d3d12_queue *dq = &dd->queues[q];
        D3D12_COMMAND_QUEUE_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Type = types[q];
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        HRESULT hr = ID3D12Device_CreateCommandQueue(dd->dev, &qd, &IID_ID3D12CommandQueue,
                                                     (void **)&dq->queue);
        if (FAILED(hr))
            return de__d3d12_fail(hr, "d3d12: %s", names[q]);
        de__d3d12_set_name((ID3D12Object *)dq->queue, names[q]);
        hr = ID3D12Device_CreateFence(dd->dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
                                      (void **)&dq->fence);
        if (FAILED(hr))
            return de__d3d12_fail(hr, "d3d12: timeline fence of the %s", names[q]);
        de_atomic_init_u64(&dq->signaled, 0);
    }
    UINT64 freq = 0;
    if (SUCCEEDED(
            ID3D12CommandQueue_GetTimestampFrequency(dd->queues[DE_QUEUE_GRAPHICS].queue, &freq)))
        dd->features.timestamp_frequency = freq;
    return DE_OK;
}

/* Releases whatever device_create made (also after a partial create). */
static void release_all(de_gpu_device *d) {
    d3d12_device *dd = de__d3d12(d);
    de__d3d12_pipelines_shutdown(d); /* DENG-324: saves the pipeline library */
    de__d3d12_bindless_destroy(d);
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        d3d12_queue *dq = &dd->queues[q];
        if (dq->fence)
            ID3D12Fence_Release(dq->fence);
        if (dq->queue)
            ID3D12CommandQueue_Release(dq->queue);
        dq->fence = NULL;
        dq->queue = NULL;
    }
    de__d3d12_drain_messages(dd);
    if (dd->info1) {
        if (dd->info_cookie)
            ID3D12InfoQueue1_UnregisterMessageCallback(dd->info1, dd->info_cookie);
        ID3D12InfoQueue1_Release(dd->info1);
    }
    if (dd->info)
        ID3D12InfoQueue_Release(dd->info);
    if (dd->dev10)
        ID3D12Device10_Release(dd->dev10);
    if (dd->dev)
        ID3D12Device_Release(dd->dev);
    if (dd->adapter)
        IDXGIAdapter1_Release(dd->adapter);
    if (dd->factory)
        IDXGIFactory4_Release(dd->factory);
    dd->info1 = NULL;
    dd->info_cookie = 0;
    dd->info = NULL;
    dd->dev10 = NULL;
    dd->dev = NULL;
    dd->adapter = NULL;
    dd->factory = NULL;
    de_mutex_destroy(&dd->info_lock);
}

/* Path of the D3D12Core.dll this process loaded (UTF-8), or "" before any device exists. */
static void runtime_path(char *out, size_t cap) {
    out[0] = '\0';
    HMODULE m = GetModuleHandleW(L"D3D12Core.dll");
    wchar_t wide[MAX_PATH];
    if (!m || GetModuleFileNameW(m, wide, MAX_PATH) == 0)
        return;
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)cap, NULL, NULL) <= 0)
        out[0] = '\0';
}

static de_result d3d12_device_create(de_gpu_device *d, const de_gpu_device_desc *desc,
                                     de_gpu_caps *caps) {
    d3d12_device *dd = de__d3d12(d);
    dd->log = d->log;
    de_atomic_init_u32(&dd->debug_errors, 0);
    de_atomic_init_u32(&dd->debug_warnings, 0);
    de_mutex_init(&dd->info_lock);
    const bool debug = desc->debug_layer || desc->gpu_validation;
    if (debug)
        enable_debug_layer(dd, desc->gpu_validation);

    HRESULT hr = CreateDXGIFactory2(debug ? DXGI_CREATE_FACTORY_DEBUG : 0, &IID_IDXGIFactory4,
                                    (void **)&dd->factory);
    if (FAILED(hr) && debug) /* no DXGI debug layer (Graphics Tools) installed */
        hr = CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&dd->factory);
    de_result r = DE_OK;
    if (FAILED(hr)) {
        dd->factory = NULL;
        r = de__d3d12_fail(hr, "d3d12: CreateDXGIFactory2");
    }
    if (r == DE_OK && desc->crash_breadcrumbs)
        de__d3d12_dred_enable(dd); /* DENG-328: DRED applies only to devices made after it */
    if (r == DE_OK)
        r = select_adapter(dd, desc->adapter);
    if (r == DE_OK) {
        if (FAILED(ID3D12Device_QueryInterface(dd->dev, &IID_ID3D12Device10, (void **)&dd->dev10)))
            dd->dev10 = NULL;
        if (debug)
            setup_info_queue(dd);
        r = create_queues(dd);
    }
    if (r == DE_OK) {
        de__d3d12_caps_evaluate(&dd->features, caps);
        r = de__d3d12_memory_init(d);
    }
    if (r == DE_OK)
        r = de__d3d12_bindless_init(d);
    if (r == DE_OK)
        r = de__d3d12_pipelines_init(d, desc->pipeline_cache_dir); /* DENG-324 */
    if (r != DE_OK) {
        release_all(d);
        return r;
    }

    const de__d3d12_features *f = &dd->features;
    char runtime[MAX_PATH];
    runtime_path(runtime, sizeof runtime);
    DE_INFO(dd->log,
            "d3d12: \"%s\" (vendor 0x%04x, device 0x%04x%s), %llu MiB dedicated; feature level "
            "%u_%u, shader model %u.%u, binding tier %u, heap tier %u, raytracing tier %u, mesh "
            "shader tier %u, enhanced barriers %s, ID3D12Device10 %s; runtime %s",
            f->adapter_name, f->vendor_id, f->device_id, f->software ? ", software" : "",
            (unsigned long long)(f->dedicated_vram >> 20), (f->feature_level >> 12) & 0xfu,
            (f->feature_level >> 8) & 0xfu, (f->shader_model >> 4) & 0xfu, f->shader_model & 0xfu,
            f->resource_binding_tier, f->resource_heap_tier, f->raytracing_tier,
            f->mesh_shader_tier, f->enhanced_barriers ? "yes" : "no", dd->dev10 ? "yes" : "no",
            runtime[0] ? runtime : "?");
    if (!caps->base_tier_ok)
        DE_WARN(dd->log, "d3d12: \"%s\" is below the Base tier: %s", f->adapter_name,
                caps->base_tier_reason);
    return DE_OK;
}

static void d3d12_wait_idle(de_gpu_device *d) {
    d3d12_device *dd = de__d3d12(d);
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        d3d12_queue *dq = &dd->queues[q];
        const u64 target = de_atomic_load_u64(&dq->signaled, DE_ATOMIC_ACQUIRE);
        /* A NULL event blocks until the fence reaches the value; a removed device completes
         * every value (GetCompletedValue is UINT64_MAX). */
        if (target && ID3D12Fence_GetCompletedValue(dq->fence) < target)
            ID3D12Fence_SetEventOnCompletion(dq->fence, target, NULL);
    }
}

static void d3d12_device_destroy(de_gpu_device *d) {
    d3d12_wait_idle(d);
    release_all(d);
}

/* ---------- Submission (d3d12_sync.c, DENG-286) ---------- */

static u64 d3d12_queue_completed(de_gpu_device *d, de_gpu_queue q) {
    return ID3D12Fence_GetCompletedValue(de__d3d12(d)->queues[q].fence);
}

/* ---------- The vtable ---------- */

const de__gpu_backend_vtable de__gpu_d3d12_vtable = {
    .name = "d3d12",
    .device_payload_size = sizeof(d3d12_device),
    .cmd_payload_size = sizeof(d3d12_cmd),
    .payload_size =
        {
            [DE__GPU_BUFFER] = sizeof(d3d12_buffer),
            [DE__GPU_TEXTURE] = sizeof(d3d12_texture),
            [DE__GPU_FENCE] = sizeof(d3d12_fence),
            [DE__GPU_SWAPCHAIN] = sizeof(d3d12_swapchain),
            [DE__GPU_PIPELINE] = sizeof(d3d12_pipeline),
            [DE__GPU_QUERY_HEAP] = sizeof(d3d12_query_heap), /* DENG-326 */
        },
    .device_create = d3d12_device_create,
    .device_destroy = d3d12_device_destroy,
    .wait_idle = d3d12_wait_idle,
    .buffer_create = de__d3d12_buffer_create,
    .buffer_destroy = de__d3d12_buffer_destroy,
    .texture_create = de__d3d12_texture_create,
    .texture_destroy = de__d3d12_texture_destroy,
    .sampler_create = de__d3d12_sampler_create,
    .bindless_write_buffer = de__d3d12_bindless_write_buffer,
    .bindless_write_texture = de__d3d12_bindless_write_texture,
    .bindless_write_sampler = de__d3d12_bindless_write_sampler,
    .cmd_init = de__d3d12_cmd_init,
    .cmd_fini = de__d3d12_cmd_fini,
    .cmd_begin = de__d3d12_cmd_begin,
    .cmd_barriers = de__d3d12_cmd_barriers,
    .cmd_begin_render = de__d3d12_cmd_begin_render,
    .cmd_end_render = de__d3d12_cmd_end_render,
    .cmd_viewport = de__d3d12_cmd_viewport,
    .cmd_copy_buffer = de__d3d12_cmd_copy_buffer,
    .cmd_copy_buffer_to_texture = de__d3d12_cmd_copy_buffer_to_texture,
    .queue_submit = de__d3d12_queue_submit,
    .queue_completed = d3d12_queue_completed,
    .fence_create = de__d3d12_fence_create,
    .fence_destroy = de__d3d12_fence_destroy,
    .fence_value = de__d3d12_fence_value,
    .fence_wait = de__d3d12_fence_wait,
    .swapchain_create = de__d3d12_swapchain_create,
    .swapchain_destroy = de__d3d12_swapchain_destroy,
    .swapchain_acquire = de__d3d12_swapchain_acquire,
    .swapchain_present = de__d3d12_swapchain_present,
    .swapchain_resize = de__d3d12_swapchain_resize,
    .swapchain_colorspace = de__d3d12_swapchain_colorspace,
    .heap_payload_size = sizeof(d3d12_heap),
    .heap_create = de__d3d12_heap_create,
    .heap_destroy = de__d3d12_heap_destroy,
    .buffer_requirements = de__d3d12_buffer_requirements,
    .texture_requirements = de__d3d12_texture_requirements,
    .buffer_create_placed = de__d3d12_buffer_create_placed,
    .texture_create_placed = de__d3d12_texture_create_placed,
    .cmd_copy_texture_to_buffer = de__d3d12_cmd_copy_texture_to_buffer,
    .bindless_shared_resource_heap = true,
    /* DENG-324: pipelines, draws, dispatches (d3d12_pipeline.c, d3d12_draw.c). */
    .pipeline_create = de__d3d12_pipeline_create,
    .pipeline_destroy = de__d3d12_pipeline_destroy,
    .cmd_set_pipeline = de__d3d12_cmd_set_pipeline,
    .cmd_push_constants = de__d3d12_cmd_push_constants,
    .cmd_draw = de__d3d12_cmd_draw,
    .cmd_draw_indexed = de__d3d12_cmd_draw_indexed,
    .cmd_set_index_buffer = de__d3d12_cmd_set_index_buffer,
    .cmd_scissor = de__d3d12_cmd_scissor,
    .cmd_dispatch = de__d3d12_cmd_dispatch,
    .cmd_dispatch_indirect = de__d3d12_cmd_dispatch_indirect,
    .cmd_draw_indexed_indirect = de__d3d12_cmd_draw_indexed_indirect,
    .cmd_dispatch_mesh = de__d3d12_cmd_dispatch_mesh,
    .cmd_dispatch_mesh_indirect = de__d3d12_cmd_dispatch_mesh_indirect,
    /* DENG-326: timestamps, markers, names (d3d12_query.c, d3d12_debug.c). */
    .query_heap_create = de__d3d12_query_heap_create,
    .query_heap_destroy = de__d3d12_query_heap_destroy,
    .cmd_timestamp = de__d3d12_cmd_timestamp,
    .cmd_resolve_queries = de__d3d12_cmd_resolve_queries,
    .cmd_marker_push = de__d3d12_cmd_marker_push,
    .cmd_marker_pop = de__d3d12_cmd_marker_pop,
    .object_name = de__d3d12_object_name,
    .queue_timestamp_frequency = de__d3d12_queue_timestamp_frequency,
    .queue_timestamp_now = de__d3d12_queue_timestamp_now,
    /* DENG-328: breadcrumbs, device lost, DRED (d3d12_dred.c). */
    .cmd_breadcrumb = de__d3d12_cmd_breadcrumb,
    .device_lost = de__d3d12_device_lost,
    .device_lost_report = de__d3d12_device_lost_report,
};

/* ---------- Test hooks ---------- */

static d3d12_device *as_d3d12(de_gpu_device *d) {
    return d && d->vt == &de__gpu_d3d12_vtable ? de__d3d12(d) : NULL;
}

u32 de__d3d12_debug_error_count(de_gpu_device *d) {
    d3d12_device *dd = as_d3d12(d);
    if (!dd)
        return 0;
    de__d3d12_drain_messages(dd);
    return de_atomic_load_u32(&dd->debug_errors, DE_ATOMIC_RELAXED);
}

u32 de__d3d12_debug_warning_count(de_gpu_device *d) {
    d3d12_device *dd = as_d3d12(d);
    if (!dd)
        return 0;
    de__d3d12_drain_messages(dd);
    return de_atomic_load_u32(&dd->debug_warnings, DE_ATOMIC_RELAXED);
}

bool de__d3d12_debug_layer_active(de_gpu_device *d) {
    d3d12_device *dd = as_d3d12(d);
    return dd && dd->info;
}

ID3D12Device *de__d3d12_native_device(de_gpu_device *d) {
    d3d12_device *dd = as_d3d12(d);
    return dd ? dd->dev : NULL;
}

ID3D12Resource *de__d3d12_buffer_resource(de_gpu_device *d, de_gpu_buffer b) {
    de__gpu_buffer *rec = as_d3d12(d) ? de__gpu_get_buffer(d, b) : NULL;
    return rec ? DE__GPU_PAYLOAD(d3d12_buffer, rec)->res : NULL;
}

ID3D12Resource *de__d3d12_texture_resource(de_gpu_device *d, de_gpu_texture t) {
    de__gpu_texture *rec = as_d3d12(d) ? de__gpu_get_texture(d, t) : NULL;
    return rec ? DE__GPU_PAYLOAD(d3d12_texture, rec)->res : NULL;
}

void de__d3d12_runtime_path(char *out, size_t cap) {
    if (out && cap)
        runtime_path(out, cap);
}
