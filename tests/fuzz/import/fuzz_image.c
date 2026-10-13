/*
 * fuzz_image.c: fuzz target fuzz_image (DENG-106, SPEC §19.1, §21.3).
 *
 * PNG, JPEG and OpenEXR through the `texture` importer (DENG-98): stb_image and tinyexr decode,
 * sRGB to linear, the max_size clamp, the mip chain and the DTEX writer. Settings (usage,
 * address, alpha_coverage, pixel_art) are picked from a hash of the input.
 *
 * Every output goes back through its runtime parser (fuzz_import_harness.c). Seed corpus:
 * corpus/image/; dictionary: dict/image.dict.
 */
#include "de_fuzz.h"
#include "fuzz_import_harness.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    (void)fz_import_run(FZ_IMPORT_IMAGE, data, size, NULL, 0);
    return 0;
}
