/*
 * fuzz_d3d12_dxil.c: fuzz target fuzz_d3d12_dxil (DENG-324): the D3D12 backend's readers of
 * shader blobs and pipeline library files, which see cooked data from packs:
 *   - de__d3d12_dxil_scan: DXBC container, DXIL program header, PSV0, OSG1/OSGN and the LLVM
 *     bitstream reader (abbreviations, BLOCKINFO, module constants and metadata);
 *   - de__d3d12_pipeline_library_check: the library file header and checksum.
 * Every input goes to both. When the DXIL reader accepts an input, its promises are checked: a
 * known kind name, at most DE__DXIL_MAX_CBUFFERS listed, push_constant_bytes from a listed b0
 * space0 buffer. A broken promise aborts.
 */
#include "de_fuzz.h"

#include <dengine/de_memory.h>

#include "gpu/d3d12/d3d12_internal.h"

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    de__d3d12_dxil_info info;
    char why[200];
    const de_result r =
        de__d3d12_dxil_scan(data, size, de_heap_allocator(), &info, why, sizeof why);
    if (r == DE_OK) {
        if (de__d3d12_dxil_kind_name(info.kind) == NULL)
            abort();
        if (info.push_constant_bytes) {
            bool found = false;
            for (u32 i = 0; i < info.cbuffer_count && i < DE__DXIL_MAX_CBUFFERS; ++i)
                found |= info.cbuffers[i].space == 0 && info.cbuffers[i].lower_bound == 0;
            if (!found && info.cbuffer_count <= DE__DXIL_MAX_CBUFFERS)
                abort();
        }
    } else if (r == DE_ERR_PARSE && why[0] == '\0') {
        abort(); /* a refusal always says why */
    }
    de__d3d12_pipeline_library_view v;
    if (de__d3d12_pipeline_library_check(data, size, 0x10DE, 0x2484, 0, &v, why, sizeof why)) {
        if (v.data + v.data_size != data + size || v.keys + (size_t)v.key_count * 8u != v.data)
            abort();
    }
    return 0;
}
