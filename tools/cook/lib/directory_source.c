/* A directory importer receives a deterministic DDIR entry-name snapshot. Every actual file
 * must still pass import_read_file, which records its content dependency in the cook cache. */
#include "cook/lib/cooker_internal.h"
#include "cook/lib/decache.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define DIRECTORY_ENTRIES    4096u
#define DIRECTORY_NAME_BYTES 256u
typedef struct directory_names {
    char (*names)[DIRECTORY_NAME_BYTES];
    u32 count;
    de_result result;
    const char *path;
} directory_names;
static bool collect(const char *name, bool is_dir, void *user) {
    directory_names *d = user;
    size_t n = strlen(name);
    if (is_dir || n >= DIRECTORY_NAME_BYTES || !n || strchr(name, '\n') || strchr(name, '\\') ||
        d->count == DIRECTORY_ENTRIES) {
        d->result =
            DE_FAIL(DE_ERR_UNSUPPORTED,
                    "directory source '%s': nested/invalid/excessive entry '%s'", d->path, name);
        return false;
    }
    char file[DE_FS_PATH_MAX];
    int length = snprintf(file, sizeof file, "%s/%s", d->path, name);
    u64 bytes;
    i64 mtime;
    if (length < 0 || (size_t)length >= sizeof file ||
        de__decache_file_stat(file, &bytes, &mtime) != DE_OK) {
        d->result = DE_FAIL(DE_ERR_UNSUPPORTED, "directory source '%s': non-regular entry '%s'",
                            d->path, name);
        return false;
    }
    memcpy(d->names[d->count++], name, n + 1);
    return true;
}
static int compare(const void *a, const void *b) {
    return strcmp(a, b);
}
de_result de__cook_read_source(const char *path, const de_allocator *a, void **out, u64 *size) {
    de_result result = de_file_read_all(path, a, out, size);
    if (result == DE_OK)
        return result;
    *out = NULL;
    *size = 0;
    directory_names d = {0};
    d.path = path;
    d.names = DE_ALLOC(a, DIRECTORY_ENTRIES * DIRECTORY_NAME_BYTES, 16, DE_MEM_ASSET);
    if (!d.names)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "directory source names");
    de_result listing = de_dir_list(path, collect, &d);
    if (listing != DE_OK || d.result != DE_OK) {
        DE_FREE(a, d.names, DIRECTORY_ENTRIES * DIRECTORY_NAME_BYTES);
        return d.result != DE_OK ? d.result : listing;
    }
    qsort(d.names, d.count, DIRECTORY_NAME_BYTES, compare);
    u64 bytes = 8;
    for (u32 i = 0; i < d.count; ++i)
        bytes += strlen(d.names[i]) + 1;
    u8 *snapshot = DE_ALLOC(a, (size_t)bytes + 1, 16, DE_MEM_ASSET);
    if (!snapshot)
        result = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "directory source snapshot");
    else {
        memcpy(snapshot, "DDIR\1\0\0\0", 8);
        u64 at = 8;
        for (u32 i = 0; i < d.count; ++i) {
            size_t n = strlen(d.names[i]) + 1;
            memcpy(snapshot + at, d.names[i], n);
            at += n;
        }
        snapshot[bytes] = 0;
        *out = snapshot;
        *size = bytes;
        result = DE_OK;
    }
    DE_FREE(a, d.names, DIRECTORY_ENTRIES * DIRECTORY_NAME_BYTES);
    return result;
}
