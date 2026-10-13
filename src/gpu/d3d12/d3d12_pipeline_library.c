/* d3d12_pipeline_library.c — the persistent D3D12 pipeline library (DENG-324; SPEC §12.1).
 *
 * One ID3D12PipelineLibrary1 per device holds the PSOs it created, keyed by
 * de__d3d12_pipeline_key (the desc's stream contents) and named by that key in hex. At device
 * creation it starts from `<dir>/d3d12-<vendor>-<device>-<driver>.bin` (`dir` from
 * de_gpu_device_desc.pipeline_cache_dir: NULL = "cache:/pipelines", "" = no library), and at device
 * destroy, when this run stored new PSOs, ID3D12PipelineLibrary::Serialize is written back
 * atomically (de_file_write_all_atomic): a crash leaves the old file or the new one.
 *
 * File: a 48-byte de__d3d12_pipeline_library_file header (magic "DDPL", version, vendor and device
 * IDs, the user-mode driver version, key count, data size, XXH3 of keys + data), the keys, then the
 * serialized library. Before the data reaches the runtime the header is checked against this
 * adapter (IDs, driver version) and the checksum against the bytes, so a damaged file or one made
 * by another driver is discarded with a warning before CreatePipelineLibrary sees it. The runtime
 * can still refuse a library that passed (D3D12_ERROR_DRIVER_VERSION_MISMATCH,
 * D3D12_ERROR_ADAPTER_NOT_FOUND, E_INVALIDARG): that is discarded with a warning too. A discarded
 * or missing file means an empty library, rebuilt by this run and saved at destroy.
 *
 * Lookup: the keys the library holds are kept sorted in memory, so LoadPipeline is only called for
 * a name the library has (a miss would be a debug-layer message). A hit loads the PSO; a miss
 * creates it with ID3D12Device2::CreatePipelineState and stores it. Library calls run under one
 * mutex; PSO creation itself runs outside it, so threads compile in parallel. Counters (hits,
 * misses) are test hooks.
 *
 * The loaded file stays in memory while the library lives: CreatePipelineLibrary does not copy it.
 */
#include "gpu/d3d12/d3d12_internal.h"

#include <stdio.h>
#include <string.h>

/* ---------- File checks (portable) ---------- */

bool de__d3d12_pipeline_library_check(const void *file, u64 size, u32 vendor_id, u32 device_id,
                                      u64 driver_version, de__d3d12_pipeline_library_view *out,
                                      char *why, size_t why_cap) {
    memset(out, 0, sizeof *out);
    de__d3d12_pipeline_library_file h;
    if (!file || size < sizeof h) {
        snprintf(why, why_cap, "%llu bytes is shorter than the %u-byte file header",
                 (unsigned long long)size, (unsigned)sizeof h);
        return false;
    }
    memcpy(&h, file, sizeof h);
    if (h.magic != DE__D3D12_PIPELINE_LIBRARY_MAGIC ||
        h.version != DE__D3D12_PIPELINE_LIBRARY_VERSION) {
        snprintf(why, why_cap, "not a DEngine D3D12 pipeline library (magic 0x%08x, version %u)",
                 h.magic, h.version);
        return false;
    }
    const u64 body = size - sizeof h;
    if ((u64)h.key_count * 8u > body || h.data_size != body - (u64)h.key_count * 8u) {
        snprintf(why, why_cap,
                 "truncated: the header says %u keys and %llu data bytes, the file has %llu bytes "
                 "after it",
                 h.key_count, (unsigned long long)h.data_size, (unsigned long long)body);
        return false;
    }
    const u8 *p = (const u8 *)file + sizeof h;
    if (de_hash64(p, (size_t)body, DE__D3D12_PIPELINE_LIBRARY_MAGIC) != h.data_hash) {
        snprintf(why, why_cap, "checksum mismatch: the data is damaged");
        return false;
    }
    if (h.vendor_id != vendor_id || h.device_id != device_id) {
        snprintf(why, why_cap, "made for vendor 0x%x device 0x%x, not this adapter", h.vendor_id,
                 h.device_id);
        return false;
    }
    if (h.driver_version != driver_version) {
        snprintf(why, why_cap,
                 "made by driver %u.%u.%u.%u, this adapter runs %u.%u.%u.%u: a library from "
                 "another driver version is rebuilt",
                 (unsigned)(h.driver_version >> 48), (unsigned)(h.driver_version >> 32) & 0xFFFFu,
                 (unsigned)(h.driver_version >> 16) & 0xFFFFu, (unsigned)h.driver_version & 0xFFFFu,
                 (unsigned)(driver_version >> 48), (unsigned)(driver_version >> 32) & 0xFFFFu,
                 (unsigned)(driver_version >> 16) & 0xFFFFu, (unsigned)driver_version & 0xFFFFu);
        return false;
    }
    out->keys = p;
    out->key_count = h.key_count;
    out->data = p + (size_t)h.key_count * 8u;
    out->data_size = h.data_size;
    return true;
}

