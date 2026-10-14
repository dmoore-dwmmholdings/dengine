/*
 * cook_cmd.c: `de-cook cook` (DENG-213). See cook_cmd.h for the command line, the steps and the
 * output.
 *
 * Files: the manifest's directory is mounted read-only as `cook:/` (like `--check`), the `-I`
 * directories as `cookinc:/`, the cache directory as `cookcache:/` and the output directory as
 * `cookout:/` (both writable). `cookmk:/` is mounted for a moment to create a missing directory.
 * Every mount is removed before the command returns. Messages name files the way `--check` does:
 * `cook:/` is replaced by the manifest argument's directory as typed.
 */
#include "cook_cmd.h"

#include <dengine/de_ecs.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset/dpak/dpak_format.h"
#include "asset/dpak/dpak_reader.h"
#include "asset/manifest_internal.h"
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"
#include "cook/lib/de_cooker.h"
#include "cook/lib/decache.h"
#include "dpak_writer.h"
#include "host/game_module.h"

#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

#ifndef DE_HAS_COOKER
#define DE_HAS_COOKER 0
#endif

/* DENG-394: 1 in a DE_BUILD_SHARED build, where `--game` is refused (see cc_load_game). */
#ifndef DE_COOK_SHARED_ENGINE
#define DE_COOK_SHARED_ENGINE 0
#endif

#define CC_ROOT          "cook"
#define CC_ROOT_PREFIX   "cook:/"
#define CC_INC_ROOT      "cookinc"
#define CC_INC_PREFIX    "cookinc:/"
#define CC_CACHE_ROOT    "cookcache"
#define CC_CACHE_VDIR    "cookcache:/"
#define CC_OUT_ROOT      "cookout"
#define CC_OUT_PREFIX    "cookout:/"
#define CC_MK_ROOT       "cookmk"
#define CC_MK_PREFIX     "cookmk:/"
#define CC_MAX_INCLUDES  16
#define CC_MAX_JOBS      1024u
#define CC_PACK_NAME_MAX 64u
#define CC_MSG_MAX       1024u

#if DE_HAS_COOKER
/* Map the manifest's bundle priority to the pack's. A switch, not a _Static_assert on the
 * enum values: MSVC reports C5287 (mixed enum types) even through int casts. */
static u32 cc_pack_priority(de__manifest_priority p) {
    switch (p) {
    case DE__MANIFEST_PRIORITY_CRITICAL:
        return DPAK_PRIORITY_CRITICAL;
    case DE__MANIFEST_PRIORITY_HIGH:
        return DPAK_PRIORITY_HIGH;
    case DE__MANIFEST_PRIORITY_NORMAL:
        return DPAK_PRIORITY_NORMAL;
    case DE__MANIFEST_PRIORITY_LOW:
        return DPAK_PRIORITY_LOW;
    case DE__MANIFEST_PRIORITY_BACKGROUND:
        return DPAK_PRIORITY_BACKGROUND;
    }
    return DPAK_PRIORITY_NORMAL;
}
#endif

/* ---- Output ------------------------------------------------------------------------------- */

static void cc_write(const de_cook_io *io, de_cook_stream s, const char *text, size_t len) {
    if (len > 0)
        io->write(io->user, s, text, len);
}

static void cc_puts(const de_cook_io *io, de_cook_stream s, const char *text) {
    cc_write(io, s, text, strlen(text));
}

#if DE_HAS_COOKER
static void cc_printf(const de_cook_io *io, de_cook_stream s, const char *fmt, ...)
    DE_PRINTF_FMT(3, 4);
