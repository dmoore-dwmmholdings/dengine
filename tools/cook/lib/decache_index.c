/*
 * decache_index.c: the warm path of the cook cache (DENG-210, SPEC §11.5). `index.bin` maps a
 * file's virtual path to the size and mtime it had when its bytes were hashed, and to that hash,
 * so a lookup whose files did not change reads no source byte. The index is a hint: keys are
 * always content hashes.
 *
 * `<cache_dir>/index.bin`, little-endian:
 *
 *   "DEIX" u32 version (1) u32 record count u32 0
 *   per record, paths sorted bytewise: u32 path length, path bytes, u64 size, i64 mtime (ns since
 *     1970 UTC), 16 bytes XXH3-128 of the bytes
 *   u64 XXH3-64 (seed 0) of everything before it
 *
 * Racy files: file systems stamp mtimes with a coarse clock (a few milliseconds on Linux, 1 s on
 * HFS+, 2 s on FAT). A file written again within the same tick, with the same size, would keep
 * its record. So a file is recorded only when its mtime is at least the racy window (2 s) older
 * than the moment it was hashed; a younger file is hashed again on its next lookup.
 */

#if defined(__linux__)
#define _GNU_SOURCE /* st_mtim, timespec_get */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE /* st_mtimespec */
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "cook/lib/decache.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "core/de_hashmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

#define INDEX_VERSION 1u
#define INDEX_NAME    "index.bin"

/* ---- Hashing --------------------------------------------------------------------------------- */

void de__decache_hash128(const void *data, size_t n, u8 out[16]) {
    XXH128_canonical_t can;
    XXH128_canonicalFromHash(&can, XXH3_128bits(data, n));
    memcpy(out, can.digest, 16);
}

/* ---- Native stat ----------------------------------------------------------------------------- */

de_result de__decache_file_stat(const char *vpath, u64 *size, i64 *mtime_ns) {
    *size = 0;
    *mtime_ns = 0;
    char native[DE_FS_PATH_MAX];
    de_result r = de_fs_resolve(vpath, native, sizeof native);
    if (r != DE_OK)
        return r;
#if defined(_WIN32)
    wchar_t w[DE_FS_PATH_MAX];
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, native, -1, w, DE_FS_PATH_MAX);
    if (n <= 0)
        return DE_ERR_INVALID_ARGUMENT;
    for (int i = 0; i < n; ++i)
        if (w[i] == L'/')
            w[i] = L'\\';
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &fad))
        return DE_ERR_NOT_FOUND;
    if ((fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return DE_ERR_NOT_FOUND;
    *size = ((u64)fad.nFileSizeHigh << 32) | (u64)fad.nFileSizeLow;
    /* FILETIME: 100 ns ticks since 1601-01-01. */
    u64 ticks =
        ((u64)fad.ftLastWriteTime.dwHighDateTime << 32) | (u64)fad.ftLastWriteTime.dwLowDateTime;
    *mtime_ns = ((i64)ticks - (i64)116444736000000000) * 100;
#else
    struct stat st;
    if (stat(native, &st) != 0 || !S_ISREG(st.st_mode))
        return DE_ERR_NOT_FOUND;
    *size = (u64)st.st_size;
#if defined(__APPLE__)
    *mtime_ns = (i64)st.st_mtimespec.tv_sec * 1000000000 + (i64)st.st_mtimespec.tv_nsec;
#else
    *mtime_ns = (i64)st.st_mtim.tv_sec * 1000000000 + (i64)st.st_mtim.tv_nsec;
#endif
#endif
    return DE_OK;
}

/* Wall-clock now in the mtime timebase (ns since 1970 UTC). */
i64 de__decache_now_utc_ns(void) {
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC)
        return 0; /* no clock: every file looks racy, so none is recorded */
    return (i64)ts.tv_sec * 1000000000 + (i64)ts.tv_nsec;
}

/* ---- Records --------------------------------------------------------------------------------- */

static u64 path_hash(const char *path) {
    return de_hash64_str(path);
}

/* The live record of `path`, or NULL. Under the lock. */
static de__decache_file *find_record(de__decache *d, const char *path) {
    u64 h = path_hash(path);
    const u32 *idx = (const u32 *)de_hashmap_get(&d->map, h, &h);
    if (idx == NULL)
        return NULL;
    de__decache_file *f = &d->files[*idx];
    return f->path != NULL && strcmp(f->path, path) == 0 ? f : NULL;
}

static void remove_record(de__decache *d, const char *path) {
    u64 h = path_hash(path);
    u32 idx = 0;
    if (!de_hashmap_remove(&d->map, h, &h, &idx))
        return;
    de__decache_file *f = &d->files[idx];
    if (f->path != NULL) {
        DE_FREE(d->alloc, f->path, f->path_size);
        f->path = NULL;
    }
    d->index_dirty = true;
}

/* Inserts or replaces the record of `path`. Under the lock. Out of memory: the index just does
 * not learn the file. */
