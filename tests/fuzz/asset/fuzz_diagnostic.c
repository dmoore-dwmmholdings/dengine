#include "asset/diagnostic_internal.h"
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static volatile uint32_t diagnostic_sink;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char text[4097], source[512];
    uint32_t line = 0;
    if (size > sizeof text - 1)
        size = sizeof text - 1;
    memcpy(text, data, size);
    text[size] = '\0';
    if (de__asset_diagnostic_location(text, source, sizeof source, &line)) {
        if (line == 0 || memchr(source, '\0', sizeof source) == NULL) abort();
        diagnostic_sink = line ^ (uint32_t)(unsigned char)source[0];
    }
    return 0;
}
