/* vfs.c: virtual paths, the mount table and the path-based file API (SPEC §6, §11.1, DENG-247).
 *
 * The mount table is one process-wide fixed array (DE_FS_MAX_MOUNTS entries), kept sorted by
 * search order: priority descending, then newest first. It is created on first use (atomic
 * state plus a spin while the winner initialises the rwlock, as in core/name.c), so tests can
 * mount temporary directories without de_fs_init. Lookups hold the read lock only while they
 * copy or stat native paths; no user callback runs under it.
 *
 * Every path-based call first normalises the virtual path into a root name and a relative path
 * ("a/b.txt", "" for the root). Normalisation is purely textual: it never asks the OS, so `..`
 * cannot escape a mount even through a symlink-free spelling, and native paths are refused. */
#include <dengine/de_platform.h>

#include "platform/de_atomic.h"
#include "platform/vfs_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Sets the error context only when `report` is true (de_path_exists probes quietly). */
#define VFS_FAIL(report, r, ...) ((report) ? DE_FAIL((r), __VA_ARGS__) : (r))

enum {
    FS_STATE_UNINIT = 0,
    FS_STATE_INITIALIZING = 1,
    FS_STATE_READY = 2,
};

typedef struct fs_mount {
    char root[DE_FS_ROOT_MAX + 1];
    char dir[DE_FS_PATH_MAX]; /* native, no trailing separator (except a bare "/") */
    u32 flags;
    i32 priority;
    u64 seq; /* mount order; newer first among equal priorities */
} fs_mount;

static struct {
    de_atomic_u32 state;
    de_rwlock lock;
    bool standard_mounted; /* de_fs_init ran */
    const de_allocator *alloc;
    u64 next_seq;
    u32 count;
    fs_mount mounts[DE_FS_MAX_MOUNTS];
} g_fs;

static de_atomic_u32 g_tmp_counter;

static void fs_ensure(void) {
    if (de_atomic_load_u32(&g_fs.state, DE_ATOMIC_ACQUIRE) == FS_STATE_READY)
        return;
    u32 expected = FS_STATE_UNINIT;
    if (de_atomic_cas_u32(&g_fs.state, &expected, FS_STATE_INITIALIZING, DE_ATOMIC_ACQ_REL,
                          DE_ATOMIC_ACQUIRE)) {
        de_rwlock_init(&g_fs.lock);
        g_fs.alloc = de_heap_allocator();
        de_atomic_store_u32(&g_fs.state, FS_STATE_READY, DE_ATOMIC_RELEASE);
        return;
    }
    while (de_atomic_load_u32(&g_fs.state, DE_ATOMIC_ACQUIRE) != FS_STATE_READY)
        de_cpu_pause();
}

/* ---- Path helpers ------------------------------------------------------------------------- */

bool de__path_append(char *buf, size_t cap, const char *src) {
    size_t len = strlen(buf);
    size_t add = strlen(src);
    if (len + add + 1 > cap)
        return false;
    memcpy(buf + len, src, add + 1);
    return true;
}

static bool is_sep(char c) {
    return c == '/' || c == '\\';
}

/* `dir` + "/" + `rel` (rel may be empty). */
static bool join_native(char *out, size_t cap, const char *dir, const char *rel) {
    size_t dl = strlen(dir);
    if (dl + 1 > cap)
        return false;
    memcpy(out, dir, dl + 1);
    if (rel[0] == '\0')
        return true;
    if (dl == 0 || !is_sep(out[dl - 1])) {
        if (!de__path_append(out, cap, "/"))
            return false;
    }
    return de__path_append(out, cap, rel);
}

static bool valid_root_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static bool valid_root(const char *root, size_t len) {
    if (len == 0 || len > DE_FS_ROOT_MAX)
        return false;
    for (size_t i = 0; i < len; ++i) {
        if (!valid_root_char(root[i]))
            return false;
    }
    return true;
}

