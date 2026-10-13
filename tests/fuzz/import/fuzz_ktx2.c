/*
 * fuzz_ktx2.c: fuzz target fuzz_ktx2 (DENG-106, SPEC §19.1, §21.3).
 *
 * KTX2 through the `texture` importer (DENG-98): the importer's own KTX2 reader (header, level
 * index, DFD, zstd supercompression), BC pass-through, raw formats, and ETC1S / UASTC through
 * the Basis Universal transcoder; then the mip pipeline and DTEX.
 *
 * Every output goes back through its runtime parser (fuzz_import_harness.c). Seed corpus:
 * corpus/ktx2/; dictionary: dict/ktx2.dict.
 */
#include "de_fuzz.h"
#include "fuzz_import_harness.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    (void)fz_import_run(FZ_IMPORT_KTX2, data, size, NULL, 0);
    return 0;
}