bool de__d3d12_pipeline_library_path(char *out, size_t cap, const char *dir, u32 vendor_id,
                                     u32 device_id, u64 driver_version) {
    const int w =
        snprintf(out, cap, "%s/d3d12-%04x-%04x-%u.%u.%u.%u.bin", dir, vendor_id, device_id,
                 (unsigned)(driver_version >> 48), (unsigned)(driver_version >> 32) & 0xFFFFu,
                 (unsigned)(driver_version >> 16) & 0xFFFFu, (unsigned)driver_version & 0xFFFFu);
    if (w < 0 || (size_t)w >= cap) {
        if (cap)
            out[0] = '\0';
        return false;
    }
    return true;
}

/* ---------- The library (Windows) ---------- */
#if defined(_WIN32)

#define DEFAULT_DIR "cache:/pipelines"

static u64 rd64(const u8 *p) {
    u64 v = 0;
    for (u32 i = 0; i < 8; ++i)
        v |= (u64)p[i] << (8u * i);
    return v;
}

/* The user-mode driver version DXGI reports (CheckInterfaceSupport answers for IDXGIDevice on every
 * D3D adapter), or 0. */
static u64 driver_version(IDXGIAdapter1 *a) {
    LARGE_INTEGER v;
    v.QuadPart = 0;
    if (a && SUCCEEDED(IDXGIAdapter1_CheckInterfaceSupport(a, &IID_IDXGIDevice, &v)))
        return (u64)v.QuadPart;
    return 0;
}

static void key_name(u64 key, wchar_t out[17]) {
    static const char hex[] = "0123456789abcdef";
    for (u32 i = 0; i < 16; ++i)
        out[i] = (wchar_t)hex[(key >> (60u - 4u * i)) & 0xFu];
    out[16] = 0;
}

/* Binary search in the sorted key list; `*at` = where `key` is or goes. */
static bool find_key(const d3d12_pipe_state *ps, u64 key, u32 *at) {
    u32 lo = 0, hi = ps->key_count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (ps->keys[mid] < key)
            lo = mid + 1u;
        else
            hi = mid;
    }
    *at = lo;
    return lo < ps->key_count && ps->keys[lo] == key;
}

static bool insert_key(de_gpu_device *d, d3d12_pipe_state *ps, u64 key) {
    u32 at = 0;
    if (find_key(ps, key, &at))
        return true;
    if (ps->key_count == ps->key_cap) {
        const u32 cap = ps->key_cap ? ps->key_cap * 2u : 64u;
        u64 *k = (u64 *)DE_ALLOC(d->alloc, (size_t)cap * 8u, 8, DE_MEM_RENDER);
        if (!k)
            return false;
        if (ps->keys) {
            memcpy(k, ps->keys, (size_t)ps->key_count * 8u);
            DE_FREE(d->alloc, ps->keys, (size_t)ps->key_cap * 8u);
        }
        ps->keys = k;
        ps->key_cap = cap;
    }
    memmove(ps->keys + at + 1u, ps->keys + at, (size_t)(ps->key_count - at) * 8u);
    ps->keys[at] = key;
    ++ps->key_count;
    return true;
}

