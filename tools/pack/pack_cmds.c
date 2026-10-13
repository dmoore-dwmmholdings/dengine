/*
 * pack_cmds.c: `de-pack list`, `extract` and `verify` (DENG-213). See pack_cmds.h.
 *
 * Files: the pack's directory is mounted read-only as `depak:/` and an `extract` output
 * directory as `depakout:/` (writable); `depakmk:/` is mounted for a moment to create a missing
 * output directory. Every mount is removed before de_pack_main returns. The pack is read with
 * the runtime reader (dpak_open), for any platform.
 */
#include "pack_cmds.h"

#include <dengine/de_asset.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset/dpak/dpak_format.h"
#include "asset/dpak/dpak_reader.h"
#include "cook/lib/de_cooker.h" /* DE_COOKER_ENGINE_TYPE_NAMES only (no cooker code) */

#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>
#include <zstd.h>

#define PK_ROOT        "depak"
#define PK_ROOT_PREFIX "depak:/"
#define PK_OUT_ROOT    "depakout"
#define PK_OUT_PREFIX  "depakout:/"
#define PK_MK_ROOT     "depakmk"
#define PK_MK_PREFIX   "depakmk:/"
#define PK_HASH_BLOCK  (1u << 20)

/* ---- Output ------------------------------------------------------------------------------- */

static void pk_write(const de_pack_io *io, de_pack_stream s, const char *text, size_t len) {
    if (len > 0)
        io->write(io->user, s, text, len);
}

static void pk_puts(const de_pack_io *io, de_pack_stream s, const char *text) {
    pk_write(io, s, text, strlen(text));
}

static void pk_printf(const de_pack_io *io, de_pack_stream s, const char *fmt, ...)
    DE_PRINTF_FMT(3, 4);
