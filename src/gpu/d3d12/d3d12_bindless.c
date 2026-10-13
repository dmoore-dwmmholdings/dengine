/* d3d12_bindless.c — the bindless descriptor heaps (DENG-285).
 *
 * Two shader-visible heaps, bound once per command list (DENG-286) and indexed by shaders with
 * SM 6.6 ResourceDescriptorHeap[] / SamplerDescriptorHeap[] (shaders/common/bindless.slang):
 *   - CBV_SRV_UAV, DE__D3D12_RESOURCE_HEAP_SIZE (1,000,000) descriptors: sampled textures, storage
 *     texture mips and buffers, each space in its own range of one numbering (the front-end's
 *     shared numbering, gpu_backend.h), so a bindless index is the descriptor slot;
 *   - SAMPLER, DE__D3D12_BINDLESS_SAMPLERS (2048) descriptors, indexed by sampler index.
 * Each has a CPU-only mirror of the same size. A write creates the view in the mirror and copies
 * it into the shader-visible heap at the same slot. Slot 0 of each heap holds the null descriptor
 * (a null 2D texture SRV; a point/wrap sampler), DE_GPU_BINDLESS_INVALID.
 *
 * Views: sampled textures get an SRV of every mip and layer; storage textures a UAV per mip (all
 * layers). Buffers: GPU_ONLY buffers a UAV (bindless.slang reads buffers as RWStructuredBuffer),
 * UPLOAD buffers an SRV (an upload heap allows no UAV). A buffer with a stride gets a structured
 * view of that stride; stride 0 gives a raw (byte address) view.
 *
 * Writes come from any thread: each touches only its own slot (Create*View and CopyDescriptors
 * are free-threaded for distinct destinations).
 */
#include "gpu/d3d12/d3d12_internal.h"

#include <string.h>

D3D12_CPU_DESCRIPTOR_HANDLE de__d3d12_desc_cpu(const d3d12_desc_heap *h, u32 index, bool visible) {
    D3D12_CPU_DESCRIPTOR_HANDLE c = visible ? h->visible_cpu : h->mirror_cpu;
    c.ptr += (SIZE_T)index * h->increment;
    return c;
}

/* Copies slot `index` of the mirror into the shader-visible heap. */
static void publish(d3d12_device *dd, const d3d12_desc_heap *h, u32 index) {
    ID3D12Device_CopyDescriptorsSimple(dd->dev, 1, de__d3d12_desc_cpu(h, index, true),
                                       de__d3d12_desc_cpu(h, index, false), h->type);
}

