/* fuzz_tiled: the Tiled front end of the tilemap importer (DENG-80, import_tiled.c), end to end:
 * `.tmx` when the input starts with '<' (after whitespace), else `.tmj`. */
#include "fuzz_import_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t i = 0;
    while (i < size && (data[i] == ' ' || data[i] == '\n' || data[i] == '\r' || data[i] == '\t'))
        ++i;
    return fz_import(data, size, i < size && data[i] == '<' ? DE__TM_FORMAT_TMX : DE__TM_FORMAT_TMJ);
}
