#include "gpu/d3d12/d3d12_internal.h"
#include "gpu/gpu_accel_internal.h"
#include <string.h>
#include <d3dcompiler.h>
#include "gpu/d3d12/d3d12_accel_shader.h"

static d3d12_cmd *cmd(de_gpu_cmd *c) {
    return de__gpu_cmd_payload(c);
}
static u64 address(de__gpu_buffer *b) {
    return ID3D12Resource_GetGPUVirtualAddress(DE__GPU_PAYLOAD(d3d12_buffer, b)->res);
}
static u64 buffer_address(de_gpu_device *d, de_gpu_buffer h) {
    de__gpu_buffer *b = de__gpu_get_buffer(d, h);
    return b ? address(b) : 0;
}
static D3D12_RAYTRACING_GEOMETRY_DESC geometry(de_gpu_device *d, const de_gpu_blas_geometry *g) {
    D3D12_RAYTRACING_GEOMETRY_DESC r = {0};
    r.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    r.Flags = g->opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : 0;
    r.Triangles.VertexBuffer.StartAddress = buffer_address(d, g->vertices) + g->vertex_offset;
    r.Triangles.VertexBuffer.StrideInBytes = g->vertex_stride;
    r.Triangles.VertexCount = g->vertex_count;
    r.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    r.Triangles.IndexBuffer = g->indices.id ? buffer_address(d, g->indices) + g->index_offset : 0;
    r.Triangles.IndexCount = g->index_count;
    r.Triangles.IndexFormat = !g->indices.id ? DXGI_FORMAT_UNKNOWN
                              : g->index_u16 ? DXGI_FORMAT_R16_UINT
                                             : DXGI_FORMAT_R32_UINT;
    return r;
}
static D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs(const de_gpu_accel_desc *x) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS r = {0};
    r.Type = x->is_tlas ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL
                        : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    r.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    r.NumDescs = x->is_tlas ? x->max_instances : x->geometry_count;
    r.Flags =
        (x->allow_update ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE : 0) |
        (x->allow_compaction ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION
                             : 0) |
        (x->prefer_fast_trace
             ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
             : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD);
    return r;
}
static ID3D12GraphicsCommandList4 *list(de_gpu_cmd *c) {
    ID3D12GraphicsCommandList4 *p = NULL;
    HRESULT hr = ID3D12GraphicsCommandList_QueryInterface(
        cmd(c)->list, &IID_ID3D12GraphicsCommandList4, (void **)&p);
    if (FAILED(hr))
        de__d3d12_fail(hr, "d3d12: DXR command list");
    return p;
}
de_result de__d3d12_accel_sizes(de_gpu_device *d, const de_gpu_accel_desc *x,
                                de_gpu_accel_sizes *s) {
    if (!d->caps.ray_query)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "d3d12: DXR 1.1 unavailable");
    u64 primitives = 0;
    for (u32 i = 0; i < x->geometry_count; ++i)
        primitives += (x->geometries[i].indices.id ? x->geometries[i].index_count
                                                   : x->geometries[i].vertex_count) /
                      3;
    if (x->geometry_count >
            D3D12_RAYTRACING_MAX_GEOMETRIES_PER_BOTTOM_LEVEL_ACCELERATION_STRUCTURE ||
        x->max_instances > D3D12_RAYTRACING_MAX_INSTANCES_PER_TOP_LEVEL_ACCELERATION_STRUCTURE ||
        primitives > D3D12_RAYTRACING_MAX_PRIMITIVES_PER_BOTTOM_LEVEL_ACCELERATION_STRUCTURE)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "d3d12: AS exceeds DXR geometry/instance/primitive limit");
    ID3D12Device5 *dev = NULL;
    HRESULT hr = ID3D12Device_QueryInterface(de__d3d12(d)->dev, &IID_ID3D12Device5, (void **)&dev);
    if (FAILED(hr))
        return de__d3d12_fail(hr, "d3d12: DXR device");
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = inputs(x);
    D3D12_RAYTRACING_GEOMETRY_DESC *g = NULL;
    if (!x->is_tlas) {
        g = DE_ALLOC(d->alloc, (size_t)x->geometry_count * sizeof *g, 16, DE_MEM_RENDER);
        if (!g) {
            ID3D12Device5_Release(dev);
            return DE_ERR_OUT_OF_MEMORY;
        }
        for (u32 i = 0; i < x->geometry_count; ++i)
            g[i] = geometry(d, &x->geometries[i]);
        in.pGeometryDescs = g;
    }
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO p = {0};
    ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(dev, &in, &p);
    ID3D12Device5_Release(dev);
    if (g)
        DE_FREE(d->alloc, g, (size_t)x->geometry_count * sizeof *g);
    s->accel_bytes = p.ResultDataMaxSizeInBytes;
    s->build_scratch_bytes = p.ScratchDataSizeInBytes;
    s->update_scratch_bytes = p.UpdateScratchDataSizeInBytes;
    return s->accel_bytes ? DE_OK
                          : DE_FAIL(DE_ERR_INVALID_ARGUMENT, "d3d12: AS prebuild size is zero");
}
void de__d3d12_accel_destroy(de_gpu_device *d, de__gpu_accel *a) {
    d3d12_accel *v = DE__GPU_PAYLOAD(d3d12_accel, a);
    de__gpu_accel_buffer_free(d, v->storage);
    de__gpu_accel_buffer_free(d, v->postbuild);
    de__gpu_accel_buffer_free(d, v->readback);
    if (v->geometry)
        DE_FREE(d->alloc, v->geometry, (size_t)a->desc.geometry_count * sizeof *v->geometry);
    memset(v, 0, sizeof *v);
}
de_result de__d3d12_accel_create(de_gpu_device *d, de__gpu_accel *a, const de_gpu_accel_desc *x) {
    d3d12_accel *v = DE__GPU_PAYLOAD(d3d12_accel, a);
    v->storage =
        de__gpu_accel_buffer(d, a->sizes.accel_bytes, DE_USAGE_ACCEL_STORAGE, DE_MEM_GPU_ONLY);
    if (!v->storage)
        goto fail;
    if (!x->is_tlas) {
        v->geometry =
            DE_ALLOC(d->alloc, (size_t)x->geometry_count * sizeof *v->geometry, 16, DE_MEM_RENDER);
        if (!v->geometry)
            goto fail;
    }
    if (x->allow_compaction) {
        v->postbuild =
            de__gpu_accel_buffer(d, 8, DE_USAGE_STORAGE | DE_USAGE_COPY_SRC, DE_MEM_GPU_ONLY);
        v->readback = de__gpu_accel_buffer(d, 8, DE_USAGE_COPY_DST, DE_MEM_READBACK);
        if (!v->postbuild || !v->readback)
            goto fail;
    }
    return DE_OK;
fail:
    de__d3d12_accel_destroy(d, a);
    return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "d3d12: AS resources");
}
u64 de__d3d12_accel_address(de_gpu_device *d, de__gpu_accel *a) {
    (void)d;
    return address(DE__GPU_PAYLOAD(d3d12_accel, a)->storage);
}
/* AS resources stay in RTAS state for their whole lifetime. Enhanced barriers use a global
 * dependency; the legacy equivalent is a UAV barrier, never a transition out of RTAS. */