/* Length of the valid UTF-8 sequence at `s` (1..4), or 0 if it is invalid. */
static size_t utf8_seq_len(const u8 *s) {
    u8 c = s[0];
    if (c < 0x80)
        return 1;
    if (c >= 0xC2 && c <= 0xDF)
        return (s[1] & 0xC0) == 0x80 ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        u8 lo = c == 0xE0 ? 0xA0 : 0x80; /* no overlongs */
        u8 hi = c == 0xED ? 0x9F : 0xBF; /* no surrogates */
        if (s[1] < lo || s[1] > hi || (s[2] & 0xC0) != 0x80)
            return 0;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        u8 lo = c == 0xF0 ? 0x90 : 0x80;
        u8 hi = c == 0xF4 ? 0x8F : 0xBF; /* <= U+10FFFF */
        if (s[1] < lo || s[1] > hi || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80)
            return 0;
        return 4;
    }
    return 0;
}

/*
 * Splits `vpath` into its root and its normalised relative path. `rel` gets "" for the root
 * itself, else segments joined by '/'. Sets the error context on failure.
 */
static de_result parse_vpath_ex(const char *vpath, char root[DE_FS_ROOT_MAX + 1], char *rel,
                                size_t rel_cap, bool report) {
    if (vpath == NULL)
        return VFS_FAIL(report, DE_ERR_INVALID_ARGUMENT, "virtual path is NULL");
    const char *colon = strchr(vpath, ':');
    size_t root_len = colon != NULL ? (size_t)(colon - vpath) : 0;
    if (colon == NULL || colon[1] != '/' || !valid_root(vpath, root_len))
        return VFS_FAIL(
            report, DE_ERR_INVALID_ARGUMENT,
            "'%s' is not a virtual path (expected <root>:/<path>, root [a-z0-9_-]{1,%d})", vpath,
            DE_FS_ROOT_MAX);
    memcpy(root, vpath, root_len);
    root[root_len] = '\0';

    const u8 *p = (const u8 *)colon + 2;
    size_t n = 0; /* bytes in rel */
    rel[0] = '\0';
    while (*p != '\0') {
        while (*p == '/')
            ++p;
        if (*p == '\0')
            break;
        const u8 *seg = p;
        while (*p != '\0' && *p != '/') {
            if (*p < 0x20 || *p == 0x7F || *p == '\\' || *p == ':')
                return VFS_FAIL(report, DE_ERR_INVALID_ARGUMENT,
                                "'%s': a virtual path cannot contain '\\', ':' or control bytes",
                                vpath);
            size_t sl = utf8_seq_len(p);
            if (sl == 0)
                return VFS_FAIL(report, DE_ERR_INVALID_ARGUMENT, "'%s': invalid UTF-8", vpath);
            p += sl;
        }
        size_t seg_len = (size_t)(p - seg);
        if (seg_len == 1 && seg[0] == '.')
            continue;
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (n == 0)
                return VFS_FAIL(report, DE_ERR_INVALID_ARGUMENT, "'%s': '..' leaves the root %s:/",
                                vpath, root);
            while (n > 0 && rel[n - 1] != '/')
                --n;
            if (n > 0)
                --n; /* the separator before the removed segment */
            rel[n] = '\0';
            continue;
        }
        size_t need = n + (n > 0 ? 1 : 0) + seg_len + 1;
        if (need > rel_cap)
            return VFS_FAIL(report, DE_ERR_INVALID_ARGUMENT, "'%s': path longer than %d bytes",
                            vpath, DE_FS_PATH_MAX - 1);
        if (n > 0)
            rel[n++] = '/';
        memcpy(rel + n, seg, seg_len);
        n += seg_len;
        rel[n] = '\0';
    }
    return DE_OK;
}

static de_result parse_vpath(const char *vpath, char root[DE_FS_ROOT_MAX + 1], char *rel,
                             size_t rel_cap) {
    return parse_vpath_ex(vpath, root, rel, rel_cap, true);
}

de_result de_path_normalize(const char *vpath, char *buf, size_t cap) {
    char root[DE_FS_ROOT_MAX + 1];
    char rel[DE_FS_PATH_MAX];
    DE_TRY(parse_vpath(vpath, root, rel, sizeof rel));
    if (buf == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_path_normalize: buf is NULL");
    size_t rl = strlen(root), ll = strlen(rel);
    if (rl + 2 + ll + 1 > cap)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_path_normalize: buffer of %zu bytes too small",
                       cap);
    memcpy(buf, root, rl);
    memcpy(buf + rl, ":/", 2);
    memcpy(buf + rl + 2, rel, ll + 1);
    return DE_OK;
}