static void pk_printf(const de_pack_io *io, de_pack_stream s, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    pk_write(io, s, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

/* Text from the pack (untrusted): control characters become spaces, so a line stays a line. */
static void pk_put_safe(const de_pack_io *io, de_pack_stream s, const char *text) {
    const char *run = text;
    for (const char *p = text; *p != '\0'; ++p) {
        if ((unsigned char)*p < 0x20 || *p == 0x7f) {
            pk_write(io, s, run, (size_t)(p - run));
            pk_write(io, s, " ", 1);
            run = p + 1;
        }
    }
    pk_puts(io, s, run);
}

/* A JSON string: quotes, backslashes and control characters escaped. */
static void pk_put_json_str(const de_pack_io *io, const char *text) {
    pk_puts(io, DE_PACK_STDOUT, "\"");
    const char *run = text;
    for (const char *p = text; *p != '\0'; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\' || c < 0x20 || c == 0x7f) {
            pk_write(io, DE_PACK_STDOUT, run, (size_t)(p - run));
            if (c == '"' || c == '\\') {
                char esc[2] = {'\\', (char)c};
                pk_write(io, DE_PACK_STDOUT, esc, 2);
            } else {
                pk_printf(io, DE_PACK_STDOUT, "\\u%04x", (unsigned)c);
            }
            run = p + 1;
        }
    }
    pk_puts(io, DE_PACK_STDOUT, run);
    pk_puts(io, DE_PACK_STDOUT, "\"");
}

void de_pack_usage(const de_pack_io *io, de_pack_stream s) {
    pk_puts(
        io, s,
        "usage: de-pack <command> <pak> [arguments]\n"
        "\n"
        "commands:\n"
        "  list <pak> [--json]            list entries (ID, type, raw and stored size, chunks,\n"
        "                                 dependencies, name) and bundles\n"
        "  extract <pak> <sel> [-o <dir>] write the raw cooked bytes of the entries that\n"
        "                                 match <sel> into <dir> (default: .)\n"
        "  verify <pak>                   check the header, the content hash, the TOC, every\n"
        "                                 chunk and the dependencies\n"
        "\n"
        "<sel>: 0x<hex> (an asset ID), a glob on asset paths (* ** ?; needs names), or an\n"
        "asset path (works on stripped packs). Files are named by asset path, or by ID.\n"
        "\n"
        "exit codes: 0 ok, 1 problems found or nothing matched, 2 usage or I/O error\n");
}

static int pk_usage_error(const de_pack_io *io, const char *what, const char *arg) {
    pk_puts(io, DE_PACK_STDERR, "de-pack: ");
    pk_puts(io, DE_PACK_STDERR, what);
    if (arg != NULL) {
        pk_puts(io, DE_PACK_STDERR, " '");
        pk_puts(io, DE_PACK_STDERR, arg);
        pk_puts(io, DE_PACK_STDERR, "'");
    }
    pk_puts(io, DE_PACK_STDERR, "\n\n");
    de_pack_usage(io, DE_PACK_STDERR);
    return DE_PACK_EXIT_USAGE;
}

static int pk_error(const de_pack_io *io, int rc, const char *what, const char *arg) {
    pk_puts(io, DE_PACK_STDERR, "de-pack: ");
    pk_puts(io, DE_PACK_STDERR, what);
    if (arg != NULL) {
        pk_puts(io, DE_PACK_STDERR, " '");
        pk_puts(io, DE_PACK_STDERR, arg);
        pk_puts(io, DE_PACK_STDERR, "'");
    }
    const de_error *e = de_error_last();
    if (e != NULL && e->message[0] != '\0') {
        pk_puts(io, DE_PACK_STDERR, ": ");
        pk_put_safe(io, DE_PACK_STDERR, e->message);
    }
    pk_puts(io, DE_PACK_STDERR, "\n");
    return rc;
}

/* ---- Opening a pack ----------------------------------------------------------------------- */

typedef struct pk_run {
    const de_pack_io *io;
    const char *pak; /* the argument */
    char dir[DE_FS_PATH_MAX];
    char vpath[DE_FS_PATH_MAX];
    bool mounted;
    const char *out_dir;
    bool out_mounted;
    dpak_reader *r;
} pk_run;

static bool pk_is_sep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

/* Mounts the pack's directory and opens it (any platform, no content hash: verify hashes it
 * itself, so a damaged pack can still be inspected entry by entry). */
static int pk_open(pk_run *run) {
    const de_pack_io *io = run->io;
    const char *arg = run->pak;
    size_t len = strlen(arg), sep = len;
    for (size_t i = 0; i < len; ++i)
        if (pk_is_sep(arg[i]))
            sep = i;
    const char *name = sep == len ? arg : arg + sep + 1;
    if (name[0] == '\0' || len + 1 > sizeof run->dir)
        return pk_usage_error(io, "not a pack file name:", arg);
    if (sep == len) {
        memcpy(run->dir, ".", 2);
    } else {
        size_t dl = (sep == 0 || (sep == 2 && arg[1] == ':')) ? sep + 1 : sep;
        memcpy(run->dir, arg, dl);
        run->dir[dl] = '\0';
    }
    char raw[DE_FS_PATH_MAX];
    int n = snprintf(raw, sizeof raw, "%s%s", PK_ROOT_PREFIX, name);
    if (n < 0 || (size_t)n >= sizeof raw ||
        de_path_normalize(raw, run->vpath, sizeof run->vpath) != DE_OK)
        return pk_usage_error(io, "cannot use this pack file name:", arg);
    de_error_clear();
    if (de_fs_mount(PK_ROOT, run->dir, DE_MOUNT_READ_ONLY, 0) != DE_OK)
        return pk_error(io, DE_PACK_EXIT_USAGE, "cannot open the directory of", arg);
    run->mounted = true;
    if (!de_path_exists(run->vpath))
        return pk_error(io, DE_PACK_EXIT_USAGE, "no such file", arg);
    de_error_clear();
    dpak_open_desc od = {.allocator = NULL, .platform = DPAK_PLATFORM_ANY, .verify = false};
    de_result r = dpak_open(run->vpath, &od, &run->r);
    if (r == DE_ERR_IO || r == DE_ERR_NOT_FOUND || r == DE_ERR_OUT_OF_MEMORY)
        return pk_error(io, DE_PACK_EXIT_USAGE, "cannot read", arg);
    if (r != DE_OK)
        return pk_error(io, DE_PACK_EXIT_PROBLEMS, "not a valid pack:", arg);
    return -1;
}

static void pk_close(pk_run *run) {
    dpak_close(run->r);
    run->r = NULL;
    if (run->out_mounted)
        (void)de_fs_unmount(PK_OUT_ROOT, run->out_dir);
    if (run->mounted)
        (void)de_fs_unmount(PK_ROOT, run->dir);
}

/* ---- Names -------------------------------------------------------------------------------- */

static const char *const k_engine_types[] = {DE_COOKER_ENGINE_TYPE_NAMES};

/* The name of a type hash when it is an engine type, else NULL (packs store hashes only). */
static const char *pk_type_name(u64 hash) {
    for (size_t i = 0; i < sizeof k_engine_types / sizeof k_engine_types[0]; ++i)
        if (de_hash64_str(k_engine_types[i]) == hash)
            return k_engine_types[i];
    return NULL;
}

static const char *pk_platform_name(u32 p) {
    switch (p) {
    case DPAK_PLATFORM_ANY:
        return "any";
    case DPAK_PLATFORM_WINDOWS:
        return "windows";
    case DPAK_PLATFORM_LINUX:
        return "linux";
    case DPAK_PLATFORM_MACOS:
        return "macos";
    default:
        return "unknown";
    }
}

static const char *pk_priority_name(u32 p) {
    static const char *const names[] = {"critical", "high", "normal", "low", "background"};
    return p < sizeof names / sizeof names[0] ? names[p] : "unknown";
}

static void pk_hex16(char out[17], u64 v) {
    snprintf(out, 17, "%016llx", (unsigned long long)v);
}

/* The bundle name for a depend hash: a bundle of this pack, or "0x<hex>". */
static void pk_put_bundle_ref(const pk_run *run, u64 hash, bool json) {
    u32 b = dpak_bundle_find(run->r, hash);
    if (b != DPAK_NOT_FOUND) {
        dpak_bundle_entry be;
        dpak_bundle(run->r, b, &be);
        if (json)
            pk_put_json_str(run->io, dpak_bundle_name(run->r, &be));
        else
            pk_put_safe(run->io, DE_PACK_STDOUT, dpak_bundle_name(run->r, &be));
        return;
    }
    char hex[17];
    pk_hex16(hex, hash);
    pk_printf(run->io, DE_PACK_STDOUT, json ? "\"0x%s\"" : "0x%s", hex);
}

/* ---- list --------------------------------------------------------------------------------- */

static int pk_list_text(pk_run *run) {
    const de_pack_io *io = run->io;
    const dpak_header *h = dpak_get_header(run->r);
    u32 n = dpak_entry_count(run->r), nb = dpak_bundle_count(run->r);
    pk_puts(io, DE_PACK_STDOUT, "pack ");
    pk_put_safe(io, DE_PACK_STDOUT, run->pak);
    pk_printf(io, DE_PACK_STDOUT,
              ": version %u, platform %s, %u entries, %u bundles, %llu bytes%s\n",
              (unsigned)h->version, pk_platform_name(h->platform), (unsigned)n, (unsigned)nb,
              (unsigned long long)h->file_size,
              (h->flags & DPAK_HEADER_HAS_NAMES) != 0 ? "" : ", no names");
    pk_puts(io, DE_PACK_STDOUT, "build id ");
    for (u32 i = 0; i < 16; ++i)
        pk_printf(io, DE_PACK_STDOUT, "%02x", (unsigned)h->build_id[i]);
    pk_puts(io, DE_PACK_STDOUT, "\n\n");
    pk_printf(io, DE_PACK_STDOUT, "%-16s  %-18s %10s %10s %7s %5s  %s\n", "ID", "TYPE", "RAW",
              "STORED", "CHUNKS", "DEPS", "NAME");
    for (u32 i = 0; i < n; ++i) {
        dpak_toc_entry e;
        dpak_entry(run->r, i, &e);
        char id[17], type[24];
        pk_hex16(id, e.id);
        const char *tn = pk_type_name(e.type_hash);
        if (tn != NULL)
            snprintf(type, sizeof type, "%s", tn);
        else
            snprintf(type, sizeof type, "0x%016llx", (unsigned long long)e.type_hash);
        pk_printf(io, DE_PACK_STDOUT, "%s  %-18s %10llu %10llu %7u %5u  ", id, type,
                  (unsigned long long)e.raw_size, (unsigned long long)e.stored_size,
                  (unsigned)e.chunk_count, (unsigned)e.dep_count);
        const char *name = dpak_name(run->r, i, NULL);
        pk_put_safe(io, DE_PACK_STDOUT, name != NULL ? name : "-");
        pk_puts(io, DE_PACK_STDOUT, "\n");
        for (u32 d = 0; d < e.dep_count; ++d) {
            u64 dep = dpak_dep(run->r, e.dep_first + d);
            u32 di = dpak_find(run->r, dep);
            const char *dn = di != DPAK_NOT_FOUND ? dpak_name(run->r, di, NULL) : NULL;
            char dh[17];
            pk_hex16(dh, dep);
            pk_printf(io, DE_PACK_STDOUT, "    -> %s  ", dh);
            if (dn != NULL)
                pk_put_safe(io, DE_PACK_STDOUT, dn);
            else
                pk_puts(io, DE_PACK_STDOUT, di != DPAK_NOT_FOUND ? "-" : "(not in this pack)");
            pk_puts(io, DE_PACK_STDOUT, "\n");
        }
    }
    if (nb != 0)
        pk_puts(io, DE_PACK_STDOUT, "\n");
    for (u32 b = 0; b < nb; ++b) {
        dpak_bundle_entry be;
        dpak_bundle(run->r, b, &be);
        pk_puts(io, DE_PACK_STDOUT, "bundle ");
        pk_put_safe(io, DE_PACK_STDOUT, dpak_bundle_name(run->r, &be));
        pk_printf(io, DE_PACK_STDOUT, ": %u members, priority %s%s", (unsigned)be.member_count,
                  pk_priority_name(be.priority),
                  (be.flags & DPAK_BUNDLE_RESIDENT) != 0 ? ", resident" : "");
        for (u32 d = 0; d < be.depend_count; ++d) {
            pk_puts(io, DE_PACK_STDOUT, d == 0 ? ", depends: " : ", ");
            pk_put_bundle_ref(run, dpak_bundle_id(run->r, be.depend_first + d), false);
        }
        pk_puts(io, DE_PACK_STDOUT, "\n");
    }
    return DE_PACK_EXIT_OK;
}

static void pk_json_id(const de_pack_io *io, u64 id) {
    char hex[17];
    pk_hex16(hex, id);
    pk_printf(io, DE_PACK_STDOUT, "\"0x%s\"", hex);
}

static int pk_list_json(pk_run *run) {
    const de_pack_io *io = run->io;
    const dpak_header *h = dpak_get_header(run->r);
    u32 n = dpak_entry_count(run->r), nb = dpak_bundle_count(run->r);
    pk_printf(io, DE_PACK_STDOUT,
              "{\n  \"pack\": {\"version\": %u, \"platform\": \"%s\", \"build_id\": \"",
              (unsigned)h->version, pk_platform_name(h->platform));
    for (u32 i = 0; i < 16; ++i)
        pk_printf(io, DE_PACK_STDOUT, "%02x", (unsigned)h->build_id[i]);
    pk_puts(io, DE_PACK_STDOUT, "\", \"content_hash\": ");
    pk_json_id(io, h->content_hash);
    pk_printf(io, DE_PACK_STDOUT,
              ", \"file_size\": %llu, \"chunk_size\": %u, \"alignment\": %u, \"has_names\": %s, "
              "\"entry_count\": %u, \"bundle_count\": %u},\n",
              (unsigned long long)h->file_size, (unsigned)h->chunk_size, (unsigned)h->alignment,
              (h->flags & DPAK_HEADER_HAS_NAMES) != 0 ? "true" : "false", (unsigned)n,
              (unsigned)nb);
    pk_puts(io, DE_PACK_STDOUT, "  \"assets\": [");
    for (u32 i = 0; i < n; ++i) {
        dpak_toc_entry e;
        dpak_entry(run->r, i, &e);
        pk_puts(io, DE_PACK_STDOUT, i == 0 ? "\n    {\"id\": " : ",\n    {\"id\": ");
        pk_json_id(io, e.id);
        pk_puts(io, DE_PACK_STDOUT, ", \"name\": ");
        const char *name = dpak_name(run->r, i, NULL);
        if (name != NULL)
            pk_put_json_str(io, name);
        else
            pk_puts(io, DE_PACK_STDOUT, "null");
        pk_puts(io, DE_PACK_STDOUT, ", \"type\": ");
        const char *tn = pk_type_name(e.type_hash);
        if (tn != NULL)
            pk_put_json_str(io, tn);
        else
            pk_puts(io, DE_PACK_STDOUT, "null");
        pk_puts(io, DE_PACK_STDOUT, ", \"type_hash\": ");
        pk_json_id(io, e.type_hash);
        pk_printf(io, DE_PACK_STDOUT,
                  ", \"raw_size\": %llu, \"stored_size\": %llu, \"chunks\": %u, "
                  "\"sub_asset\": %s, \"uncompressed\": %s, \"deps\": [",
                  (unsigned long long)e.raw_size, (unsigned long long)e.stored_size,
                  (unsigned)e.chunk_count, (e.flags & DPAK_ENTRY_SUB_ASSET) != 0 ? "true" : "false",
                  (e.flags & DPAK_ENTRY_UNCOMPRESSED) != 0 ? "true" : "false");
        for (u32 d = 0; d < e.dep_count; ++d) {
            if (d > 0)
                pk_puts(io, DE_PACK_STDOUT, ", ");
            pk_json_id(io, dpak_dep(run->r, e.dep_first + d));
        }
        pk_puts(io, DE_PACK_STDOUT, "]}");
    }
    pk_puts(io, DE_PACK_STDOUT, n != 0 ? "\n  ],\n  \"bundles\": [" : "],\n  \"bundles\": [");
    for (u32 b = 0; b < nb; ++b) {
        dpak_bundle_entry be;
        dpak_bundle(run->r, b, &be);
        pk_puts(io, DE_PACK_STDOUT, b == 0 ? "\n    {\"name\": " : ",\n    {\"name\": ");
        pk_put_json_str(io, dpak_bundle_name(run->r, &be));
        pk_printf(io, DE_PACK_STDOUT, ", \"priority\": \"%s\", \"resident\": %s, \"members\": [",
                  pk_priority_name(be.priority),
                  (be.flags & DPAK_BUNDLE_RESIDENT) != 0 ? "true" : "false");
        for (u32 m = 0; m < be.member_count; ++m) {
            if (m > 0)
                pk_puts(io, DE_PACK_STDOUT, ", ");
            pk_json_id(io, dpak_bundle_id(run->r, be.member_first + m));
        }
        pk_puts(io, DE_PACK_STDOUT, "], \"depends\": [");
        for (u32 d = 0; d < be.depend_count; ++d) {
            if (d > 0)
                pk_puts(io, DE_PACK_STDOUT, ", ");
            pk_put_bundle_ref(run, dpak_bundle_id(run->r, be.depend_first + d), true);
        }
        pk_puts(io, DE_PACK_STDOUT, "]}");
    }
    pk_puts(io, DE_PACK_STDOUT, nb != 0 ? "\n  ]\n}\n" : "]\n}\n");
    return DE_PACK_EXIT_OK;
}

static int pk_list(pk_run *run, int argc, const char *const *argv) {
    bool json = false;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0)
            json = true;
        else if (argv[i][0] == '-')
            return pk_usage_error(run->io, "unknown option", argv[i]);
        else if (run->pak == NULL)
            run->pak = argv[i];
        else
            return pk_usage_error(run->io, "more than one pack given:", argv[i]);
    }
    if (run->pak == NULL)
        return pk_usage_error(run->io, "no pack given", NULL);
    int rc = pk_open(run);
    if (rc >= 0)
        return rc;
    return json ? pk_list_json(run) : pk_list_text(run);
}

