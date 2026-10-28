/* fuzz_tilemap_xml: the tilemap importer's TMX-subset XML reader (DENG-80,
 * tools/cook/importers/tilemap/tilemap_xml.c). */
#include "de_fuzz.h"

#include "cook/importers/tilemap/tilemap_ir.h"

#include <stdlib.h>

static void walk(const tmx_node *n, u32 depth) {
    if (depth > TMX_MAX_DEPTH + 1u)
        abort();
    (void)tmx_attr_num(n, "width", 0);
    (void)tmx_attr_str(n, "name", "");
    for (u32 i = 0; i < n->child_count; ++i)
        walk(&n->children[i], depth + 1u);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    tm_arena ar;
    tm_arena_init(&ar, NULL, (u64)64 * 1024 * 1024);
    char err[160];
    const tmx_node *root = tmx_parse(&ar, (const char *)data, size, err, sizeof err);
    if (root != NULL)
        walk(root, 0);
    tm_arena_free(&ar);
    return 0;
}