static de_result heap_create(d3d12_device *dd, d3d12_desc_heap *h, D3D12_DESCRIPTOR_HEAP_TYPE type,
                             u32 count, const char *name) {
    h->type = type;
    h->count = count;
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    memset(&hd, 0, sizeof hd);
    hd.Type = type;
    hd.NumDescriptors = count;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = ID3D12Device_CreateDescriptorHeap(dd->dev, &hd, &IID_ID3D12DescriptorHeap,
                                                   (void **)&h->visible);
    if (FAILED(hr))
        return de__d3d12_fail(hr, "d3d12: shader-visible %s heap of %u descriptors", name, count);
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = ID3D12Device_CreateDescriptorHeap(dd->dev, &hd, &IID_ID3D12DescriptorHeap,
                                           (void **)&h->mirror);
    if (FAILED(hr))
        return de__d3d12_fail(hr, "d3d12: CPU %s heap of %u descriptors", name, count);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(h->visible, &h->visible_cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(h->visible, &h->visible_gpu);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(h->mirror, &h->mirror_cpu);
    h->increment = ID3D12Device_GetDescriptorHandleIncrementSize(dd->dev, type);
    de__d3d12_set_name((ID3D12Object *)h->visible, name);
    return DE_OK;
}

static void heap_release(d3d12_desc_heap *h) {
    if (h->visible)
        ID3D12DescriptorHeap_Release(h->visible);
    if (h->mirror)
        ID3D12DescriptorHeap_Release(h->mirror);
    memset(h, 0, sizeof *h);
}

void de__d3d12_sampler_desc(const de_gpu_sampler_desc *s, D3D12_SAMPLER_DESC *out) {
    memset(out, 0, sizeof *out);
    const D3D12_FILTER_REDUCTION_TYPE red = s->compare_reverse_z
                                                ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON
                                                : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    const UINT aniso =
        s->max_aniso > 1.0f ? (s->max_aniso >= 16.0f ? 16u : (UINT)s->max_aniso) : 1u;
    if (aniso > 1) {
        out->Filter = D3D12_ENCODE_ANISOTROPIC_FILTER(red);
    } else {
#define DE__FT(f) ((f) == DE_FILTER_LINEAR ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT)
        out->Filter =
            D3D12_ENCODE_BASIC_FILTER(DE__FT(s->min), DE__FT(s->mag), DE__FT(s->mip), red);
#undef DE__FT
    }
    static const D3D12_TEXTURE_ADDRESS_MODE addr[] = {
        [DE_ADDR_WRAP] = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        [DE_ADDR_CLAMP] = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        [DE_ADDR_MIRROR] = D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
        [DE_ADDR_BORDER] = D3D12_TEXTURE_ADDRESS_MODE_BORDER,
    };
    out->AddressU = addr[s->u];
    out->AddressV = addr[s->v];
    out->AddressW = addr[s->w];
    out->MaxAnisotropy = aniso;
    /* Reverse-Z (Decision: Math conventions): a sample passes when it is nearer, i.e. greater. */
    out->ComparisonFunc =
        s->compare_reverse_z ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_NEVER;
    out->MinLOD = 0.0f;
    out->MaxLOD = D3D12_FLOAT32_MAX;
}

de_result de__d3d12_bindless_init(de_gpu_device *d) {
    d3d12_device *dd = de__d3d12(d);
    DE_TRY(heap_create(dd, &dd->resources, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                       DE__D3D12_RESOURCE_HEAP_SIZE, "de bindless resources"));
    DE_TRY(heap_create(dd, &dd->samplers, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                       DE__D3D12_BINDLESS_SAMPLERS, "de bindless samplers"));

    /* Slot 0: DE_GPU_BINDLESS_INVALID reads a null texture (zeros) and a point/wrap sampler. */
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
    memset(&srv, 0, sizeof srv);
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(dd->dev, NULL, &srv,
                                          de__d3d12_desc_cpu(&dd->resources, 0, false));
    publish(dd, &dd->resources, 0);
    de_gpu_sampler_desc zero;
    memset(&zero, 0, sizeof zero);
    D3D12_SAMPLER_DESC sd;
    de__d3d12_sampler_desc(&zero, &sd);
    ID3D12Device_CreateSampler(dd->dev, &sd, de__d3d12_desc_cpu(&dd->samplers, 0, false));
    publish(dd, &dd->samplers, 0);
    return DE_OK;
}

void de__d3d12_bindless_destroy(de_gpu_device *d) {
    d3d12_device *dd = de__d3d12(d);
    heap_release(&dd->resources);
    heap_release(&dd->samplers);
}

void de__d3d12_bindless_write_buffer(de_gpu_device *d, u32 index, de__gpu_buffer *b) {
    d3d12_device *dd = de__d3d12(d);
    ID3D12Resource *res = DE__GPU_PAYLOAD(d3d12_buffer, b)->res;
    const u64 elements = b->stride ? b->size / b->stride : b->size / 4u;
    /* A buffer smaller than one element (a raw view counts 4-byte words) has nothing to view:
     * its slot gets a null typed descriptor. */
    ID3D12Resource *viewed = elements ? res : NULL;
    const u32 stride = elements ? b->stride : 0u;
    const DXGI_FORMAT fmt = !elements ? DXGI_FORMAT_R32_UINT
                            : stride  ? DXGI_FORMAT_UNKNOWN
                                      : DXGI_FORMAT_R32_TYPELESS;
    const bool raw = elements && !stride;
    const UINT count = (UINT)(elements ? elements : 1u);
    const D3D12_CPU_DESCRIPTOR_HANDLE dst = de__d3d12_desc_cpu(&dd->resources, index, false);
    if (b->memory == DE_MEM_GPU_ONLY) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
        memset(&uav, 0, sizeof uav);
        uav.Format = fmt;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = count;
        uav.Buffer.StructureByteStride = stride;
        uav.Buffer.Flags = raw ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
        ID3D12Device_CreateUnorderedAccessView(dd->dev, viewed, NULL, &uav, dst);
    } else {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv;
        memset(&srv, 0, sizeof srv);
        srv.Format = fmt;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = count;
        srv.Buffer.StructureByteStride = stride;
        srv.Buffer.Flags = raw ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
        ID3D12Device_CreateShaderResourceView(dd->dev, viewed, &srv, dst);
    }
    publish(dd, &dd->resources, index);
}