/* ---- extract ------------------------------------------------------------------------------ */

/* `*` any run without `/`, `**` any run, `?` one byte but `/`; everything else itself. */
static bool pk_glob(const char *g, const char *s) {
    while (*g != '\0') {
        if (*g == '*') {
            bool deep = g[1] == '*';
            while (*g == '*')
                ++g;
            for (const char *t = s;; ++t) {
                if (pk_glob(g, t))
                    return true;
                if (*t == '\0' || (!deep && *t == '/'))
                    return false;
            }
        }
        if (*s == '\0' || (*g == '?' ? *s == '/' : *g != *s))
            return false;
        ++g;
        ++s;
    }
    return *s == '\0';
}

/* A pack name usable as a relative file path on every OS: segments that are not empty, `.` or
 * `..`, and no character Windows refuses. Otherwise the caller names the file by its ID. */
static bool pk_safe_path(const char *name) {
    if (name == NULL || name[0] == '\0' || strlen(name) > 512)
        return false;
    const char *seg = name;
    for (const char *p = name;; ++p) {
        if (*p == '/' || *p == '\0') {
            size_t sl = (size_t)(p - seg);
            if (sl == 0 || (sl == 1 && seg[0] == '.') ||
                (sl == 2 && seg[0] == '.' && seg[1] == '.'))
                return false;
            if (seg[sl - 1] == '.' || seg[sl - 1] == ' ')
                return false; /* Windows drops them */
            if (*p == '\0')
                return true;
            seg = p + 1;
            continue;
        }
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f || strchr("\\:<>\"|?*", (int)c) != NULL)
            return false;
    }
}

