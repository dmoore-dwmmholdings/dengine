/* d3d12_memory.c — device heaps, memory classes and requirements (DENG-285, on DENG-282).
 *
 * The front-end heap manager (gpu/memory/gpu_heap.h) sub-allocates ID3D12Heaps that this file
 * creates; resources are placed in them (d3d12_resources.c). A class's `backend_bits` hold the
 * D3D12 heap type (low byte) and heap flags (bits 8+).
 *
 * Classes by resource heap tier:
 *   - tier 2: GPU_ONLY (buffers and textures in one heap), UPLOAD, READBACK.
 *   - tier 1: a heap holds one category of resource, so GPU_ONLY serves buffers and two more
 *     GPU_ONLY classes serve "textures" (non render-target, non depth) and "rt_ds" textures.
 * UPLOAD and READBACK hold buffers only on both tiers.
 *
 * Texture-capable DEFAULT heaps whose size is a multiple of 4 MiB are created with the MSAA
 * placement alignment, so a block heap (256 MiB) can hold MSAA textures; other heaps use 64 KiB.
 */
#include "gpu/d3d12/d3d12_internal.h"

#include <stdio.h>
#include <string.h>

#define DE__D3D12_MSAA_ALIGN ((u64)D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT)

static u32 heap_bits(D3D12_HEAP_TYPE type, D3D12_HEAP_FLAGS flags) {
    return (u32)type | ((u32)flags << 8);
}

