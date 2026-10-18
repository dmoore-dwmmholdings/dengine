#include "de_fuzz.h"
#include "cook/importers/sound/sound_import.h"
#include <stdlib.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 1024 * 1024)
        return 0;
    de__sound_view hostile;
    (void)de__sound_parse(data, size, &hostile);
    de__sound_settings settings = {0};
    settings.stream_set = true;
    if (size) {
        settings.stream = (data[size - 1] & 1) != 0;
        if (data[size - 1] & 2)
            settings.compression = "adpcm";
        settings.mono = (data[size - 1] & 4) != 0;
    }
    void *out = NULL;
    u64 n = 0;
    const de_allocator *a = de_heap_allocator();
    if (de__sound_cook(a, data, size, &settings, &out, &n) == DE_OK) {
        de__sound_view view;
        if (de__sound_parse(out, n, &view) != DE_OK)
            abort();
    }
    if (out)
        DE_FREE(a, out, (size_t)n);
    return 0;
}
