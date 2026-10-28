/* Shared by fuzz_ldtk.c and fuzz_tiled.c: one whole import of the input, then the runtime parser
 * over the cooked bytes (an import that succeeds must produce a valid DTMP). External files
 * (tilesets, levels) are refused: the seeds embed what they need. */
#ifndef DE_FUZZ_TILEMAP_IMPORT_COMMON_H
#define DE_FUZZ_TILEMAP_IMPORT_COMMON_H

#include "de_fuzz.h"

#include "cook/importers/tilemap/tilemap_import.h"
#include "gameplay/tilemap/tilemap_cooked.h"

#include <dengine/de_memory.h>

#include <stdlib.h>
#include <string.h>

static const char *const k_fz_entities[] = {"Player=prefabs/player", "Coin=prefabs/coin"};

static int fz_import(const uint8_t *data, size_t size, de__tm_format format) {
    de__tm_import_desc d;
    memset(&d, 0, sizeof d);
    d.source = data;
    d.size = size;
    d.source_path = "maps/fuzz";
    d.format = format;
    d.entities = k_fz_entities;
    d.entity_count = 2;
    d.memory_limit = (u64)64 * 1024 * 1024;
    de__tm_import_result r;
    de_result res = de__tm_import(&d, &r);
    if (res == DE_OK) {
        de__tm_view v;
        const char *why = NULL;
        if (!de__tm_view_parse(r.data, r.size, &v, &why))
            abort(); /* the importer wrote bytes its own runtime refuses */
        de__tm_import_free(NULL, &r);
    } else if (r.data != NULL) {
        abort(); /* a failed import must not leave output behind */
    }
    de_error_clear();
    return 0;
}

#endif
