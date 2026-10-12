/*
 * mount.c: the pack mount stack (SPEC §11.7, §21.3; DENG-212): de_asset_pack_mount and
 * de_asset_pack_unmount, ID lookup through the stack, merged bundle tables. See
 * mount_internal.h for the rules; pack_stage.c reads the packs for the load pipeline.
 */

#include "asset/mount_internal.h"

#include "asset/asset_internal.h"
#include "asset/budget_internal.h"
#include "asset/dpak/dpak_reader.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "core/de_hashmap.h"
#include "platform/de_atomic.h"

#include <string.h>

/* Pack and load priorities share their values, so a pack bundle's priority is used as it is. */
DE_STATIC_ASSERT(DPAK_PRIORITY_CRITICAL == 0 && DE__LOAD_PRIORITY_CRITICAL == 0, "priorities");
DE_STATIC_ASSERT(DPAK_PRIORITY_HIGH == 1 && DE__LOAD_PRIORITY_HIGH == 1, "priorities");
DE_STATIC_ASSERT(DPAK_PRIORITY_NORMAL == 2 && DE__LOAD_PRIORITY_NORMAL == 2, "priorities");
DE_STATIC_ASSERT(DPAK_PRIORITY_LOW == 3 && DE__LOAD_PRIORITY_LOW == 3, "priorities");
DE_STATIC_ASSERT(DPAK_PRIORITY_BACKGROUND == 4 && DE__LOAD_PRIORITY_BACKGROUND == 4, "priorities");

/* Debug builds check every pack's content hash at mount (it reads the whole file). */
#if defined(DE_DEBUG)
#define DE__PACK_VERIFY true
#else
#define DE__PACK_VERIFY false
#endif

/** Most assets one pack bundle closure may name. */
#define DE__PACK_BUNDLE_MAX_IDS (1u << 24)

struct de__mount_stack {
    de__pack **packs; /**< lookup order: highest priority first, then the latest mount */
    u32 count, capacity;
    de__pack *retired; /**< unmounted, still read by jobs */
    u32 next_seq;
    de_atomic_u64 jobs, runs, chunks, bytes_read;
};

static de__mount_stack *stack_get(de_asset_system *s, bool create) {
    if (s->mounts != NULL || !create)
        return s->mounts;
    de__mount_stack *st = DE_NEW(s->alloc, de__mount_stack, DE_MEM_ASSET);
    if (st == NULL)
        return NULL;
    memset(st, 0, sizeof *st);
    de_atomic_init_u64(&st->jobs, 0);
    de_atomic_init_u64(&st->runs, 0);
    de_atomic_init_u64(&st->chunks, 0);
    de_atomic_init_u64(&st->bytes_read, 0);
    s->mounts = st;
    return st;
}

static void pack_free(de_asset_system *s, de__pack *p) {
    dpak_close(p->reader);
    if (p->vpath != NULL)
        DE_FREE(s->alloc, p->vpath, p->vpath_size);
    DE_FREE(s->alloc, p, sizeof *p);
}

void de__mount_pack_ref(de__pack *p) {
    p->refs++;
}

void de__mount_pack_release(de_asset_system *s, de__pack *p) {
    DE_ASSERT(p->refs != 0);
    if (--p->refs != 0)
        return;
    DE_ASSERT(!p->mounted);
    de__mount_stack *st = s->mounts;
    for (de__pack **at = st != NULL ? &st->retired : NULL; at != NULL && *at != NULL;
         at = &(*at)->retired_next) {
        if (*at == p) {
            *at = p->retired_next;
            break;
        }
    }
    DE_INFO(s->log, "pack \"%s\" closed", p->vpath);
    pack_free(s, p);
}

void de__mount_shutdown(de_asset_system *s) {
    de__mount_stack *st = s != NULL ? s->mounts : NULL;
    if (st == NULL)
        return;
    /* The pipeline is gone, so no job holds a pack: every retired pack was freed already. */
    DE_ASSERT(st->retired == NULL);
    while (st->retired != NULL) {
        de__pack *p = st->retired;
        st->retired = p->retired_next;
        pack_free(s, p);
    }
    for (u32 i = 0; i < st->count; ++i)
        pack_free(s, st->packs[i]);
    if (st->packs != NULL)
        DE_FREE(s->alloc, st->packs, sizeof(de__pack *) * st->capacity);
    DE_FREE(s->alloc, st, sizeof *st);
    s->mounts = NULL;
}

