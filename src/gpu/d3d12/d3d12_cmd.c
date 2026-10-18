/* d3d12_cmd.c — D3D12 command lists: recording, render passes, viewports, copies (DENG-286;
 * SPEC §12.1).
 *
 * Allocators: every front-end command-list object owns one ID3D12CommandAllocator of its queue's
 * type and one ID3D12GraphicsCommandList. The front-end recycles a list (per queue) only after the
 * internal timeline value of its submission has completed, and one thread records a list at a
 * time, so the allocator is reset only when the GPU is done with it and needs no lock: this is
 * the per-thread, per-frame allocator pool of the issue, with the front-end's free lists as the
 * pool (as on Vulkan: Decision "Vulkan command pools live on command lists; present follows the
 * graphics timeline"). Nothing hangs off threads.
 *
 * Begin: Reset(allocator), Reset(list); direct and compute lists then bind the two shader-visible
 * bindless heaps (d3d12_bindless.c). Lists record enhanced barriers through
 * ID3D12GraphicsCommandList7 when the device uses them (d3d12_barriers.c).
 *
 * Render passes: begin_render writes one RTV per color attachment (the attachment's mip and layer)
 * into the list's own CPU-only RTV heap, and the DSV into its DSV heap; D3D12 reads the views when
 * OMSetRenderTargets and the clears are recorded, so the heaps are rewritten by the next pass.
 * Clears: ClearRenderTargetView / ClearDepthStencilView (depth, and stencil 0 for depth-stencil
 * formats). Viewport and scissor: the first attachment's mip size. `store` false needs nothing on
 * D3D12 (the render graph discards through barriers), so end_render records nothing. The caller's
 * barriers put color attachments in RENDER_TARGET and depth in DEPTH_WRITE.
 *
 * Viewport: D3D12's own convention, which every backend matches (y up in clip space, the
 * rectangle measured down from the top-left); minimum depth 0, maximum 1.
 *
 * Copies: CopyBufferRegion; buffer <-> texture with CopyTextureRegion and a placed footprint from
 * GetCopyableFootprints for the subresource, at the caller's offset and row pitch (the front-end
 * checks the 512 / 256 alignment). A 3D mip copies every slice; a depth-stencil texture copies its
 * depth plane.
 */
#include "gpu/d3d12/d3d12_internal.h"

#include <string.h>

static const D3D12_COMMAND_LIST_TYPE k_list_types[DE_QUEUE_COUNT_] = {
    [DE_QUEUE_GRAPHICS] = D3D12_COMMAND_LIST_TYPE_DIRECT,
    [DE_QUEUE_COMPUTE] = D3D12_COMMAND_LIST_TYPE_COMPUTE,
    [DE_QUEUE_COPY] = D3D12_COMMAND_LIST_TYPE_COPY,
};

/* ---------- Lifetime ---------- */

static de_result cpu_heap(d3d12_device *dd, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 count,
                          ID3D12DescriptorHeap **out, D3D12_CPU_DESCRIPTOR_HANDLE *start) {
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    memset(&hd, 0, sizeof hd);
    hd.Type = type;
    hd.NumDescriptors = count;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HRESULT hr =
        ID3D12Device_CreateDescriptorHeap(dd->dev, &hd, &IID_ID3D12DescriptorHeap, (void **)out);
    if (FAILED(hr)) {
        *out = NULL;
        return de__d3d12_fail(hr, "d3d12: command list %s heap",
                              type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ? "RTV" : "DSV");
    }
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(*out, start);
    return DE_OK;
}

