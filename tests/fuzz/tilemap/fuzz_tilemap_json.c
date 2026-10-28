/* fuzz_tilemap_json: the tilemap importer's strict JSON reader (DENG-80,
 * tools/cook/importers/tilemap/tilemap_json.c). Every byte comes from a capped arena that is
 * freed after each input. */
#include "de_fuzz.h"

#include "cook/importers/tilemap/tilemap_ir.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    tm_arena ar;
    tm_arena_init(&ar, NULL, (u64)64 * 1024 * 1024);
    char err[160];
    const tmj_val *v = tmj_parse(&ar, (const char *)data, size, err, sizeof err);
    if (v != NULL) {
        /* Walk the getters over the root. */
        (void)tmj_num(v, "width", 0);
        (void)tmj_str(v, "type", "");
        (void)tmj_arr(v, "layers");
        (void)tmj_bool(v, "infinite", false);
    }
    tm_arena_free(&ar);
    return 0;
}