de_result de__d3d12_memory_init(de_gpu_device *d) {
    d3d12_device *dd = de__d3d12(d);
    const bool tier2 = dd->features.resource_heap_tier >= 2;
    de__gpu_mem_class_desc c;

    memset(&c, 0, sizeof c);
    c.memory = DE_MEM_GPU_ONLY;
    c.name = tier2 ? "gpu_only" : "gpu_only buffers";
    c.backend_bits =
        heap_bits(D3D12_HEAP_TYPE_DEFAULT, tier2 ? D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES
                                                 : D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
    DE_TRY(de__gpu_mem_configure_class(d, DE__GPU_MEM_CLASS_GPU_ONLY, &c));

    memset(&c, 0, sizeof c);
    c.memory = DE_MEM_UPLOAD;
    c.backend_bits = heap_bits(D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
    DE_TRY(de__gpu_mem_configure_class(d, DE__GPU_MEM_CLASS_UPLOAD, &c));

    memset(&c, 0, sizeof c);
    c.memory = DE_MEM_READBACK;
    c.backend_bits = heap_bits(D3D12_HEAP_TYPE_READBACK, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
    DE_TRY(de__gpu_mem_configure_class(d, DE__GPU_MEM_CLASS_READBACK, &c));

    if (tier2) {
        dd->class_textures = dd->class_rt_ds = DE__GPU_MEM_CLASS_GPU_ONLY;
        return DE_OK;
    }
    memset(&c, 0, sizeof c);
    c.memory = DE_MEM_GPU_ONLY;
    c.name = "gpu_only textures";
    c.backend_bits =
        heap_bits(D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES);
    DE_TRY(de__gpu_mem_add_class(d, &c, &dd->class_textures));
    c.name = "gpu_only rt_ds";
    c.backend_bits = heap_bits(D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES);
    DE_TRY(de__gpu_mem_add_class(d, &c, &dd->class_rt_ds));
    return DE_OK;
}

de_result de__d3d12_heap_create(de_gpu_device *d, de__gpu_heap *h,
                                const de__gpu_mem_class_desc *cls) {
    d3d12_device *dd = de__d3d12(d);
    const D3D12_HEAP_TYPE type = (D3D12_HEAP_TYPE)(cls->backend_bits & 0xffu);
    const D3D12_HEAP_FLAGS flags = (D3D12_HEAP_FLAGS)(cls->backend_bits >> 8);
    D3D12_HEAP_DESC hd;
    memset(&hd, 0, sizeof hd);
    hd.SizeInBytes = h->size;
    hd.Properties.Type = type;
    hd.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hd.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    const bool textures = type == D3D12_HEAP_TYPE_DEFAULT &&
                          flags != D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS &&
                          flags != D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    hd.Alignment = textures && h->size % DE__D3D12_MSAA_ALIGN == 0 ? DE__D3D12_MSAA_ALIGN : 0;
    hd.Flags = flags;
    d3d12_heap *p = DE__GPU_PAYLOAD(d3d12_heap, h);
    HRESULT hr = ID3D12Device_CreateHeap(dd->dev, &hd, &IID_ID3D12Heap, (void **)&p->heap);
    if (FAILED(hr))
        return de__d3d12_fail(hr, "d3d12: %s heap of %llu bytes", cls->name,
                              (unsigned long long)h->size);
    char name[64];
    snprintf(name, sizeof name, "de %s heap %u%s", cls->name, h->index,
             h->dedicated ? " (dedicated)" : "");
    de__d3d12_set_name((ID3D12Object *)p->heap, name);
    return DE_OK;
}

void de__d3d12_heap_destroy(de_gpu_device *d, de__gpu_heap *h) {
    (void)d;
    d3d12_heap *p = DE__GPU_PAYLOAD(d3d12_heap, h);
    if (p->heap)
        ID3D12Heap_Release(p->heap);
    p->heap = NULL;
}

/* ---------- Resource descs and requirements ---------- */

#define BINDLESS_BUFFER_USAGE                                                                      \
    (DE_USAGE_VERTEX | DE_USAGE_INDEX | DE_USAGE_CONSTANT | DE_USAGE_STORAGE | DE_USAGE_ACCEL_INPUT)

void de__d3d12_buffer_resource_desc(const de_gpu_buffer_desc *desc, D3D12_RESOURCE_DESC *out) {
    memset(out, 0, sizeof *out);
    out->Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    out->Width = desc->size;
    out->Height = 1;
    out->DepthOrArraySize = 1;
    out->MipLevels = 1;
    out->Format = DXGI_FORMAT_UNKNOWN;
    out->SampleDesc.Count = 1;
    out->Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    /* GPU_ONLY buffers with a bindless index get a UAV (shaders use RWStructuredBuffer); upload
     * and readback heaps allow no UAV. */
    if (desc->memory == DE_MEM_GPU_ONLY &&
        (desc->usage & (BINDLESS_BUFFER_USAGE | DE_USAGE_ACCEL_STORAGE)))
        out->Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
}

void de__d3d12_texture_resource_desc(const de_gpu_texture_desc *desc, D3D12_RESOURCE_DESC *out) {
    memset(out, 0, sizeof *out);
    out->Dimension = desc->dim == DE_TEX_3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                            : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    out->Width = desc->width;
    out->Height = desc->height;
    out->DepthOrArraySize = (UINT16)desc->depth_or_layers;
    out->MipLevels = (UINT16)desc->mip_count;
    out->Format = de__d3d12_format_resource(desc->format, desc->usage);
    out->SampleDesc.Count = desc->sample_count;
    out->Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_RESOURCE_FLAGS f = D3D12_RESOURCE_FLAG_NONE;
    if (desc->usage & DE_USAGE_RENDER_TARGET)
        f |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (desc->usage & DE_USAGE_DEPTH) {
        f |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (!(desc->usage & DE_USAGE_SAMPLED))
            f |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    }
    if (desc->usage & DE_USAGE_STORAGE)
        f |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    /* D3D12 multisampled resources must be render targets or depth buffers. */
    if (desc->sample_count > 1 && !(f & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        f |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    out->Flags = f;
}

u32 de__d3d12_texture_class(de_gpu_device *d, const D3D12_RESOURCE_DESC *rd) {
    d3d12_device *dd = de__d3d12(d);
    const bool rt_ds = (rd->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                     D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
    return rt_ds ? dd->class_rt_ds : dd->class_textures;
}

static de_result allocation_info(de_gpu_device *d, const D3D12_RESOURCE_DESC *rd, const char *what,
                                 de__gpu_mem_req *out) {
    D3D12_RESOURCE_ALLOCATION_INFO info;
    ID3D12Device_GetResourceAllocationInfo(de__d3d12(d)->dev, &info, 0, 1, rd);
    if (info.SizeInBytes == UINT64_MAX || info.SizeInBytes == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "d3d12: GetResourceAllocationInfo refused the %s desc (%llux%u, format %d)",
                       what, (unsigned long long)rd->Width, rd->Height, (int)rd->Format);
    out->size = info.SizeInBytes;
    out->align = info.Alignment ? info.Alignment : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    return DE_OK;
}

de_result de__d3d12_buffer_requirements(de_gpu_device *d, const de_gpu_buffer_desc *desc,
                                        de__gpu_mem_req *out) {
    D3D12_RESOURCE_DESC rd;
    de__d3d12_buffer_resource_desc(desc, &rd);
    DE_TRY(allocation_info(d, &rd, "buffer", out));
    out->class_mask = 1u << (u32)desc->memory; /* base class ids equal de_gpu_memory values */
    return DE_OK;
}

de_result de__d3d12_texture_requirements(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                         de__gpu_mem_req *out) {
    D3D12_RESOURCE_DESC rd;
    de__d3d12_texture_resource_desc(desc, &rd);
    DE_TRY(allocation_info(d, &rd, "texture", out));
    out->class_mask = 1u << de__d3d12_texture_class(d, &rd);
    return DE_OK;
}
