/* d3d12_resources.c — buffers, textures, samplers and formats (DENG-285).
 *
 * Every buffer and texture is a placed resource: its memory comes from the front-end heap manager
 * (de__gpu_mem_alloc, d3d12_memory.c), which gives a request above the class's dedicated threshold
 * a heap of its own; the resource is then placed at offset 0 of that heap, the same memory a
 * committed resource would get, but counted in the heap statistics. Transient resources are placed
 * where the render graph says (de__gpu_place_transient_*), without an allocation of their own.
 *
 * Initial states (legacy barriers; enhanced barriers treat them as the COMMON layout): GPU_ONLY
 * resources COMMON, UPLOAD buffers GENERIC_READ, READBACK buffers COPY_DEST. UPLOAD and READBACK
 * buffers stay mapped for their lifetime (b->mapped). Samplers have no D3D12 object: the
 * descriptor written into the sampler heap is the whole sampler (d3d12_bindless.c).
 *
 * A placed render target or depth buffer must be initialized (clear, copy or discard) before its
 * first use; the render graph does that (DENG-286, S12).
 */
#include "gpu/d3d12/d3d12_internal.h"

#include <string.h>

/* ---------- Formats ---------- */

static const DXGI_FORMAT k_formats[DE_FMT_COUNT_] = {
    [DE_FMT_UNKNOWN] = DXGI_FORMAT_UNKNOWN,
    [DE_FMT_R8_UNORM] = DXGI_FORMAT_R8_UNORM,
    [DE_FMT_RG8_UNORM] = DXGI_FORMAT_R8G8_UNORM,
    [DE_FMT_RGBA8_UNORM] = DXGI_FORMAT_R8G8B8A8_UNORM,
    [DE_FMT_RGBA8_SRGB] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
    [DE_FMT_BGRA8_UNORM] = DXGI_FORMAT_B8G8R8A8_UNORM,
    [DE_FMT_BGRA8_SRGB] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
    [DE_FMT_R16_FLOAT] = DXGI_FORMAT_R16_FLOAT,
    [DE_FMT_RG16_FLOAT] = DXGI_FORMAT_R16G16_FLOAT,
    [DE_FMT_RGBA16_FLOAT] = DXGI_FORMAT_R16G16B16A16_FLOAT,
    [DE_FMT_R32_UINT] = DXGI_FORMAT_R32_UINT,
    [DE_FMT_RG32_UINT] = DXGI_FORMAT_R32G32_UINT,
    [DE_FMT_R32_FLOAT] = DXGI_FORMAT_R32_FLOAT,
    [DE_FMT_RG32_FLOAT] = DXGI_FORMAT_R32G32_FLOAT,
    [DE_FMT_RGBA32_FLOAT] = DXGI_FORMAT_R32G32B32A32_FLOAT,
    [DE_FMT_R11G11B10_FLOAT] = DXGI_FORMAT_R11G11B10_FLOAT,
    [DE_FMT_RGB10A2_UNORM] = DXGI_FORMAT_R10G10B10A2_UNORM,
    [DE_FMT_D32_FLOAT] = DXGI_FORMAT_D32_FLOAT,
    [DE_FMT_D32_FLOAT_S8] = DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
    [DE_FMT_BC1_SRGB] = DXGI_FORMAT_BC1_UNORM_SRGB,
    [DE_FMT_BC3_SRGB] = DXGI_FORMAT_BC3_UNORM_SRGB,
    [DE_FMT_BC4_UNORM] = DXGI_FORMAT_BC4_UNORM,
    [DE_FMT_BC5_UNORM] = DXGI_FORMAT_BC5_UNORM,
    [DE_FMT_BC6H_UFLOAT] = DXGI_FORMAT_BC6H_UF16,
    [DE_FMT_BC7_UNORM] = DXGI_FORMAT_BC7_UNORM,
    [DE_FMT_BC7_SRGB] = DXGI_FORMAT_BC7_UNORM_SRGB,
};

