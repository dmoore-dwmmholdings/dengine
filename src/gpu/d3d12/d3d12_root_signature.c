/* d3d12_root_signature.c — the one global D3D12 root signature (DENG-324; SPEC §12.1).
 *
 * Every graphics, mesh and compute pipeline uses it, so a command list sets it once per bind point
 * and recording (d3d12_draw.c) and the root constants survive pipeline changes:
 *   - flags CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED: shaders index the
 *     bindless heaps with ResourceDescriptorHeap[i] / SamplerDescriptorHeap[i] (SM 6.6,
 *     shaders/common/bindless.slang), so there are no descriptor tables;
 *   - one parameter: 32 root constants (DE_GPU_MAX_PUSH_CONSTANT_BYTES) at b0 space0, visible to
 *     every stage: the push constants of shaders/common/push_constants.slang.
 * No input-assembler layout (vertex pulling), no static samplers.
 *
 * It is serialized once (version 1.1) and created at device creation. Heap-direct indexing needs
 * shader model 6.6 and resource binding tier 3: on a device without them (CI's WARP has SM 6.2)
 * nothing is created, because the runtime would refuse it with a debug-layer error, and
 * pipeline_create reports DE_ERR_UNSUPPORTED with the reason.
 */
#include "gpu/d3d12/d3d12_internal.h"

#include <stdio.h>
#include <string.h>

de_result de__d3d12_root_signature_serialize(ID3DBlob **out) {
    *out = NULL;
    D3D12_ROOT_PARAMETER1 param;
    memset(&param, 0, sizeof param);
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.ShaderRegister = 0;
    param.Constants.RegisterSpace = 0;
    param.Constants.Num32BitValues = DE_GPU_MAX_PUSH_CONSTANT_BYTES / 4u;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC vd;
    memset(&vd, 0, sizeof vd);
    vd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    vd.Desc_1_1.NumParameters = 1;
    vd.Desc_1_1.pParameters = &param;
    vd.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                        D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    ID3DBlob *err = NULL;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&vd, out, &err);
    if (FAILED(hr)) {
        char msg[200] = "";
        if (err && ID3D10Blob_GetBufferPointer(err))
            snprintf(msg, sizeof msg, "%.*s", (int)ID3D10Blob_GetBufferSize(err),
                     (const char *)ID3D10Blob_GetBufferPointer(err));
        if (err)
            ID3D10Blob_Release(err);
        *out = NULL;
        return de__d3d12_fail(hr, "d3d12: D3D12SerializeVersionedRootSignature: %s", msg);
    }
    if (err)
        ID3D10Blob_Release(err);
    return DE_OK;
}

void de__d3d12_root_signature_init(de_gpu_device *d) {
    d3d12_device *dd = de__d3d12(d);
    d3d12_pipe_state *ps = dd->pipe;
    const de__d3d12_features *f = &dd->features;
    ps->root = NULL;
    ps->root_why[0] = '\0';
    if (f->shader_model < DE__D3D12_SHADER_MODEL_6_6 ||
        f->resource_binding_tier < DE__D3D12_BINDING_TIER_3) {
        snprintf(ps->root_why, sizeof ps->root_why,
                 "d3d12: \"%s\" has shader model %u.%u and resource binding tier %u; pipelines "
                 "need 6.6 and tier 3 (bindless heap indexing)",
                 f->adapter_name, (f->shader_model >> 4) & 0xFu, f->shader_model & 0xFu,
                 f->resource_binding_tier);
        DE_INFO(dd->log, "%s: no root signature, no pipelines", ps->root_why);
        return;
    }
    ID3DBlob *blob = NULL;
    if (de__d3d12_root_signature_serialize(&blob) != DE_OK) {
        snprintf(ps->root_why, sizeof ps->root_why, "d3d12: the root signature does not serialize");
        DE_WARN(dd->log, "%s", ps->root_why);
        de_error_clear();
        return;
    }
    HRESULT hr = ID3D12Device_CreateRootSignature(dd->dev, 0, ID3D10Blob_GetBufferPointer(blob),
                                                  ID3D10Blob_GetBufferSize(blob),
                                                  &IID_ID3D12RootSignature, (void **)&ps->root);
    ID3D10Blob_Release(blob);
    if (FAILED(hr)) {
        ps->root = NULL;
        snprintf(ps->root_why, sizeof ps->root_why,
                 "d3d12: CreateRootSignature failed (HRESULT 0x%08lx)", (unsigned long)hr);
        DE_WARN(dd->log, "%s", ps->root_why);
        return;
    }
    de__d3d12_set_name((ID3D12Object *)ps->root, "de global root signature");
}