/* Creates the native directory `path` and its missing parents (see cook_cmd.c, which does the
 * same for de-cook's output): the deepest existing ancestor is mounted, the rest created through
 * the VFS. */
static de_result pk_make_native_dir(const char *path) {
    char cand[DE_FS_PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len + 1 > sizeof cand)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "directory path '%s' is empty or too long", path);
    memcpy(cand, path, len + 1);
    while (len > 1 && pk_is_sep(cand[len - 1]))
        cand[--len] = '\0';
    size_t rest = len;
    for (;;) {
        if (de_fs_mount(PK_MK_ROOT, cand, DE_MOUNT_WRITABLE, 0) == DE_OK)
            break;
        de_error_clear();
        size_t sep = len;
        for (size_t i = 0; i < len; ++i)
            if (pk_is_sep(cand[i]))
                sep = i;
        if (sep == len) {
            if (strcmp(cand, ".") == 0)
                return DE_FAIL(DE_ERR_IO, "cannot open the working directory");
            memcpy(cand, ".", 2);
            rest = 0;
            len = 1;
            continue;
        }
        if (sep == 0 || (sep == 2 && cand[1] == ':')) {
            if (len == sep + 1)
                return DE_FAIL(DE_ERR_IO, "cannot open the directory '%s'", cand);
            cand[sep + 1] = '\0';
            len = sep + 1;
        } else {
            cand[sep] = '\0';
            len = sep;
        }
        rest = sep + 1;
    }
    de_result r = DE_OK;
    if (rest < strlen(path)) {
        char v[DE_FS_PATH_MAX];
        int n = snprintf(v, sizeof v, "%s%s", PK_MK_PREFIX, path + rest);
        if (n < 0 || (size_t)n >= sizeof v) {
            r = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "directory path '%s' is too long", path);
        } else {
            for (char *p = v; *p != '\0'; ++p)
                if (*p == '\\')
                    *p = '/';
            r = de_dir_create(v);
        }
    }
    (void)de_fs_unmount(PK_MK_ROOT, cand);
    return r;
}