static void free_file(de_gpu_device *d, d3d12_pipe_state *ps) {
    if (ps->file)
        DE_FREE(d->alloc, ps->file, (size_t)ps->file_size + 1u);
    ps->file = NULL;
    ps->file_size = 0;
}

static bool create_library(de_gpu_device *d, d3d12_pipe_state *ps, const void *data, u64 size,
                           HRESULT *hr_out) {
    HRESULT hr =
        ID3D12Device1_CreatePipelineLibrary(ps->dev1, size ? data : NULL, (SIZE_T)size,
                                            &IID_ID3D12PipelineLibrary1, (void **)&ps->library);
    (void)d;
    if (hr_out)
        *hr_out = hr;
    if (FAILED(hr)) {
        ps->library = NULL;
        return false;
    }
    return true;
}

static const char *library_error(HRESULT hr) {
    if (hr == D3D12_ERROR_DRIVER_VERSION_MISMATCH)
        return "D3D12_ERROR_DRIVER_VERSION_MISMATCH: made by another driver version";
    if (hr == D3D12_ERROR_ADAPTER_NOT_FOUND)
        return "D3D12_ERROR_ADAPTER_NOT_FOUND: made on another adapter";
    if (hr == E_INVALIDARG)
        return "E_INVALIDARG: the runtime does not accept the data";
    return "the runtime refused it";
}

void de__d3d12_library_init(de_gpu_device *d, const char *dir) {
    d3d12_device *dd = de__d3d12(d);
    d3d12_pipe_state *ps = dd->pipe;
    ps->library_state = DE__D3D12_LIBRARY_OFF;
    ps->path[0] = '\0';
    if (!dir)
        dir = DEFAULT_DIR;
    if (!dir[0] || !ps->dev1)
        return;
    D3D12_FEATURE_DATA_SHADER_CACHE sc;
    memset(&sc, 0, sizeof sc);
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dd->dev, D3D12_FEATURE_SHADER_CACHE, &sc,
                                                   sizeof sc)) &&
        !(sc.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY)) {
        DE_INFO(dd->log, "d3d12: \"%s\" has no pipeline library support; pipelines are not cached",
                dd->features.adapter_name);
        return;
    }
    ps->driver_version = driver_version(dd->adapter);
    if (!de__d3d12_pipeline_library_path(ps->path, sizeof ps->path, dir, dd->features.vendor_id,
                                         dd->features.device_id, ps->driver_version)) {
        DE_WARN(dd->log, "d3d12: pipeline library directory \"%s\" is too long; no library", dir);
        return;
    }

    void *file = NULL;
    u64 size = 0;
    de_result r = de_file_read_all(ps->path, d->alloc, &file, &size);
    HRESULT hr = S_OK;
    if (r != DE_OK) {
        de_error_clear(); /* a missing file is the normal first run */
        DE_INFO(dd->log, "d3d12: no pipeline library at %s (%s); starting empty", ps->path,
                de_result_str(r));
        ps->library_state = DE__D3D12_LIBRARY_EMPTY;
    } else {
        char why[256];
        de__d3d12_pipeline_library_view v;
        if (!de__d3d12_pipeline_library_check(file, size, dd->features.vendor_id,
                                              dd->features.device_id, ps->driver_version, &v, why,
                                              sizeof why)) {
            DE_WARN(dd->log, "d3d12: pipeline library %s discarded: %s", ps->path, why);
            ps->library_state = DE__D3D12_LIBRARY_REJECTED;
        } else if (!create_library(d, ps, v.data, v.data_size, &hr)) {
            DE_WARN(dd->log, "d3d12: pipeline library %s discarded: %s (HRESULT 0x%08lx)", ps->path,
                    library_error(hr), (unsigned long)hr);
            de__d3d12_drain_messages(dd);
            ps->library_state = DE__D3D12_LIBRARY_REJECTED;
        } else {
            bool ok = true;
            for (u32 i = 0; i < v.key_count && ok; ++i)
                ok = insert_key(d, ps, rd64(v.keys + (size_t)i * 8u));
            ps->file = file;
            ps->file_size = size;
            file = NULL;
            ps->loaded_bytes = v.data_size;
            ps->library_state = DE__D3D12_LIBRARY_LOADED;
            if (!ok) { /* out of memory for the key list: forget the file's contents */
                ID3D12PipelineLibrary1_Release(ps->library);
                ps->library = NULL;
                ps->key_count = 0;
                free_file(d, ps);
                ps->loaded_bytes = 0;
                ps->library_state = DE__D3D12_LIBRARY_REJECTED;
                DE_WARN(dd->log, "d3d12: pipeline library %s discarded: out of memory", ps->path);
            } else {
                DE_INFO(dd->log, "d3d12: pipeline library %s loaded (%u pipelines, %llu bytes)",
                        ps->path, v.key_count, (unsigned long long)v.data_size);
            }
        }
        if (file)
            DE_FREE(d->alloc, file, (size_t)size + 1u);
    }
    if (!ps->library && !create_library(d, ps, NULL, 0, &hr)) {
        /* DXGI_ERROR_UNSUPPORTED: no library support (also with some debug configurations). */
        if (hr == DXGI_ERROR_UNSUPPORTED)
            DE_INFO(dd->log, "d3d12: pipeline libraries are not supported here; no library");
        else
            DE_WARN(dd->log, "d3d12: CreatePipelineLibrary failed (HRESULT 0x%08lx); no library",
                    (unsigned long)hr);
        de__d3d12_drain_messages(dd);
        ps->library_state = DE__D3D12_LIBRARY_OFF;
        ps->path[0] = '\0';
    }
}