/* ---- Mount table ------------------------------------------------------------------------- */

/* Kinds of lookup. */
typedef enum lookup_kind {
    LOOKUP_EXISTING,          /* first mount where the path exists */
    LOOKUP_EXISTING_WRITABLE, /* first writable mount where the path exists */
    LOOKUP_WRITE_TARGET,      /* first writable mount */
    LOOKUP_RESOLVE,           /* existing, else write target, else first mount */
} lookup_kind;

/*
 * Finds the native path for root/rel. `*is_dir` (optional) is set when the path exists. Sets
 * the error context on failure: DE_ERR_NOT_FOUND (root not mounted, or the path does not exist),
 * DE_ERR_INVALID_ARGUMENT (no writable mount, path too long).
 */
static de_result lookup_ex(const char *vpath, const char *root, const char *rel, lookup_kind kind,
                           char *native, size_t cap, bool *is_dir, bool report) {
    fs_ensure();
    de_rwlock_read_lock(&g_fs.lock);
    bool any_mount = false, any_writable = false, too_long = false;
    de_result r = DE_ERR_NOT_FOUND;
    const fs_mount *first = NULL, *first_writable = NULL;
    for (u32 i = 0; i < g_fs.count; ++i) {
        const fs_mount *m = &g_fs.mounts[i];
        if (strcmp(m->root, root) != 0)
            continue;
        bool writable = (m->flags & DE_MOUNT_WRITABLE) != 0;
        any_mount = true;
        if (first == NULL)
            first = m;
        if (writable) {
            any_writable = true;
            if (first_writable == NULL)
                first_writable = m;
        }
        if (kind == LOOKUP_WRITE_TARGET)
            continue;
        if (kind == LOOKUP_EXISTING_WRITABLE && !writable)
            continue;
        if (!join_native(native, cap, m->dir, rel)) {
            too_long = true;
            continue;
        }
        bool d = false;
        if (de__native_stat(native, &d) == DE_OK) {
            if (is_dir != NULL)
                *is_dir = d;
            r = DE_OK;
            break;
        }
    }
    if (r != DE_OK && (kind == LOOKUP_WRITE_TARGET || kind == LOOKUP_RESOLVE)) {
        const fs_mount *m = first_writable != NULL   ? first_writable
                            : kind == LOOKUP_RESOLVE ? first
                                                     : NULL;
        if (m != NULL) {
            if (join_native(native, cap, m->dir, rel))
                r = DE_OK;
            else
                too_long = true;
        }
    }
    de_rwlock_read_unlock(&g_fs.lock);
    if (r == DE_OK)
        return DE_OK;
    if (!any_mount)
        return VFS_FAIL(report, DE_ERR_NOT_FOUND, "'%s': root %s:/ is not mounted", vpath, root);
    if (too_long)
        return VFS_FAIL(report, DE_ERR_INVALID_ARGUMENT, "'%s': native path longer than %d bytes",
                        vpath, DE_FS_PATH_MAX - 1);
    if ((kind == LOOKUP_WRITE_TARGET || kind == LOOKUP_EXISTING_WRITABLE) && !any_writable)
        return VFS_FAIL(report, DE_ERR_INVALID_ARGUMENT, "'%s': root %s:/ has no writable mount",
                        vpath, root);
    return VFS_FAIL(report, DE_ERR_NOT_FOUND, "'%s': not found", vpath);
}

static de_result lookup(const char *vpath, const char *root, const char *rel, lookup_kind kind,
                        char *native, size_t cap, bool *is_dir) {
    return lookup_ex(vpath, root, rel, kind, native, cap, is_dir, true);
}