void de__d3d12_accel_dependency(de_gpu_cmd *c) {
    d3d12_cmd *dc = cmd(c);
    if (dc->enhanced) {
        D3D12_GLOBAL_BARRIER b = {0};
        b.SyncBefore = D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE |
                       D3D12_BARRIER_SYNC_COPY_RAYTRACING_ACCELERATION_STRUCTURE |
                       D3D12_BARRIER_SYNC_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO |
                       D3D12_BARRIER_SYNC_ALL_SHADING | D3D12_BARRIER_SYNC_RAYTRACING;
        b.SyncAfter = b.SyncBefore;
        b.AccessBefore = D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE |
                         D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ |
                         D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        b.AccessAfter = b.AccessBefore;
        D3D12_BARRIER_GROUP g = {0};
        g.Type = D3D12_BARRIER_TYPE_GLOBAL;
        g.NumBarriers = 1;
        g.pGlobalBarriers = &b;
        ID3D12GraphicsCommandList7_Barrier(dc->list7, 1, &g);
    } else {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ID3D12GraphicsCommandList_ResourceBarrier(dc->list, 1, &b);
    }
}
static void transition(de_gpu_cmd *c, de__gpu_buffer *b, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after) {
    d3d12_cmd *dc = cmd(c);
    d3d12_buffer *db = DE__GPU_PAYLOAD(d3d12_buffer, b);
    /* Buffers decay to COMMON after ExecuteCommandLists even after explicit transitions.
     * Within a recording use actual state, so repeated emits/uses remain valid. */
    if (!dc->enhanced)
        before = db->legacy_serial == dc->serial ? db->legacy_state : D3D12_RESOURCE_STATE_COMMON;
    if (dc->enhanced) {
        D3D12_BUFFER_BARRIER bb = {0};
        bb.SyncBefore = D3D12_BARRIER_SYNC_ALL;
        bb.SyncAfter = D3D12_BARRIER_SYNC_ALL;
        bb.AccessBefore = before == D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                              ? D3D12_BARRIER_ACCESS_UNORDERED_ACCESS
                              : D3D12_BARRIER_ACCESS_COMMON;
        bb.AccessAfter =
            after == D3D12_RESOURCE_STATE_UNORDERED_ACCESS ? D3D12_BARRIER_ACCESS_UNORDERED_ACCESS
            : after == D3D12_RESOURCE_STATE_COPY_SOURCE    ? D3D12_BARRIER_ACCESS_COPY_SOURCE
                                                           : D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        bb.pResource = DE__GPU_PAYLOAD(d3d12_buffer, b)->res;
        bb.Size = UINT64_MAX;
        D3D12_BARRIER_GROUP g = {0};
        g.Type = D3D12_BARRIER_TYPE_BUFFER;
        g.NumBarriers = 1;
        g.pBufferBarriers = &bb;
        ID3D12GraphicsCommandList7_Barrier(dc->list7, 1, &g);
    } else if (before != after) {
        D3D12_RESOURCE_BARRIER bb = {0};
        bb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bb.Transition.pResource = DE__GPU_PAYLOAD(d3d12_buffer, b)->res;
        bb.Transition.StateBefore = before;
        bb.Transition.StateAfter = after;
        bb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ID3D12GraphicsCommandList_ResourceBarrier(dc->list, 1, &bb);
    }
    db->legacy_serial = dc->serial;
    db->legacy_state = after;
}
static void build(de_gpu_cmd *c, de__gpu_accel *a, bool update, de__gpu_accel_scratch *s,
                  u32 count) {
    d3d12_accel *v = DE__GPU_PAYLOAD(d3d12_accel, a);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b = {0};
    b.Inputs = inputs(&a->desc);
    if (a->is_tlas) {
        b.Inputs.NumDescs = count;
        b.Inputs.InstanceDescs = address(s->instances);
    } else
        b.Inputs.pGeometryDescs = v->geometry;
    b.DestAccelerationStructureData = address(v->storage);
    b.ScratchAccelerationStructureData = address(s->scratch);
    if (update) {
        b.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        b.SourceAccelerationStructureData = b.DestAccelerationStructureData;
    }
    transition(c, s->scratch, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    de__d3d12_accel_dependency(c);
    ID3D12GraphicsCommandList4 *l = list(c);
    if (!l)
        return;
    ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(l, &b, 0, NULL);
    ID3D12GraphicsCommandList4_Release(l);
    de__d3d12_accel_dependency(c);
    a->built = true;
}
void de__d3d12_accel_blas(de_gpu_cmd *c, de__gpu_accel *a, bool update) {
    a->built = false;
    de__gpu_accel_scratch *s = de__gpu_accel_scratch_get(
        c, update ? a->sizes.update_scratch_bytes : a->sizes.build_scratch_bytes, 0);
    if (!s)
        return;
    d3d12_accel *v = DE__GPU_PAYLOAD(d3d12_accel, a);
    for (u32 i = 0; i < a->desc.geometry_count; ++i)
        v->geometry[i] = geometry(c->device, &a->geometries[i]);
    build(c, a, update, s, 0);
}
void de__d3d12_accel_tlas(de_gpu_cmd *c, de__gpu_accel *a, de__gpu_buffer *b, u32 n, bool update) {
    a->built = false;
    de__gpu_accel_scratch *s = de__gpu_accel_scratch_get(
        c, update ? a->sizes.update_scratch_bytes : a->sizes.build_scratch_bytes, n ? n : 1);
    if (!s)
        return;
    transition(c, s->instances, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!de__gpu_accel_translate_gpu(c, b, s, n))
        return;
    transition(c, s->instances, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    build(c, a, update, s, n);
}
void de__d3d12_accel_emit(de_gpu_cmd *c, de__gpu_accel *a) {
    d3d12_accel *v = DE__GPU_PAYLOAD(d3d12_accel, a);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC p = {0};
    p.DestBuffer = address(v->postbuild);
    p.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
    u64 src = address(v->storage);
    transition(c, v->postbuild, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    de__d3d12_accel_dependency(c);
    ID3D12GraphicsCommandList4 *l = list(c);
    if (!l)
        return;
    ID3D12GraphicsCommandList4_EmitRaytracingAccelerationStructurePostbuildInfo(l, &p, 1, &src);
    ID3D12GraphicsCommandList4_Release(l);
    transition(c, v->postbuild, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(
        cmd(c)->list, DE__GPU_PAYLOAD(d3d12_buffer, v->readback)->res, 0,
        DE__GPU_PAYLOAD(d3d12_buffer, v->postbuild)->res, 0, 8);
}
bool de__d3d12_accel_read(de_gpu_device *d, de__gpu_accel *a, u64 *bytes) {
    (void)d;
    memcpy(bytes, DE__GPU_PAYLOAD(d3d12_accel, a)->readback->mapped, 8);
    return *bytes != 0;
}
void de__d3d12_accel_compact(de_gpu_cmd *c, de__gpu_accel *src, de__gpu_accel *dst) {
    de__d3d12_accel_dependency(c);
    ID3D12GraphicsCommandList4 *l = list(c);
    if (!l)
        return;
    ID3D12GraphicsCommandList4_CopyRaytracingAccelerationStructure(
        l, de__d3d12_accel_address(c->device, dst), de__d3d12_accel_address(c->device, src),
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
    ID3D12GraphicsCommandList4_Release(l);
    de__d3d12_accel_dependency(c);
    dst->built = true;
}
void de__d3d12_bindless_write_accel(de_gpu_device *d, u32 index, de__gpu_accel *a) {
    d3d12_device *dd = de__d3d12(d);
    D3D12_SHADER_RESOURCE_VIEW_DESC s = {0};
    s.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    s.RaytracingAccelerationStructure.Location = de__d3d12_accel_address(d, a);
    ID3D12Device_CreateShaderResourceView(dd->dev, NULL, &s,
                                          de__d3d12_desc_cpu(&dd->resources, index, false));
    ID3D12Device_CopyDescriptorsSimple(dd->dev, 1, de__d3d12_desc_cpu(&dd->resources, index, true),
                                       de__d3d12_desc_cpu(&dd->resources, index, false),
                                       D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

bool de__d3d12_accel_translate(de_gpu_cmd *c, de__gpu_buffer *input, de__gpu_buffer *output,
                               de__gpu_buffer *addresses, u32 count, u32 table_count) {
    d3d12_cmd *dc = cmd(c);
    d3d12_device *dd = de__d3d12(c->device);
    if (!dc->accel_pipeline) {
        typedef HRESULT(WINAPI * compile_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                             ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **,
                                             ID3DBlob **);
        de_dylib *library = NULL;
        if (de_dylib_open("d3dcompiler_47.dll", &library) != DE_OK)
            return false;
        compile_fn compile = (compile_fn)de_dylib_function(library, "D3DCompile");
        ID3DBlob *code = NULL, *errors = NULL;
        HRESULT hr = compile
                         ? compile(de__accel_instances_hlsl, sizeof de__accel_instances_hlsl - 1,
                                   "accel_instances.slang", NULL, NULL, "main", "cs_5_0",
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)
                         : E_NOINTERFACE;
        if (errors)
            ID3D10Blob_Release(errors);
        if (FAILED(hr)) {
            de_dylib_close(library);
            de__d3d12_fail(hr, "d3d12: instance translation shader");
            return false;
        }
        D3D12_ROOT_PARAMETER params[4] = {0};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[0].Descriptor.ShaderRegister = 0;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 1;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[3].Constants.Num32BitValues = 5;
        D3D12_ROOT_SIGNATURE_DESC rs = {0};
        rs.NumParameters = 4;
        rs.pParameters = params;
        ID3DBlob *root = NULL;
        hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &root, NULL);
        if (SUCCEEDED(hr))
            hr = ID3D12Device_CreateRootSignature(
                dd->dev, 0, ID3D10Blob_GetBufferPointer(root), ID3D10Blob_GetBufferSize(root),
                &IID_ID3D12RootSignature, (void **)&dc->accel_root);
        if (root)
            ID3D10Blob_Release(root);
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps = {0};
        ps.pRootSignature = dc->accel_root;
        ps.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(code);
        ps.CS.BytecodeLength = ID3D10Blob_GetBufferSize(code);
        if (SUCCEEDED(hr))
            hr = ID3D12Device_CreateComputePipelineState(dd->dev, &ps, &IID_ID3D12PipelineState,
                                                         (void **)&dc->accel_pipeline);
        ID3D10Blob_Release(code);
        de_dylib_close(library);
        if (FAILED(hr)) {
            if (dc->accel_root) {
                ID3D12RootSignature_Release(dc->accel_root);
                dc->accel_root = NULL;
            }
            de__d3d12_fail(hr, "d3d12: translation pipeline");
            return false;
        }
    }
    const u32 pc[5] = {0, 0, 0, count, table_count};
    ID3D12GraphicsCommandList_SetComputeRootSignature(dc->list, dc->accel_root);
    ID3D12GraphicsCommandList_SetPipelineState(dc->list, dc->accel_pipeline);
    ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(dc->list, 0, address(input));
    ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(dc->list, 1, address(addresses));
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(dc->list, 2, address(output));
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(dc->list, 3, 5, pc, 0);
    if (count)
        ID3D12GraphicsCommandList_Dispatch(
            dc->list, (count + 63) / 64 > 65535 ? 65535 : (count + 63) / 64, 1, 1);
    dc->root_serial[1] = 0;
    return true;
}
