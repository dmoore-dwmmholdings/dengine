/* d3d12_draw.c — D3D12 pipeline binding, root constants, draws and dispatches (DENG-324;
 * SPEC §12.1).
 *
 * Root signature: every pipeline uses the global one (d3d12_root_signature.c). set_pipeline sets
 * it the first time a pipeline of a bind point (graphics, compute) is bound in a recording, not
 * per pipeline; root constants then survive pipeline changes, as Vulkan push constants do. The
 * recording is d3d12_cmd.serial (d3d12_cmd.c numbers each begin), so nothing has to be reset at
 * cmd_begin. The bindless heaps are already bound by cmd_begin (DENG-286), as heap-direct indexing
 * requires before the root signature.
 *
 * Push constants: SetGraphicsRoot32BitConstants or SetComputeRoot32BitConstants (parameter 0, from
 * offset 0), by the bound pipeline's kind.
 *
 * Indirect: ExecuteIndirect with command signatures made once per device: DRAW_INDEXED (20-byte
 * records, D3D12_DRAW_INDEXED_ARGUMENTS), DISPATCH (12) and, with mesh shaders, DISPATCH_MESH (12),
 * the layouts de_gpu.h documents for every backend. The count buffer is native (a u32 at
 * count_offset, capped at the max).
 *
 * Mesh: DispatchMesh through ID3D12GraphicsCommandList6, queried on the first mesh dispatch of a
 * list object.
 *
 * Scissor: de_gpu coordinates are pixels from the top-left of the render area, D3D12's own; the
 * rectangle is cut to 0 .. D3D12_VIEWPORT_BOUNDS_MAX (de__d3d12_scissor_rect).
 */
#include "gpu/d3d12/d3d12_internal.h"

#include <string.h>

#define INDEXED_STRIDE  20u /* sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) */
#define DISPATCH_STRIDE 12u /* sizeof(D3D12_DISPATCH_ARGUMENTS) */
#define MESH_STRIDE     12u /* sizeof(D3D12_DISPATCH_MESH_ARGUMENTS) */

_Static_assert(sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) == INDEXED_STRIDE, "indexed draw record");
_Static_assert(sizeof(D3D12_DISPATCH_ARGUMENTS) == DISPATCH_STRIDE, "dispatch record");
_Static_assert(sizeof(D3D12_DISPATCH_MESH_ARGUMENTS) == MESH_STRIDE, "mesh dispatch record");

static ID3D12CommandSignature *command_signature(d3d12_device *dd,
                                                 D3D12_INDIRECT_ARGUMENT_TYPE type, u32 stride,
                                                 const char *what) {
    D3D12_INDIRECT_ARGUMENT_DESC arg;
    memset(&arg, 0, sizeof arg);
    arg.Type = type;
    D3D12_COMMAND_SIGNATURE_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.ByteStride = stride;
    sd.NumArgumentDescs = 1;
    sd.pArgumentDescs = &arg;
    ID3D12CommandSignature *sig = NULL;
    HRESULT hr = ID3D12Device_CreateCommandSignature(dd->dev, &sd, NULL,
                                                     &IID_ID3D12CommandSignature, (void **)&sig);
    if (FAILED(hr)) {
        DE_WARN(dd->log, "d3d12: %s command signature: HRESULT 0x%08lx; those commands are refused",
                what, (unsigned long)hr);
        return NULL;
    }
    return sig;
}

void de__d3d12_command_signatures_init(de_gpu_device *d) {
    d3d12_device *dd = de__d3d12(d);
    d3d12_pipe_state *ps = dd->pipe;
    ps->sig_draw_indexed = command_signature(dd, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED,
                                             INDEXED_STRIDE, "draw-indexed");
    ps->sig_dispatch =
        command_signature(dd, D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH, DISPATCH_STRIDE, "dispatch");
    if (d->caps.mesh_shaders)
        ps->sig_dispatch_mesh = command_signature(dd, D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH,
                                                  MESH_STRIDE, "mesh-dispatch");
}

static ID3D12Resource *res_of(de__gpu_buffer *b) {
    return DE__GPU_PAYLOAD(d3d12_buffer, b)->res;
}

/* The open list of `c`, or NULL (a list that failed to begin records nothing). */
static d3d12_cmd *open_list(de_gpu_cmd *c) {
    d3d12_cmd *dc = de__d3d12_cmd(c);
    return dc->open ? dc : NULL;
}

