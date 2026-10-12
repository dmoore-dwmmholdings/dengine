/*
 * reverse_index.c: source path -> asset IDs for hot reload (SPEC §11.9; DENG-214). See
 * hot_reload_internal.h. Links in one array (with a free list), each on two lists: the links of
 * its path (doubly linked) and the links of its asset. Two hash maps hold the list heads; they
 * are probed, never iterated, so nothing depends on hash map order.
 */

#include "asset/hot_reload_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "core/de_hashmap.h"

#include <string.h>

de_result de__rindex_init(de__rindex *r, const de_allocator *alloc) {
    memset(r, 0, sizeof *r);
    r->alloc = alloc;
    de_hashmap_desc d = {.allocator = alloc,
                         .key_size = sizeof(u64),
                         .value_size = sizeof(u32),
                         .tag = DE_MEM_ASSET};
    de_result res = de_hashmap_init(&r->by_path, &d);
    if (res == DE_OK)
        res = de_hashmap_init(&r->by_id, &d);
    return res;
}

void de__rindex_free(de__rindex *r) {
    if (r->links != NULL)
        DE_FREE(r->alloc, r->links, sizeof(de__rindex_link) * r->link_cap);
    r->links = NULL;
    r->link_count = r->link_cap = r->free_head = r->count = 0;
    de_hashmap_free(&r->by_path);
    de_hashmap_free(&r->by_id);
}

u64 de__rindex_path_hash(const char *vpath) {
    char norm[DE_FS_PATH_MAX];
    if (vpath == NULL)
        vpath = "";
    if (de_path_normalize(vpath, norm, sizeof norm) == DE_OK)
        return de_hash64(norm, strlen(norm), 0);
    return de_hash64(vpath, strlen(vpath), 0);
}

static u32 *head_of(const de_hashmap *m, u64 key) {
    return (u32 *)de_hashmap_get(m, key, &key);
}

static void link_free(de__rindex *r, u32 n) {
    de__rindex_link *l = &r->links[n - 1u];
    memset(l, 0, sizeof *l);
    l->id_next = r->free_head;
    r->free_head = n;
}

/* A free link (index + 1), or 0 when out of memory. */
static u32 link_alloc(de__rindex *r) {
    if (r->free_head != 0) {
        u32 n = r->free_head;
        r->free_head = r->links[n - 1u].id_next;
        return n;
    }
    if (r->link_count == r->link_cap) {
        u32 cap = r->link_cap != 0 ? r->link_cap * 2u : 64u;
        void *p =
            DE_REALLOC(r->alloc, r->links, sizeof(de__rindex_link) * r->link_cap,
                       sizeof(de__rindex_link) * cap, DE_ALIGNOF(de__rindex_link), DE_MEM_ASSET);
        if (p == NULL)
            return 0;
        r->links = (de__rindex_link *)p;
        r->link_cap = cap;
    }
    return ++r->link_count;
}

void de__rindex_remove(de__rindex *r, de_asset_id id) {
    u32 head = 0;
    if (!de_hashmap_remove(&r->by_id, id.hash, &id.hash, &head))
        return;
    for (u32 n = head; n != 0;) {
        de__rindex_link *l = &r->links[n - 1u];
        u32 next_id = l->id_next;
        /* Off the list of its path. */
        if (l->path_prev != 0) {
            r->links[l->path_prev - 1u].path_next = l->path_next;
        } else {
            u32 *ph = head_of(&r->by_path, l->path_hash);
            if (ph != NULL) {
                if (l->path_next != 0)
                    *ph = l->path_next;
                else
                    (void)de_hashmap_remove(&r->by_path, l->path_hash, &l->path_hash, NULL);
            }
        }
        if (l->path_next != 0)
            r->links[l->path_next - 1u].path_prev = l->path_prev;
        link_free(r, n);
        r->count--;
        n = next_id;
    }
}

/* Adds (hash of `path`, id) unless `id` has it already. False when out of memory. */
static bool add(de__rindex *r, de_asset_id id, const char *path) {
    u64 h = de__rindex_path_hash(path);
    const u32 *idh = head_of(&r->by_id, id.hash);
    for (u32 n = idh != NULL ? *idh : 0u; n != 0; n = r->links[n - 1u].id_next)
        if (r->links[n - 1u].path_hash == h)
            return true; /* the same path twice */
    u32 n = link_alloc(r);
    if (n == 0)
        return false;
    void *v = NULL;
    if (de_hashmap_get_or_put(&r->by_path, h, &h, &v, NULL) != DE_OK) {
        link_free(r, n);
        return false;
    }
    u32 *ph = (u32 *)v;
    const u32 path_head = *ph;
    if (de_hashmap_get_or_put(&r->by_id, id.hash, &id.hash, &v, NULL) != DE_OK) {
        if (path_head == 0)
            (void)de_hashmap_remove(&r->by_path, h, &h, NULL);
        link_free(r, n);
        return false;
    }
    u32 *ih = (u32 *)v;
    de__rindex_link *l = &r->links[n - 1u];
    l->path_hash = h;
    l->id = id;
    l->live = 1;
    l->path_prev = 0;
    l->path_next = path_head;
    if (path_head != 0)
        r->links[path_head - 1u].path_prev = n;
    *ph = n; /* the by_id insert cannot move a by_path value: two maps, two blocks */
    l->id_next = *ih;
    *ih = n;
    r->count++;
    return true;
}

de_result de__rindex_set(de__rindex *r, de_asset_id id, const char *main_path,
                         const char *extra_block, u32 extra_count) {
    de__rindex_remove(r, id);
    bool ok = main_path == NULL || add(r, id, main_path);
    const char *p = extra_block;
    for (u32 i = 0; ok && p != NULL && i < extra_count; ++i) {
        ok = add(r, id, p);
        p += strlen(p) + 1u;
    }
    if (ok)
        return DE_OK;
    de__rindex_remove(r, id);
    return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset hot reload: out of memory for the index");
}

u32 de__rindex_lookup(const de__rindex *r, const char *vpath,
                      void (*fn)(de_asset_id id, void *user), void *user) {
    u64 h = de__rindex_path_hash(vpath);
    const u32 *ph = head_of(&r->by_path, h);
    u32 calls = 0;
    for (u32 n = ph != NULL ? *ph : 0u; n != 0; n = r->links[n - 1u].path_next) {
        fn(r->links[n - 1u].id, user);
        calls++;
    }
    return calls;
}

u32 de__rindex_each_id(const de__rindex *r, void (*fn)(de_asset_id id, void *user), void *user) {
    u32 calls = 0;
    for (u32 i = 0; i < r->link_count; ++i) {
        if (r->links[i].live == 0)
            continue;
        fn(r->links[i].id, user);
        calls++;
    }
    return calls;
}