bool de__mount_any(const de_asset_system *s) {
    return s != NULL && s->mounts != NULL && s->mounts->count != 0;
}

/* The mounted pack whose normalised path is `vpath`, as an index, or -1. */
static i64 find_mounted(const de__mount_stack *st, const char *vpath) {
    for (u32 i = 0; st != NULL && i < st->count; ++i)
        if (strcmp(st->packs[i]->vpath, vpath) == 0)
            return (i64)i;
    return -1;
}

/* ---- Public API ------------------------------------------------------------------------------ */

de_result de_asset_pack_mount(de_asset_system *s, const char *vpath, i32 priority) {
    if (s == NULL || vpath == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_asset_pack_mount: NULL system or path");
    char norm[DE_FS_PATH_MAX];
    if (de_path_normalize(vpath, norm, sizeof norm) != DE_OK)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_asset_pack_mount: \"%.200s\" is not a valid "
                       "virtual path",
                       vpath);
    de__mount_stack *st = stack_get(s, true);
    if (st == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_asset_pack_mount: out of memory");
    if (find_mounted(st, norm) >= 0)
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "pack \"%s\" is already mounted", norm);
    if (st->count == st->capacity) {
        u32 cap = st->capacity != 0 ? st->capacity * 2u : 4u;
        void *p = DE_REALLOC(s->alloc, st->packs, sizeof(de__pack *) * st->capacity,
                             sizeof(de__pack *) * cap, DE_ALIGNOF(de__pack *), DE_MEM_ASSET);
        if (p == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_asset_pack_mount: out of memory");
        st->packs = (de__pack **)p;
        st->capacity = cap;
    }

    dpak_open_desc od = {
        .allocator = s->alloc, .platform = dpak_platform_host(), .verify = DE__PACK_VERIFY};
    dpak_reader *r = NULL;
    de_result res = dpak_open(norm, &od, &r);
    if (res != DE_OK)
        return res; /* the reader set the error context, naming the pack */
    de__pack *p = DE_NEW(s->alloc, de__pack, DE_MEM_ASSET);
    size_t n = strlen(norm) + 1u;
    char *path = p != NULL ? (char *)DE_ALLOC(s->alloc, n, 1, DE_MEM_ASSET) : NULL;
    if (path == NULL) {
        if (p != NULL)
            DE_FREE(s->alloc, p, sizeof *p);
        dpak_close(r);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_asset_pack_mount: out of memory");
    }
    memset(p, 0, sizeof *p);
    memcpy(path, norm, n);
    p->vpath = path;
    p->vpath_size = n;
    p->priority = priority;
    p->seq = st->next_seq++;
    p->reader = r;
    p->refs = 1;
    p->mounted = true;

    /* Before the first pack of the same or a lower priority: the latest mount wins a tie. */
    u32 at = 0;
    while (at < st->count && st->packs[at]->priority > priority)
        ++at;
    memmove(st->packs + at + 1, st->packs + at, sizeof(de__pack *) * (st->count - at));
    st->packs[at] = p;
    st->count++;
    DE_INFO(s->log, "mounted pack \"%s\" (priority %d, %u assets, %u bundles)", norm, (int)priority,
            dpak_entry_count(r), dpak_bundle_count(r));
    return DE_OK;
}

void de_asset_pack_unmount(de_asset_system *s, const char *vpath) {
    if (s == NULL || vpath == NULL)
        return;
    char norm[DE_FS_PATH_MAX];
    de__mount_stack *st = s->mounts;
    i64 i = de_path_normalize(vpath, norm, sizeof norm) == DE_OK ? find_mounted(st, norm) : -1;
    if (i < 0) {
        DE_WARN(s->log, "de_asset_pack_unmount: \"%.200s\" is not mounted", vpath);
        return;
    }
    de__pack *p = st->packs[i];
    memmove(st->packs + i, st->packs + i + 1, sizeof(de__pack *) * (st->count - (u32)i - 1u));
    st->count--;
    p->mounted = false;
    if (p->refs > 1) { /* jobs still read it: closed when the last one is freed */
        p->retired_next = st->retired;
        st->retired = p;
        DE_INFO(s->log, "unmounted pack \"%s\" (%u loads still read it)", p->vpath, p->refs - 1u);
    } else {
        DE_INFO(s->log, "unmounted pack \"%s\"", p->vpath);
    }
    de__mount_pack_release(s, p);
}

/* ---- Lookups --------------------------------------------------------------------------------- */

bool de__mount_lookup(de_asset_system *s, de_asset_id id, de__mount_entry *out) {
    const de__mount_stack *st = s != NULL ? s->mounts : NULL;
    if (st == NULL || id.hash == 0)
        return false;
    for (u32 i = 0; i < st->count; ++i) {
        u32 k = dpak_find(st->packs[i]->reader, id.hash);
        if (k == DPAK_NOT_FOUND)
            continue;
        if (out != NULL) {
            out->pack = st->packs[i];
            out->index = k;
            dpak_entry(st->packs[i]->reader, k, &out->toc);
        }
        return true;
    }
    return false;
}

/* The topmost bundle whose name hashes to `hash`. */
static bool find_bundle(const de__mount_stack *st, u64 hash, de__pack **pack,
                        dpak_bundle_entry *b) {
    for (u32 i = 0; i < st->count; ++i) {
        u32 k = dpak_bundle_find(st->packs[i]->reader, hash);
        if (k == DPAK_NOT_FOUND)
            continue;
        *pack = st->packs[i];
        dpak_bundle(st->packs[i]->reader, k, b);
        return true;
    }
    return false;
}

static bool ids_grow(de_asset_system *s, de_asset_id **ids, u32 *capacity, u32 count, u32 more) {
    if ((u64)count + more <= *capacity)
        return true;
    u64 cap = *capacity != 0 ? *capacity : 64u;
    while (cap < (u64)count + more)
        cap *= 2u;
    if (cap > DE__PACK_BUNDLE_MAX_IDS)
        return false;
    void *p = DE_REALLOC(s->alloc, *ids, sizeof(de_asset_id) * *capacity,
                         sizeof(de_asset_id) * (size_t)cap, DE_ALIGNOF(de_asset_id), DE_MEM_ASSET);
    if (p == NULL)
        return false;
    *ids = (de_asset_id *)p;
    *capacity = (u32)cap;
    return true;
}

/* Appends the members of bundle `b` of `pack` (to the pins too when it is resident). */
static bool add_members(de_asset_system *s, de__mount_bundle *out, const de__pack *pack,
                        const dpak_bundle_entry *b) {
    bool resident = (b->flags & DPAK_BUNDLE_RESIDENT) != 0;
    if (!ids_grow(s, &out->ids, &out->capacity, out->count, b->member_count))
        return false;
    if (resident && !ids_grow(s, &out->pins, &out->pin_capacity, out->pin_count, b->member_count))
        return false;
    for (u32 i = 0; i < b->member_count; ++i) {
        de_asset_id id = {dpak_bundle_id(pack->reader, b->member_first + i)};
        out->ids[out->count++] = id;
        if (resident)
            out->pins[out->pin_count++] = id;
    }
    return true;
}

typedef struct closure_frame {
    de__pack *pack;
    dpak_bundle_entry b;
    u32 next; /* next depend to visit */
} closure_frame;

de_result de__mount_bundle_closure(de_asset_system *s, const char *name, de__mount_bundle *out) {
    memset(out, 0, sizeof *out);
    const de__mount_stack *st = s != NULL ? s->mounts : NULL;
    if (st == NULL || st->count == 0 || name == NULL)
        return DE_ERR_NOT_FOUND;
    u64 root_hash = de_hash64_str(name);
    de__pack *root_pack = NULL;
    dpak_bundle_entry root;
    if (!find_bundle(st, root_hash, &root_pack, &root) ||
        strcmp(dpak_bundle_name(root_pack->reader, &root), name) != 0)
        return DE_ERR_NOT_FOUND;
    out->priority = root.priority;

    /* Depth-first over the depends, members appended after the depends' (dependencies first).
     * A bundle is visited once, so a cycle in (untrusted) packs ends. */
    de_hashmap seen;
    de_hashmap_desc hd = {.allocator = s->alloc, .key_size = sizeof(u64), .tag = DE_MEM_ASSET};
    closure_frame *frames = NULL;
    u32 depth = 0, frame_cap = 0;
    de_result res = DE_OK;
    void *unused = NULL;
    bool inserted = false;
    if (de_hashmap_init(&seen, &hd) != DE_OK)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "bundle \"%.200s\": out of memory", name);
    if (de_hashmap_get_or_put(&seen, root_hash, &root_hash, &unused, &inserted) != DE_OK)
        res = DE_ERR_OUT_OF_MEMORY;
    if (res == DE_OK) {
        frame_cap = 16u;
        frames = (closure_frame *)DE_ALLOC(s->alloc, sizeof(closure_frame) * frame_cap,
                                           DE_ALIGNOF(closure_frame), DE_MEM_ASSET);
        if (frames == NULL)
            res = DE_ERR_OUT_OF_MEMORY;
        else
            frames[depth++] = (closure_frame){root_pack, root, 0};
    }
    while (res == DE_OK && depth != 0) {
        closure_frame *top = &frames[depth - 1u];
        if (top->next == top->b.depend_count) {
            if (!add_members(s, out, top->pack, &top->b))
                res = DE_ERR_OUT_OF_MEMORY;
            depth--;
            continue;
        }
        u64 h = dpak_bundle_id(top->pack->reader, top->b.depend_first + top->next++);
        if (de_hashmap_get_or_put(&seen, h, &h, &unused, &inserted) != DE_OK) {
            res = DE_ERR_OUT_OF_MEMORY;
            break;
        }
        if (!inserted)
            continue;
        closure_frame next = {NULL, {0}, 0};
        if (!find_bundle(st, h, &next.pack, &next.b)) {
            DE_WARN(s->log,
                    "bundle \"%s\" depends on bundle %016llx, which no mounted pack defines",
                    dpak_bundle_name(top->pack->reader, &top->b), (unsigned long long)h);
            continue;
        }
        if (depth == frame_cap) {
            void *p = DE_REALLOC(s->alloc, frames, sizeof(closure_frame) * frame_cap,
                                 sizeof(closure_frame) * frame_cap * 2u, DE_ALIGNOF(closure_frame),
                                 DE_MEM_ASSET);
            if (p == NULL) {
                res = DE_ERR_OUT_OF_MEMORY;
                break;
            }
            frames = (closure_frame *)p;
            frame_cap *= 2u;
        }
        frames[depth++] = next;
    }
    if (frames != NULL)
        DE_FREE(s->alloc, frames, sizeof(closure_frame) * frame_cap);
    de_hashmap_free(&seen);
    if (res != DE_OK) {
        de__mount_bundle_free(s, out);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                       "bundle \"%.200s\": out of memory (or more than %u "
                       "assets)",
                       name, DE__PACK_BUNDLE_MAX_IDS);
    }
    return DE_OK;
}