static void put_record(de__decache *d, const char *path, u64 size, i64 mtime_ns,
                       const u8 hash[16]) {
    u64 h = path_hash(path);
    u32 *idx = (u32 *)de_hashmap_get(&d->map, h, &h);
    de__decache_file *f = NULL;
    if (idx != NULL) {
        f = &d->files[*idx];
        if (f->path == NULL || strcmp(f->path, path) != 0) { /* a 64-bit hash collision */
            if (f->path != NULL)
                DE_FREE(d->alloc, f->path, f->path_size);
            f->path = NULL;
        }
    } else {
        if (d->file_count == d->file_cap) {
            u32 ncap = d->file_cap != 0 ? d->file_cap * 2u : 64u;
            void *p = DE_REALLOC(d->alloc, d->files, sizeof(de__decache_file) * d->file_cap,
                                 sizeof(de__decache_file) * ncap, DE_ALIGNOF(de__decache_file),
                                 DE_MEM_ASSET);
            if (p == NULL)
                return;
            d->files = (de__decache_file *)p;
            d->file_cap = ncap;
        }
        u32 n = d->file_count;
        if (de_hashmap_put(&d->map, h, &h, &n) != DE_OK)
            return;
        f = &d->files[d->file_count++];
        memset(f, 0, sizeof *f);
    }
    if (f->path == NULL) {
        size_t ps = strlen(path) + 1u;
        f->path = (char *)DE_ALLOC(d->alloc, ps, 1, DE_MEM_ASSET);
        if (f->path == NULL) {
            (void)de_hashmap_remove(&d->map, h, &h, NULL);
            return;
        }
        memcpy(f->path, path, ps);
        f->path_size = ps;
    }
    f->size = size;
    f->mtime_ns = mtime_ns;
    memcpy(f->hash, hash, 16);
    d->index_dirty = true;
}

void de__decache_index_free(de__decache *d) {
    for (u32 i = 0; i < d->file_count; ++i)
        if (d->files[i].path != NULL)
            DE_FREE(d->alloc, d->files[i].path, d->files[i].path_size);
    if (d->files != NULL)
        DE_FREE(d->alloc, d->files, sizeof(de__decache_file) * d->file_cap);
    d->files = NULL;
    d->file_count = d->file_cap = 0;
    de_hashmap_free(&d->map);
}

/* ---- File hashes ----------------------------------------------------------------------------- */

de_result de__decache_file_hash(de__decache *d, const char *vpath, u8 hash[16], bool *present,
                                void **keep, u64 *keep_size) {
    memset(hash, 0, 16);
    *present = false;
    if (keep != NULL) {
        *keep = NULL;
        *keep_size = 0;
    }
    u64 size = 0;
    i64 mtime = 0;
    bool stat_ok = de__decache_file_stat(vpath, &size, &mtime) == DE_OK;
    if (stat_ok) {
        de_mutex_lock(&d->lock);
        const de__decache_file *f = find_record(d, vpath);
        bool hit = f != NULL && f->size == size && f->mtime_ns == mtime;
        if (hit)
            memcpy(hash, f->hash, 16);
        de_mutex_unlock(&d->lock);
        if (hit) {
            *present = true;
            return DE_OK;
        }
    }

    void *bytes = NULL;
    u64 n = 0;
    de_result r = de_file_read_all(vpath, d->alloc, &bytes, &n);
    if (r == DE_ERR_NOT_FOUND) {
        de_error_clear();
        de_mutex_lock(&d->lock);
        remove_record(d, vpath);
        de_mutex_unlock(&d->lock);
        return DE_OK; /* missing: part of the key as "not present" */
    }
    if (r != DE_OK)
        return r;
    de__decache_hash128(bytes, (size_t)n, hash);
    *present = true;
    i64 now = de__decache_now_utc_ns();

    de_mutex_lock(&d->lock);
    d->stats.source_bytes_read += n;
    d->stats.files_hashed += 1u;
    /* Record it only if the stat described these bytes (same size) and the file is old enough
     * that a later write gets another mtime. */
    if (stat_ok && size == n && now - mtime >= d->racy_ns)
        put_record(d, vpath, size, mtime, hash);
    else
        remove_record(d, vpath);
    de_mutex_unlock(&d->lock);

    if (keep != NULL) {
        *keep = bytes;
        *keep_size = n;
    } else {
        DE_FREE(d->alloc, bytes, (size_t)n + 1u);
    }
    return DE_OK;
}

/* ---- index.bin ------------------------------------------------------------------------------- */

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

static bool index_path(const de__decache *d, char *buf, size_t cap) {
    size_t n = strlen(d->dir);
    bool slash = n > 0 && d->dir[n - 1] == '/';
    int w = snprintf(buf, cap, "%s%s%s", d->dir, slash ? "" : "/", INDEX_NAME);
    return w > 0 && (size_t)w < cap;
}