/* Writes entry `index` to depakout:/<file>, creating its directories. */
static de_result pk_extract_one(pk_run *run, u32 index, const char *file, void *dctx, u8 **buf,
                                u64 *cap, u64 *out_size) {
    dpak_toc_entry e;
    dpak_entry(run->r, index, &e);
    if (e.raw_size + 1 > *cap) {
        u8 *q = (u8 *)realloc(*buf, (size_t)e.raw_size + 1);
        if (q == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for %llu bytes",
                           (unsigned long long)e.raw_size);
        *buf = q;
        *cap = e.raw_size + 1;
    }
    DE_TRY(dpak_read_entry(run->r, index, dctx, *buf, e.raw_size));
    char vpath[DE_FS_PATH_MAX];
    int n = snprintf(vpath, sizeof vpath, "%s%s", PK_OUT_PREFIX, file);
    if (n < 0 || (size_t)n >= sizeof vpath)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "path too long for '%s'", file);
    for (char *p = strchr(vpath + sizeof PK_OUT_PREFIX - 1, '/'); p != NULL;
         p = strchr(p + 1, '/')) {
        *p = '\0';
        de_result r = de_dir_create(vpath);
        *p = '/';
        DE_TRY(r);
    }
    *out_size = e.raw_size;
    return de_file_write_all_atomic(vpath, *buf, e.raw_size);
}

static bool pk_parse_id(const char *s, u64 *out) {
    if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X') || s[2] == '\0' || strlen(s + 2) > 16)
        return false;
    u64 v = 0;
    for (const char *p = s + 2; *p != '\0'; ++p) {
        char c = *p;
        int d = c >= '0' && c <= '9'   ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                       : -1;
        if (d < 0)
            return false;
        v = v * 16u + (u64)d;
    }
    *out = v;
    return true;
}