void de__mount_bundle_pin(de_asset_system *s, const de__mount_bundle *b) {
    if (b->pin_count != 0)
        de__budget_pin_ids(s, b->pins, b->pin_count);
}

void de__mount_bundle_free(de_asset_system *s, de__mount_bundle *b) {
    if (b->ids != NULL)
        DE_FREE(s->alloc, b->ids, sizeof(de_asset_id) * b->capacity);
    if (b->pins != NULL)
        DE_FREE(s->alloc, b->pins, sizeof(de_asset_id) * b->pin_capacity);
    memset(b, 0, sizeof *b);
}

/* ---- Counters -------------------------------------------------------------------------------- */

void de__mount_count(de_asset_system *s, u64 jobs, u64 runs, u64 chunks, u64 bytes) {
    de__mount_stack *st = s->mounts;
    if (st == NULL)
        return;
    if (jobs != 0)
        (void)de_atomic_fetch_add_u64(&st->jobs, jobs, DE_ATOMIC_RELAXED);
    if (runs != 0)
        (void)de_atomic_fetch_add_u64(&st->runs, runs, DE_ATOMIC_RELAXED);
    if (chunks != 0)
        (void)de_atomic_fetch_add_u64(&st->chunks, chunks, DE_ATOMIC_RELAXED);
    if (bytes != 0)
        (void)de_atomic_fetch_add_u64(&st->bytes_read, bytes, DE_ATOMIC_RELAXED);
}

void de__mount_get_stats(const de_asset_system *s, de__mount_stats *out) {
    memset(out, 0, sizeof *out);
    const de__mount_stack *st = s != NULL ? s->mounts : NULL;
    if (st == NULL)
        return;
    out->mounted = st->count;
    for (const de__pack *p = st->retired; p != NULL; p = p->retired_next)
        out->retired++;
    out->jobs = de_atomic_load_u64(&st->jobs, DE_ATOMIC_RELAXED);
    out->runs = de_atomic_load_u64(&st->runs, DE_ATOMIC_RELAXED);
    out->chunks = de_atomic_load_u64(&st->chunks, DE_ATOMIC_RELAXED);
    out->bytes_read = de_atomic_load_u64(&st->bytes_read, DE_ATOMIC_RELAXED);
}
