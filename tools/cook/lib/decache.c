/*
 * decache.c: the cook cache `.decache` (DENG-210, SPEC §11.5): keys, the entry format, lookup and
 * store. decache.h describes the key and the files; decache_index.c the warm-path index.
 *
 * Entry file `<cache_dir>/<2 hex>/<32 hex>.dc`, little-endian:
 *
 *   header (56 bytes):
 *     "DECE" u32 format (DE__DECACHE_FORMAT) u32 zstd version u32 flags
 *     16 bytes: the key the file is named after
 *     u64 body size, u64 stored size (the bytes after the header), u64 XXH3-64 of the body
 *   flags: 1 = redirect (the body lists extra sources), 2 = the body is one zstd frame
 *
 *   entry body (zstd, level 3):
 *     u64 output hash
 *     u64 cooked size, cooked bytes
 *     u32 dependency count, u64 ID each
 *     u32 sub-asset count, per sub-asset: str name, str type, u64 ID, u64 size, bytes
 *     u32 extra source count, str path each (relative to the asset root, in read order)
 *   redirect body (stored as is): u32 extra source count, str path each
 *
 * `str` is u32 length + bytes. A file that is too short, has another magic, format, zstd version
 * or key, a size that does not match, a zstd error, a body hash that does not match, or a body
 * that does not parse is damaged: the lookup deletes it, logs it at Warn and misses.
 */

#include "cook/lib/decache.h"

#include "cooker_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "core/de_hashmap.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>
#include <zstd.h>

#define HEADER_SIZE   56u
#define FLAG_REDIRECT 1u
#define FLAG_ZSTD     2u
#define ZSTD_LEVEL    3
/* Attempts at writing one file: on Windows a rename over a file that another process is
 * renaming at the same moment can fail with a sharing error. */
#define WRITE_ATTEMPTS 3

/* ---- Little-endian buffers ------------------------------------------------------------------- */

static void put_u32(u8 *p, u32 v) {
    for (int i = 0; i < 4; ++i)
        p[i] = (u8)(v >> (8 * i));
}
static void put_u64(u8 *p, u64 v) {
    for (int i = 0; i < 8; ++i)
        p[i] = (u8)(v >> (8 * i));
}
static u32 get_u32(const u8 *p) {
    u32 v = 0;
    for (int i = 3; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}
static u64 get_u64(const u8 *p) {
    u64 v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}

/* A growable output buffer. */
typedef struct wbuf {
    const de_allocator *a;
    u8 *p;
    size_t n, cap;
    bool oom;
} wbuf;

static void wb_put(wbuf *b, const void *src, size_t n) {
    if (b->oom || n == 0)
        return;
    if (n > SIZE_MAX / 2u - b->n) {
        b->oom = true;
        return;
    }
    if (b->n + n > b->cap) {
        size_t ncap = b->cap != 0 ? b->cap : 256u;
        while (ncap < b->n + n)
            ncap *= 2u;
        void *q = DE_REALLOC(b->a, b->p, b->cap, ncap, 16, DE_MEM_ASSET);
        if (q == NULL) {
            b->oom = true;
            return;
        }
        b->p = (u8 *)q;
        b->cap = ncap;
    }
    memcpy(b->p + b->n, src, n);
    b->n += n;
}
static void wb_u32(wbuf *b, u32 v) {
    u8 t[4];
    put_u32(t, v);
    wb_put(b, t, 4);
}
static void wb_u64(wbuf *b, u64 v) {
    u8 t[8];
    put_u64(t, v);
    wb_put(b, t, 8);
}
static void wb_str(wbuf *b, const char *s) {
    size_t n = strlen(s);
    wb_u32(b, (u32)n);
    wb_put(b, s, n);
}
static void wb_free(wbuf *b) {
    if (b->p != NULL)
        DE_FREE(b->a, b->p, b->cap);
    b->p = NULL;
    b->n = b->cap = 0;
}

/* A bounds-checked reader. */
typedef struct rbuf {
    const u8 *p;
    size_t n, off;
    bool bad;
} rbuf;

static const u8 *rb_take(rbuf *b, u64 n) {
    if (b->bad || n > (u64)(b->n - b->off)) {
        b->bad = true;
        return NULL;
    }
    const u8 *q = b->p + b->off;
    b->off += (size_t)n;
    return q;
}
static u32 rb_u32(rbuf *b) {
    const u8 *q = rb_take(b, 4);
    return q != NULL ? get_u32(q) : 0u;
}
static u64 rb_u64(rbuf *b) {
    const u8 *q = rb_take(b, 8);
    return q != NULL ? get_u64(q) : 0u;
}
/* A string: its bytes (not NUL-terminated) and length. Refuses an embedded NUL. */
static const char *rb_str(rbuf *b, u32 *len) {
    *len = rb_u32(b);
    const u8 *q = rb_take(b, *len);
    if (q == NULL)
        return NULL;
    if (memchr(q, 0, *len) != NULL) {
        b->bad = true;
        return NULL;
    }
    return (const char *)q;
}

/* ---- Keys ------------------------------------------------------------------------------------ */

static void key_u32(XXH3_state_t *s, u32 v) {
    u8 t[4];
    put_u32(t, v);
    (void)XXH3_128bits_update(s, t, 4);
}
static void key_u64(XXH3_state_t *s, u64 v) {
    u8 t[8];
    put_u64(t, v);
    (void)XXH3_128bits_update(s, t, 8);
}
static void key_str(XXH3_state_t *s, const char *str) {
    size_t n = strlen(str);
    key_u32(s, (u32)n);
    (void)XXH3_128bits_update(s, str, n);
}

/* One extra source as the full key sees it. */
typedef struct key_extra {
    const char *path;
    bool present;
    u8 hash[16];
} key_extra;

/* The base key (`extras` NULL) or the full key of a cook (decache.h). */
static void key_compute(const de_cooker *c, const de__decache_probe *p, const u8 src_hash[16],
                        const key_extra *extras, u32 extra_count, u8 out[16]) {
    XXH3_state_t s;
    (void)XXH3_128bits_reset(&s);
    (void)XXH3_128bits_update(&s, extras != NULL ? "DECK" : "DEBK", 4);
    key_u32(&s, DE__DECACHE_FORMAT);
    key_u32(&s, (u32)ZSTD_VERSION_NUMBER);
    key_u32(&s, (u32)c->platform);
    key_u32(&s, p->importer_version);
    key_u64(&s, p->settings_hash);
    key_str(&s, p->type);
    key_str(&s, p->id);
    key_str(&s, p->rel);
    (void)XXH3_128bits_update(&s, src_hash, 16);
    if (extras != NULL) {
        key_u32(&s, extra_count);
        for (u32 i = 0; i < extra_count; ++i) {
            key_str(&s, extras[i].path);
            u8 present = extras[i].present ? 1u : 0u;
            (void)XXH3_128bits_update(&s, &present, 1);
            (void)XXH3_128bits_update(&s, extras[i].hash, 16);
        }
    }
    XXH128_canonical_t can;
    XXH128_canonicalFromHash(&can, XXH3_128bits_digest(&s));
    memcpy(out, can.digest, 16);
}

static void key_hex(const u8 key[16], char out[33]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        out[2 * i] = hex[key[i] >> 4];
        out[2 * i + 1] = hex[key[i] & 15u];
    }
    out[32] = '\0';
}

