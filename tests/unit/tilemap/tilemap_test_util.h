/* tilemap_test_util.h: shared helpers of the tilemap tests (DENG-80). */
#ifndef DE_TEST_TILEMAP_UTIL_H
#define DE_TEST_TILEMAP_UTIL_H

#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "cook/importers/tilemap/tilemap_import.h"
#include "gameplay/tilemap/tilemap_cooked.h"

#include <stdio.h>
#include <string.h>

#define TM_MAX_FILES 16

/* Files read by an import: freed by tm_files_free. */
typedef struct tm_files {
    const de_allocator *a;
    const char *dir; /* "tmd:/maps/" */
    void *data[TM_MAX_FILES];
    u64 size[TM_MAX_FILES];
    u32 count;
} tm_files;

static inline void tm_mount(void) {
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("tmd", DE_TILEMAP_TEST_DATA_DIR, 0, 0), DE_OK);
        mounted = true;
    }
}

static inline de_result tm_files_read(void *user, const char *rel, const void **data, u64 *size) {
    tm_files *f = (tm_files *)user;
    if (f->count >= TM_MAX_FILES)
        return DE_ERR_OUT_OF_MEMORY;
    char path[512];
    snprintf(path, sizeof path, "%s%s", f->dir, rel);
    void *d = NULL;
    u64 n = 0;
    de_result r = de_file_read_all(path, f->a, &d, &n);
    if (r != DE_OK)
        return r;
    f->data[f->count] = d;
    f->size[f->count] = n;
    f->count++;
    *data = d;
    *size = n;
    return DE_OK;
}

static inline void tm_files_free(tm_files *f) {
    for (u32 i = 0; i < f->count; ++i)
        DE_FREE(f->a, f->data[i], (size_t)f->size[i] + 1u);
    f->count = 0;
}

typedef struct tm_settings {
    const char *const *entities;
    u32 entity_count;
    const char *const *tilesets;
    u32 tileset_count;
    f64 ppu;
} tm_settings;

/* Imports `rel` (below tmd:/, e.g. "maps/fixture.tmj") with the core importer. */
static inline de_result tm_import_file(const de_allocator *a, const char *rel,
                                       const tm_settings *st, de__tm_import_result *out) {
    char path[512];
    snprintf(path, sizeof path, "tmd:/%s", rel);
    void *src = NULL;
    u64 size = 0;
    de_result r = de_file_read_all(path, a, &src, &size);
    if (r != DE_OK)
        return r;
    tm_files files;
    memset(&files, 0, sizeof files);
    files.a = a;
    char dir[512];
    snprintf(dir, sizeof dir, "tmd:/%s", rel);
    char *slash = strrchr(dir, '/');
    if (slash != NULL)
        slash[1] = '\0';
    files.dir = dir;
    de__tm_import_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = a;
    d.source = src;
    d.size = size;
    d.source_path = rel;
    d.read = tm_files_read;
    d.read_user = &files;
    if (st != NULL) {
        d.entities = st->entities;
        d.entity_count = st->entity_count;
        d.tilesets = st->tilesets;
        d.tileset_count = st->tileset_count;
        d.pixels_per_unit = st->ppu;
    }
    r = de__tm_import(&d, out);
    tm_files_free(&files);
    DE_FREE(a, src, (size_t)size + 1u);
    if (r != DE_OK)
        printf("  import %s: %s\n", rel, out->message);
    return r;
}

/* The cell of (layer, x, y) of a cooked view. */
static inline u32 tm_view_cell(const de__tm_view *v, u32 layer, u32 x, u32 y) {
    de__tm_layer l = de__tm_layer_at(v, layer);
    if (x >= l.width || y >= l.height)
        return 0;
    u32 chunk = l.chunk_first + (y / DE__TM_CHUNK) * l.chunks_x + x / DE__TM_CHUNK;
    return de__tm_rd32(de__tm_chunk_cells(v, chunk) + 4u * ((y % DE__TM_CHUNK) * DE__TM_CHUNK +
                                                            x % DE__TM_CHUNK));
}

/* The index of the layer called `name`, or UINT32_MAX. */
static inline u32 tm_view_layer(const de__tm_view *v, const char *name) {
    for (u32 i = 0; i < v->h.layer_count; ++i)
        if (strcmp(de__tm_str(v, de__tm_layer_at(v, i).name), name) == 0)
            return i;
    return 0xFFFFFFFFu;
}

#endif