de_result de_fs_mount(const char *root, const char *native_dir, u32 flags, i32 priority) {
    if (root == NULL || native_dir == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_fs_mount: root or directory is NULL");
    size_t rl = strlen(root);
    if (!valid_root(root, rl))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_fs_mount: bad root name '%s' (expected [a-z0-9_-]{1,%d})", root,
                       DE_FS_ROOT_MAX);
    /* DENG-393: there is no pack:/ root. Packs are read by asset ID through the asset system
     * (Trama "Decision: There is no pack:/ VFS root; packs are read only by asset ID"); the name
     * stays reserved so no game builds on it. */
    if (strcmp(root, "pack") == 0)
        return DE_FAIL(DE_ERR_UNSUPPORTED,
                       "de_fs_mount: pack:/ is reserved (packs mount in the asset "
                       "system: de_asset_pack_mount)");
    size_t dl = strlen(native_dir);
    /* Keep one separator for "/" and "C:/"; strip the others. */
    while (dl > 1 && is_sep(native_dir[dl - 1]) && !(dl == 3 && native_dir[1] == ':'))
        --dl;
    if (dl == 0 || dl + 2 > DE_FS_PATH_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_fs_mount: directory path empty or too long");
    fs_mount m;
    memset(&m, 0, sizeof m);
    memcpy(m.root, root, rl + 1);
    memcpy(m.dir, native_dir, dl);
    m.dir[dl] = '\0';
    m.flags = flags & DE_MOUNT_WRITABLE;
    m.priority = priority;
    bool is_dir = false;
    if (de__native_stat(m.dir, &is_dir) != DE_OK || !is_dir)
        return DE_FAIL(DE_ERR_NOT_FOUND, "de_fs_mount: '%s' is not a directory", m.dir);

    fs_ensure();
    de_rwlock_write_lock(&g_fs.lock);
    if (g_fs.count == DE_FS_MAX_MOUNTS) {
        de_rwlock_write_unlock(&g_fs.lock);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_fs_mount: all %d mounts are in use",
                       DE_FS_MAX_MOUNTS);
    }
    m.seq = ++g_fs.next_seq;
    u32 pos = 0; /* first entry that sorts after the new one */
    while (pos < g_fs.count && g_fs.mounts[pos].priority > priority)
        ++pos;
    /* Equal priority: the new mount goes before the older ones. */
    memmove(&g_fs.mounts[pos + 1], &g_fs.mounts[pos], (g_fs.count - pos) * sizeof(fs_mount));
    g_fs.mounts[pos] = m;
    g_fs.count++;
    de_rwlock_write_unlock(&g_fs.lock);
    return DE_OK;
}

de_result de_fs_unmount(const char *root, const char *native_dir) {
    if (root == NULL || native_dir == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_fs_unmount: root or directory is NULL");
    size_t dl = strlen(native_dir);
    while (dl > 1 && is_sep(native_dir[dl - 1]) && !(dl == 3 && native_dir[1] == ':'))
        --dl;
    fs_ensure();
    de_rwlock_write_lock(&g_fs.lock);
    for (u32 i = 0; i < g_fs.count; ++i) {
        fs_mount *m = &g_fs.mounts[i];
        if (strcmp(m->root, root) == 0 && strlen(m->dir) == dl &&
            memcmp(m->dir, native_dir, dl) == 0) {
            memmove(m, m + 1, (g_fs.count - i - 1) * sizeof(fs_mount));
            g_fs.count--;
            de_rwlock_write_unlock(&g_fs.lock);
            return DE_OK;
        }
    }
    de_rwlock_write_unlock(&g_fs.lock);
    return DE_FAIL(DE_ERR_NOT_FOUND, "de_fs_unmount: '%s' is not mounted at %s:/", native_dir,
                   root);
}

de_result de__native_mkdirs(const char *native) {
    char buf[DE_FS_PATH_MAX];
    size_t len = strlen(native);
    if (len == 0 || len + 1 > sizeof buf)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "path empty or too long");
    memcpy(buf, native, len + 1);
    /* Every prefix that ends before a separator, then the whole path. */
    for (size_t i = 1; i <= len; ++i) {
        if (i < len && !is_sep(buf[i]))
            continue;
        if (i < len && is_sep(buf[i - 1]))
            continue; /* "//" */
        char saved = buf[i];
        buf[i] = '\0';
        bool is_dir = false;
        de_result st = de__native_stat(buf, &is_dir);
        if (st != DE_OK || !is_dir) {
            de_result r = st == DE_OK ? DE_ERR_IO : de__native_mkdir(buf);
            if (r != DE_OK) {
                char shown[DE_FS_PATH_MAX];
                memcpy(shown, buf, i + 1);
                buf[i] = saved;
                return DE_FAIL(r, "cannot create directory '%s'", shown);
            }
        }
        buf[i] = saved;
    }
    return DE_OK;
}