/* `dir` joined with `name` ("dev:/" + "a" -> "dev:/a", "dev:/x" + "a" -> "dev:/x/a"). */
static bool join(char *buf, size_t cap, const char *dir, const char *name) {
    size_t n = strlen(dir);
    bool slash = n > 0 && dir[n - 1] == '/';
    int w = snprintf(buf, cap, "%s%s%s", dir, slash ? "" : "/", name);
    return w > 0 && (size_t)w < cap;
}

bool de__decache_entry_path(const de_cooker *c, const u8 key[16], char *buf, size_t cap) {
    if (c == NULL || c->cache == NULL)
        return false;
    char hex[33], name[40];
    key_hex(key, hex);
    snprintf(name, sizeof name, "%.2s/%s.dc", hex, hex);
    return join(buf, cap, c->cache->dir, name);
}

/* ---- Lifetime -------------------------------------------------------------------------------- */

de_result de__decache_create(de_cooker *c) {
    if (c->cache_dir == NULL)
        return DE_OK;
    de__decache *d = DE_NEW(c->alloc, de__decache, DE_MEM_ASSET);
    if (d == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_cooker_create: out of memory for the cache");
    memset(d, 0, sizeof *d);
    d->alloc = c->alloc;
    d->racy_ns = DE__DECACHE_RACY_NS;
    d->dir_ok = de_path_normalize(c->cache_dir, d->dir, sizeof d->dir) == DE_OK;
    if (!d->dir_ok)
        de_error_clear(); /* reported once, at the first use */
    de_hashmap_desc hd = {.allocator = c->alloc,
                          .key_size = sizeof(u64),
                          .value_size = sizeof(u32),
                          .tag = DE_MEM_ASSET};
    de_result r = de_hashmap_init(&d->map, &hd);
    if (r != DE_OK) {
        DE_FREE(c->alloc, d, sizeof *d);
        return r;
    }
    de_mutex_init(&d->lock);
    c->cache = d;
    return DE_OK;
}

void de__decache_destroy(de_cooker *c) {
    de__decache *d = c->cache;
    if (d == NULL)
        return;
    if (de__decache_index_write(d) != DE_OK) {
        const de_error *e = de_error_last();
        DE_WARN(d->log, "%s", e != NULL ? e->message : "cook cache: cannot write the index");
    }
    de__decache_index_free(d);
    de_mutex_destroy(&d->lock);
    DE_FREE(c->alloc, d, sizeof *d);
    c->cache = NULL;
}

void de__decache_set_racy_window(de_cooker *c, i64 ns) {
    if (c == NULL || c->cache == NULL)
        return;
    de_mutex_lock(&c->cache->lock);
    c->cache->racy_ns = ns;
    de_mutex_unlock(&c->cache->lock);
}

/* Opens the cache on first use: creates the directory and loads the index. False when the cache
 * cannot be used (logged once). */
static bool cache_open(de__decache *d) {
    de_mutex_lock(&d->lock);
    if (!d->opened) {
        d->opened = true;
        d->log = de_log_register_category("cook", DE_LOG_INFO);
        if (!d->dir_ok) {
            d->disabled = true;
            DE_WARN(d->log, "cook cache disabled: the directory is not a valid virtual path");
        } else if (de_dir_create(d->dir) != DE_OK) {
            const de_error *e = de_error_last();
            d->disabled = true;
            DE_WARN(d->log, "cook cache disabled: cannot create '%s': %s", d->dir,
                    e != NULL ? e->message : "error");
            de_error_clear();
        } else {
            de__decache_index_load(d);
        }
    }
    bool ok = !d->disabled;
    de_mutex_unlock(&d->lock);
    return ok;
}

void de_cooker_get_cache_stats(const de_cooker *c, de_cooker_cache_stats *out) {
    if (out == NULL)
        return;
    memset(out, 0, sizeof *out);
    if (c == NULL || c->cache == NULL)
        return;
    de__decache *d = c->cache;
    de_mutex_lock(&d->lock);
    *out = d->stats;
    de_mutex_unlock(&d->lock);
}

de_result de_cooker_flush_cache(de_cooker *c) {
    if (c == NULL || c->cache == NULL)
        return DE_OK;
    return de__decache_index_write(c->cache);
}

/* ---- Reading entries ------------------------------------------------------------------------- */

typedef enum read_status { READ_OK, READ_MISSING, READ_DAMAGED, READ_FAILED } read_status;

/* A parsed entry file: the body in `body` (owned), and the header fields. */
typedef struct entry {
    u32 flags;
    u8 *body;
    size_t body_size;
} entry;

static void entry_free(de__decache *d, entry *e) {
    if (e->body != NULL)
        DE_FREE(d->alloc, e->body, e->body_size != 0 ? e->body_size : 1u);
    e->body = NULL;
}

/* Deletes a damaged file and logs it. */
static void drop_damaged(de__decache *d, const char *path, const char *why) {
    DE_WARN(d->log, "cook cache: '%s' is damaged (%s); deleted, cooking again", path, why);
    if (de_file_delete(path) != DE_OK)
        de_error_clear();
    de_mutex_lock(&d->lock);
    d->stats.corrupt += 1u;
    de_mutex_unlock(&d->lock);
}

/* Reads and checks the entry file of `key`. */
static read_status read_entry(de_cooker *c, const u8 key[16], entry *out) {
    de__decache *d = c->cache;
    memset(out, 0, sizeof *out);
    char path[DE_FS_PATH_MAX];
    if (!de__decache_entry_path(c, key, path, sizeof path))
        return READ_FAILED;
    void *data = NULL;
    u64 size = 0;
    de_result r = de_file_read_all(path, d->alloc, &data, &size);
    if (r != DE_OK) {
        de_error_clear();
        return r == DE_ERR_NOT_FOUND ? READ_MISSING : READ_FAILED;
    }
    de_mutex_lock(&d->lock);
    d->stats.bytes_read += size;
    de_mutex_unlock(&d->lock);

    const u8 *p = (const u8 *)data;
    const char *why = NULL;
    u32 flags = 0;
    u64 raw = 0, stored = 0, body_hash = 0;
    if (size < HEADER_SIZE)
        why = "truncated header";
    else if (memcmp(p, "DECE", 4) != 0)
        why = "bad magic";
    else if (get_u32(p + 4) != DE__DECACHE_FORMAT || get_u32(p + 8) != (u32)ZSTD_VERSION_NUMBER)
        why = "another cache format";
    else if (memcmp(p + 16, key, 16) != 0)
        why = "key mismatch";
    if (why == NULL) {
        flags = get_u32(p + 12);
        raw = get_u64(p + 32);
        stored = get_u64(p + 40);
        body_hash = get_u64(p + 48);
        if ((flags & ~(FLAG_REDIRECT | FLAG_ZSTD)) != 0)
            why = "unknown flags";
        else if (stored != size - HEADER_SIZE)
            why = "size mismatch (truncated?)";
        else if (raw > (u64)SIZE_MAX / 2u || ((flags & FLAG_ZSTD) == 0 && raw != stored))
            why = "bad body size";
    }
    u8 *body = NULL;
    size_t body_alloc = 0;
    if (why == NULL) {
        body_alloc = raw != 0 ? (size_t)raw : 1u;
        if ((flags & FLAG_ZSTD) != 0) {
            unsigned long long fcs = ZSTD_getFrameContentSize(p + HEADER_SIZE, (size_t)stored);
            if (fcs != (unsigned long long)raw)
                why = "zstd frame size mismatch";
        }
        if (why == NULL) {
            body = (u8 *)DE_ALLOC(d->alloc, body_alloc, 16, DE_MEM_ASSET);
            if (body == NULL) {
                DE_FREE(d->alloc, data, (size_t)size + 1u);
                return READ_FAILED;
            }
            if ((flags & FLAG_ZSTD) != 0) {
                size_t got = ZSTD_decompress(body, (size_t)raw, p + HEADER_SIZE, (size_t)stored);
                if (ZSTD_isError(got) || got != (size_t)raw)
                    why = "zstd error";
            } else if (raw != 0) {
                memcpy(body, p + HEADER_SIZE, (size_t)raw);
            }
        }
        if (why == NULL && XXH3_64bits(body, (size_t)raw) != body_hash)
            why = "body hash mismatch";
    }
    DE_FREE(d->alloc, data, (size_t)size + 1u);
    if (why != NULL) {
        if (body != NULL)
            DE_FREE(d->alloc, body, body_alloc);
        drop_damaged(d, path, why);
        return READ_DAMAGED;
    }
    out->flags = flags;
    out->body = body;
    out->body_size = (size_t)raw;
    return READ_OK;
}

/* The extra source paths of a redirect body, or of the tail of an entry body: parses `count`
 * strings from `b` into `paths` (pointers into the body, lengths in `lens`). */
typedef struct path_list {
    const char **paths; /* not NUL-terminated */
    u32 *lens;
    u32 count;
    size_t alloc_count;
} path_list;

static void path_list_free(de__decache *d, path_list *l) {
    if (l->paths != NULL)
        DE_FREE(d->alloc, (void *)l->paths, sizeof(const char *) * l->alloc_count);
    if (l->lens != NULL)
        DE_FREE(d->alloc, l->lens, sizeof(u32) * l->alloc_count);
    memset(l, 0, sizeof *l);
}

static bool path_list_read(de__decache *d, rbuf *b, path_list *l) {
    memset(l, 0, sizeof *l);
    u32 n = rb_u32(b);
    if (b->bad || (u64)n > (u64)(b->n - b->off) / 4u) {
        b->bad = true;
        return false;
    }
    l->alloc_count = n != 0 ? n : 1u;
    l->paths = (const char **)DE_ALLOC(d->alloc, sizeof(const char *) * l->alloc_count,
                                       DE_ALIGNOF(const char *), DE_MEM_ASSET);
    l->lens =
        (u32 *)DE_ALLOC(d->alloc, sizeof(u32) * l->alloc_count, DE_ALIGNOF(u32), DE_MEM_ASSET);
    if (l->paths == NULL || l->lens == NULL) {
        path_list_free(d, l);
        return false;
    }
    for (u32 i = 0; i < n; ++i) {
        l->paths[i] = rb_str(b, &l->lens[i]);
        if (l->paths[i] == NULL || l->lens[i] == 0 || l->lens[i] >= DE_FS_PATH_MAX) {
            b->bad = true;
            path_list_free(d, l);
            return false;
        }
    }
    l->count = n;
    return true;
}

static size_t align_up(size_t v, size_t a) {
    return (v + (a - 1u)) & ~(a - 1u);
}

/* Builds a de_cook_result from an entry body (the layout of cooker.c's build_result). False when
 * the body does not parse (`*damaged`) or memory runs out. */
static bool result_from_body(de_cooker *c, const de__decache_probe *p, const entry *e,
                             const path_list *expect_extras, de_cook_result *out, bool *damaged) {
    de__decache *d = c->cache;
    *damaged = true;
    rbuf b = {.p = e->body, .n = e->body_size};
    u64 output_hash = rb_u64(&b);
    u64 data_size = rb_u64(&b);
    const u8 *data = rb_take(&b, data_size);
    u32 dep_count = rb_u32(&b);
    if (b.bad || (u64)dep_count > (u64)(b.n - b.off) / 8u)
        return false;
    const u8 *deps = rb_take(&b, (u64)dep_count * 8u);
    u32 sub_count = rb_u32(&b);
    if (b.bad || (u64)sub_count > (u64)(b.n - b.off) / 24u)
        return false;
    /* First pass over the sub-assets: sizes only. */
    size_t sub_start = b.off;
    size_t str_bytes = strlen(p->rel) + 1u;
    size_t sub_data_bytes = 0;
    for (u32 i = 0; i < sub_count && !b.bad; ++i) {
        u32 nl = 0, tl = 0;
        (void)rb_str(&b, &nl);
        (void)rb_str(&b, &tl);
        (void)rb_u64(&b);
        u64 sz = rb_u64(&b);
        (void)rb_take(&b, sz);
        if (nl == 0 || tl == 0)
            b.bad = true;
        str_bytes += (size_t)nl + 1u + (size_t)tl + 1u;
        sub_data_bytes = align_up(sub_data_bytes + (size_t)sz, 16);
    }
    path_list extras;
    if (b.bad || !path_list_read(d, &b, &extras))
        return false;
    bool ok = !b.bad && b.off == b.n;
    /* The entry's extra sources must be the list the key was computed from. */
    u32 want = expect_extras != NULL ? expect_extras->count : 0u;
    ok = ok && extras.count == want;
    for (u32 i = 0; ok && i < want; ++i)
        ok = extras.lens[i] == expect_extras->lens[i] &&
             memcmp(extras.paths[i], expect_extras->paths[i], extras.lens[i]) == 0;
    if (!ok) {
        path_list_free(d, &extras);
        return false;
    }
    for (u32 i = 0; i < extras.count; ++i)
        str_bytes += (size_t)extras.lens[i] + 1u;
    *damaged = false;

    size_t off_deps = align_up((size_t)data_size, DE_ALIGNOF(de_asset_id));
    size_t size = off_deps + sizeof(de_asset_id) * dep_count;
    size_t off_subs = align_up(size, DE_ALIGNOF(de_cook_subasset));
    size = off_subs + sizeof(de_cook_subasset) * sub_count;
    size_t off_extra = align_up(size, DE_ALIGNOF(const char *));
    size = off_extra + sizeof(const char *) * extras.count;
    size_t off_sub_data = align_up(size, 16);
    size = off_sub_data + sub_data_bytes;
    size_t off_str = size;
    size += str_bytes;
    u8 *block = (u8 *)DE_ALLOC(c->alloc, size, 16, DE_MEM_ASSET);
    if (block == NULL) {
        path_list_free(d, &extras);
        return false;
    }
    if (data_size != 0)
        memcpy(block, data, (size_t)data_size);
    de_asset_id *dep_out = (de_asset_id *)(void *)(block + off_deps);
    for (u32 i = 0; i < dep_count; ++i)
        dep_out[i].hash = get_u64(deps + 8u * i);
    de_cook_subasset *subs = (de_cook_subasset *)(void *)(block + off_subs);
    const char **extra_out = (const char **)(void *)(block + off_extra);
    u8 *sub_data = block + off_sub_data;
    char *str = (char *)(block + off_str);

    size_t n = strlen(p->rel) + 1u;
    memcpy(str, p->rel, n);
    out->source = str;
    str += n;
    rbuf s = {.p = e->body, .n = e->body_size, .off = sub_start};
    for (u32 i = 0; i < sub_count; ++i) {
        u32 nl = 0, tl = 0;
        const char *name = rb_str(&s, &nl);
        const char *type = rb_str(&s, &tl);
        u64 id = rb_u64(&s);
        u64 sz = rb_u64(&s);
        const u8 *bytes = rb_take(&s, sz);
        de_cook_subasset *dst = &subs[i];
        memcpy(str, name, nl);
        str[nl] = '\0';
        dst->name = str;
        str += nl + 1u;
        memcpy(str, type, tl);
        str[tl] = '\0';
        dst->type = str;
        str += tl + 1u;
        dst->id.hash = id;
        dst->size = sz;
        if (sz != 0) {
            memcpy(sub_data, bytes, (size_t)sz);
            dst->data = sub_data;
            sub_data = block + align_up((size_t)(sub_data - block) + (size_t)sz, 16);
        } else {
            dst->data = "";
        }
    }
    for (u32 i = 0; i < extras.count; ++i) {
        memcpy(str, extras.paths[i], extras.lens[i]);
        str[extras.lens[i]] = '\0';
        extra_out[i] = str;
        str += extras.lens[i] + 1u;
    }
    out->data = data_size != 0 ? (const void *)block : (const void *)"";
    out->size = data_size;
    out->dependencies = dep_count != 0 ? dep_out : NULL;
    out->dependency_count = dep_count;
    out->subassets = sub_count != 0 ? subs : NULL;
    out->subasset_count = sub_count;
    out->extra_sources = extras.count != 0 ? extra_out : NULL;
    out->extra_source_count = extras.count;
    out->settings_hash = p->settings_hash;
    out->importer_version = p->importer_version;
    out->platform = c->platform;
    out->output_hash = output_hash;
    out->alloc_ = c->alloc;
    out->block_ = block;
    out->block_size_ = size;
    path_list_free(d, &extras);
    return true;
}

/* Hashes the extra sources of `l` (paths relative to `root`) into `out` (`l->count` entries;
 * paths NUL-terminated into `names`, DE_FS_PATH_MAX bytes each). */
static de_result hash_extras(de__decache *d, const char *root, const char *const *paths,
                             const u32 *lens, u32 count, key_extra *out, char *names) {
    for (u32 i = 0; i < count; ++i) {
        char *name = names + (size_t)i * DE_FS_PATH_MAX;
        size_t len = lens != NULL ? lens[i] : strlen(paths[i]);
        if (len >= DE_FS_PATH_MAX)
            return DE_ERR_INVALID_ARGUMENT;
        memcpy(name, paths[i], len);
        name[len] = '\0';
        char vpath[DE_FS_PATH_MAX];
        if (!join(vpath, sizeof vpath, root, name))
            return DE_ERR_INVALID_ARGUMENT;
        out[i].path = name;
        de_result r = de__decache_file_hash(d, vpath, out[i].hash, &out[i].present, NULL, NULL);
        if (r != DE_OK)
            return r;
    }
    return DE_OK;
}

/* Scratch for `count` extra sources: key_extra entries and their names. */
typedef struct extra_scratch {
    key_extra *ex;
    char *names;
    u32 count;
} extra_scratch;

static bool extra_scratch_alloc(de__decache *d, extra_scratch *s, u32 count) {
    memset(s, 0, sizeof *s);
    s->count = count;
    if (count == 0)
        return true;
    s->ex = (key_extra *)DE_ALLOC(d->alloc, sizeof(key_extra) * count, DE_ALIGNOF(key_extra),
                                  DE_MEM_ASSET);
    s->names = (char *)DE_ALLOC(d->alloc, (size_t)DE_FS_PATH_MAX * count, 1, DE_MEM_ASSET);
    if (s->ex == NULL || s->names == NULL) {
        if (s->ex != NULL)
            DE_FREE(d->alloc, s->ex, sizeof(key_extra) * count);
        if (s->names != NULL)
            DE_FREE(d->alloc, s->names, (size_t)DE_FS_PATH_MAX * count);
        memset(s, 0, sizeof *s);
        return false;
    }
    return true;
}

static void extra_scratch_free(de__decache *d, extra_scratch *s) {
    if (s->ex != NULL)
        DE_FREE(d->alloc, s->ex, sizeof(key_extra) * s->count);
    if (s->names != NULL)
        DE_FREE(d->alloc, s->names, (size_t)DE_FS_PATH_MAX * s->count);
    memset(s, 0, sizeof *s);
}

/* ---- Lookup ---------------------------------------------------------------------------------- */

static void count_lookup(de__decache *d, bool hit, u64 t0) {
    u64 dt = de_time_now_ns() - t0;
    de_mutex_lock(&d->lock);
    if (hit)
        d->stats.hits += 1u;
    else
        d->stats.misses += 1u;
    d->stats.lookup_ns += dt;
    de_mutex_unlock(&d->lock);
}

de_result de__decache_lookup(de_cooker *c, de__decache_probe *p, const char *root, const char *rel,
                             const char *id, const char *type, u32 importer_version,
                             u64 settings_hash, de_cook_result *out) {
    memset(p, 0, sizeof *p);
    if (c == NULL || c->cache == NULL)
        return DE_ERR_NOT_FOUND;
    de__decache *d = c->cache;
    u64 t0 = de_time_now_ns();
    p->active = true;
    p->root = root;
    p->rel = rel;
    p->id = id;
    p->type = type;
    p->importer_version = importer_version;
    p->settings_hash = settings_hash;
    p->start_utc_ns = de__decache_now_utc_ns();
    if (!cache_open(d)) {
        count_lookup(d, false, t0);
        return DE_ERR_NOT_FOUND;
    }

    char vpath[DE_FS_PATH_MAX];
    u8 src_hash[16];
    bool present = false;
    if (!join(vpath, sizeof vpath, root, rel) ||
        de__decache_file_hash(d, vpath, src_hash, &present, &p->source, &p->source_size) != DE_OK ||
        !present) {
        de_error_clear(); /* the cook reads the source itself and reports the error */
        count_lookup(d, false, t0);
        return DE_ERR_NOT_FOUND;
    }
    key_compute(c, p, src_hash, NULL, 0, p->base_key);

    bool hit = false;
    entry e;
    read_status st = read_entry(c, p->base_key, &e);
    if (st == READ_OK && (e.flags & FLAG_REDIRECT) == 0) {
        bool damaged = false;
        hit = result_from_body(c, p, &e, NULL, out, &damaged);
        if (damaged) {
            char path[DE_FS_PATH_MAX];
            if (de__decache_entry_path(c, p->base_key, path, sizeof path))
                drop_damaged(d, path, "the body does not parse");
        }
        entry_free(d, &e);
    } else if (st == READ_OK) {
        /* A redirect: the full key needs the extra sources' hashes. */
        rbuf b = {.p = e.body, .n = e.body_size};
        path_list l;
        bool parsed = path_list_read(d, &b, &l);
        if (parsed && (b.bad || b.off != b.n || l.count == 0)) {
            path_list_free(d, &l);
            parsed = false;
        }
        if (!parsed) {
            char path[DE_FS_PATH_MAX];
            if (de__decache_entry_path(c, p->base_key, path, sizeof path))
                drop_damaged(d, path, "the redirect does not parse");
        } else {
            extra_scratch s;
            if (extra_scratch_alloc(d, &s, l.count) &&
                hash_extras(d, root, l.paths, l.lens, l.count, s.ex, s.names) == DE_OK) {
                u8 full[16];
                key_compute(c, p, src_hash, s.ex, l.count, full);
                entry fe;
                if (read_entry(c, full, &fe) == READ_OK) {
                    bool damaged = (fe.flags & FLAG_REDIRECT) != 0;
                    if (!damaged)
                        hit = result_from_body(c, p, &fe, &l, out, &damaged);
                    if (damaged) {
                        char path[DE_FS_PATH_MAX];
                        if (de__decache_entry_path(c, full, path, sizeof path))
                            drop_damaged(d, path, "the body does not parse");
                    }
                    entry_free(d, &fe);
                }
            }
            de_error_clear();
            extra_scratch_free(d, &s);
            path_list_free(d, &l);
        }
        entry_free(d, &e);
    }
    if (!hit)
        memset(out, 0, sizeof *out);
    count_lookup(d, hit, t0);
    if (hit && p->source != NULL) {
        DE_FREE(c->alloc, p->source, (size_t)p->source_size + 1u);
        p->source = NULL;
        p->source_size = 0;
    }
    return hit ? DE_OK : DE_ERR_NOT_FOUND;
}

/* ---- Store ----------------------------------------------------------------------------------- */

/* Writes one cache file atomically. A file already there with the same name is a success:
 * entries are content-addressed, so it holds the same bytes. */
static de_result write_file(de_cooker *c, const u8 key[16], const u8 *data, size_t size,
                            bool replace, bool *skipped) {
    de__decache *d = c->cache;
    *skipped = false;
    char path[DE_FS_PATH_MAX], shard[DE_FS_PATH_MAX];
    if (!de__decache_entry_path(c, key, path, sizeof path))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "cook cache: the entry path is too long");
    if (!replace && de_path_exists(path)) {
        *skipped = true;
        return DE_OK;
    }
    /* The shard directory, once per cooker. */
    u32 bit = key[0];
    de_mutex_lock(&d->lock);
    bool made = (d->shards_made[bit >> 6] >> (bit & 63u)) & 1u;
    de_mutex_unlock(&d->lock);
    if (!made) {
        memcpy(shard, path, strlen(path) + 1u);
        char *slash = strrchr(shard, '/');
        if (slash != NULL)
            *slash = '\0';
        de_result r = de_dir_create(shard);
        if (r != DE_OK)
            return r;
        de_mutex_lock(&d->lock);
        d->shards_made[bit >> 6] |= (u64)1 << (bit & 63u);
        de_mutex_unlock(&d->lock);
    }
    de_result r = DE_OK;
    for (int attempt = 0; attempt < WRITE_ATTEMPTS; ++attempt) {
        r = de_file_write_all_atomic(path, data, size);
        if (r == DE_OK)
            break;
        /* Another writer may have renamed the same entry in at this moment. */
        if (!replace && de_path_exists(path)) {
            de_error_clear();
            *skipped = true;
            return DE_OK;
        }
        de_sleep_ns(1000000);
    }
    if (r != DE_OK && replace && de_path_exists(path)) {
        /* A redirect another cook wrote at the same moment: it lists the same paths. */
        de_error_clear();
        *skipped = true;
        return DE_OK;
    }
    if (r == DE_OK) {
        de_mutex_lock(&d->lock);
        d->stats.bytes_written += size;
        de_mutex_unlock(&d->lock);
    }
    return r;
}