static DXGI_FORMAT dxgi(de_gpu_format f) {
    return (u32)f < DE_FMT_COUNT_ ? k_formats[f] : DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT de__d3d12_format_resource(de_gpu_format f, u32 usage) {
    /* A depth buffer that is also sampled needs a typeless resource: the DSV and the SRV see it
     * with different formats. */
    if (usage & DE_USAGE_SAMPLED) {
        if (f == DE_FMT_D32_FLOAT)
            return DXGI_FORMAT_R32_TYPELESS;
        if (f == DE_FMT_D32_FLOAT_S8)
            return DXGI_FORMAT_R32G8X24_TYPELESS;
    }
    return dxgi(f);
}

DXGI_FORMAT de__d3d12_format_srv(de_gpu_format f) {
    if (f == DE_FMT_D32_FLOAT)
        return DXGI_FORMAT_R32_FLOAT;
    if (f == DE_FMT_D32_FLOAT_S8)
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    return dxgi(f);
}

DXGI_FORMAT de__d3d12_format_uav(de_gpu_format f) {
    return dxgi(f);
}

/* ---------- Buffers ---------- */

static de_result place_buffer(de_gpu_device *d, de__gpu_buffer *b, const de_gpu_buffer_desc *desc,
                              ID3D12Heap *heap, u64 offset) {
    d3d12_device *dd = de__d3d12(d);
    d3d12_buffer *db = DE__GPU_PAYLOAD(d3d12_buffer, b);
    D3D12_RESOURCE_DESC rd;
    de__d3d12_buffer_resource_desc(desc, &rd);
    const D3D12_RESOURCE_STATES state =
        desc->memory == DE_MEM_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
        : desc->memory == DE_MEM_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                          : D3D12_RESOURCE_STATE_COMMON;
    HRESULT hr = ID3D12Device_CreatePlacedResource(dd->dev, heap, offset, &rd, state, NULL,
                                                   &IID_ID3D12Resource, (void **)&db->res);
    if (FAILED(hr))
        return de__d3d12_fail(hr, "d3d12: buffer \"%s\" (%llu bytes)", b->name,
                              (unsigned long long)b->size);
    de__d3d12_set_name((ID3D12Object *)db->res, b->name);
    if (desc->memory != DE_MEM_GPU_ONLY) {
        /* Persistent mapping. The CPU never reads an upload buffer: empty read range. */
        const D3D12_RANGE none = {0, 0};
        hr = ID3D12Resource_Map(db->res, 0, desc->memory == DE_MEM_UPLOAD ? &none : NULL,
                                &b->mapped);
        if (FAILED(hr)) {
            ID3D12Resource_Release(db->res);
            db->res = NULL;
            return de__d3d12_fail(hr, "d3d12: mapping buffer \"%s\"", b->name);
        }
    }
    return DE_OK;
}

de_result de__d3d12_buffer_create(de_gpu_device *d, de__gpu_buffer *b,
                                  const de_gpu_buffer_desc *desc) {
    d3d12_buffer *db = DE__GPU_PAYLOAD(d3d12_buffer, b);
    de__gpu_mem_req req;
    DE_TRY(de__d3d12_buffer_requirements(d, desc, &req));
    de_result r =
        de__gpu_mem_alloc(d, (u32)desc->memory, req.size, req.align, DE_MEM_RENDER, &db->alloc);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "d3d12: memory for buffer \"%s\"", b->name);
    r = place_buffer(d, b, desc, DE__GPU_PAYLOAD(d3d12_heap, db->alloc.heap)->heap,
                     db->alloc.offset);
    if (r != DE_OK)
        de__gpu_mem_free(d, &db->alloc);
    return r;
}

de_result de__d3d12_buffer_create_placed(de_gpu_device *d, de__gpu_buffer *b,
                                         const de_gpu_buffer_desc *desc, de__gpu_heap *heap,
                                         u64 offset) {
    return place_buffer(d, b, desc, DE__GPU_PAYLOAD(d3d12_heap, heap)->heap, offset);
}