de_result de_fs_init(const de_fs_desc *desc) {
    /* Explicit initialiser: MSVC warns C4132 on an uninitialised const object (DENG-360). */
    static const de_fs_desc zero = {0};
    if (desc == NULL)
        desc = &zero;
    fs_ensure();
    de_rwlock_read_lock(&g_fs.lock);
    bool already = g_fs.standard_mounted;
    de_rwlock_read_unlock(&g_fs.lock);
    if (already)
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "de_fs_init: already initialised");

    const char *app = desc->app != NULL && desc->app[0] != '\0' ? desc->app : "dengine";
    const char *org = desc->org != NULL && desc->org[0] != '\0' ? desc->org : NULL;
    char game[DE_FS_PATH_MAX], user[DE_FS_PATH_MAX], cache[DE_FS_PATH_MAX];
    if (desc->game_dir != NULL) {
        if (strlen(desc->game_dir) + 1 > sizeof game)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_fs_init: game_dir too long");
        memcpy(game, desc->game_dir, strlen(desc->game_dir) + 1);
    } else {
        DE_TRY(de__known_exe_dir(game, sizeof game));
    }
    if (desc->user_dir != NULL) {
        if (strlen(desc->user_dir) + 1 > sizeof user)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_fs_init: user_dir too long");
        memcpy(user, desc->user_dir, strlen(desc->user_dir) + 1);
    } else {
        DE_TRY(de__known_user_dir(org, app, user, sizeof user));
    }
    if (desc->cache_dir != NULL) {
        if (strlen(desc->cache_dir) + 1 > sizeof cache)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_fs_init: cache_dir too long");
        memcpy(cache, desc->cache_dir, strlen(desc->cache_dir) + 1);
    } else {
        DE_TRY(de__known_cache_dir(org, app, cache, sizeof cache));
    }
    DE_TRY(de__native_mkdirs(user));
    DE_TRY(de__native_mkdirs(cache));

    const char *dev = NULL;
#if (defined(DE_DEBUG) || defined(DE_DEV)) && !defined(DE_SHIPPING)
    dev = desc->dev_dir;
#endif
    struct {
        const char *root, *dir;
        u32 flags;
    } std_mounts[4] = {
        {"game", game, DE_MOUNT_READ_ONLY},
        {"user", user, DE_MOUNT_WRITABLE},
        {"cache", cache, DE_MOUNT_WRITABLE},
        {"dev", dev, DE_MOUNT_WRITABLE},
    };
    for (int i = 0; i < 4; ++i) {
        if (std_mounts[i].dir == NULL)
            continue;
        de_result r = de_fs_mount(std_mounts[i].root, std_mounts[i].dir, std_mounts[i].flags, 0);
        if (r != DE_OK) {
            for (int k = 0; k < i; ++k) {
                if (std_mounts[k].dir != NULL)
                    (void)de_fs_unmount(std_mounts[k].root, std_mounts[k].dir);
            }
            return DE_FAIL_WRAP(r, "de_fs_init: cannot mount %s:/", std_mounts[i].root);
        }
    }
    de_rwlock_write_lock(&g_fs.lock);
    g_fs.standard_mounted = true;
    g_fs.alloc = desc->allocator != NULL ? desc->allocator : de_heap_allocator();
    de_rwlock_write_unlock(&g_fs.lock);
    return DE_OK;
}

void de_fs_shutdown(void) {
    fs_ensure();
    de_rwlock_write_lock(&g_fs.lock);
    g_fs.count = 0;
    g_fs.standard_mounted = false;
    g_fs.alloc = de_heap_allocator();
    de_rwlock_write_unlock(&g_fs.lock);
}