static int pk_extract(pk_run *run, int argc, const char *const *argv) {
    const de_pack_io *io = run->io;
    const char *sel = NULL;
    run->out_dir = ".";
    for (int i = 0; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "-o") == 0 || strcmp(a, "--out") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0')
                return pk_usage_error(io, "missing value for", a);
            run->out_dir = argv[++i];
        } else if (a[0] == '-' && a[1] != '\0') {
            return pk_usage_error(io, "unknown option", a);
        } else if (run->pak == NULL) {
            run->pak = a;
        } else if (sel == NULL) {
            sel = a;
        } else {
            return pk_usage_error(io, "too many arguments:", a);
        }
    }
    if (run->pak == NULL)
        return pk_usage_error(io, "no pack given", NULL);
    if (sel == NULL || sel[0] == '\0')
        return pk_usage_error(io, "no asset ID, path or glob given", NULL);
    int rc = pk_open(run);
    if (rc >= 0)
        return rc;

    /* What to extract. */
    u32 n = dpak_entry_count(run->r);
    u64 id = 0;
    bool by_id = pk_parse_id(sel, &id);
    bool glob = !by_id && strpbrk(sel, "*?") != NULL;
    const char *path_name = NULL; /* the selector names the file of a stripped entry */
    if (!by_id && !glob) {
        id = de_asset_id_from(sel).hash;
        path_name = sel;
        by_id = true;
    }
    if (glob && (dpak_get_header(run->r)->flags & DPAK_HEADER_HAS_NAMES) == 0) {
        pk_puts(io, DE_PACK_STDERR, "de-pack: ");
        pk_put_safe(io, DE_PACK_STDERR, run->pak);
        pk_puts(io, DE_PACK_STDERR,
                " has no names (stripped): select by ID (0x...) or by asset path\n");
        return DE_PACK_EXIT_PROBLEMS;
    }

    de_error_clear();
    if (pk_make_native_dir(run->out_dir) != DE_OK)
        return pk_error(io, DE_PACK_EXIT_USAGE, "cannot create the output directory", run->out_dir);
    if (de_fs_mount(PK_OUT_ROOT, run->out_dir, DE_MOUNT_WRITABLE, 0) != DE_OK)
        return pk_error(io, DE_PACK_EXIT_USAGE, "cannot open the output directory", run->out_dir);
    run->out_mounted = true;

    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    if (dctx == NULL)
        return pk_error(io, DE_PACK_EXIT_USAGE, "out of memory", NULL);
    u8 *buf = NULL;
    u64 cap = 0;
    u32 written = 0;
    rc = DE_PACK_EXIT_OK;
    for (u32 i = 0; i < n; ++i) {
        dpak_toc_entry e;
        dpak_entry(run->r, i, &e);
        const char *name = dpak_name(run->r, i, NULL);
        if (by_id ? e.id != id : (name == NULL || !pk_glob(sel, name)))
            continue;
        char file[DE_FS_PATH_MAX];
        if (pk_safe_path(name))
            snprintf(file, sizeof file, "%s", name);
        else if (name == NULL && pk_safe_path(path_name))
            snprintf(file, sizeof file, "%s", path_name);
        else
            pk_hex16(file, e.id);
        u64 size = 0;
        de_error_clear();
        if (pk_extract_one(run, i, file, dctx, &buf, &cap, &size) != DE_OK) {
            rc = pk_error(io, DE_PACK_EXIT_PROBLEMS, "cannot extract", file);
            continue;
        }
        pk_put_safe(io, DE_PACK_STDOUT, file);
        pk_printf(io, DE_PACK_STDOUT, "  %llu bytes\n", (unsigned long long)size);
        written++;
    }
    free(buf);
    ZSTD_freeDCtx(dctx);
    if (written == 0 && rc == DE_PACK_EXIT_OK) {
        pk_puts(io, DE_PACK_STDERR, "de-pack: no entry of ");
        pk_put_safe(io, DE_PACK_STDERR, run->pak);
        pk_puts(io, DE_PACK_STDERR, " matches '");
        pk_put_safe(io, DE_PACK_STDERR, sel);
        pk_puts(io, DE_PACK_STDERR, "'\n");
        return DE_PACK_EXIT_PROBLEMS;
    }
    pk_printf(io, DE_PACK_STDOUT, "de-pack extract: %u entr%s written to ", (unsigned)written,
              written == 1 ? "y" : "ies");
    pk_put_safe(io, DE_PACK_STDOUT, run->out_dir);
    pk_puts(io, DE_PACK_STDOUT, "\n");
    return rc;
}

/* ---- verify ------------------------------------------------------------------------------- */

typedef struct pk_verify {
    pk_run *run;
    u32 problems, external;
} pk_verify;

