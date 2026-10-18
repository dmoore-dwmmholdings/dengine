#include "de_fuzz.h"
#include "text/import/font_import.h"
#include <dengine/de_memory.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536)
        return 0;
    de_arena *arena = NULL;
    if (de_arena_create(&(de_arena_desc){.reserve_bytes = 32 * 1024 * 1024}, &arena) != DE_OK)
        return 0;
    de_allocator a = de_arena_allocator(arena);
    de__bmfont b;
    (void)de__bmfont_parse(data, size, &a, &b);
    de_arena_destroy(arena);
    return 0;
}