/* Header + body into one buffer: `body` compressed when `compress`. */
static de_result make_file(de__decache *d, const u8 key[16], u32 flags, const wbuf *body,
                           bool compress, wbuf *out) {
    out->a = d->alloc;
    size_t bound = compress ? ZSTD_compressBound(body->n) : body->n;
    u8 header[HEADER_SIZE];
    memset(header, 0, sizeof header);
    wb_put(out, header, HEADER_SIZE);
    if (out->oom)
        return DE_ERR_OUT_OF_MEMORY;
    if (bound > out->cap - out->n) { /* reserve */
        size_t ncap = out->n + bound;
        void *q = DE_REALLOC(d->alloc, out->p, out->cap, ncap, 16, DE_MEM_ASSET);
        if (q == NULL)
            return DE_ERR_OUT_OF_MEMORY;
        out->p = (u8 *)q;
        out->cap = ncap;
    }
    size_t stored = body->n;
    if (compress) {
        stored = ZSTD_compress(out->p + HEADER_SIZE, bound, body->p != NULL ? body->p : header,
                               body->n, ZSTD_LEVEL);
        if (ZSTD_isError(stored))
            return DE_FAIL(DE_ERR_INTERNAL, "cook cache: zstd: %s", ZSTD_getErrorName(stored));
        flags |= FLAG_ZSTD;
    } else if (body->n != 0) {
        memcpy(out->p + HEADER_SIZE, body->p, body->n);
    }
    out->n = HEADER_SIZE + stored;
    u8 *h = out->p;
    memcpy(h, "DECE", 4);
    put_u32(h + 4, DE__DECACHE_FORMAT);
    put_u32(h + 8, (u32)ZSTD_VERSION_NUMBER);
    put_u32(h + 12, flags);
    memcpy(h + 16, key, 16);
    put_u64(h + 32, (u64)body->n);
    put_u64(h + 40, (u64)stored);
    put_u64(h + 48, XXH3_64bits(body->p != NULL ? body->p : header, body->n));
    return DE_OK;
}