/* Writes the library file: header + keys + Serialize. */
static void save(de_gpu_device *d, d3d12_pipe_state *ps) {
    d3d12_device *dd = de__d3d12(d);
    de_mutex_lock(&ps->lock);
    const SIZE_T n = ID3D12PipelineLibrary1_GetSerializedSize(ps->library);
    const size_t keys = (size_t)ps->key_count * 8u;
    de__d3d12_pipeline_library_file h;
    const size_t total = sizeof h + keys + (size_t)n;
    u8 *buf = n ? (u8 *)DE_ALLOC(d->alloc, total, 16, DE_MEM_RENDER) : NULL;
    HRESULT hr = buf ? ID3D12PipelineLibrary1_Serialize(ps->library, buf + sizeof h + keys, n)
                     : E_OUTOFMEMORY;
    if (buf)
        for (u32 i = 0; i < ps->key_count; ++i)
            for (u32 b = 0; b < 8; ++b)
                buf[sizeof h + (size_t)i * 8u + b] = (u8)(ps->keys[i] >> (8u * b));
    const u32 key_count = ps->key_count;
    de_mutex_unlock(&ps->lock);
    if (FAILED(hr)) {
        DE_WARN(dd->log, "d3d12: pipeline library %s not saved: %s (HRESULT 0x%08lx)", ps->path,
                n ? "Serialize failed" : "empty", (unsigned long)hr);
        if (buf)
            DE_FREE(d->alloc, buf, total);
        return;
    }
    memset(&h, 0, sizeof h);
    h.magic = DE__D3D12_PIPELINE_LIBRARY_MAGIC;
    h.version = DE__D3D12_PIPELINE_LIBRARY_VERSION;
    h.vendor_id = dd->features.vendor_id;
    h.device_id = dd->features.device_id;
    h.driver_version = ps->driver_version;
    h.key_count = key_count;
    h.data_size = n;
    h.data_hash = de_hash64(buf + sizeof h, keys + (size_t)n, DE__D3D12_PIPELINE_LIBRARY_MAGIC);
    memcpy(buf, &h, sizeof h);

    /* The directory: the path without its last component. */
    char dir[sizeof ps->path];
    snprintf(dir, sizeof dir, "%s", ps->path);
    char *slash = strrchr(dir, '/');
    if (slash && slash > dir && slash[-1] != ':' && slash[-1] != '/')
        *slash = '\0';
    else
        dir[0] = '\0';
    de_result r = dir[0] ? de_dir_create(dir) : DE_OK;
    if (r == DE_OK)
        r = de_file_write_all_atomic(ps->path, buf, total);
    if (r == DE_OK) {
        DE_INFO(dd->log, "d3d12: pipeline library %s saved (%u pipelines, %llu bytes)", ps->path,
                key_count, (unsigned long long)n);
    } else {
        DE_WARN(dd->log, "d3d12: pipeline library %s not saved: %s", ps->path, de_result_str(r));
        de_error_clear();
    }
    DE_FREE(d->alloc, buf, total);
}

