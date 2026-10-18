#include "gpu/d3d12/d3d12_internal.h"
#include <string.h>
#include <wchar.h>
typedef struct rt_library {
    wchar_t name[32], original[128];
    D3D12_EXPORT_DESC export;
    D3D12_DXIL_LIBRARY_DESC library;
} rt_library;
static void library(rt_library *l, de_gpu_shader_blob b, u32 index) {
    memset(l, 0, sizeof *l);
    swprintf(l->name, 32, L"deRtStage%u", index);
    MultiByteToWideChar(CP_UTF8, 0, b.entry ? b.entry : "main", -1, l->original, 128);
    l->export.Name = l->name;
    l->export.ExportToRename = l->original;
    l->library.DXILLibrary = (D3D12_SHADER_BYTECODE){b.code, (SIZE_T)b.size};
    l->library.NumExports = 1;
    l->library.pExports = &l->export;
}
de_result de__d3d12_rt_pipeline_create(de_gpu_device *d, de__gpu_pipeline *p,
                                       const de_gpu_pipeline_desc *desc) {
    de_gpu_rt_desc r;
    de_gpu_rt_hit_group shorthand;
    de__gpu_rt_desc_init(desc, &r, &shorthand);
    ID3D12Device5 *device = NULL;
    HRESULT hr =
        ID3D12Device_QueryInterface(de__d3d12(d)->dev, &IID_ID3D12Device5, (void **)&device);
    if (FAILED(hr))
        return de__d3d12_fail(hr, "RT Device5");
    u32 capacity = 1 + r.miss_count + 2 * r.hit_group_count,
        total = 1 + r.miss_count + r.hit_group_count;
    rt_library *libs = DE_ALLOC(d->alloc, (size_t)capacity * sizeof *libs, 16, DE_MEM_RENDER);
    D3D12_STATE_SUBOBJECT *sub = DE_ALLOC(
        d->alloc, (size_t)(capacity + r.hit_group_count + 3) * sizeof *sub, 16, DE_MEM_RENDER);
    D3D12_HIT_GROUP_DESC *groups =
        DE_ALLOC(d->alloc, (size_t)(r.hit_group_count ? r.hit_group_count : 1) * sizeof *groups, 16,
                 DE_MEM_RENDER);
    wchar_t(*names)[32] = DE_ALLOC(d->alloc, (size_t)total * sizeof *names, 16, DE_MEM_RENDER);
    u8 *handles = DE_ALLOC(d->alloc, (size_t)total * D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, 16,
                           DE_MEM_RENDER);
    de_result result = DE_ERR_OUT_OF_MEMORY;
    u32 count = 0, nlibs = 0;
    if (!libs || !sub || !groups || !names || !handles)
        goto cleanup;
    for (u32 i = 0; i < 1 + r.miss_count; ++i) {
        library(&libs[nlibs], i ? r.miss[i - 1] : r.raygen, nlibs);
        memcpy(names[i], libs[nlibs].name, sizeof names[i]);
        sub[count++] = (D3D12_STATE_SUBOBJECT){D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,
                                               &libs[nlibs++].library};
    }
    for (u32 i = 0; i < r.hit_group_count; ++i) {
        D3D12_HIT_GROUP_DESC *g = &groups[i];
        memset(g, 0, sizeof *g);
        swprintf(names[1 + r.miss_count + i], 32, L"deRtHit%u", i);
        g->HitGroupExport = names[1 + r.miss_count + i];
        g->Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        library(&libs[nlibs], r.hit_groups[i].closest_hit, nlibs);
        g->ClosestHitShaderImport = libs[nlibs].name;
        sub[count++] = (D3D12_STATE_SUBOBJECT){D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,
                                               &libs[nlibs++].library};
        if (r.hit_groups[i].any_hit.code) {
            library(&libs[nlibs], r.hit_groups[i].any_hit, nlibs);
            g->AnyHitShaderImport = libs[nlibs].name;
            sub[count++] = (D3D12_STATE_SUBOBJECT){D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,
                                                   &libs[nlibs++].library};
        }
        sub[count++] = (D3D12_STATE_SUBOBJECT){D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, g};
    }
    D3D12_GLOBAL_ROOT_SIGNATURE root = {de__d3d12(d)->pipe->root};
    D3D12_RAYTRACING_SHADER_CONFIG shader = {r.payload_bytes, r.attribute_bytes};
    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline = {r.max_recursion};
    sub[count++] = (D3D12_STATE_SUBOBJECT){D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &root};
    sub[count++] =
        (D3D12_STATE_SUBOBJECT){D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader};
    sub[count++] =
        (D3D12_STATE_SUBOBJECT){D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline};
    D3D12_STATE_OBJECT_DESC object = {D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, count, sub};
    d3d12_pipeline *out = DE__GPU_PAYLOAD(d3d12_pipeline, p);
    out->kind = DE_PIPE_RAYTRACING;
    hr = ID3D12Device5_CreateStateObject(device, &object, &IID_ID3D12StateObject,
                                         (void **)&out->rt_state);
    if (FAILED(hr)) {
        result = de__d3d12_fail(hr, "CreateStateObject RT");
        goto cleanup;
    }
    ID3D12StateObjectProperties *properties = NULL;
    hr = ID3D12StateObject_QueryInterface(out->rt_state, &IID_ID3D12StateObjectProperties,
                                          (void **)&properties);
    if (FAILED(hr)) {
        result = de__d3d12_fail(hr, "RT identifiers");
        goto state_fail;
    }
    for (u32 i = 0; i < total; ++i) {
        const void *id = ID3D12StateObjectProperties_GetShaderIdentifier(properties, names[i]);
        if (!id) {
            result = de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "DXR missing shader identifier");
            ID3D12StateObjectProperties_Release(properties);
            goto state_fail;
        }
        memcpy(handles + (size_t)i * 32, id, 32);
    }
    ID3D12StateObjectProperties_Release(properties);
    result = de__gpu_rt_sbt_create(d, p, handles, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
                                   D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT,
                                   D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT, r.miss_count,
                                   r.hit_group_count);
