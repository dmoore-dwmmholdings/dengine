/*
 * fuzz_gltf.c: fuzz target fuzz_gltf (DENG-106, SPEC §19.1, §21.3).
 *
 * glTF 2.0 (`.gltf` JSON, or `.glb` by its magic) through the `model` importer (DENG-101):
 * cgltf parse and validate, the IR, axes, the mesh stage (DENG-103), skeletons and clips
 * (DENG-104), materials, embedded textures through the texture importer, the prefab and DMDL.
 * External buffers and images are never found.
 *
 * Every output goes back through its runtime parser (fuzz_import_harness.c). Seed corpus:
 * corpus/gltf/; dictionary: dict/gltf.dict.
 */
#include "de_fuzz.h"
#include "fuzz_import_harness.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    (void)fz_import_run(FZ_IMPORT_GLTF, data, size, NULL, 0);
    return 0;
}