/* `asset "<name>" (0x<id>)` or `asset 0x<id>`. */
static void pk_put_asset(pk_run *run, u32 index, u64 id) {
    char hex[17];
    pk_hex16(hex, id);
    const char *name = index != DPAK_NOT_FOUND ? dpak_name(run->r, index, NULL) : NULL;
    if (name != NULL) {
        pk_puts(run->io, DE_PACK_STDOUT, "asset \"");
        pk_put_safe(run->io, DE_PACK_STDOUT, name);
        pk_printf(run->io, DE_PACK_STDOUT, "\" (0x%s)", hex);
    } else {
        pk_printf(run->io, DE_PACK_STDOUT, "asset 0x%s", hex);
    }
}

static void pk_problem(pk_verify *v, u32 index, u64 id, const char *msg) {
    v->problems++;
    pk_puts(v->run->io, DE_PACK_STDOUT, "error: ");
    if (index != DPAK_NOT_FOUND || id != 0) {
        pk_put_asset(v->run, index, id);
        pk_puts(v->run->io, DE_PACK_STDOUT, ": ");
    }
    pk_put_safe(v->run->io, DE_PACK_STDOUT, msg);
    pk_puts(v->run->io, DE_PACK_STDOUT, "\n");
}

/* XXH3-64 (de_hash64, seed 0) of the file bytes after the header, read in blocks. */
static de_result pk_content_hash(pk_run *run, u64 *out) {
    const dpak_header *h = dpak_get_header(run->r);
    de_file *f = dpak_file(run->r);
    XXH3_state_t *st = XXH3_createState();
    u8 *block = (u8 *)malloc(PK_HASH_BLOCK);
    de_result r = DE_OK;
    if (st == NULL || block == NULL || XXH3_64bits_reset(st) == XXH_ERROR) {
        r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory");
    } else {
        for (u64 off = h->header_size; off < h->file_size && r == DE_OK;) {
            u64 want = h->file_size - off < PK_HASH_BLOCK ? h->file_size - off : PK_HASH_BLOCK;
            u64 got = 0;
            r = de_file_read(f, block, off, want, &got);
            if (r == DE_OK && got != want)
                r = DE_FAIL(DE_ERR_IO, "short read at offset %llu", (unsigned long long)off);
            if (r == DE_OK)
                (void)XXH3_64bits_update(st, block, (size_t)got);
            off += got;
        }
        if (r == DE_OK)
            *out = XXH3_64bits_digest(st);
    }
    free(block);
    XXH3_freeState(st);
    return r;
}

static int pk_verify_pack(pk_run *run) {
    const de_pack_io *io = run->io;
    pk_verify v = {run, 0, 0};
    const dpak_header *h = dpak_get_header(run->r);
    u32 n = dpak_entry_count(run->r), nb = dpak_bundle_count(run->r);
    char msg[DE_ERROR_MESSAGE_MAX + 64];

    /* Content hash. */
    u64 hash = 0;
    de_error_clear();
    if (pk_content_hash(run, &hash) != DE_OK)
        return pk_error(io, DE_PACK_EXIT_USAGE, "cannot read", run->pak);
    if (hash != h->content_hash) {
        snprintf(msg, sizeof msg,
                 "content hash mismatch: the header says %016llx, the bytes hash to %016llx",
                 (unsigned long long)h->content_hash, (unsigned long long)hash);
        pk_problem(&v, DPAK_NOT_FOUND, 0, msg);
    }

    /* TOC order, names, every chunk, dependencies. */
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    u8 *buf = NULL;
    u64 cap = 0, chunks = 0;
    if (dctx == NULL)
        return pk_error(io, DE_PACK_EXIT_USAGE, "out of memory", NULL);
    u32 damaged = 0;
    for (u32 i = 0; i < n; ++i) {
        dpak_toc_entry e;
        dpak_entry(run->r, i, &e);
        chunks += e.chunk_count;
        if (i > 0) {
            dpak_toc_entry prev;
            dpak_entry(run->r, i - 1, &prev);
            if (prev.id >= e.id)
                pk_problem(&v, i, e.id, "the TOC is not sorted by ID");
        }
        u32 nl = 0;
        const char *name = dpak_name(run->r, i, &nl);
        if (name != NULL && de_asset_id_from(name).hash != e.id) {
            snprintf(msg, sizeof msg, "its name hashes to %016llx",
                     (unsigned long long)de_asset_id_from(name).hash);
            pk_problem(&v, i, e.id, msg);
        }
        if (e.raw_size + 1 > cap) {
            u8 *q = (u8 *)realloc(buf, (size_t)e.raw_size + 1);
            if (q == NULL) {
                pk_problem(&v, i, e.id, "out of memory");
                continue;
            }
            buf = q;
            cap = e.raw_size + 1;
        }
        de_error_clear();
        if (dpak_read_entry(run->r, i, dctx, buf, e.raw_size) != DE_OK) {
            const de_error *err = de_error_last();
            snprintf(msg, sizeof msg, "does not decode: %s",
                     err != NULL ? err->message : "unknown error");
            pk_problem(&v, i, e.id, msg);
            damaged++;
        }
        for (u32 d = 0; d < e.dep_count; ++d) {
            u64 dep = dpak_dep(run->r, e.dep_first + d);
            if (dpak_find(run->r, dep) != DPAK_NOT_FOUND)
                continue;
            v.external++;
            char hex[17];
            pk_hex16(hex, dep);
            pk_puts(io, DE_PACK_STDOUT, "note: ");
            pk_put_asset(run, i, e.id);
            pk_printf(io, DE_PACK_STDOUT,
                      " depends on 0x%s, which is not in this pack (external)\n", hex);
        }
    }
    free(buf);
    ZSTD_freeDCtx(dctx);
    if (hash != h->content_hash && damaged == 0)
        pk_puts(io, DE_PACK_STDOUT,
                "note: every chunk decodes, so the damage is in a chunk stored raw or in the "
                "metadata; format v1 has no per-asset hash to name it\n");

    /* Bundles: members and depends that this pack does not hold. */
    for (u32 b = 0; b < nb; ++b) {
        dpak_bundle_entry be;
        dpak_bundle(run->r, b, &be);
        for (u32 m = 0; m < be.member_count; ++m) {
            u64 id = dpak_bundle_id(run->r, be.member_first + m);
            if (dpak_find(run->r, id) != DPAK_NOT_FOUND)
                continue;
            v.external++;
            char hex[17];
            pk_hex16(hex, id);
            pk_puts(io, DE_PACK_STDOUT, "note: bundle \"");
            pk_put_safe(io, DE_PACK_STDOUT, dpak_bundle_name(run->r, &be));
            pk_printf(io, DE_PACK_STDOUT, "\" names 0x%s, which is not in this pack (external)\n",
                      hex);
        }
        for (u32 d = 0; d < be.depend_count; ++d) {
            u64 dh = dpak_bundle_id(run->r, be.depend_first + d);
            if (dpak_bundle_find(run->r, dh) != DPAK_NOT_FOUND)
                continue;
            v.external++;
            char hex[17];
            pk_hex16(hex, dh);
            pk_puts(io, DE_PACK_STDOUT, "note: bundle \"");
            pk_put_safe(io, DE_PACK_STDOUT, dpak_bundle_name(run->r, &be));
            pk_printf(io, DE_PACK_STDOUT,
                      "\" depends on bundle 0x%s, which is not in this pack (external)\n", hex);
        }
    }

    pk_puts(io, DE_PACK_STDOUT, "de-pack verify: ");
    pk_put_safe(io, DE_PACK_STDOUT, run->pak);
    if (v.problems == 0) {
        pk_printf(io, DE_PACK_STDOUT,
                  ": OK (platform %s, %u entries, %llu chunks, %u bundles, %u external "
                  "references)\n",
                  pk_platform_name(h->platform), (unsigned)n, (unsigned long long)chunks,
                  (unsigned)nb, (unsigned)v.external);
        return DE_PACK_EXIT_OK;
    }
    pk_printf(io, DE_PACK_STDOUT, ": FAILED (%u problem%s)\n", (unsigned)v.problems,
              v.problems == 1 ? "" : "s");
    return DE_PACK_EXIT_PROBLEMS;
}