de_result de__d3d12_cmd_init(de_gpu_device *d, de_gpu_cmd *c) {
    d3d12_device *dd = de__d3d12(d);
    d3d12_cmd *dc = de__d3d12_cmd(c);
    const D3D12_COMMAND_LIST_TYPE type = k_list_types[c->queue];
    dc->enhanced = de__d3d12_use_enhanced_barriers(d);
    HRESULT hr = ID3D12Device_CreateCommandAllocator(dd->dev, type, &IID_ID3D12CommandAllocator,
                                                     (void **)&dc->alloc);
    if (FAILED(hr)) {
        dc->alloc = NULL;
        return de__d3d12_fail(hr, "d3d12: CreateCommandAllocator (queue %d)", (int)c->queue);
    }
    /* Created open: close it, so every use starts with the same Reset. */
    hr = ID3D12Device_CreateCommandList(dd->dev, 0, type, dc->alloc, NULL,
                                        &IID_ID3D12GraphicsCommandList, (void **)&dc->list);
    if (FAILED(hr)) {
        dc->list = NULL;
        de__d3d12_cmd_fini(d, c);
        return de__d3d12_fail(hr, "d3d12: CreateCommandList (queue %d)", (int)c->queue);
    }
    ID3D12GraphicsCommandList_Close(dc->list);
    if (dc->enhanced && FAILED(ID3D12GraphicsCommandList_QueryInterface(
                            dc->list, &IID_ID3D12GraphicsCommandList7, (void **)&dc->list7))) {
        dc->list7 = NULL;
        de__d3d12_cmd_fini(d, c);
        return DE_FAIL(DE_ERR_UNSUPPORTED, "d3d12: enhanced barriers, but no "
                                           "ID3D12GraphicsCommandList7 (runtime too old)");
    }
    if (c->queue == DE_QUEUE_GRAPHICS) {
        de_result r = cpu_heap(dd, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, DE_GPU_MAX_COLOR_ATTACHMENTS,
                               &dc->rtv_heap, &dc->rtv_start);
        if (r == DE_OK)
            r = cpu_heap(dd, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, &dc->dsv_heap, &dc->dsv_start);
        if (r != DE_OK) {
            de__d3d12_cmd_fini(d, c);
            return r;
        }
        dc->rtv_increment =
            ID3D12Device_GetDescriptorHandleIncrementSize(dd->dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    return DE_OK;
}

void de__d3d12_cmd_fini(de_gpu_device *d, de_gpu_cmd *c) {
    (void)d;
    d3d12_cmd *dc = de__d3d12_cmd(c);
    if (dc->open && dc->list)
        ID3D12GraphicsCommandList_Close(dc->list);
    if (dc->accel_root)
        ID3D12RootSignature_Release(dc->accel_root);
    if (dc->accel_pipeline)
        ID3D12PipelineState_Release(dc->accel_pipeline);
    if (dc->rtv_heap)
        ID3D12DescriptorHeap_Release(dc->rtv_heap);
    if (dc->dsv_heap)
        ID3D12DescriptorHeap_Release(dc->dsv_heap);
    if (dc->list7)
        ID3D12GraphicsCommandList7_Release(dc->list7);
    if (dc->list)
        ID3D12GraphicsCommandList_Release(dc->list);
    if (dc->alloc)
        ID3D12CommandAllocator_Release(dc->alloc);
    memset(dc, 0, sizeof *dc);
}

de_result de__d3d12_cmd_begin(de_gpu_cmd *c, const char *name) {
    (void)name;
    de_gpu_device *d = c->device;
    d3d12_device *dd = de__d3d12(d);
    d3d12_cmd *dc = de__d3d12_cmd(c);
    if (dc->open) {
        /* Begun before and never submitted: drop that recording. */
        ID3D12GraphicsCommandList_Close(dc->list);
        dc->open = false;
    }
    /* The previous submission of this list has completed (front-end rule). */
    HRESULT hr = ID3D12CommandAllocator_Reset(dc->alloc);
    if (FAILED(hr))
        return de__d3d12_fail(hr, "d3d12: ID3D12CommandAllocator::Reset(\"%s\")", c->name);
    hr = ID3D12GraphicsCommandList_Reset(dc->list, dc->alloc, NULL);
    if (FAILED(hr))
        return de__d3d12_fail(hr, "d3d12: ID3D12GraphicsCommandList::Reset(\"%s\")", c->name);
    dc->open = true;
    dc->serial = de_atomic_fetch_add_u64(&dd->cmd_serial, 1, DE_ATOMIC_RELAXED) + 1u;
    if (c->name[0])
        de__d3d12_set_name((ID3D12Object *)dc->list, c->name);
    if (c->queue != DE_QUEUE_COPY && dd->resources.visible && dd->samplers.visible) {
        ID3D12DescriptorHeap *heaps[2] = {dd->resources.visible, dd->samplers.visible};
        ID3D12GraphicsCommandList_SetDescriptorHeaps(dc->list, 2, heaps);
    }
    return DE_OK;
}

de_result de__d3d12_cmd_close(de_gpu_cmd *c) {
    d3d12_cmd *dc = de__d3d12_cmd(c);
    if (!dc->open)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "d3d12: command list \"%s\" is not recording",
                       c->name);
    dc->open = false;
    HRESULT hr = ID3D12GraphicsCommandList_Close(dc->list);
    if (FAILED(hr))
        return de__d3d12_fail(hr,
                              "d3d12: ID3D12GraphicsCommandList::Close(\"%s\") (a command "
                              "recorded in it was invalid; see the debug layer)",
                              c->name);
    return DE_OK;
}

/* ---------- Render passes ---------- */

static u32 mip_dim(u32 v, u32 mip) {
    v >>= mip;
    return v ? v : 1u;
}

static void set_viewport(d3d12_cmd *dc, f32 x, f32 y, f32 w, f32 h) {
    D3D12_VIEWPORT vp;
    vp.TopLeftX = x;
    vp.TopLeftY = y;
    vp.Width = w;
    vp.Height = h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D12GraphicsCommandList_RSSetViewports(dc->list, 1, &vp);
}

/* The RTV of mip `mip`, layer `layer` (3D: slice) of `t`. */
static void rtv_desc(const de__gpu_texture *t, u32 mip, u32 layer,
                     D3D12_RENDER_TARGET_VIEW_DESC *v) {
    memset(v, 0, sizeof *v);
    v->Format = de__d3d12_format_resource(t->format, 0);
    const bool ms = t->sample_count > 1;
    if (t->dim == DE_TEX_3D) {
        v->ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        v->Texture3D.MipSlice = mip;
        v->Texture3D.FirstWSlice = layer;
        v->Texture3D.WSize = 1;
    } else if (t->depth_or_layers > 1) {
        if (ms) {
            v->ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
            v->Texture2DMSArray.FirstArraySlice = layer;
            v->Texture2DMSArray.ArraySize = 1;
        } else {
            v->ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            v->Texture2DArray.MipSlice = mip;
            v->Texture2DArray.FirstArraySlice = layer;
            v->Texture2DArray.ArraySize = 1;
        }
    } else if (ms) {
        v->ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    } else {
        v->ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        v->Texture2D.MipSlice = mip;
    }
}

static void dsv_desc(const de__gpu_texture *t, u32 mip, u32 layer,
                     D3D12_DEPTH_STENCIL_VIEW_DESC *v) {
    memset(v, 0, sizeof *v);
    v->Format = de__d3d12_format_resource(t->format, 0); /* D32 / D32S8, also for typeless */
    const bool ms = t->sample_count > 1;
    if (t->depth_or_layers > 1 && t->dim != DE_TEX_3D) {
        if (ms) {
            v->ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
            v->Texture2DMSArray.FirstArraySlice = layer;
            v->Texture2DMSArray.ArraySize = 1;
        } else {
            v->ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            v->Texture2DArray.MipSlice = mip;
            v->Texture2DArray.FirstArraySlice = layer;
            v->Texture2DArray.ArraySize = 1;
        }
    } else if (ms) {
        v->ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    } else {
        v->ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        v->Texture2D.MipSlice = mip;
    }
}

void de__d3d12_cmd_begin_render(de_gpu_cmd *c, const de_gpu_attachment *colors, u32 color_count,
                                const de_gpu_attachment *depth) {
    de_gpu_device *d = c->device;
    d3d12_device *dd = de__d3d12(d);
    d3d12_cmd *dc = de__d3d12_cmd(c);
    if (!dc->open || !dc->rtv_heap)
        return;
    if (color_count > DE_GPU_MAX_COLOR_ATTACHMENTS)
        color_count = DE_GPU_MAX_COLOR_ATTACHMENTS;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[DE_GPU_MAX_COLOR_ATTACHMENTS];
    u32 w = 0, h = 0, n = 0;
    for (u32 i = 0; i < color_count; ++i) {
        de__gpu_texture *t = de__gpu_get_texture(d, colors[i].texture);
        if (!t)
            continue; /* validated in Debug/Dev; a stale handle records nothing */
        D3D12_RENDER_TARGET_VIEW_DESC vd;
        rtv_desc(t, colors[i].mip, colors[i].layer, &vd);
        rtv[n] = dc->rtv_start;
        rtv[n].ptr += (SIZE_T)n * dc->rtv_increment;
        ID3D12Device_CreateRenderTargetView(dd->dev, DE__GPU_PAYLOAD(d3d12_texture, t)->res, &vd,
                                            rtv[n]);
        if (colors[i].clear)
            ID3D12GraphicsCommandList_ClearRenderTargetView(dc->list, rtv[n], colors[i].clear_color,
                                                            0, NULL);
        if (w == 0) {
            w = mip_dim(t->width, colors[i].mip);
            h = mip_dim(t->height, colors[i].mip);
        }
        ++n;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dc->dsv_start;
    bool has_depth = false;
    if (depth) {
        de__gpu_texture *t = de__gpu_get_texture(d, depth->texture);
        if (t) {
            D3D12_DEPTH_STENCIL_VIEW_DESC vd;
            dsv_desc(t, depth->mip, depth->layer, &vd);
            ID3D12Device_CreateDepthStencilView(dd->dev, DE__GPU_PAYLOAD(d3d12_texture, t)->res,
                                                &vd, dsv);
            has_depth = true;
            if (depth->clear) {
                D3D12_CLEAR_FLAGS f = D3D12_CLEAR_FLAG_DEPTH;
                if (t->format == DE_FMT_D32_FLOAT_S8)
                    f |= D3D12_CLEAR_FLAG_STENCIL;
                ID3D12GraphicsCommandList_ClearDepthStencilView(dc->list, dsv, f,
                                                                depth->clear_depth, 0, 0, NULL);
            }
            if (w == 0) {
                w = mip_dim(t->width, depth->mip);
                h = mip_dim(t->height, depth->mip);
            }
        }
    }
    ID3D12GraphicsCommandList_OMSetRenderTargets(dc->list, n, n ? rtv : NULL, FALSE,
                                                 has_depth ? &dsv : NULL);
    if (w) {
        set_viewport(dc, 0.0f, 0.0f, (f32)w, (f32)h);
        D3D12_RECT sc = {0, 0, (LONG)w, (LONG)h};
        ID3D12GraphicsCommandList_RSSetScissorRects(dc->list, 1, &sc);
    }
}

void de__d3d12_cmd_end_render(de_gpu_cmd *c) {
    /* Nothing to end: D3D12 has no render-pass object (the list's targets stay bound). */
    (void)c;
}

void de__d3d12_cmd_viewport(de_gpu_cmd *c, f32 x, f32 y, f32 w, f32 h) {
    d3d12_cmd *dc = de__d3d12_cmd(c);
    if (dc->open)
        set_viewport(dc, x, y, w, h);
}

/* ---------- Copies ---------- */

void de__d3d12_cmd_copy_buffer(de_gpu_cmd *c, de__gpu_buffer *dst, u64 dst_off, de__gpu_buffer *src,
                               u64 src_off, u64 bytes) {
    d3d12_cmd *dc = de__d3d12_cmd(c);
    if (!dc->open)
        return;
    ID3D12GraphicsCommandList_CopyBufferRegion(dc->list, DE__GPU_PAYLOAD(d3d12_buffer, dst)->res,
                                               dst_off, DE__GPU_PAYLOAD(d3d12_buffer, src)->res,
                                               src_off, bytes);
}

/* The two copy locations of a buffer <-> texture copy: subresource (mip, layer) of `t`, and the
 * placed footprint of that subresource at `offset` with rows `row_pitch` apart. */
static void copy_locations(de_gpu_device *d, de__gpu_texture *t, u32 mip, u32 layer,
                           de__gpu_buffer *b, u64 offset, u32 row_pitch,
                           D3D12_TEXTURE_COPY_LOCATION *tex, D3D12_TEXTURE_COPY_LOCATION *buf) {
    ID3D12Resource *res = DE__GPU_PAYLOAD(d3d12_texture, t)->res;
    D3D12_RESOURCE_DESC rd;
    ID3D12Resource_GetDesc(res, &rd);
    const de__d3d12_texture_shape s = de__d3d12_texture_shape_of(t);
    const u32 sub =
        de__d3d12_subresource(mip, t->dim == DE_TEX_3D ? 0u : layer, 0, s.mips, s.layers);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    ID3D12Device_GetCopyableFootprints(de__d3d12(d)->dev, &rd, sub, 1, 0, &fp, &rows, &row_bytes,
                                       &total);
    fp.Offset = offset;
    fp.Footprint.RowPitch = row_pitch;
    memset(tex, 0, sizeof *tex);
    tex->pResource = res;
    tex->Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    tex->SubresourceIndex = sub;
    memset(buf, 0, sizeof *buf);
    buf->pResource = DE__GPU_PAYLOAD(d3d12_buffer, b)->res;
    buf->Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    buf->PlacedFootprint = fp;
}

void de__d3d12_cmd_copy_buffer_to_texture(de_gpu_cmd *c, de__gpu_texture *dst, u32 mip, u32 layer,
                                          de__gpu_buffer *src, u64 src_off, u32 row_pitch) {
    d3d12_cmd *dc = de__d3d12_cmd(c);
    if (!dc->open)
        return;
    D3D12_TEXTURE_COPY_LOCATION tex, buf;
    copy_locations(c->device, dst, mip, layer, src, src_off, row_pitch, &tex, &buf);
    ID3D12GraphicsCommandList_CopyTextureRegion(dc->list, &tex, 0, 0, 0, &buf, NULL);
}

void de__d3d12_cmd_copy_texture_to_buffer(de_gpu_cmd *c, de__gpu_buffer *dst, u64 dst_off,
                                          u32 row_pitch, de__gpu_texture *src, u32 mip, u32 layer) {
    d3d12_cmd *dc = de__d3d12_cmd(c);
    if (!dc->open)
        return;
    D3D12_TEXTURE_COPY_LOCATION tex, buf;
    copy_locations(c->device, src, mip, layer, dst, dst_off, row_pitch, &tex, &buf);
    ID3D12GraphicsCommandList_CopyTextureRegion(dc->list, &buf, 0, 0, 0, &tex, NULL);
}