void de__decache_index_load(de__decache *d) {
    char path[DE_FS_PATH_MAX];
    if (!index_path(d, path, sizeof path))
        return;
    void *data = NULL;
    u64 size = 0;
    de_result r = de_file_read_all(path, d->alloc, &data, &size);
    if (r != DE_OK) {
        de_error_clear(); /* no index yet: a cold cache */
        return;
    }
    const u8 *p = (const u8 *)data;
    bool ok = size >= 24u && memcmp(p, "DEIX", 4) == 0 && get_u32(p + 4) == INDEX_VERSION &&
              get_u64(p + size - 8u) == XXH3_64bits(p, (size_t)size - 8u);
    if (ok) {
        u32 count = get_u32(p + 8);
        u64 off = 16;
        u64 end = size - 8u;
        for (u32 i = 0; i < count && ok; ++i) {
            if (end - off < 4u) {
                ok = false;
                break;
            }
            u32 len = get_u32(p + off);
            off += 4u;
            if (len == 0 || len >= DE_FS_PATH_MAX || end - off < (u64)len + 32u) {
                ok = false;
                break;
            }
            char vpath[DE_FS_PATH_MAX];
            memcpy(vpath, p + off, len);
            vpath[len] = '\0';
            off += len;
            u64 fsize = get_u64(p + off);
            i64 mtime = (i64)get_u64(p + off + 8u);
            u8 hash[16];
            memcpy(hash, p + off + 16u, 16);
            off += 32u;
            if (strlen(vpath) != len) {
                ok = false;
                break;
            }
            put_record(d, vpath, fsize, mtime, hash);
        }
        ok = ok && off == end;
    }
    if (!ok) {
        DE_WARN(d->log, "cook cache: '%s' is damaged; starting with an empty index", path);
        de__decache_index_free(d);
        de_hashmap_desc hd = {.allocator = d->alloc,
                              .key_size = sizeof(u64),
                              .value_size = sizeof(u32),
                              .tag = DE_MEM_ASSET};
        (void)de_hashmap_init(&d->map, &hd);
        d->index_dirty = true; /* write a good one */
    } else {
        d->index_dirty = false;
    }
    DE_FREE(d->alloc, data, (size_t)size + 1u);
}

static int cmp_record_ptr(const void *a, const void *b) {
    const de__decache_file *x = *(const de__decache_file *const *)a;
    const de__decache_file *y = *(const de__decache_file *const *)b;
    return strcmp(x->path, y->path);
}

de_result de__decache_index_write(de__decache *d) {
    char path[DE_FS_PATH_MAX];
    if (!index_path(d, path, sizeof path))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "cook cache: the index path is too long");

    de_mutex_lock(&d->lock);
    if (!d->opened || d->disabled || !d->index_dirty) {
        de_mutex_unlock(&d->lock);
        return DE_OK;
    }
    u32 live = 0;
    size_t size = 16u + 8u;
    for (u32 i = 0; i < d->file_count; ++i)
        if (d->files[i].path != NULL) {
            ++live;
            size += 4u + (d->files[i].path_size - 1u) + 32u;
        }
    const de__decache_file **order = NULL;
    size_t order_size = sizeof(*order) * (live != 0 ? live : 1u);
    order =
        (const de__decache_file **)DE_ALLOC(d->alloc, order_size, DE_ALIGNOF(void *), DE_MEM_ASSET);
    u8 *buf = (u8 *)DE_ALLOC(d->alloc, size, 8, DE_MEM_ASSET);
    if (order == NULL || buf == NULL) {
        de_mutex_unlock(&d->lock);
        if (order != NULL)
            DE_FREE(d->alloc, order, order_size);
        if (buf != NULL)
            DE_FREE(d->alloc, buf, size);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "cook cache: out of memory for the index");
    }
    u32 k = 0;
    for (u32 i = 0; i < d->file_count; ++i)
        if (d->files[i].path != NULL)
            order[k++] = &d->files[i];
    if (live > 1u)
        qsort(order, live, sizeof(*order), cmp_record_ptr);
    memcpy(buf, "DEIX", 4);
    put_u32(buf + 4, INDEX_VERSION);
    put_u32(buf + 8, live);
    put_u32(buf + 12, 0);
    size_t off = 16;
    for (u32 i = 0; i < live; ++i) {
        const de__decache_file *f = order[i];
        u32 len = (u32)(f->path_size - 1u);
        put_u32(buf + off, len);
        off += 4u;
        memcpy(buf + off, f->path, len);
        off += len;
        put_u64(buf + off, f->size);
        put_u64(buf + off + 8u, (u64)f->mtime_ns);
        memcpy(buf + off + 16u, f->hash, 16);
        off += 32u;
    }
    put_u64(buf + off, XXH3_64bits(buf, off));
    d->index_dirty = false;
    d->stores_since_write = 0;
    de_mutex_unlock(&d->lock);
    DE_FREE(d->alloc, order, order_size);

    /* Outside the lock: lookups go on while the file is written. */
    de_result r = de_file_write_all_atomic(path, buf, size);
    DE_FREE(d->alloc, buf, size);
    de_mutex_lock(&d->lock);
    if (r == DE_OK) {
        d->stats.index_writes += 1u;
        d->stats.bytes_written += size;
    } else {
        d->index_dirty = true; /* try again later */
    }
    de_mutex_unlock(&d->lock);
    if (r != DE_OK)
        return DE_FAIL(r, "cook cache: cannot write '%s'", path);
    return DE_OK;
}
