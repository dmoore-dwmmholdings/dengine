#include "shader_cmd.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static volatile uint32_t sink;
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char text[16385], original[16385];
    if (size > sizeof text - 1)
        size = sizeof text - 1;
    memcpy(text, data, size);
    text[size] = '\0';
    memcpy(original, text, size + 1);
    const char *args[256];
    int argc = 0;
    for (size_t at = 0; at < size && argc < 256;) {
        args[argc++] = text + at;
        while (at < size && text[at] && text[at] != '\n')
            ++at;
        if (at < size)
            text[at++] = '\0';
    }
    de_cook_shader_options o;
    char error[256];
    if (de_cook_shader_parse(argc, args, &o, error)) {
        if (o.include_count > 32 || o.define_count > 32 || o.entry_count > 32)
            abort();
        sink = o.include_count ^ o.define_count;
    } else if (!memchr(error, '\0', sizeof error))
        abort();
    de_cook_shader_diagnostic d;
    if (de_cook_shader_diagnostic_parse(original, &d)) {
        if (!memchr(d.source, '\0', sizeof d.source) || !d.severity || !d.message)
            abort();
        sink = d.line ^ d.column;
    }
    return 0;
}