de_result de_fs_resolve(const char *vpath, char *buf, size_t cap) {
    char root[DE_FS_ROOT_MAX + 1];
    char rel[DE_FS_PATH_MAX];
    char native[DE_FS_PATH_MAX];
    DE_TRY(parse_vpath(vpath, root, rel, sizeof rel));
    if (buf == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_fs_resolve: buf is NULL");
    DE_TRY(lookup(vpath, root, rel, LOOKUP_RESOLVE, native, sizeof native, NULL));
    size_t len = strlen(native);
    if (len + 1 > cap)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_fs_resolve: buffer of %zu bytes too small",
                       cap);
    memcpy(buf, native, len + 1);
    return DE_OK;
}

/* ---- Path-based file API ----------------------------------------------------------------- */

static const de_allocator *fs_allocator(void) {
    fs_ensure();
    de_rwlock_read_lock(&g_fs.lock);
    const de_allocator *a = g_fs.alloc;
    de_rwlock_read_unlock(&g_fs.lock);
    return a;
}

de_result de_file_open(const char *vpath, de_file_mode mode, de_file **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_file_open: out is NULL");
    *out = NULL;
    if (mode != DE_FILE_READ && mode != DE_FILE_WRITE && mode != DE_FILE_APPEND)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_file_open: bad mode %d", (int)mode);
    char root[DE_FS_ROOT_MAX + 1];
    char rel[DE_FS_PATH_MAX];
    char native[DE_FS_PATH_MAX];
    DE_TRY(parse_vpath(vpath, root, rel, sizeof rel));
    bool is_dir = false;
    if (mode == DE_FILE_READ) {
        DE_TRY(lookup(vpath, root, rel, LOOKUP_EXISTING, native, sizeof native, &is_dir));
    } else {
        DE_TRY(lookup(vpath, root, rel, LOOKUP_WRITE_TARGET, native, sizeof native, NULL));
        if (de__native_stat(native, &is_dir) != DE_OK)
            is_dir = false;
    }
    if (is_dir || rel[0] == '\0')
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_file_open: '%s' is a directory", vpath);
    de_result r = de__file_open_native(native, mode, fs_allocator(), out);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "de_file_open: '%s'", vpath);
    return DE_OK;
}

de_result de_file_read(de_file *f, void *dst, u64 offset, u64 bytes, u64 *read) {
    if (read != NULL)
        *read = 0;
    if (f == NULL || (dst == NULL && bytes > 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_file_read: file or dst is NULL");
    if (!de__file_readable(f))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_file_read: file not opened for reading");
    u64 done = 0;
    while (done < bytes) {
        u64 want = bytes - done;
        if (want > ((u64)1 << 30))
            want = (u64)1 << 30; /* one OS call reads at most 1 GiB (Win32 DWORD) */
        u64 got = 0;
        de_result r = de__file_pread(f, (u8 *)dst + done, offset + done, want, &got);
        if (r != DE_OK) {
            if (read != NULL)
                *read = done;
            return r;
        }
        if (got == 0)
            break;
        done += got;
    }
    if (read != NULL)
        *read = done;
    return DE_OK;
}

de_result de_file_read_all(const char *vpath, const de_allocator *a, void **data, u64 *size) {
    if (data == NULL || size == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_file_read_all: data or size is NULL");
    *data = NULL;
    *size = 0;
    if (a == NULL)
        a = de_heap_allocator();
    de_file *f = NULL;
    DE_TRY(de_file_open(vpath, DE_FILE_READ, &f));
    u64 n = de_file_size(f);
    if (n >= (u64)SIZE_MAX) {
        de_file_close(f);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "'%s': %llu bytes do not fit in memory", vpath,
                       (unsigned long long)n);
    }
    u8 *buf = (u8 *)DE_ALLOC(a, (size_t)n + 1, 16, DE_MEM_GENERAL);
    if (buf == NULL) {
        de_file_close(f);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "'%s': cannot allocate %llu bytes", vpath,
                       (unsigned long long)n + 1);
    }
    u64 got = 0;
    de_result r = de_file_read(f, buf, 0, n, &got);
    de_file_close(f);
    if (r == DE_OK && got != n)
        r = DE_FAIL(DE_ERR_IO, "'%s': file changed size while being read", vpath);
    if (r != DE_OK) {
        DE_FREE(a, buf, (size_t)n + 1);
        return DE_FAIL_WRAP(r, "de_file_read_all: '%s'", vpath);
    }
    buf[n] = 0;
    *data = buf;
    *size = n;
    return DE_OK;
}

de_result de_file_write_all_atomic(const char *vpath, const void *data, u64 bytes) {
    if (data == NULL && bytes > 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_file_write_all_atomic: data is NULL");
    char root[DE_FS_ROOT_MAX + 1];
    char rel[DE_FS_PATH_MAX];
    char native[DE_FS_PATH_MAX];
    char tmp[DE_FS_PATH_MAX];
    DE_TRY(parse_vpath(vpath, root, rel, sizeof rel));
    if (rel[0] == '\0')
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_file_write_all_atomic: '%s' is a root", vpath);
    DE_TRY(lookup(vpath, root, rel, LOOKUP_WRITE_TARGET, native, sizeof native, NULL));
    bool is_dir = false;
    if (de__native_stat(native, &is_dir) == DE_OK && is_dir)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_file_write_all_atomic: '%s' is a directory",
                       vpath);
    /* Same directory as the target, so the rename never crosses file systems. */
    u32 n = de_atomic_fetch_add_u32(&g_tmp_counter, 1, DE_ATOMIC_RELAXED);
    char suffix[48];
    snprintf(suffix, sizeof suffix, ".tmp-%u-%u", (unsigned)de__native_process_id(), (unsigned)n);
    memcpy(tmp, native, strlen(native) + 1);
    if (!de__path_append(tmp, sizeof tmp, suffix))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "'%s': native path too long", vpath);
    de_result r = de__native_write_new_synced(tmp, data, bytes);
    if (r != DE_OK) {
        (void)de__native_remove(tmp);
        return DE_FAIL(r, "'%s': cannot write the temporary file %s", vpath, tmp);
    }
    r = de__native_rename_replace(tmp, native);
    if (r != DE_OK) {
        (void)de__native_remove(tmp);
        return DE_FAIL(r, "'%s': cannot rename %s over it", vpath, tmp);
    }
    return DE_OK;
}