void de__d3d12_buffer_destroy(de_gpu_device *d, de__gpu_buffer *b) {
    d3d12_buffer *db = DE__GPU_PAYLOAD(d3d12_buffer, b);
    if (db->res)
        ID3D12Resource_Release(db->res); /* also unmaps */
    db->res = NULL;
    b->mapped = NULL;
    de__gpu_mem_free(d, &db->alloc); /* a zero record (placed transient buffer) is ignored */
}

/* ---------- Textures ---------- */

static de_result place_texture(de_gpu_device *d, de__gpu_texture *t,
                               const de_gpu_texture_desc *desc, ID3D12Heap *heap, u64 offset) {
    d3d12_device *dd = de__d3d12(d);
    d3d12_texture *dt = DE__GPU_PAYLOAD(d3d12_texture, t);
    D3D12_RESOURCE_DESC rd;
    de__d3d12_texture_resource_desc(desc, &rd);
    HRESULT hr =
        ID3D12Device_CreatePlacedResource(dd->dev, heap, offset, &rd, D3D12_RESOURCE_STATE_COMMON,
                                          NULL, &IID_ID3D12Resource, (void **)&dt->res);
    if (FAILED(hr))
        return de__d3d12_fail(hr, "d3d12: texture \"%s\" (%ux%ux%u, %u mips, %s)", t->name,
                              t->width, t->height, t->depth_or_layers, t->mip_count,
                              de__gpu_format_name(t->format));
    de__d3d12_set_name((ID3D12Object *)dt->res, t->name);
    return DE_OK;
}

de_result de__d3d12_texture_create(de_gpu_device *d, de__gpu_texture *t,
                                   const de_gpu_texture_desc *desc) {
    d3d12_texture *dt = DE__GPU_PAYLOAD(d3d12_texture, t);
    D3D12_RESOURCE_DESC rd;
    de__d3d12_texture_resource_desc(desc, &rd);
    de__gpu_mem_req req;
    DE_TRY(de__d3d12_texture_requirements(d, desc, &req));
    de_result r = de__gpu_mem_alloc(d, de__d3d12_texture_class(d, &rd), req.size, req.align,
                                    DE_MEM_RENDER, &dt->alloc);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "d3d12: memory for texture \"%s\"", t->name);
    r = place_texture(d, t, desc, DE__GPU_PAYLOAD(d3d12_heap, dt->alloc.heap)->heap,
                      dt->alloc.offset);
    if (r != DE_OK)
        de__gpu_mem_free(d, &dt->alloc);
    return r;
}

de_result de__d3d12_texture_create_placed(de_gpu_device *d, de__gpu_texture *t,
                                          const de_gpu_texture_desc *desc, de__gpu_heap *heap,
                                          u64 offset) {
    return place_texture(d, t, desc, DE__GPU_PAYLOAD(d3d12_heap, heap)->heap, offset);
}

void de__d3d12_texture_destroy(de_gpu_device *d, de__gpu_texture *t) {
    d3d12_texture *dt = DE__GPU_PAYLOAD(d3d12_texture, t);
    /* A wrapped swapchain image (t->external, DENG-286) holds its own reference in `res`, so
     * releasing it here is right for it too; it has no allocation. */
    if (dt->res)
        ID3D12Resource_Release(dt->res);
    dt->res = NULL;
    de__gpu_mem_free(d, &dt->alloc);
    de__d3d12_texture_legacy_release(d, t); /* DENG-286: legacy barrier states */
}

/* ---------- Samplers ---------- */

de_result de__d3d12_sampler_create(de_gpu_device *d, de__gpu_sampler *s,
                                   const de_gpu_sampler_desc *desc) {
    /* Nothing to create: de__d3d12_bindless_write_sampler writes the descriptor from s->desc. */
    (void)d;
    (void)s;
    (void)desc;
    return DE_OK;
}