static void store_warn(de__decache *d, const char *id, de_result r) {
    const de_error *e = de_error_last();
    DE_WARN(d->log, "cook cache: cannot store asset \"%s\": %s", id,
            e != NULL && e->result == r ? e->message : de_result_str(r));
    de_error_clear();
    de_mutex_lock(&d->lock);
    d->stats.store_failures += 1u;
    de_mutex_unlock(&d->lock);
}

void de__decache_store(de_cooker *c, const de__decache_probe *p, const void *source,
                       u64 source_size, const de_cook_result *r) {
    if (c == NULL || c->cache == NULL || p == NULL || !p->active || r == NULL)
        return;
    de__decache *d = c->cache;
    if (!cache_open(d))
        return;
    u64 t0 = de_time_now_ns();

    /* The key of the bytes that were imported (not of what the index said before the read). */
    u8 src_hash[16];
    de__decache_hash128(source, (size_t)source_size, src_hash);
    u8 base[16], full[16];
    key_compute(c, p, src_hash, NULL, 0, base);

    de_result res = DE_OK;
    extra_scratch s;
    memset(&s, 0, sizeof s);
    const u8 *entry_key = base;
    if (r->extra_source_count != 0) {
        if (!extra_scratch_alloc(d, &s, r->extra_source_count))
            res = DE_ERR_OUT_OF_MEMORY;
        else
            res = hash_extras(d, p->root, r->extra_sources, NULL, r->extra_source_count, s.ex,
                              s.names);
        if (res == DE_OK) {
            key_compute(c, p, src_hash, s.ex, r->extra_source_count, full);
            entry_key = full;
        }
    }
    /* The extra sources are hashed now, after the importer read them. One that changed less than
     * the racy window before the cook began may have changed during the import, and its hash
     * need not describe what the importer read: store nothing (the next cook tries again). */
    bool deferred = false;
    if (res == DE_OK && r->extra_source_count != 0) {
        de_mutex_lock(&d->lock);
        i64 racy = d->racy_ns;
        de_mutex_unlock(&d->lock);
        for (u32 i = 0; i < r->extra_source_count && !deferred; ++i) {
            if (!s.ex[i].present)
                continue;
            char vpath[DE_FS_PATH_MAX];
            u64 size = 0;
            i64 mtime = 0;
            deferred = !join(vpath, sizeof vpath, p->root, s.ex[i].path) ||
                       de__decache_file_stat(vpath, &size, &mtime) != DE_OK ||
                       mtime > p->start_utc_ns - racy;
        }
    }

    wbuf body = {.a = d->alloc}, file = {.a = d->alloc};
    u32 written = 0, skipped_n = 0;
    if (res == DE_OK && !deferred) {
        wb_u64(&body, r->output_hash);
        wb_u64(&body, r->size);
        wb_put(&body, r->data, (size_t)r->size);
        wb_u32(&body, r->dependency_count);
        for (u32 i = 0; i < r->dependency_count; ++i)
            wb_u64(&body, r->dependencies[i].hash);
        wb_u32(&body, r->subasset_count);
        for (u32 i = 0; i < r->subasset_count; ++i) {
            const de_cook_subasset *sa = &r->subassets[i];
            wb_str(&body, sa->name);
            wb_str(&body, sa->type);
            wb_u64(&body, sa->id.hash);
            wb_u64(&body, sa->size);
            wb_put(&body, sa->data, (size_t)sa->size);
        }
        wb_u32(&body, r->extra_source_count);
        for (u32 i = 0; i < r->extra_source_count; ++i)
            wb_str(&body, r->extra_sources[i]);
        res = body.oom ? DE_ERR_OUT_OF_MEMORY : make_file(d, entry_key, 0, &body, true, &file);
    }
    bool skipped = false;
    if (res == DE_OK && !deferred) {
        /* The entry first, then the redirect that leads to it. */
        res = write_file(c, entry_key, file.p, file.n, false, &skipped);
        written += skipped ? 0u : 1u;
        skipped_n += skipped ? 1u : 0u;
    }
    if (res == DE_OK && !deferred && r->extra_source_count != 0) {
        wb_free(&body);
        wb_free(&file);
        body.a = file.a = d->alloc;
        wb_u32(&body, r->extra_source_count);
        for (u32 i = 0; i < r->extra_source_count; ++i)
            wb_str(&body, r->extra_sources[i]);
        res = body.oom ? DE_ERR_OUT_OF_MEMORY
                       : make_file(d, base, FLAG_REDIRECT, &body, false, &file);
        if (res == DE_OK) {
            res = write_file(c, base, file.p, file.n, true, &skipped);
            written += skipped ? 0u : 1u;
        }
    }
    wb_free(&body);
    wb_free(&file);
    extra_scratch_free(d, &s);
    if (res != DE_OK)
        store_warn(d, p->id, res);

    u64 dt = de_time_now_ns() - t0;
    de_mutex_lock(&d->lock);
    d->stats.stores += written;
    d->stats.store_skipped += skipped_n;
    d->stats.store_deferred += deferred ? 1u : 0u;
    d->stats.store_ns += dt;
    d->stores_since_write += 1u;
    bool write_index = d->stores_since_write >= DE__DECACHE_INDEX_WRITE_EVERY && d->index_dirty;
    if (write_index)
        d->stores_since_write = 0;
    de_mutex_unlock(&d->lock);
    if (write_index && de__decache_index_write(d) != DE_OK) {
        const de_error *e = de_error_last();
        DE_WARN(d->log, "%s", e != NULL ? e->message : "cook cache: cannot write the index");
        de_error_clear();
    }
}
