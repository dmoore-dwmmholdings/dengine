/* fuzz_ldtk: the LDtk front end of the tilemap importer (DENG-80, import_ldtk.c), end to end. */
#include "fuzz_import_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    return fz_import(data, size, DE__TM_FORMAT_LDTK);
}