static void cc_printf(const de_cook_io *io, de_cook_stream s, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    cc_write(io, s, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}
#endif

void de_cook_cook_usage(const de_cook_io *io, de_cook_stream s) {
    cc_puts(
        io, s,
        "usage: de-cook cook <assets.toml> --platform <windows|linux|macos> --out <dir> "
        "[options]\n"
        "\n"
        "Checks the manifest (as --check does), cooks every asset and sub-asset through the\n"
        "cook cache and writes one pack <out>/<pack-name>.dpak with the manifest's bundles.\n"
        "The same inputs give a byte-identical pack, with a cold or warm cache and any --jobs.\n"
        "\n"
        "options:\n"
        "  --platform <p>         target platform: windows, linux or macos (required)\n"
        "  --out <dir>            output directory, created if missing (required)\n"
        "  --jobs <n>             cook on n threads (default: one per logical core)\n"
        "  --cache <dir>          cook cache directory (default: .decache next to the "
        "manifest)\n"
        "  --no-cache             do not use a cook cache\n"
        "  --build-id <hex>       build ID, 32 hex digits (default: a hash of the pack)\n"
        "  --strip-names          leave asset paths out of the pack (shipping)\n"
        "  --pack-name <name>     pack file name without .dpak (default: game)\n"
        "  --no-dictionaries      do not train zstd dictionaries per asset type\n"
        "  --verify-determinism   run every import twice and compare the outputs\n"
        "  --game <library>       load a game library (de_add_game) and register its asset\n"
        "                         and component types before cooking\n"
        "  -I, --include-dir <dir>\n"
        "                         read includes missing next to the including file from "
        "<dir>\n"
        "  --quiet                no progress lines\n"
        "  -h, --help             print this text\n"
        "\n"
        "Progress and diagnostics go to stderr, the summary to stdout. Manifest errors print\n"
        "as --check prints them.\n"
        "\n"
        "exit codes: 0 pack written, 1 manifest errors or failed cooks (no pack written),\n"
        "2 usage or I/O error\n");
}

static int cc_usage_error(const de_cook_io *io, const char *what, const char *arg) {
    cc_puts(io, DE_COOK_STDERR, "de-cook: ");
    cc_puts(io, DE_COOK_STDERR, what);
    if (arg != NULL) {
        cc_puts(io, DE_COOK_STDERR, " '");
        cc_puts(io, DE_COOK_STDERR, arg);
        cc_puts(io, DE_COOK_STDERR, "'");
    }
    cc_puts(io, DE_COOK_STDERR, "\n\n");
    de_cook_cook_usage(io, DE_COOK_STDERR);
    return DE_COOK_EXIT_USAGE;
}

static int cc_io_error(const de_cook_io *io, const char *what, const char *arg) {
    cc_puts(io, DE_COOK_STDERR, "de-cook: ");
    cc_puts(io, DE_COOK_STDERR, what);
    if (arg != NULL) {
        cc_puts(io, DE_COOK_STDERR, " '");
        cc_puts(io, DE_COOK_STDERR, arg);
        cc_puts(io, DE_COOK_STDERR, "'");
    }
    const de_error *e = de_error_last();
    if (e != NULL && e->message[0] != '\0') {
        cc_puts(io, DE_COOK_STDERR, ": ");
        cc_puts(io, DE_COOK_STDERR, e->message);
    }
    cc_puts(io, DE_COOK_STDERR, "\n");
    return DE_COOK_EXIT_USAGE;
}

/* ---- Arguments ---------------------------------------------------------------------------- */

typedef struct cc_options {
    const char *manifest;
    const char *platform_arg;
    de_cook_platform platform;
    u32 dpak_platform;
    const char *out;
    u32 jobs; /* 0 = one per logical core */
    const char *cache;
    bool no_cache;
    bool has_build_id;
    u8 build_id[16];
    bool strip_names;
    const char *pack_name;
    bool no_dictionaries;
    bool verify;
    bool quiet;
    bool help;
    const char *include_dirs[CC_MAX_INCLUDES];
    u32 include_dir_count;
    const char *game; /* DENG-394: --game <library>, a native path; NULL = none */
} cc_options;

/* `--name value` or `--name=value`. Returns the value, or NULL if `arg` is not this option.
 * `*missing` is set when the option is present but has no value. */
static const char *cc_value_opt(int argc, const char *const *argv, int *i, const char *name,
                                bool *missing) {
    const char *arg = argv[*i];
    size_t n = strlen(name);
    if (strncmp(arg, name, n) != 0)
        return NULL;
    if (arg[n] == '=')
        return arg + n + 1;
    if (arg[n] != '\0')
        return NULL;
    if (*i + 1 >= argc) {
        *missing = true;
        return NULL;
    }
    *i += 1;
    return argv[*i];
}

static int cc_hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool cc_parse_build_id(const char *v, u8 out[16]) {
    if (strlen(v) != 32)
        return false;
    for (u32 i = 0; i < 16; ++i) {
        int hi = cc_hex_digit(v[2 * i]), lo = cc_hex_digit(v[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (u8)(hi * 16 + lo);
    }
    return true;
}

static bool cc_pack_name_ok(const char *v) {
    size_t n = strlen(v);
    if (n == 0 || n > CC_PACK_NAME_MAX || v[0] == '.')
        return false;
    for (size_t i = 0; i < n; ++i) {
        char c = v[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

static int cc_parse_args(int argc, const char *const *argv, cc_options *o, const de_cook_io *io) {
    memset(o, 0, sizeof *o);
    o->pack_name = "game";
    for (int i = 0; i < argc; ++i) {
        const char *arg = argv[i];
        if (arg == NULL)
            return cc_usage_error(io, "missing argument", NULL);
        bool missing = false;
        const char *v;
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            o->help = true;
        } else if (strcmp(arg, "--no-cache") == 0) {
            o->no_cache = true;
        } else if (strcmp(arg, "--strip-names") == 0) {
            o->strip_names = true;
        } else if (strcmp(arg, "--no-dictionaries") == 0) {
            o->no_dictionaries = true;
        } else if (strcmp(arg, "--verify-determinism") == 0) {
            o->verify = true;
        } else if (strcmp(arg, "--quiet") == 0) {
            o->quiet = true;
        } else if ((v = cc_value_opt(argc, argv, &i, "--platform", &missing)) != NULL || missing) {
            if (missing)
                return cc_usage_error(io, "missing value for", "--platform");
            o->platform_arg = v;
            if (strcmp(v, "windows") == 0) {
                o->platform = DE_COOK_PLATFORM_WINDOWS;
                o->dpak_platform = DPAK_PLATFORM_WINDOWS;
            } else if (strcmp(v, "linux") == 0) {
                o->platform = DE_COOK_PLATFORM_LINUX;
                o->dpak_platform = DPAK_PLATFORM_LINUX;
            } else if (strcmp(v, "macos") == 0) {
                o->platform = DE_COOK_PLATFORM_MACOS;
                o->dpak_platform = DPAK_PLATFORM_MACOS;
            } else {
                return cc_usage_error(io, "unknown platform", v);
            }
        } else if ((v = cc_value_opt(argc, argv, &i, "--out", &missing)) != NULL || missing) {
            if (missing || v[0] == '\0')
                return cc_usage_error(io, "missing value for", "--out");
            o->out = v;
        } else if ((v = cc_value_opt(argc, argv, &i, "--jobs", &missing)) != NULL || missing) {
            if (missing)
                return cc_usage_error(io, "missing value for", "--jobs");
            char *end = NULL;
            unsigned long n = strtoul(v, &end, 10);
            if (v[0] < '0' || v[0] > '9' || end == NULL || *end != '\0' || n < 1 || n > CC_MAX_JOBS)
                return cc_usage_error(io, "--jobs takes a number from 1 to 1024, not", v);
            o->jobs = (u32)n;
        } else if ((v = cc_value_opt(argc, argv, &i, "--cache", &missing)) != NULL || missing) {
            if (missing || v[0] == '\0')
                return cc_usage_error(io, "missing value for", "--cache");
            o->cache = v;
        } else if ((v = cc_value_opt(argc, argv, &i, "--build-id", &missing)) != NULL || missing) {
            if (missing)
                return cc_usage_error(io, "missing value for", "--build-id");
            if (!cc_parse_build_id(v, o->build_id))
                return cc_usage_error(io, "--build-id takes 32 hex digits, not", v);
            o->has_build_id = true;
        } else if ((v = cc_value_opt(argc, argv, &i, "--pack-name", &missing)) != NULL || missing) {
            if (missing)
                return cc_usage_error(io, "missing value for", "--pack-name");
            if (!cc_pack_name_ok(v))
                return cc_usage_error(
                    io, "--pack-name takes 1 to 64 of A-Z a-z 0-9 _ - . (not first), not", v);
            o->pack_name = v;
        } else if ((v = cc_value_opt(argc, argv, &i, "--game", &missing)) != NULL || missing) {
            if (missing || v[0] == '\0')
                return cc_usage_error(io, "missing value for", "--game");
            if (o->game != NULL)
                return cc_usage_error(io, "more than one game library given:", v);
            o->game = v;
        } else if ((v = cc_value_opt(argc, argv, &i, "--include-dir", &missing)) != NULL ||
                   missing || (v = cc_value_opt(argc, argv, &i, "-I", &missing)) != NULL ||
                   missing || (strncmp(arg, "-I", 2) == 0 && arg[2] != '\0')) {
            if (missing)
                return cc_usage_error(io, "missing value for", arg);
            if (v == NULL)
                v = arg + 2; /* -I<dir> */
            if (v[0] == '\0')
                return cc_usage_error(io, "empty include directory", NULL);
            if (o->include_dir_count == CC_MAX_INCLUDES)
                return cc_usage_error(io, "too many include directories", NULL);
            o->include_dirs[o->include_dir_count++] = v;
        } else if (arg[0] == '-') {
            return cc_usage_error(io, "unknown option", arg);
        } else if (o->manifest != NULL) {
            return cc_usage_error(io, "more than one manifest given:", arg);
        } else {
            o->manifest = arg;
        }
    }
    if (o->help)
        return DE_COOK_EXIT_OK;
    if (o->manifest == NULL || o->manifest[0] == '\0')
        return cc_usage_error(io, "no manifest given", NULL);
    if (o->platform_arg == NULL)
        return cc_usage_error(io, "no --platform given", NULL);
    if (o->out == NULL)
        return cc_usage_error(io, "no --out directory given", NULL);
    if (o->no_cache && o->cache != NULL)
        return cc_usage_error(io, "--cache and --no-cache exclude each other", NULL);
    return -1; /* go on */
}

#if DE_HAS_COOKER
/* ---- A growable text buffer (the captured --check output) ---------------------------------- */

typedef struct cc_text {
    char *p;
    size_t len, cap;
    bool oom;
} cc_text;

static void cc_text_add(cc_text *t, const char *s, size_t n) {
    if (t->oom || n == 0)
        return;
    if (t->len + n + 1 > t->cap) {
        size_t cap = t->cap != 0 ? t->cap : 4096;
        while (cap < t->len + n + 1)
            cap *= 2;
        char *q = (char *)realloc(t->p, cap);
        if (q == NULL) {
            t->oom = true;
            return;
        }
        t->p = q;
        t->cap = cap;
    }
    memcpy(t->p + t->len, s, n);
    t->len += n;
    t->p[t->len] = '\0';
}

typedef struct cc_capture {
    cc_text out, err;
} cc_capture;

static void cc_capture_write(void *user, de_cook_stream stream, const char *text, size_t len) {
    cc_capture *c = (cc_capture *)user;
    cc_text_add(stream == DE_COOK_STDERR ? &c->err : &c->out, text, len);
}

#endif /* DE_HAS_COOKER */

/* ---- State of one run --------------------------------------------------------------------- */

/* One manifest asset to cook, and what came out. */
typedef struct cc_item {
    const de__manifest_asset *asset;
    de_asset_id id;
    de_cook_result res;
    de_result rc;
    char msg[CC_MSG_MAX];
} cc_item;

/* One pack entry: an asset (`sub` = UINT32_MAX) or sub-asset `sub` of item `item`. */
typedef struct cc_entry {
    u64 id;
    u32 item;
    u32 sub;
} cc_entry;

typedef struct cc_run {
    const de_allocator *a;
    const cc_options *o;
    const de_cook_io *io;
    const de_cook_hooks *hooks;

    char dir[DE_FS_PATH_MAX];    /* native directory of the manifest */
    char prefix[DE_FS_PATH_MAX]; /* how messages name that directory: the argument's own text */
    char root[DE_FS_PATH_MAX];   /* cook:/<manifest file name> */
    char asset_root[DE_FS_PATH_MAX];
    char cache_native[DE_FS_PATH_MAX];
    bool root_mounted, cache_mounted, out_mounted;
    u32 inc_mounted;

    de_task_system *tasks;
    de_type_registry *types;
    de_world *world;
    de_asset_system *assets;
    de_cooker *cooker;

    /* DENG-394: the game library of --game, and the engine its register_types got. Both live
     * until the run ends: the types it registered point into the library, and a game may keep
     * the engine pointer. */
    de__game_module game;
    de_engine engine;
    u32 game_asset_types; /* asset types the game added */

    de__manifest *manifest;
    de__resolved *resolved;

    cc_item *items;
    u32 item_count;
    de_mutex progress_lock;
    bool progress_lock_ok;
    u32 done;
} cc_run;

static void cc_cleanup(cc_run *run) {
    for (u32 i = 0; i < run->item_count; ++i)
        de_cook_result_free(&run->items[i].res);
    free(run->items);
    if (run->resolved != NULL)
        de__resolved_destroy(run->resolved);
    if (run->manifest != NULL)
        de__manifest_destroy(run->manifest);
    de_cooker_destroy(run->cooker); /* writes the cache index */
    de_asset_system_destroy(run->assets);
    if (run->world != NULL)
        de_world_destroy(run->world);
    if (run->types != NULL)
        de_type_registry_destroy(run->types);
    if (run->tasks != NULL)
        de_task_system_destroy(run->tasks);
    if (run->progress_lock_ok)
        de_mutex_destroy(&run->progress_lock);
    if (run->out_mounted)
        (void)de_fs_unmount(CC_OUT_ROOT, run->o->out);
    if (run->cache_mounted)
        (void)de_fs_unmount(CC_CACHE_ROOT, run->cache_native);
    for (u32 i = 0; i < run->inc_mounted; ++i)
        (void)de_fs_unmount(CC_INC_ROOT, run->o->include_dirs[i]);
    if (run->root_mounted)
        (void)de_fs_unmount(CC_ROOT, run->dir);
    /* Last: everything above may hold pointers into the game library (type names, importers,
     * loaders, component hooks), and so may the error context (file and function names). */
    if (run->game.api != NULL) {
        de_error_clear();
        de__game_module_unload(&run->game);
    }
}

#if DE_HAS_COOKER /* the cook path; Release and Shipping trees only refuse (cc_cook) */

static bool cc_is_sep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

/* Writes `text` with `cook:/` replaced by the manifest argument's directory (as --check names
 * files) and control characters replaced by spaces, so each message stays on one line. */
static void cc_put_message(const cc_run *run, de_cook_stream s, const char *text) {
    static const char root[] = CC_ROOT_PREFIX;
    const size_t rl = sizeof root - 1;
    const de_cook_io *io = run->io;
    const char *start = text;
    for (const char *p = text; *p != '\0'; ++p) {
        char b = p == text ? ' ' : p[-1];
        bool at_root = strncmp(p, root, rl) == 0 &&
                       !((b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '_' || b == '-');
        if (at_root || (unsigned char)*p < 0x20 || *p == 0x7f) {
            cc_write(io, s, start, (size_t)(p - start));
            if (at_root) {
                cc_puts(io, s, run->prefix);
                p += rl - 1;
            } else {
                cc_write(io, s, " ", 1);
            }
            start = p + 1;
        }
    }
    cc_puts(io, s, start);
}

/* Splits the manifest argument into the directory to mount, the prefix messages use for it, and
 * the root virtual path (as `--check` does). */
static int cc_split_manifest(cc_run *run) {
    const char *arg = run->o->manifest;
    size_t len = strlen(arg);
    size_t sep = len;
    for (size_t i = 0; i < len; ++i)
        if (cc_is_sep(arg[i]))
            sep = i;
    const char *name = sep == len ? arg : arg + sep + 1;
    if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return cc_usage_error(run->io, "the manifest must name a file:", arg);
    if (len + 1 > sizeof run->dir)
        return cc_usage_error(run->io, "manifest path too long:", arg);
    if (sep == len) {
        memcpy(run->dir, ".", 2);
        run->prefix[0] = '\0';
    } else {
        size_t dl = sep;
        if (sep == 0 || (sep == 2 && arg[1] == ':'))
            dl = sep + 1; /* keep the separator of "/" and "C:\" */
        memcpy(run->dir, arg, dl);
        run->dir[dl] = '\0';
        memcpy(run->prefix, arg, sep + 1);
        run->prefix[sep + 1] = '\0';
    }
    char raw[DE_FS_PATH_MAX];
    int n = snprintf(raw, sizeof raw, "%s%s", CC_ROOT_PREFIX, name);
    if (n < 0 || (size_t)n >= sizeof raw ||
        de_path_normalize(raw, run->root, sizeof run->root) != DE_OK)
        return cc_io_error(run->io, "cannot use this file name as a manifest:", name);
    return -1;
}

/* Creates the native directory `path` and its missing parents: mounts the deepest ancestor that
 * exists (the working directory for a relative path without one) under cookmk:/ and creates the
 * rest through the VFS, so no native file call is needed. */
static de_result cc_make_native_dir(const char *path) {
    char cand[DE_FS_PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len + 1 > sizeof cand)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "directory path '%s' is empty or too long", path);
    memcpy(cand, path, len + 1);
    while (len > 1 && cc_is_sep(cand[len - 1]))
        cand[--len] = '\0';
    size_t rest = len; /* the part of `path` below the mounted ancestor starts here */
    for (;;) {
        if (de_fs_mount(CC_MK_ROOT, cand, DE_MOUNT_WRITABLE, 0) == DE_OK)
            break;
        de_error_clear();
        size_t sep = len;
        for (size_t i = 0; i < len; ++i)
            if (cc_is_sep(cand[i]))
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
        int n = snprintf(v, sizeof v, "%s%s", CC_MK_PREFIX, path + rest);
        if (n < 0 || (size_t)n >= sizeof v) {
            r = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "directory path '%s' is too long", path);
        } else {
            for (char *p = v; *p != '\0'; ++p)
                if (*p == '\\')
                    *p = '/';
            r = de_dir_create(v);
        }
    }
    (void)de_fs_unmount(CC_MK_ROOT, cand);
    return r;
}

/* ---- Game library (DENG-394) ---------------------------------------------------------------- */

/*
 * Loads --game with the host's loader (de__game_module_load: de_game_get_api, the API version,
 * the required callbacks) and calls the game's register_types, and nothing else: no init, no
 * register_systems, no shutdown. The game gets a de_engine with the tool's allocator, task
 * system (NULL with --jobs 1), type registry, world and asset system; every other field is NULL,
 * as on a headless server without the optional modules. Its asset types (with their importers)
 * and component types land in the tool's asset system and registry, so the cooker mirrors them
 * like the built-in ones.
 */
static int cc_load_game(cc_run *run) {
    const de_cook_io *io = run->io;
    const char *arg = run->o->game;
    de_error_clear();
#if DE_COOK_SHARED_ENGINE
    (void)arg;
    /* With DE_BUILD_SHARED the library binds to the shared dengine, a second engine next to the
     * one linked into this tool, whose VFS has none of the tool's mounts: importers that read
     * files would fail. */
    return cc_io_error(io,
                       "--game needs a de-cook that exports its engine; this one was built with "
                       "DE_BUILD_SHARED",
                       NULL);
#else
    /* A bare file name would make the OS loader search its library path: take it from the
     * current directory instead, like any other relative path. */
    bool has_dir = strchr(arg, '/') != NULL;
#ifdef _WIN32
    has_dir = has_dir || strchr(arg, '\\') != NULL || (arg[0] != '\0' && arg[1] == ':');
#endif
    char path[DE_FS_PATH_MAX];
    int n = snprintf(path, sizeof path, "%s%s", has_dir ? "" : "./", arg);
    if (n < 0 || (size_t)n >= sizeof path)
        return cc_io_error(io, "game library path too long:", arg);
    u32 before = 0;
    (void)de__asset_type_names(run->assets, &before);
    if (de__game_module_load(&run->game, path) != DE_OK)
        return cc_io_error(io, "cannot load the game library", arg);

    de_engine *e = &run->engine;
    memset(e, 0, sizeof *e);
    e->allocator = run->a;
    e->tasks = run->tasks;
    e->types = run->types;
    e->world = run->world;
    e->assets = run->assets;
    run->game.table.register_types(e, run->types);
    /* register_types cannot report a failure; a type it did not register shows up as an unknown
     * type in the manifest check. Drop what it left in the error context (it may point into
     * the library). */
    de_error_clear();
    u32 after = 0;
    (void)de__asset_type_names(run->assets, &after);
    run->game_asset_types = after - before;
    return -1;
#endif
}

/* ---- Tool runtime: registry, world, asset system, cooker ---------------------------------- */

static int cc_setup_runtime(cc_run *run) {
    const de_cook_io *io = run->io;
    u32 jobs = run->o->jobs != 0 ? run->o->jobs : de_cpu_logical_cores();
    if (jobs > 1) {
        de_fp_env_init_thread();
        de_task_system_desc td;
        memset(&td, 0, sizeof td);
        td.worker_count = jobs - 1;
        td.background_worker_count = 1;
        td.allocator = run->a;
        if (de_task_system_create(&td, &run->tasks) != DE_OK)
            return cc_io_error(io, "cannot start the cook threads", NULL);
    }
    if (de_type_registry_create(run->a, &run->types) != DE_OK)
        return cc_io_error(io, "cannot create the type registry", NULL);
    /* A world registers the ECS built-in components (DeTransform, ...) in the registry, so scenes
     * and prefabs that use them cook. Nothing else uses it. */
    de_world_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.allocator = run->a;
    wd.types = run->types;
    if (de_world_create(&wd, &run->world) != DE_OK)
        return cc_io_error(io, "cannot create the ECS world", NULL);
    if (!DE_HANDLE_VALID(de_builtin_components(run->world).transform))
        return cc_io_error(io, "cannot register the ECS built-in components", NULL);
    /* The asset system holds the asset types (built-ins at create) and gives the scene importer
     * its component registry (DENG-215). It loads nothing. */
    de_asset_system_desc ad;
    memset(&ad, 0, sizeof ad);
    ad.allocator = run->a;
    ad.types = run->types;
    ad.io_threads = 1;
    if (de_asset_system_create(&ad, &run->assets) != DE_OK)
        return cc_io_error(io, "cannot create the asset system", NULL);
    if (run->o->game != NULL) {
        int rc = cc_load_game(run);
        if (rc >= 0)
            return rc;
    }
    if (run->hooks != NULL && run->hooks->register_types != NULL) {
        de_error_clear();
        if (run->hooks->register_types(run->hooks->user, run->assets, run->types) != DE_OK)
            return cc_io_error(io, "registering the project's types failed", NULL);
    }

    de_cooker_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.allocator = run->a;
    cd.tasks = run->tasks;
    cd.platform = run->o->platform;
    cd.cache_dir = run->o->no_cache ? NULL : CC_CACHE_VDIR;
    cd.verify_determinism = run->o->verify ? DE_COOK_VERIFY_ON : DE_COOK_VERIFY_DEFAULT;
    if (de_cooker_create(&cd, &run->cooker) != DE_OK)
        return cc_io_error(io, "cannot create the cooker", NULL);
    u32 count = 0;
    const char *const *names = de__asset_type_names(run->assets, &count);
    for (u32 i = 0; i < count; ++i) {
        const de_asset_type_desc *td =
            de__asset_type_get(run->assets, de__asset_type_find(run->assets, names[i]));
        if (td == NULL || de_cooker_register_type(run->cooker, td) != DE_OK)
            return cc_io_error(io, "the cooker refused the asset type", names[i]);
    }
    /* DENG-98, DENG-409: the cooker library's built-in importers (texture, model, shader) for
     * the engine types this asset system did not register. The tool has no renderer, so its asset
     * system has none of them unless the game or the hooks registered one, which is mirrored
     * above and wins. */
    u32 engine_count = 0;
    const char *const *engine = de_cooker_engine_type_names(&engine_count);
    for (u32 i = 0; i < engine_count; ++i) {
        de_asset_type_desc builtin;
        if (de_cooker_has_type(run->cooker, engine[i]) ||
            !de_cooker_builtin_importer(engine[i], &builtin))
            continue;
        if (de_cooker_register_type(run->cooker, &builtin) != DE_OK)
            return cc_io_error(io, "the cooker refused the asset type", engine[i]);
    }
    return -1;
}

/* ---- Validation: the --check code ----------------------------------------------------------- */

/* Runs `--check` on the manifest with the cooker's type names as extra known types. */
static int cc_check(cc_run *run) {
    const de_cook_io *io = run->io;
    u32 count = de_cooker_type_names(run->cooker, NULL, 0);
    const char **names = (const char **)calloc(count != 0 ? count : 1, sizeof *names);
    cc_text types;
    memset(&types, 0, sizeof types);
    if (names == NULL)
        return cc_io_error(io, "out of memory", NULL);
    (void)de_cooker_type_names(run->cooker, names, count);
    for (u32 i = 0; i < count; ++i) {
        if (i > 0)
            cc_text_add(&types, ",", 1);
        cc_text_add(&types, names[i], strlen(names[i]));
    }
    free((void *)names);

    const char *argv[3 + 2 * CC_MAX_INCLUDES];
    int argc = 0;
    argv[argc++] = run->o->manifest;
    if (types.len > 0) {
        argv[argc++] = "--types";
        argv[argc++] = types.p;
    }
    for (u32 i = 0; i < run->o->include_dir_count; ++i) {
        argv[argc++] = "-I";
        argv[argc++] = run->o->include_dirs[i];
    }
    cc_capture cap;
    memset(&cap, 0, sizeof cap);
    de_cook_io cio = {cc_capture_write, &cap};
    int rc = types.oom ? DE_COOK_EXIT_USAGE : de_cook_check_main(argc, argv, &cio);
    free(types.p);
    if (cap.out.oom || cap.err.oom)
        rc = DE_COOK_EXIT_USAGE;

    if (rc == DE_COOK_EXIT_OK) {
        /* Only the summary line: nothing to say. Warnings and notes are diagnostics. */
        u32 lines = 0;
        for (size_t i = 0; i < cap.out.len; ++i)
            lines += cap.out.p[i] == '\n';
        if (lines > 1)
            cc_write(io, DE_COOK_STDERR, cap.out.p, cap.out.len);
        cc_write(io, DE_COOK_STDERR, cap.err.p, cap.err.len);
    } else {
        /* Exactly what --check prints. */
        cc_write(io, DE_COOK_STDOUT, cap.out.p, cap.out.len);
        cc_write(io, DE_COOK_STDERR, cap.err.p, cap.err.len);
        if (rc == DE_COOK_EXIT_ERRORS)
            cc_puts(io, DE_COOK_STDERR, "de-cook: the manifest has errors; nothing was cooked\n");
    }
    free(cap.out.p);
    free(cap.err.p);
    return rc == DE_COOK_EXIT_OK ? -1 : rc;
}

/* ---- Reading the manifest ----------------------------------------------------------------- */

static int cc_mount_inputs(cc_run *run) {
    const de_cook_io *io = run->io;
    if (de_fs_mount(CC_ROOT, run->dir, DE_MOUNT_READ_ONLY, 0) != DE_OK)
        return cc_io_error(io, "cannot open the manifest directory", run->dir);
    run->root_mounted = true;
    for (u32 i = 0; i < run->o->include_dir_count; ++i) {
        i32 prio = (i32)(run->o->include_dir_count - i);
        if (de_fs_mount(CC_INC_ROOT, run->o->include_dirs[i], DE_MOUNT_READ_ONLY, prio) != DE_OK)
            return cc_io_error(io, "cannot open the include directory", run->o->include_dirs[i]);
        run->inc_mounted = i + 1;
    }
    if (!run->o->no_cache) {
        int n;
        if (run->o->cache != NULL)
            n = snprintf(run->cache_native, sizeof run->cache_native, "%s", run->o->cache);
        else
            n = snprintf(run->cache_native, sizeof run->cache_native, "%s%s.decache", run->dir,
                         cc_is_sep(run->dir[strlen(run->dir) - 1]) ? "" : "/");
        if (n < 0 || (size_t)n >= sizeof run->cache_native)
            return cc_usage_error(io, "cache path too long", NULL);
        if (cc_make_native_dir(run->cache_native) != DE_OK)
            return cc_io_error(io, "cannot create the cache directory", run->cache_native);
        if (de_fs_mount(CC_CACHE_ROOT, run->cache_native, DE_MOUNT_WRITABLE, 0) != DE_OK)
            return cc_io_error(io, "cannot open the cache directory", run->cache_native);
        run->cache_mounted = true;
    }
    return -1;
}

/* The de__manifest_read_fn: the file under cook:/, else the same path under cookinc:/. */
static de_result cc_read(void *user, const char *vpath, const de_allocator *a, void **data,
                         u64 *size) {
    cc_run *run = (cc_run *)user;
    de_result r = de_file_read_all(vpath, a, data, size);
    if (r != DE_ERR_NOT_FOUND || run->inc_mounted == 0 || strcmp(vpath, run->root) == 0 ||
        strncmp(vpath, CC_ROOT_PREFIX, sizeof CC_ROOT_PREFIX - 1) != 0)
        return r;
    char alt[DE_FS_PATH_MAX];
    int n = snprintf(alt, sizeof alt, "%s%s", CC_INC_PREFIX, vpath + sizeof CC_ROOT_PREFIX - 1);
    if (n < 0 || (size_t)n >= sizeof alt)
        return r;
    return de_file_read_all(alt, a, data, size) == DE_OK ? DE_OK : r;
}

static int cc_load_manifest(cc_run *run) {
    const de_cook_io *io = run->io;
    de__manifest_parse_desc pd = {
        .path = run->root, .allocator = run->a, .read = cc_read, .read_user = run};
    if (de__manifest_parse(&pd, &run->manifest) != DE_OK)
        return cc_io_error(io, "out of memory while parsing", run->o->manifest);
    de__resolve_desc rd = {.allocator = run->a};
    if (de__manifest_resolve(run->manifest, &rd, &run->resolved) != DE_OK)
        return cc_io_error(io, "out of memory while resolving", run->o->manifest);
    if (run->resolved->error_count != 0) {
        /* --check passed a moment ago: the files changed in between. */
        return cc_io_error(io, "the manifest changed while it was cooked", run->o->manifest);
    }
    /* The asset root: `asset_root` next to the root manifest. */
    char raw[DE_FS_PATH_MAX];
    int n = snprintf(raw, sizeof raw, "%s%s", CC_ROOT_PREFIX, run->manifest->asset_root);
    if (n < 0 || (size_t)n >= sizeof raw ||
        de_path_normalize(raw, run->asset_root, sizeof run->asset_root) != DE_OK)
        return cc_io_error(io,
                           "the asset_root is not a valid directory:", run->manifest->asset_root);
    return -1;
}

/* ---- Cooking ------------------------------------------------------------------------------ */

static void cc_cook_one(cc_run *run, u32 i) {
    cc_item *it = &run->items[i];
    de_error_clear();
    de_cook_input in;
    memset(&in, 0, sizeof in);
    in.asset = it->asset;
    in.asset_root = run->asset_root;
    it->rc = de_cooker_cook(run->cooker, &in, &it->res);
    if (it->rc != DE_OK) {
        const de_error *e = de_error_last();
        if (e != NULL && e->message[0] != '\0')
            snprintf(it->msg, sizeof it->msg, "%s", e->message);
        else
            snprintf(it->msg, sizeof it->msg, "asset \"%s\": %s", it->asset->id,
                     de_result_str(it->rc));
    }
    if (!run->o->quiet) {
        if (run->progress_lock_ok)
            de_mutex_lock(&run->progress_lock);
        u32 k = ++run->done;
        char line[CC_MSG_MAX];
        int n =
            snprintf(line, sizeof line, "[%u/%u] %s%s\n", (unsigned)k, (unsigned)run->item_count,
                     it->asset->id, it->rc == DE_OK ? "" : " FAILED");
        if (n > 0)
            cc_write(run->io, DE_COOK_STDERR, line,
                     (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
        if (run->progress_lock_ok)
            de_mutex_unlock(&run->progress_lock);
    }
}

static void cc_cook_range(u32 begin, u32 end, void *user) {
    for (u32 i = begin; i < end; ++i)
        cc_cook_one((cc_run *)user, i);
}

static int cc_cook_all(cc_run *run) {
    const de_cook_io *io = run->io;
    const de__manifest *m = run->manifest;
    run->item_count = m->asset_count;
    run->items = (cc_item *)calloc(m->asset_count != 0 ? m->asset_count : 1, sizeof *run->items);
    if (run->items == NULL)
        return cc_io_error(io, "out of memory", NULL);
    u32 missing_types = 0;
    for (u32 i = 0; i < m->asset_count; ++i) {
        const de__manifest_asset *a = &m->assets[i];
        run->items[i].asset = a;
        run->items[i].id = run->resolved->asset_ids[i];
        if (!de_cooker_has_type(run->cooker, a->type)) {
            cc_puts(io, DE_COOK_STDERR, "de-cook: error: asset \"");
            cc_put_message(run, DE_COOK_STDERR, a->id);
            cc_puts(io, DE_COOK_STDERR, "\": no importer for type \"");
            cc_put_message(run, DE_COOK_STDERR, a->type);
            cc_puts(io, DE_COOK_STDERR, "\" in this de-cook (the type is not registered)\n");
            missing_types++;
        } else if (a->locale_count != 0) {
            cc_puts(io, DE_COOK_STDERR, "de-cook: warning: asset \"");
            cc_put_message(run, DE_COOK_STDERR, a->id);
            cc_puts(io, DE_COOK_STDERR,
                    "\": locale overrides are not packed (a pack holds one variant per ID)\n");
        }
    }
    if (missing_types != 0)
        return DE_COOK_EXIT_ERRORS;

    de_mutex_init(&run->progress_lock);
    run->progress_lock_ok = true;
    if (run->tasks != NULL)
        de_task_parallel_for(run->tasks, run->item_count, 1, cc_cook_range, run);
    else
        cc_cook_range(0, run->item_count, run);

    u32 failed = 0;
    for (u32 i = 0; i < run->item_count; ++i) {
        if (run->items[i].rc == DE_OK)
            continue;
        cc_puts(io, DE_COOK_STDERR, "de-cook: error: ");
        cc_put_message(run, DE_COOK_STDERR, run->items[i].msg);
        cc_puts(io, DE_COOK_STDERR, "\n");
        failed++;
    }
    if (failed != 0) {
        cc_printf(io, DE_COOK_STDERR, "de-cook: %u of %u assets failed to cook; no pack written\n",
                  (unsigned)failed, (unsigned)run->item_count);
        return DE_COOK_EXIT_ERRORS;
    }
    return -1;
}

/* ---- Writing the pack ----------------------------------------------------------------------- */

static int cc_cmp_entry(const void *pa, const void *pb) {
    const cc_entry *a = (const cc_entry *)pa, *b = (const cc_entry *)pb;
    if (a->id != b->id)
        return a->id < b->id ? -1 : 1;
    if (a->item != b->item)
        return a->item < b->item ? -1 : 1;
    return a->sub < b->sub ? -1 : (a->sub > b->sub ? 1 : 0);
}

typedef struct cc_totals {
    u32 assets, subassets, bundles;
    u64 raw, stored, file;
    u8 build_id[16];
} cc_totals;

/* Adds every entry, sorted by ID (the writer sorts too; this keeps the add order a function of
 * the inputs), and every bundle. */
static de_result cc_fill_writer(cc_run *run, dpak_writer *w, cc_totals *t) {
    u32 count = 0;
    for (u32 i = 0; i < run->item_count; ++i)
        count += 1 + run->items[i].res.subasset_count;
    cc_entry *ents = (cc_entry *)calloc(count != 0 ? count : 1, sizeof *ents);
    if (ents == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory");
    u32 n = 0;
    for (u32 i = 0; i < run->item_count; ++i) {
        const cc_item *it = &run->items[i];
        ents[n++] = (cc_entry){it->id.hash, i, UINT32_MAX};
        for (u32 s = 0; s < it->res.subasset_count; ++s)
            ents[n++] = (cc_entry){it->res.subassets[s].id.hash, i, s};
    }
    qsort(ents, n, sizeof *ents, cc_cmp_entry);

    de_result r = DE_OK;
    u64 *deps = NULL;
    u32 deps_cap = 0;
    char name[DE_FS_PATH_MAX];
    for (u32 k = 0; k < n && r == DE_OK; ++k) {
        const cc_item *it = &run->items[ents[k].item];
        if (ents[k].sub == UINT32_MAX) {
            u32 dc = it->res.dependency_count;
            if (dc > deps_cap) {
                u64 *q = (u64 *)realloc(deps, dc * sizeof *deps);
                if (q == NULL) {
                    r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory");
                    break;
                }
                deps = q;
                deps_cap = dc;
            }
            for (u32 d = 0; d < dc; ++d)
                deps[d] = it->res.dependencies[d].hash;
            r = dpak_writer_add(w, ents[k].id, it->asset->type, it->res.data, it->res.size, deps,
                                dc, it->asset->id);
            t->assets++;
            t->raw += it->res.size;
        } else {
            const de_cook_subasset *sa = &it->res.subassets[ents[k].sub];
            int nn = snprintf(name, sizeof name, "%s#%s", it->asset->id, sa->name);
            if (nn < 0 || (size_t)nn >= sizeof name) {
                r = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "sub-asset name of \"%s\" too long",
                            it->asset->id);
                break;
            }
            r = dpak_writer_add(w, ents[k].id, sa->type, sa->data, sa->size, NULL, 0, name);
            t->subassets++;
            t->raw += sa->size;
        }
    }
    free(deps);
    free(ents);

    const de__resolved *res = run->resolved;
    for (u32 b = 0; b < res->bundle_count && r == DE_OK; ++b) {
        const de__resolved_bundle *rb = &res->bundles[b];
        const de__manifest_bundle *mb = rb->bundle;
        u64 *ids = (u64 *)calloc(rb->member_count != 0 ? rb->member_count : 1, sizeof *ids);
        if (ids == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory");
        for (u32 i = 0; i < rb->member_count; ++i)
            ids[i] = rb->members[i].hash;
        r = dpak_writer_add_bundle(w, mb->name, ids, rb->member_count,
                                   mb->depends_count != 0 ? mb->depends : NULL, mb->depends_count,
                                   mb->resident, cc_pack_priority(mb->priority));
        free(ids);
        t->bundles++;
    }
    return r;
}

static int cc_write_pack(cc_run *run, cc_totals *t) {
    const de_cook_io *io = run->io;
    const cc_options *o = run->o;
    dpak_writer_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.allocator = run->a;
    wd.platform = o->dpak_platform;
    if (o->has_build_id)
        memcpy(wd.build_id, o->build_id, sizeof wd.build_id);
    wd.use_dictionaries = !o->no_dictionaries;
    wd.include_names = !o->strip_names;
    dpak_writer *w = NULL;
    if (dpak_writer_create(&wd, &w) != DE_OK)
        return cc_io_error(io, "cannot create the pack writer", NULL);
    de_result r = cc_fill_writer(run, w, t);
    void *data = NULL;
    u64 size = 0;
    if (r == DE_OK)
        r = dpak_writer_finish_memory(w, run->a, &data, &size);
    dpak_writer_destroy(w);
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        cc_puts(io, DE_COOK_STDERR, "de-cook: error: ");
        cc_put_message(run, DE_COOK_STDERR,
                       e != NULL && e->message[0] != '\0' ? e->message : de_result_str(r));
        cc_puts(io, DE_COOK_STDERR, "\nde-cook: no pack written\n");
        return r == DE_ERR_OUT_OF_MEMORY ? DE_COOK_EXIT_USAGE : DE_COOK_EXIT_ERRORS;
    }

    /* The build ID: given, or XXH3-128 of the whole pack with the field still zero. The header's
     * content hash covers only the bytes after the header, so it stays valid. */
    dpak_header h;
    dpak_header_load((const u8 *)data, &h);
    if (!o->has_build_id) {
        XXH128_canonical_t c;
        XXH128_canonicalFromHash(&c, XXH3_128bits(data, (size_t)size));
        memcpy(h.build_id, c.digest, sizeof h.build_id);
        dpak_header_store((u8 *)data, &h);
    }
    memcpy(t->build_id, h.build_id, sizeof t->build_id);
    t->file = size;

    /* Read it back: the stored sizes for the summary, and a check of what is written. */
    dpak_reader *rd = NULL;
    if (dpak_open_memory(data, size, &(dpak_open_desc){.allocator = run->a, .verify = true}, &rd) !=
        DE_OK) {
        DE_FREE(run->a, data, size);
        return cc_io_error(io, "the pack just built does not read back", NULL);
    }
    for (u32 i = 0; i < dpak_entry_count(rd); ++i) {
        dpak_toc_entry e;
        dpak_entry(rd, i, &e);
        t->stored += e.stored_size;
    }
    dpak_close(rd);

    int rc = -1;
    char vpath[DE_FS_PATH_MAX];
    snprintf(vpath, sizeof vpath, "%s%s.dpak", CC_OUT_PREFIX, o->pack_name);
    if (cc_make_native_dir(o->out) != DE_OK) {
        rc = cc_io_error(io, "cannot create the output directory", o->out);
    } else if (de_fs_mount(CC_OUT_ROOT, o->out, DE_MOUNT_WRITABLE, 0) != DE_OK) {
        rc = cc_io_error(io, "cannot open the output directory", o->out);
    } else {
        run->out_mounted = true;
        if (de_file_write_all_atomic(vpath, data, size) != DE_OK)
            rc = cc_io_error(io, "cannot write the pack into", o->out);
    }
    DE_FREE(run->a, data, size);
    return rc;
}

#endif /* DE_HAS_COOKER */

/* ---- The command -------------------------------------------------------------------------- */

static int cc_cook(cc_run *run) {
#if !DE_HAS_COOKER
    return cc_io_error(run->io,
                       "this de-cook was built without the runtime importers (a Release or "
                       "Shipping tree); build de-cook from a Debug or Dev tree",
                       NULL);
#else
    u64 t0 = de_time_now_ns();
    de_error_clear();
    int rc = cc_split_manifest(run);
    if (rc < 0)
        rc = cc_setup_runtime(run);
    if (rc < 0)
        rc = cc_check(run);
    if (rc < 0)
        rc = cc_mount_inputs(run);
    if (rc < 0)
        rc = cc_load_manifest(run);
    if (rc < 0)
        rc = cc_cook_all(run);
    cc_totals t;
    memset(&t, 0, sizeof t);
    if (rc < 0)
        rc = cc_write_pack(run, &t);
    if (rc >= 0)
        return rc;

    /* Summary (stdout). */
    const de_cook_io *io = run->io;
    const cc_options *o = run->o;
    de_cooker_cache_stats cs;
    de_cooker_get_cache_stats(run->cooker, &cs);
    u64 ms = (de_time_now_ns() - t0) / 1000000u;
    cc_puts(io, DE_COOK_STDOUT, "de-cook cook: ");
    cc_puts(io, DE_COOK_STDOUT, o->out);
    if (!cc_is_sep(o->out[strlen(o->out) - 1]))
        cc_puts(io, DE_COOK_STDOUT, "/");
    cc_printf(io, DE_COOK_STDOUT, "%s.dpak (%s)\n", o->pack_name, o->platform_arg);
    cc_printf(io, DE_COOK_STDOUT, "  assets cooked: %u (+%u sub-assets), bundles: %u\n",
              (unsigned)t.assets, (unsigned)t.subassets, (unsigned)t.bundles);
    if (o->no_cache)
        cc_puts(io, DE_COOK_STDOUT, "  cache: off\n");
    else
        cc_printf(io, DE_COOK_STDOUT, "  cache hits: %llu, misses: %llu, stores: %llu\n",
                  (unsigned long long)cs.hits, (unsigned long long)cs.misses,
                  (unsigned long long)cs.stores);
    if (o->game != NULL)
        cc_printf(io, DE_COOK_STDOUT, "  game: %s (%u asset types added)\n", run->game.table.name,
                  (unsigned)run->game_asset_types);
    cc_printf(io, DE_COOK_STDOUT, "  bytes raw: %llu, stored: %llu, pack: %llu\n",
              (unsigned long long)t.raw, (unsigned long long)t.stored, (unsigned long long)t.file);
    cc_puts(io, DE_COOK_STDOUT, "  build id: ");
    for (u32 i = 0; i < 16; ++i)
        cc_printf(io, DE_COOK_STDOUT, "%02x", (unsigned)t.build_id[i]);
    cc_printf(io, DE_COOK_STDOUT, "\n  time: %llu ms\n", (unsigned long long)ms);
    return DE_COOK_EXIT_OK;
#endif
}

int de_cook_cook_run(int argc, const char *const *argv, const de_cook_io *io,
                     const de_cook_hooks *hooks) {
    cc_options o;
    int rc = cc_parse_args(argc, argv, &o, io);
    if (rc >= 0) {
        if (rc == DE_COOK_EXIT_OK && o.help)
            de_cook_cook_usage(io, DE_COOK_STDOUT);
        return rc;
    }
    cc_run run;
    memset(&run, 0, sizeof run);
    run.a = de_heap_allocator();
    run.o = &o;
    run.io = io;
    run.hooks = hooks;
    rc = cc_cook(&run);
    cc_cleanup(&run);
    return rc;
}

int de_cook_cook_main(int argc, const char *const *argv, const de_cook_io *io) {
    return de_cook_cook_run(argc, argv, io, NULL);
}
