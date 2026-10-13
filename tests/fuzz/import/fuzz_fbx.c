/*
 * fuzz_fbx.c: fuzz target fuzz_fbx (DENG-106, SPEC §19.1, §21.3).
 *
 * FBX (binary or ASCII) through the `model` importer: ufbx load from memory and the IR
 * conversion of de_model_fbx_load (DENG-102), then the same stages as fuzz_gltf.
 *
 * Every output goes back through its runtime parser (fuzz_import_harness.c). Seed corpus:
 * corpus/fbx/; dictionary: dict/fbx.dict.
 */
#include "de_fuzz.h"
#include "fuzz_import_harness.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    (void)fz_import_run(FZ_IMPORT_FBX, data, size, NULL, 0);
    return 0;
}