void de__d3d12_cmd_set_pipeline(de_gpu_cmd *c, de__gpu_pipeline *p) {
    if (p->kind == DE_PIPE_RAYTRACING) {
        de__d3d12_cmd_set_rt_pipeline(c, p);
        return;
    }
    d3d12_cmd *dc = open_list(c);
    const d3d12_pipeline *dp = DE__GPU_PAYLOAD(d3d12_pipeline, p);
    const d3d12_pipe_state *ps = de__d3d12(c->device)->pipe;
    if (!dc || !dp->pso || !ps || !ps->root)
        return;
    const bool compute = dp->kind == DE_PIPE_COMPUTE;
    const u32 slot = compute ? 1u : 0u;
    if (dc->root_serial[slot] != dc->serial) {
        if (compute)
            ID3D12GraphicsCommandList_SetComputeRootSignature(dc->list, ps->root);
        else
            ID3D12GraphicsCommandList_SetGraphicsRootSignature(dc->list, ps->root);
        dc->root_serial[slot] = dc->serial;
    }
    ID3D12GraphicsCommandList_SetPipelineState(dc->list, dp->pso);
    if (dp->kind == DE_PIPE_GRAPHICS)
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(dc->list,
                                                         D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->bound_kind = dp->kind;
}

void de__d3d12_cmd_push_constants(de_gpu_cmd *c, const void *data, u32 bytes) {
    d3d12_cmd *dc = open_list(c);
    if (!dc)
        return;
    if (dc->bound_kind == DE_PIPE_COMPUTE || dc->bound_kind == DE_PIPE_RAYTRACING)
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(dc->list, 0, bytes / 4u, data, 0);
    else
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(dc->list, 0, bytes / 4u, data, 0);
}

void de__d3d12_cmd_draw(de_gpu_cmd *c, u32 vertex_count, u32 instance_count, u32 first_vertex,
                        u32 first_instance) {
    d3d12_cmd *dc = open_list(c);
    if (dc)
        ID3D12GraphicsCommandList_DrawInstanced(dc->list, vertex_count, instance_count,
                                                first_vertex, first_instance);
}

void de__d3d12_cmd_draw_indexed(de_gpu_cmd *c, u32 index_count, u32 instance_count, u32 first_index,
                                i32 vertex_offset, u32 first_instance) {
    d3d12_cmd *dc = open_list(c);
    if (dc)
        ID3D12GraphicsCommandList_DrawIndexedInstanced(dc->list, index_count, instance_count,
                                                       first_index, vertex_offset, first_instance);
}

void de__d3d12_cmd_set_index_buffer(de_gpu_cmd *c, de__gpu_buffer *b, u64 offset, bool index_u16) {
    d3d12_cmd *dc = open_list(c);
    if (!dc)
        return;
    D3D12_INDEX_BUFFER_VIEW v;
    v.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(res_of(b)) + offset;
    const u64 left = b->size > offset ? b->size - offset : 0u;
    v.SizeInBytes = left > 0xFFFFFFFFu ? 0xFFFFFFFFu : (UINT)left;
    v.Format = index_u16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    ID3D12GraphicsCommandList_IASetIndexBuffer(dc->list, &v);
}

void de__d3d12_cmd_scissor(de_gpu_cmd *c, i32 x, i32 y, u32 w, u32 h) {
    d3d12_cmd *dc = open_list(c);
    if (!dc)
        return;
    const de__d3d12_rect r = de__d3d12_scissor_rect(x, y, w, h);
    const D3D12_RECT rect = {r.left, r.top, r.right, r.bottom};
    ID3D12GraphicsCommandList_RSSetScissorRects(dc->list, 1, &rect);
}

void de__d3d12_cmd_dispatch(de_gpu_cmd *c, u32 x, u32 y, u32 z) {
    d3d12_cmd *dc = open_list(c);
    if (dc && x && y && z)
        ID3D12GraphicsCommandList_Dispatch(dc->list, x, y, z);
}

/* ExecuteIndirect with `sig`, or an error when the device has no such signature. */
static void execute_indirect(de_gpu_cmd *c, ID3D12CommandSignature *sig, const char *f,
                             de__gpu_buffer *args, u64 offset, de__gpu_buffer *count,
                             u64 count_offset, u32 max) {
    d3d12_cmd *dc = open_list(c);
    if (!dc || max == 0)
        return;
    if (!sig) {
        de__gpu_fail(c->device, DE_ERR_UNSUPPORTED,
                     "%s(\"%s\"): this D3D12 device has no command signature for it; nothing was "
                     "recorded",
                     f, c->name);
        return;
    }
    ID3D12GraphicsCommandList_ExecuteIndirect(dc->list, sig, max, res_of(args), offset,
                                              count ? res_of(count) : NULL,
                                              count ? count_offset : 0u);
}

void de__d3d12_cmd_dispatch_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset) {
    execute_indirect(c, de__d3d12(c->device)->pipe->sig_dispatch, "de_gpu_cmd_dispatch_indirect",
                     args, offset, NULL, 0, 1);
}

void de__d3d12_cmd_draw_indexed_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset,
                                         de__gpu_buffer *count, u64 count_offset, u32 max_draws) {
    execute_indirect(c, de__d3d12(c->device)->pipe->sig_draw_indexed,
                     "de_gpu_cmd_draw_indexed_indirect", args, offset, count, count_offset,
                     max_draws);
}

/* The list as ID3D12GraphicsCommandList6. The reference QueryInterface adds is dropped at once:
 * the same object stays alive through d3d12_cmd.list, so cmd_fini has nothing extra to release. */
static ID3D12GraphicsCommandList6 *list6(d3d12_cmd *dc) {
    if (!dc->list6) {
        ID3D12GraphicsCommandList6 *l6 = NULL;
        if (FAILED(ID3D12GraphicsCommandList_QueryInterface(
                dc->list, &IID_ID3D12GraphicsCommandList6, (void **)&l6)))
            return NULL;
        ID3D12GraphicsCommandList6_Release(l6);
        dc->list6 = l6;
    }
    return dc->list6;
}

void de__d3d12_cmd_dispatch_mesh(de_gpu_cmd *c, u32 x, u32 y, u32 z) {
    d3d12_cmd *dc = open_list(c);
    if (!dc)
        return;
    ID3D12GraphicsCommandList6 *l6 = list6(dc);
    if (!l6) {
        de__gpu_fail(c->device, DE_ERR_UNSUPPORTED,
                     "de_gpu_cmd_dispatch_mesh(\"%s\"): no ID3D12GraphicsCommandList6", c->name);
        return;
    }
    ID3D12GraphicsCommandList6_DispatchMesh(l6, x, y, z);
}

void de__d3d12_cmd_dispatch_mesh_indirect(de_gpu_cmd *c, de__gpu_buffer *args, u64 offset,
                                          de__gpu_buffer *count, u64 count_offset, u32 max) {
    execute_indirect(c, de__d3d12(c->device)->pipe->sig_dispatch_mesh,
                     "de_gpu_cmd_dispatch_mesh_indirect", args, offset, count, count_offset, max);
}