void de__d3d12_bindless_write_texture(de_gpu_device *d, u32 index, de__gpu_texture *t, u32 mip,
                                      bool storage) {
    d3d12_device *dd = de__d3d12(d);
    ID3D12Resource *res = DE__GPU_PAYLOAD(d3d12_texture, t)->res;
    const D3D12_CPU_DESCRIPTOR_HANDLE dst = de__d3d12_desc_cpu(&dd->resources, index, false);
    const UINT layers = t->depth_or_layers;
    if (storage) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
        memset(&uav, 0, sizeof uav);
        uav.Format = de__d3d12_format_uav(t->format);
        switch (t->dim) {
        case DE_TEX_3D:
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            uav.Texture3D.MipSlice = mip;
            uav.Texture3D.FirstWSlice = 0;
            uav.Texture3D.WSize = (UINT)-1; /* every slice of the mip */
            break;
        case DE_TEX_2D:
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Texture2D.MipSlice = mip;
            break;
        default: /* 2D arrays and cubes: every layer (face) */
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uav.Texture2DArray.MipSlice = mip;
            uav.Texture2DArray.ArraySize = layers;
            break;
        }
        ID3D12Device_CreateUnorderedAccessView(dd->dev, res, NULL, &uav, dst);
    } else {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv;
        memset(&srv, 0, sizeof srv);
        srv.Format = de__d3d12_format_srv(t->format);
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        const bool ms = t->sample_count > 1;
        switch (t->dim) {
        case DE_TEX_3D:
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            srv.Texture3D.MipLevels = t->mip_count;
            break;
        case DE_TEX_CUBE:
            if (layers == 6) {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srv.TextureCube.MipLevels = t->mip_count;
            } else {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                srv.TextureCubeArray.MipLevels = t->mip_count;
                srv.TextureCubeArray.NumCubes = layers / 6u;
            }
            break;
        case DE_TEX_2D_ARRAY:
            if (ms) {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
                srv.Texture2DMSArray.ArraySize = layers;
            } else {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                srv.Texture2DArray.MipLevels = t->mip_count;
                srv.Texture2DArray.ArraySize = layers;
            }
            break;
        default:
            if (ms) {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
            } else {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Texture2D.MipLevels = t->mip_count;
            }
            break;
        }
        ID3D12Device_CreateShaderResourceView(dd->dev, res, &srv, dst);
    }
    publish(dd, &dd->resources, index);
}

void de__d3d12_bindless_write_sampler(de_gpu_device *d, u32 index, de__gpu_sampler *s) {
    d3d12_device *dd = de__d3d12(d);
    D3D12_SAMPLER_DESC sd;
    de__d3d12_sampler_desc(&s->desc, &sd);
    ID3D12Device_CreateSampler(dd->dev, &sd, de__d3d12_desc_cpu(&dd->samplers, index, false));
    publish(dd, &dd->samplers, index);
}

u32 de__d3d12_descriptor_bytes(de_gpu_device *d, bool sampler_heap, bool visible, u32 index,
                               void *out, u32 cap) {
    if (!d || d->vt != &de__gpu_d3d12_vtable)
        return 0;
    d3d12_device *dd = de__d3d12(d);
    const d3d12_desc_heap *h = sampler_heap ? &dd->samplers : &dd->resources;
    if (index >= h->count || !out)
        return 0;
    /* CPU descriptor handles are addresses on every D3D12 implementation the tests run on
     * (WARP); reading the shader-visible heap is slow (write-combined) but allowed. */
    const D3D12_CPU_DESCRIPTOR_HANDLE c = de__d3d12_desc_cpu(h, index, visible);
    const u32 n = h->increment < cap ? h->increment : cap;
    memcpy(out, (const void *)c.ptr, n);
    return h->increment;
}