void de__d3d12_library_shutdown(de_gpu_device *d) {
    d3d12_pipe_state *ps = de__d3d12(d)->pipe;
    if (!ps)
        return;
    if (ps->library) {
        if (ps->path[0] && de_atomic_load_u32(&ps->stored, DE_ATOMIC_RELAXED) > 0)
            save(d, ps);
        ID3D12PipelineLibrary1_Release(ps->library);
        ps->library = NULL;
    }
    free_file(d, ps);
    if (ps->keys)
        DE_FREE(d->alloc, ps->keys, (size_t)ps->key_cap * 8u);
    ps->keys = NULL;
    ps->key_count = ps->key_cap = 0;
}

HRESULT de__d3d12_library_pipeline(de_gpu_device *d, u64 key,
                                   const D3D12_PIPELINE_STATE_STREAM_DESC *stream,
                                   ID3D12PipelineState **out) {
    d3d12_pipe_state *ps = de__d3d12(d)->pipe;
    *out = NULL;
    wchar_t name[17];
    key_name(key, name);
    bool in_library = false;
    if (ps->library) {
        de_mutex_lock(&ps->lock);
        u32 at = 0;
        in_library = find_key(ps, key, &at);
        if (in_library && SUCCEEDED(ID3D12PipelineLibrary1_LoadPipeline(
                              ps->library, name, stream, &IID_ID3D12PipelineState, (void **)out))) {
            de_mutex_unlock(&ps->lock);
            de_atomic_fetch_add_u32(&ps->hits, 1u, DE_ATOMIC_RELAXED);
            return S_OK;
        }
        *out = NULL;
        de_mutex_unlock(&ps->lock);
    }
    HRESULT hr =
        ID3D12Device2_CreatePipelineState(ps->dev2, stream, &IID_ID3D12PipelineState, (void **)out);
    if (FAILED(hr)) {
        *out = NULL;
        return hr;
    }
    if (ps->library) {
        de_atomic_fetch_add_u32(&ps->misses, 1u, DE_ATOMIC_RELAXED);
        de_mutex_lock(&ps->lock);
        u32 at = 0;
        /* Not when the name is taken: by another thread meanwhile, or by an entry that did not
         * load (StorePipeline would refuse the duplicate name). */
        if (!in_library && !find_key(ps, key, &at) &&
            SUCCEEDED(ID3D12PipelineLibrary1_StorePipeline(ps->library, name, *out)) &&
            insert_key(d, ps, key))
            de_atomic_fetch_add_u32(&ps->stored, 1u, DE_ATOMIC_RELAXED);
        de_mutex_unlock(&ps->lock);
    }
    return S_OK;
}

u32 de__d3d12_pipeline_library_state(de_gpu_device *d, u32 *hits, u32 *misses, u64 *loaded_bytes,
                                     const char **path) {
    d3d12_pipe_state *ps = d && d->vt == &de__gpu_d3d12_vtable ? de__d3d12(d)->pipe : NULL;
    if (hits)
        *hits = ps ? de_atomic_load_u32(&ps->hits, DE_ATOMIC_RELAXED) : 0u;
    if (misses)
        *misses = ps ? de_atomic_load_u32(&ps->misses, DE_ATOMIC_RELAXED) : 0u;
    if (loaded_bytes)
        *loaded_bytes = ps ? ps->loaded_bytes : 0u;
    if (path)
        *path = ps ? ps->path : "";
    return ps ? ps->library_state : (u32)DE__D3D12_LIBRARY_OFF;
}

#endif /* _WIN32 */