bool de_path_exists(const char *vpath) {
    /* Quiet: a probe that answers false leaves no error behind. */
    char root[DE_FS_ROOT_MAX + 1];
    char rel[DE_FS_PATH_MAX];
    char native[DE_FS_PATH_MAX];
    if (parse_vpath_ex(vpath, root, rel, sizeof rel, false) != DE_OK)
        return false;
    return lookup_ex(vpath, root, rel, LOOKUP_EXISTING, native, sizeof native, NULL, false) ==
           DE_OK;
}

de_result de_file_delete(const char *vpath) {
    char root[DE_FS_ROOT_MAX + 1];
    char rel[DE_FS_PATH_MAX];
    char native[DE_FS_PATH_MAX];
    DE_TRY(parse_vpath(vpath, root, rel, sizeof rel));
    if (rel[0] == '\0')
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_file_delete: '%s' is a root", vpath);
    DE_TRY(lookup(vpath, root, rel, LOOKUP_EXISTING_WRITABLE, native, sizeof native, NULL));
    de_result r = de__native_remove(native);
    if (r != DE_OK)
        return DE_FAIL(r, "de_file_delete: cannot delete '%s' (%s)", vpath, native);
    return DE_OK;
}

de_result de_dir_create(const char *vpath) {
    char root[DE_FS_ROOT_MAX + 1];
    char rel[DE_FS_PATH_MAX];
    char native[DE_FS_PATH_MAX];
    DE_TRY(parse_vpath(vpath, root, rel, sizeof rel));
    /* The mount directory exists, so only the segments of `rel` are created. */
    DE_TRY(lookup(vpath, root, "", LOOKUP_WRITE_TARGET, native, sizeof native, NULL));
    for (size_t i = 0;; ++i) {
        if (rel[i] != '/' && rel[i] != '\0')
            continue;
        if (i == 0)
            break; /* the root itself */
        char seg[DE_FS_PATH_MAX];
        char path[DE_FS_PATH_MAX];
        memcpy(seg, rel, i);
        seg[i] = '\0';
        if (!join_native(path, sizeof path, native, seg))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "'%s': native path too long", vpath);
        bool is_dir = false;
        de_result st = de__native_stat(path, &is_dir);
        if (st == DE_OK && !is_dir)
            return DE_FAIL(DE_ERR_IO, "de_dir_create: '%s' is a file", path);
        if (st != DE_OK) {
            de_result r = de__native_mkdir(path);
            if (r != DE_OK)
                return DE_FAIL(r, "de_dir_create: cannot create '%s'", path);
        }
        if (rel[i] == '\0')
            break;
    }
    return DE_OK;
}