static int pk_verify_cmd(pk_run *run, int argc, const char *const *argv) {
    for (int i = 0; i < argc; ++i) {
        if (argv[i][0] == '-')
            return pk_usage_error(run->io, "unknown option", argv[i]);
        if (run->pak != NULL)
            return pk_usage_error(run->io, "more than one pack given:", argv[i]);
        run->pak = argv[i];
    }
    if (run->pak == NULL)
        return pk_usage_error(run->io, "no pack given", NULL);
    de_error_clear();
    int rc = pk_open(run);
    if (rc == DE_PACK_EXIT_PROBLEMS) {
        pk_puts(run->io, DE_PACK_STDOUT, "de-pack verify: ");
        pk_put_safe(run->io, DE_PACK_STDOUT, run->pak);
        pk_puts(run->io, DE_PACK_STDOUT, ": FAILED (the pack does not open)\n");
    }
    if (rc >= 0)
        return rc;
    return pk_verify_pack(run);
}

/* ---- Dispatch ----------------------------------------------------------------------------- */

int de_pack_main(int argc, const char *const *argv, const de_pack_io *io) {
    if (argc < 1 || argv[0] == NULL) {
        de_pack_usage(io, DE_PACK_STDERR);
        return DE_PACK_EXIT_USAGE;
    }
    for (int i = 0; i < argc; ++i)
        if (argv[i] == NULL)
            return pk_usage_error(io, "missing argument", NULL);
    const char *cmd = argv[0];
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        de_pack_usage(io, DE_PACK_STDOUT);
        return DE_PACK_EXIT_OK;
    }
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            de_pack_usage(io, DE_PACK_STDOUT);
            return DE_PACK_EXIT_OK;
        }
    }
    pk_run run;
    memset(&run, 0, sizeof run);
    run.io = io;
    int rc;
    if (strcmp(cmd, "list") == 0)
        rc = pk_list(&run, argc - 1, argv + 1);
    else if (strcmp(cmd, "extract") == 0)
        rc = pk_extract(&run, argc - 1, argv + 1);
    else if (strcmp(cmd, "verify") == 0)
        rc = pk_verify_cmd(&run, argc - 1, argv + 1);
    else
        rc = pk_usage_error(io, "unknown command", cmd);
    pk_close(&run);
    return rc;
}