state_fail:
    if (result != DE_OK) {
        ID3D12StateObject_Release(out->rt_state);
        out->rt_state = NULL;
    }
cleanup:
    if (libs)
        DE_FREE(d->alloc, libs, (size_t)capacity * sizeof *libs);
    if (sub)
        DE_FREE(d->alloc, sub, (size_t)(capacity + r.hit_group_count + 3) * sizeof *sub);
    if (groups)
        DE_FREE(d->alloc, groups,
                (size_t)(r.hit_group_count ? r.hit_group_count : 1) * sizeof *groups);
    if (names)
        DE_FREE(d->alloc, names, (size_t)total * sizeof *names);
    if (handles)
        DE_FREE(d->alloc, handles, (size_t)total * D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    ID3D12Device5_Release(device);
    return result;
}
void de__d3d12_cmd_set_rt_pipeline(de_gpu_cmd *c, de__gpu_pipeline *p) {
    d3d12_cmd *dc = de__d3d12_cmd(c);
    ID3D12GraphicsCommandList4 *list = NULL;
    if (SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(
            dc->list, &IID_ID3D12GraphicsCommandList4, (void **)&list))) {
        ID3D12GraphicsCommandList_SetComputeRootSignature(dc->list,
                                                          de__d3d12(c->device)->pipe->root);
        ID3D12GraphicsCommandList4_SetPipelineState1(list,
                                                     DE__GPU_PAYLOAD(d3d12_pipeline, p)->rt_state);
        ID3D12GraphicsCommandList4_Release(list);
        dc->bound_kind = DE_PIPE_RAYTRACING;
    }
}
void de__d3d12_cmd_dispatch_rays(de_gpu_cmd *c, u32 x, u32 y, u32 z) {
    de__gpu_pipeline *p = c->bound_pipeline;
    d3d12_cmd *dc = de__d3d12_cmd(c);
    ID3D12GraphicsCommandList4 *list = NULL;
    if (FAILED(ID3D12GraphicsCommandList_QueryInterface(dc->list, &IID_ID3D12GraphicsCommandList4,
                                                        (void **)&list)))
        return;
    ID3D12Resource *buffer = de__d3d12_buffer_resource(c->device, p->rt_sbt);
    D3D12_GPU_VIRTUAL_ADDRESS base = ID3D12Resource_GetGPUVirtualAddress(buffer);
    de__gpu_sbt_layout *l = &p->rt_layout;
    D3D12_DISPATCH_RAYS_DESC args = {0};
    args.RayGenerationShaderRecord = (D3D12_GPU_VIRTUAL_ADDRESS_RANGE){base, l->raygen_size};
    args.MissShaderTable = (D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE){base + l->miss_offset,
                                                                        l->miss_size, l->stride};
    args.HitGroupTable =
        (D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE){base + l->hit_offset, l->hit_size, l->stride};
    args.Width = x;
    args.Height = y;
    args.Depth = z;
    ID3D12GraphicsCommandList4_DispatchRays(list, &args);
    ID3D12GraphicsCommandList4_Release(list);
}