/* ---- Directory listing ------------------------------------------------------------------- */

typedef struct list_ctx {
    char (*dirs)[DE_FS_PATH_MAX]; /* native directory per mount, search order */
    u32 index;                    /* mount being listed */
    de_dir_fn fn;
    void *user;
    bool stopped;
} list_ctx;

static bool list_entry(const char *name, bool is_dir, void *user) {
    list_ctx *c = (list_ctx *)user;
    /* A name that a higher mount also has was (or will be) reported for that mount. */
    for (u32 j = 0; j < c->index; ++j) {
        char other[DE_FS_PATH_MAX];
        bool d = false;
        if (c->dirs[j][0] != '\0' && join_native(other, sizeof other, c->dirs[j], name) &&
            de__native_stat(other, &d) == DE_OK)
            return true;
    }
    if (!c->fn(name, is_dir, c->user)) {
        c->stopped = true;
        return false;
    }
    return true;
}

de_result de_dir_list(const char *vpath, de_dir_fn fn, void *user) {
    char root[DE_FS_ROOT_MAX + 1];
    char rel[DE_FS_PATH_MAX];
    DE_TRY(parse_vpath(vpath, root, rel, sizeof rel));
    if (fn == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_dir_list: callback is NULL");

    /* Snapshot the mount directories, so `fn` runs without the lock (it may open files). */
    const de_allocator *a = de_heap_allocator();
    fs_ensure();
    de_rwlock_read_lock(&g_fs.lock);
    u32 n = 0;
    for (u32 i = 0; i < g_fs.count; ++i)
        n += strcmp(g_fs.mounts[i].root, root) == 0 ? 1u : 0u;
    char (*dirs)[DE_FS_PATH_MAX] = NULL;
    if (n > 0)
        dirs = (char (*)[DE_FS_PATH_MAX])DE_ALLOC(a, (size_t)n * DE_FS_PATH_MAX, 8, DE_MEM_GENERAL);
    bool too_long = false;
    if (dirs != NULL) {
        u32 k = 0;
        for (u32 i = 0; i < g_fs.count; ++i) {
            if (strcmp(g_fs.mounts[i].root, root) != 0)
                continue;
            if (!join_native(dirs[k], DE_FS_PATH_MAX, g_fs.mounts[i].dir, rel)) {
                dirs[k][0] = '\0';
                too_long = true;
            }
            ++k;
        }
    }
    de_rwlock_read_unlock(&g_fs.lock);
    if (n == 0)
        return DE_FAIL(DE_ERR_NOT_FOUND, "'%s': root %s:/ is not mounted", vpath, root);
    if (dirs == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_dir_list: cannot allocate the mount snapshot");

    list_ctx c = {dirs, 0, fn, user, false};
    bool found = false;
    de_result r = DE_OK;
    for (u32 i = 0; i < n && !c.stopped; ++i) {
        bool is_dir = false;
        if (dirs[i][0] == '\0' || de__native_stat(dirs[i], &is_dir) != DE_OK || !is_dir) {
            dirs[i][0] = '\0'; /* nothing to shadow from this mount */
            continue;
        }
        found = true;
        c.index = i;
        r = de__native_dir_list(dirs[i], list_entry, &c);
        if (r != DE_OK) {
            r = DE_FAIL(r, "de_dir_list: cannot list '%s'", dirs[i]);
            break;
        }
    }
    DE_FREE(a, dirs, (size_t)n * DE_FS_PATH_MAX);
    if (r != DE_OK)
        return r;
    if (!found)
        return DE_FAIL(DE_ERR_NOT_FOUND,
                       too_long ? "'%s': native path too long" : "'%s': no such directory", vpath);
    return DE_OK;
}
