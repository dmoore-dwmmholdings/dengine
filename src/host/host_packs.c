/*
 * host_packs.c: de__host_mount_packs (DENG-213). See host_packs.h.
 */
#include "host/host_packs.h"

#include <dengine/de_platform.h>

#include <stdlib.h>
#include <string.h>

typedef struct pack_list {
    const de_allocator *a;
    char **names;
    u32 count, cap;
    bool oom;
} pack_list;

static bool ends_with_dpak(const char *name) {
    size_t n = strlen(name);
    return n > 5 && memcmp(name + n - 5, ".dpak", 5) == 0;
}

static bool collect(const char *name, bool is_dir, void *user) {
    pack_list *l = (pack_list *)user;
    if (is_dir || !ends_with_dpak(name))
        return true;
    if (l->count == l->cap) {
        u32 cap = l->cap != 0 ? l->cap * 2u : 8u;
        char **names =
            (char **)DE_ALLOC(l->a, cap * sizeof *names, DE_ALIGNOF(char *), DE_MEM_GENERAL);
        if (names == NULL) {
            l->oom = true;
            return false;
        }
        if (l->count != 0) {
            memcpy(names, l->names, l->count * sizeof *names);
            DE_FREE(l->a, l->names, l->cap * sizeof *l->names);
        }
        l->names = names;
        l->cap = cap;
    }
    size_t n = strlen(name) + 1;
    char *copy = (char *)DE_ALLOC(l->a, n, 1, DE_MEM_GENERAL);
    if (copy == NULL) {
        l->oom = true;
        return false;
    }
    memcpy(copy, name, n);
    l->names[l->count] = copy;
    l->count++;
    return true;
}

static int cmp_name(const void *pa, const void *pb) {
    return strcmp(*(char *const *)pa, *(char *const *)pb);
}

de_result de__host_mount_packs(de_asset_system *s, const char *dir, const de_allocator *a,
                               u32 *count) {
    if (count != NULL)
        *count = 0;
    if (s == NULL || dir == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__host_mount_packs: NULL argument");
    pack_list l;
    memset(&l, 0, sizeof l);
    l.a = a != NULL ? a : de_heap_allocator();
    de_result r = de_dir_list(dir, collect, &l);
    if (r == DE_OK && l.oom)
        r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory listing the packs of '%s'", dir);
    if (r == DE_OK && l.count > 1)
        qsort(l.names, l.count, sizeof *l.names, cmp_name); /* de_dir_list order is the OS's */
    size_t dl = strlen(dir);
    bool slash = dl > 0 && dir[dl - 1] == '/';
    for (u32 i = 0; i < l.count && r == DE_OK; ++i) {
        char vpath[DE_FS_PATH_MAX];
        size_t need = dl + (slash ? 0u : 1u) + strlen(l.names[i]) + 1u;
        if (need > sizeof vpath) {
            r = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "pack path too long: %s/%s", dir, l.names[i]);
            break;
        }
        memcpy(vpath, dir, dl);
        size_t at = dl;
        if (!slash)
            vpath[at++] = '/';
        memcpy(vpath + at, l.names[i], strlen(l.names[i]) + 1);
        r = de_asset_pack_mount(s, vpath, 0);
        if (r == DE_OK && count != NULL)
            *count += 1;
    }
    for (u32 i = 0; i < l.count; ++i)
        DE_FREE(l.a, l.names[i], strlen(l.names[i]) + 1);
    if (l.cap != 0)
        DE_FREE(l.a, l.names, l.cap * sizeof *l.names);
    return r;
}
