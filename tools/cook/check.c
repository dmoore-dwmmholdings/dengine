/*
 * check.c: `de-cook --check` (DENG-145). See check.h for the command line and the output format.
 *
 * Files: the directory of the manifest argument is mounted as the virtual root `cook:/` and every
 * `-I` directory under `cookinc:/` (first one searched first), so the manifest code only ever
 * sees virtual paths (SPEC §6). An include that is missing under `cook:/` is read from
 * `cookinc:/` at the same relative path; the output names the file it was read from. Both mounts
 * are removed before de_cook_check_main returns.
 *
 * Order: issues are sorted by (virtual file path, line, the order they were found), so the output
 * is a function of the manifest bytes only.
 */
#include "check.h"

#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset/manifest_internal.h"
#include "asset/resolve_internal.h"

#define CK_ROOT        "cook"
#define CK_ROOT_PREFIX "cook:/"
#define CK_INC_ROOT    "cookinc"
#define CK_INC_PREFIX  "cookinc:/"
/** Most `-I` directories (the VFS has DE_FS_MAX_MOUNTS slots for the whole process). */
#define CK_MAX_INCLUDE_DIRS 16
/** Most `--types` options. */
#define CK_MAX_TYPE_ARGS 32

/* ---- Output ------------------------------------------------------------------------------- */

static void ck_write(const de_cook_io *io, de_cook_stream s, const char *text, size_t len) {
    if (len > 0)
        io->write(io->user, s, text, len);
}

static void ck_puts(const de_cook_io *io, de_cook_stream s, const char *text) {
    ck_write(io, s, text, strlen(text));
}

/* Short formatted pieces only (numbers, fixed words); user strings go through ck_puts. */
static void ck_printf(const de_cook_io *io, de_cook_stream s, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    ck_write(io, s, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

/* Writes `len` bytes of `text` with control characters replaced by spaces, so every issue stays
 * on one line. */
static void ck_put_safe_n(const de_cook_io *io, de_cook_stream s, const char *text, size_t len) {
    const char *run = text;
    for (const char *p = text; p < text + len; ++p) {
        if ((unsigned char)*p < 0x20 || *p == 0x7f) {
            ck_write(io, s, run, (size_t)(p - run));
            ck_write(io, s, " ", 1);
            run = p + 1;
        }
    }
    ck_write(io, s, run, (size_t)(text + len - run));
}

static void ck_put_line_safe(const de_cook_io *io, de_cook_stream s, const char *text) {
    ck_put_safe_n(io, s, text, strlen(text));
}

void de_cook_check_usage(const de_cook_io *io, de_cook_stream s) {
    ck_puts(io, s,
            "usage: de-cook --check <assets.toml> [options]\n"
            "\n"
            "Checks an asset manifest and the files it includes: TOML syntax, asset IDs, bundle\n"
            "globs and dependencies, asset types and source files.\n"
            "\n"
            "options:\n"
            "  --types a,b,c          also accept these asset types (repeatable)\n"
            "  --no-sources           do not check that source files exist\n"
            "  --quiet                print errors only (no warnings, notes or summary)\n"
            "  -I, --include-dir <dir>\n"
            "                         read includes that are missing next to the including\n"
            "                         file from <dir> (repeatable, searched in order)\n"
            "  -h, --help             print this text\n"
            "\n"
            "Each issue is printed as\n"
            "  <file>:<line>: <error|warning|note>: <message> [<key path>]\n"
            "sorted by file, then line, followed by a summary line.\n"
            "\n"
            "exit codes: 0 no errors (warnings allowed), 1 errors found, 2 usage or I/O error\n");
}

static int ck_usage_error(const de_cook_io *io, const char *what, const char *arg) {
    ck_puts(io, DE_COOK_STDERR, "de-cook: ");
    ck_puts(io, DE_COOK_STDERR, what);
    if (arg != NULL) {
        ck_puts(io, DE_COOK_STDERR, " '");
        ck_puts(io, DE_COOK_STDERR, arg);
        ck_puts(io, DE_COOK_STDERR, "'");
    }
    ck_puts(io, DE_COOK_STDERR, "\n\n");
    de_cook_check_usage(io, DE_COOK_STDERR);
    return DE_COOK_EXIT_USAGE;
}

static int ck_io_error(const de_cook_io *io, const char *what, const char *arg) {
    ck_puts(io, DE_COOK_STDERR, "de-cook: ");
    ck_puts(io, DE_COOK_STDERR, what);
    if (arg != NULL) {
        ck_puts(io, DE_COOK_STDERR, " '");
        ck_puts(io, DE_COOK_STDERR, arg);
        ck_puts(io, DE_COOK_STDERR, "'");
    }
    ck_puts(io, DE_COOK_STDERR, "\n");
    return DE_COOK_EXIT_USAGE;
}

/* ---- Arguments ---------------------------------------------------------------------------- */

typedef struct ck_options {
    const char *manifest;
    const char *type_args[CK_MAX_TYPE_ARGS];
    u32 type_arg_count;
    const char *include_dirs[CK_MAX_INCLUDE_DIRS];
    u32 include_dir_count;
    bool no_sources;
    bool quiet;
    bool help;
} ck_options;

/* `--name value` or `--name=value`. Returns the value, or NULL if `arg` is not this option.
 * `*missing` is set when the option is present but has no value. */
static const char *ck_value_opt(int argc, const char *const *argv, int *i, const char *name,
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

static int ck_parse_args(int argc, const char *const *argv, ck_options *o, const de_cook_io *io) {
    memset(o, 0, sizeof *o);
    for (int i = 0; i < argc; ++i) {
        const char *arg = argv[i];
        if (arg == NULL)
            return ck_usage_error(io, "missing argument", NULL);
        bool missing = false;
        const char *v;
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            o->help = true;
        } else if (strcmp(arg, "--no-sources") == 0) {
            o->no_sources = true;
        } else if (strcmp(arg, "--quiet") == 0) {
            o->quiet = true;
        } else if ((v = ck_value_opt(argc, argv, &i, "--types", &missing)) != NULL || missing) {
            if (missing)
                return ck_usage_error(io, "missing value for", "--types");
            if (o->type_arg_count == CK_MAX_TYPE_ARGS)
                return ck_usage_error(io, "too many --types options", NULL);
            o->type_args[o->type_arg_count++] = v;
        } else if ((v = ck_value_opt(argc, argv, &i, "--include-dir", &missing)) != NULL ||
                   missing || (v = ck_value_opt(argc, argv, &i, "-I", &missing)) != NULL ||
                   missing || (strncmp(arg, "-I", 2) == 0 && arg[2] != '\0')) {
            if (missing)
                return ck_usage_error(io, "missing value for", arg);
            if (v == NULL)
                v = arg + 2; /* -I<dir> */
            if (v[0] == '\0')
                return ck_usage_error(io, "empty include directory", NULL);
            if (o->include_dir_count == CK_MAX_INCLUDE_DIRS)
                return ck_usage_error(io, "too many include directories", NULL);
            o->include_dirs[o->include_dir_count++] = v;
        } else if (arg[0] == '-') {
            return ck_usage_error(io, "unknown option", arg);
        } else if (o->manifest != NULL) {
            return ck_usage_error(io, "more than one manifest given:", arg);
        } else {
            o->manifest = arg;
        }
    }
    if (o->help)
        return DE_COOK_EXIT_OK;
    if (o->manifest == NULL || o->manifest[0] == '\0')
        return ck_usage_error(io, "no manifest given", NULL);
    return -1; /* go on */
}

/* ---- State of one run --------------------------------------------------------------------- */

/* An include read from an -I directory: its virtual path under cook:/ and the native path of the
 * file that was actually read. One allocation; the strings follow the struct. */
typedef struct ck_redirect {
    struct ck_redirect *next;
    size_t bytes;
    const char *vpath;
    const char *native;
} ck_redirect;

typedef struct ck_run {
    const de_allocator *a;
    const ck_options *o;
    const de_cook_io *io;
    char dir[DE_FS_PATH_MAX];    /* native directory mounted as cook:/ */
    char prefix[DE_FS_PATH_MAX]; /* how the output names that directory: the argument's own text */
    char root[DE_FS_PATH_MAX];   /* virtual path of the root manifest */
    bool root_mounted;
    u32 inc_mounted;
    ck_redirect *redirects;
    char *type_buf; /* copies of the --types values, split in place */
    size_t type_buf_size;
    const char **types; /* known types + extras */
    u32 type_count, type_cap;
} ck_run;

static bool ck_is_sep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

/* Splits the manifest argument into the directory to mount, the prefix the output uses for it, and
 * the root virtual path. */
static int ck_split_manifest(ck_run *run) {
    const char *arg = run->o->manifest;
    size_t len = strlen(arg);
    size_t sep = len; /* index of the last separator, len = none */
    for (size_t i = 0; i < len; ++i)
        if (ck_is_sep(arg[i]))
            sep = i;
    const char *name = sep == len ? arg : arg + sep + 1;
    if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return ck_usage_error(run->io, "the manifest must name a file:", arg);
    if (len + 1 > sizeof run->dir)
        return ck_usage_error(run->io, "manifest path too long:", arg);
    if (sep == len) {
        memcpy(run->dir, ".", 2);
        run->prefix[0] = '\0';
    } else {
        /* Keep the separator for "/" and for "C:\"; drop it otherwise. */
        size_t dl = sep;
        if (sep == 0 || (sep == 2 && arg[1] == ':'))
            dl = sep + 1;
        memcpy(run->dir, arg, dl);
        run->dir[dl] = '\0';
        memcpy(run->prefix, arg, sep + 1);
        run->prefix[sep + 1] = '\0';
    }
    char raw[DE_FS_PATH_MAX];
    int n = snprintf(raw, sizeof raw, "%s%s", CK_ROOT_PREFIX, name);
    if (n < 0 || (size_t)n >= sizeof raw ||
        de_path_normalize(raw, run->root, sizeof run->root) != DE_OK)
        return ck_io_error(run->io, "cannot use this file name as a manifest:", name);
    return -1;
}

static int ck_mount(ck_run *run) {
    if (de_fs_mount(CK_ROOT, run->dir, DE_MOUNT_READ_ONLY, 0) != DE_OK)
        return ck_io_error(run->io, "cannot open the manifest directory", run->dir);
    run->root_mounted = true;
    for (u32 i = 0; i < run->o->include_dir_count; ++i) {
        /* Higher priority is searched first, so the first -I gets the highest. */
        i32 prio = (i32)(run->o->include_dir_count - i);
        if (de_fs_mount(CK_INC_ROOT, run->o->include_dirs[i], DE_MOUNT_READ_ONLY, prio) != DE_OK)
            return ck_io_error(run->io, "cannot open the include directory",
                               run->o->include_dirs[i]);
        run->inc_mounted = i + 1;
    }
    return -1;
}

static void ck_cleanup(ck_run *run) {
    for (u32 i = 0; i < run->inc_mounted; ++i)
        (void)de_fs_unmount(CK_INC_ROOT, run->o->include_dirs[i]);
    if (run->root_mounted)
        (void)de_fs_unmount(CK_ROOT, run->dir);
    while (run->redirects != NULL) {
        ck_redirect *r = run->redirects;
        run->redirects = r->next;
        DE_FREE(run->a, r, r->bytes);
    }
    if (run->types != NULL)
        DE_FREE(run->a, (void *)run->types, run->type_cap * sizeof *run->types);
    if (run->type_buf != NULL)
        DE_FREE(run->a, run->type_buf, run->type_buf_size);
}

/* Known types: the static table, then every name of every --types value. */
static int ck_build_types(ck_run *run) {
    size_t bytes = 0;
    u32 cap = de_cook_known_type_count;
    for (u32 i = 0; i < run->o->type_arg_count; ++i) {
        const char *v = run->o->type_args[i];
        bytes += strlen(v) + 1;
        cap += 1;
        for (const char *p = v; *p != '\0'; ++p)
            cap += *p == ',';
    }
    run->types = (const char **)DE_ALLOC(run->a, cap * sizeof *run->types, DE_ALIGNOF(const char *),
                                         DE_MEM_GENERAL);
    if (run->types == NULL)
        return ck_io_error(run->io, "out of memory", NULL);
    run->type_cap = cap;
    for (u32 i = 0; i < de_cook_known_type_count; ++i)
        run->types[run->type_count++] = de_cook_known_types[i];
    if (bytes == 0)
        return -1;
    run->type_buf = (char *)DE_ALLOC(run->a, bytes, 1, DE_MEM_GENERAL);
    if (run->type_buf == NULL)
        return ck_io_error(run->io, "out of memory", NULL);
    run->type_buf_size = bytes;
    char *w = run->type_buf;
    for (u32 i = 0; i < run->o->type_arg_count; ++i) {
        const char *v = run->o->type_args[i];
        size_t vl = strlen(v);
        memcpy(w, v, vl + 1);
        char *item = w;
        for (;;) {
            char *end = item;
            while (*end != '\0' && *end != ',')
                ++end;
            bool last = *end == '\0';
            *end = '\0';
            /* Trim spaces, so "--types 'a, b'" works. */
            while (*item == ' ')
                ++item;
            char *tail = end;
            while (tail > item && tail[-1] == ' ')
                *--tail = '\0';
            if (item[0] == '\0')
                return ck_usage_error(run->io, "empty type name in --types", v);
            run->types[run->type_count++] = item;
            if (last)
                break;
            item = end + 1;
        }
        w += vl + 1;
    }
    return -1;
}

/* ---- Reading manifests -------------------------------------------------------------------- */

static void ck_add_redirect(ck_run *run, const char *vpath, const char *alt) {
    char native[DE_FS_PATH_MAX];
    if (de_fs_resolve(alt, native, sizeof native) != DE_OK) {
        /* Name it by its virtual path then; the read itself worked. */
        snprintf(native, sizeof native, "%s", alt);
    }
    size_t vl = strlen(vpath) + 1, nl = strlen(native) + 1;
    size_t bytes = sizeof(ck_redirect) + vl + nl;
    ck_redirect *r =
        (ck_redirect *)DE_ALLOC(run->a, bytes, DE_ALIGNOF(ck_redirect), DE_MEM_GENERAL);
    if (r == NULL)
        return; /* the output then names the file under cook:/; the check is unaffected */
    char *s = (char *)(r + 1);
    memcpy(s, vpath, vl);
    memcpy(s + vl, native, nl);
    r->bytes = bytes;
    r->vpath = s;
    r->native = s + vl;
    r->next = run->redirects;
    run->redirects = r;
}

/* The de__manifest_read_fn: the file under cook:/, else the same relative path under cookinc:/. */
static de_result ck_read(void *user, const char *vpath, const de_allocator *a, void **data,
                         u64 *size) {
    ck_run *run = (ck_run *)user;
    de_result r = de_file_read_all(vpath, a, data, size);
    if (r != DE_ERR_NOT_FOUND || run->inc_mounted == 0 || strcmp(vpath, run->root) == 0 ||
        strncmp(vpath, CK_ROOT_PREFIX, sizeof CK_ROOT_PREFIX - 1) != 0)
        return r;
    char alt[DE_FS_PATH_MAX];
    int n = snprintf(alt, sizeof alt, "%s%s", CK_INC_PREFIX, vpath + sizeof CK_ROOT_PREFIX - 1);
    if (n < 0 || (size_t)n >= sizeof alt)
        return r;
    if (de_file_read_all(alt, a, data, size) != DE_OK)
        return r; /* report the original "not found" */
    ck_add_redirect(run, vpath, alt);
    return DE_OK;
}

/* ---- Printing issues ---------------------------------------------------------------------- */

typedef struct ck_sorted {
    const de__manifest_issue *is;
    u32 index;
} ck_sorted;

static int ck_cmp_str(const char *a, const char *b) {
    return strcmp(a != NULL ? a : "", b != NULL ? b : "");
}

static int ck_cmp_issue(const void *pa, const void *pb) {
    const ck_sorted *a = (const ck_sorted *)pa, *b = (const ck_sorted *)pb;
    int c = ck_cmp_str(a->is->file, b->is->file);
    if (c != 0)
        return c;
    if (a->is->line != b->is->line)
        return a->is->line < b->is->line ? -1 : 1;
    return a->index < b->index ? -1 : (a->index > b->index ? 1 : 0);
}

static void ck_put_file(const ck_run *run, const char *file) {
    const de_cook_io *io = run->io;
    if (file == NULL || file[0] == '\0') {
        ck_put_line_safe(io, DE_COOK_STDOUT, run->o->manifest);
        return;
    }
    for (const ck_redirect *r = run->redirects; r != NULL; r = r->next) {
        if (strcmp(r->vpath, file) == 0) {
            ck_put_line_safe(io, DE_COOK_STDOUT, r->native);
            return;
        }
    }
    if (strncmp(file, CK_ROOT_PREFIX, sizeof CK_ROOT_PREFIX - 1) == 0) {
        ck_put_line_safe(io, DE_COOK_STDOUT, run->prefix);
        ck_put_line_safe(io, DE_COOK_STDOUT, file + sizeof CK_ROOT_PREFIX - 1);
        return;
    }
    ck_put_line_safe(io, DE_COOK_STDOUT, file);
}

static bool ck_is_root_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/* A message names files by virtual path ("source file cook:/assets/x.png does not exist"). Print
 * those the way the file names of the issue lines are printed: `cook:/` becomes the manifest
 * argument's directory. */
static void ck_put_message(const ck_run *run, const char *msg) {
    static const char root[] = CK_ROOT_PREFIX;
    const size_t rl = sizeof root - 1;
    const char *run_start = msg;
    for (const char *p = msg; *p != '\0'; ++p) {
        if (strncmp(p, root, rl) == 0 && (p == msg || !ck_is_root_char(p[-1]))) {
            ck_put_safe_n(run->io, DE_COOK_STDOUT, run_start, (size_t)(p - run_start));
            ck_put_line_safe(run->io, DE_COOK_STDOUT, run->prefix);
            p += rl - 1;
            run_start = p + 1;
        }
    }
    ck_put_line_safe(run->io, DE_COOK_STDOUT, run_start);
}

static const char *ck_severity_name(de_asset_issue_severity s) {
    switch (s) {
    case DE_ASSET_ISSUE_ERROR:
        return "error";
    case DE_ASSET_ISSUE_WARNING:
        return "warning";
    case DE_ASSET_ISSUE_INFO:
    default:
        return "note";
    }
}

static int ck_print(ck_run *run, const de__resolved *r) {
    const de_cook_io *io = run->io;
    ck_sorted *list = NULL;
    size_t list_bytes = (size_t)r->issue_count * sizeof *list;
    if (r->issue_count > 0) {
        list = (ck_sorted *)DE_ALLOC(run->a, list_bytes, DE_ALIGNOF(ck_sorted), DE_MEM_GENERAL);
        if (list == NULL)
            return ck_io_error(io, "out of memory", NULL);
        for (u32 i = 0; i < r->issue_count; ++i) {
            list[i].is = &r->issues[i];
            list[i].index = i;
        }
        /* The index makes the order total, so qsort's instability cannot show. */
        qsort(list, r->issue_count, sizeof *list, ck_cmp_issue);
    }
    for (u32 i = 0; i < r->issue_count; ++i) {
        const de__manifest_issue *is = list[i].is;
        if (run->o->quiet && is->severity != DE_ASSET_ISSUE_ERROR)
            continue;
        ck_put_file(run, is->file);
        ck_printf(io, DE_COOK_STDOUT, ":%u: %s: ", (unsigned)is->line,
                  ck_severity_name(is->severity));
        ck_put_message(run, is->message != NULL ? is->message : "");
        if (is->key_path != NULL && is->key_path[0] != '\0') {
            ck_puts(io, DE_COOK_STDOUT, " [");
            ck_put_line_safe(io, DE_COOK_STDOUT, is->key_path);
            ck_puts(io, DE_COOK_STDOUT, "]");
        }
        ck_puts(io, DE_COOK_STDOUT, "\n");
    }
    if (list != NULL)
        DE_FREE(run->a, list, list_bytes);
    if (!run->o->quiet) {
        ck_printf(io, DE_COOK_STDOUT, "de-cook --check: %u error%s, %u warning%s, %u note%s\n",
                  (unsigned)r->error_count, r->error_count == 1 ? "" : "s",
                  (unsigned)r->warning_count, r->warning_count == 1 ? "" : "s",
                  (unsigned)r->info_count, r->info_count == 1 ? "" : "s");
    }
    return r->error_count > 0 ? DE_COOK_EXIT_ERRORS : DE_COOK_EXIT_OK;
}

/* ---- The command -------------------------------------------------------------------------- */

/* True if the root manifest itself could not be read: the parser reports that as an issue on the
 * root file with the read's error code (any code but DE_ERR_PARSE) and no key path. */
static const de__manifest_issue *ck_root_read_error(const ck_run *run, const de__manifest *m) {
    for (u32 i = 0; i < m->issue_count; ++i) {
        const de__manifest_issue *is = &m->issues[i];
        if (is->severity == DE_ASSET_ISSUE_ERROR && is->code != DE_ERR_PARSE && is->file != NULL &&
            strcmp(is->file, run->root) == 0 && (is->key_path == NULL || is->key_path[0] == '\0'))
            return is;
    }
    return NULL;
}

static int ck_check(ck_run *run) {
    int rc = ck_split_manifest(run);
    if (rc >= 0)
        return rc;
    rc = ck_build_types(run);
    if (rc >= 0)
        return rc;
    rc = ck_mount(run);
    if (rc >= 0)
        return rc;
    if (!de_path_exists(run->root))
        return ck_io_error(run->io, "cannot read the manifest", run->o->manifest);

    de__manifest *m = NULL;
    de__manifest_parse_desc pd = {
        .path = run->root, .allocator = run->a, .read = ck_read, .read_user = run};
    if (de__manifest_parse(&pd, &m) != DE_OK)
        return ck_io_error(run->io, "out of memory while parsing", run->o->manifest);
    const de__manifest_issue *rd = ck_root_read_error(run, m);
    if (rd != NULL) {
        ck_io_error(run->io, "cannot read the manifest", run->o->manifest);
        de__manifest_destroy(m);
        return DE_COOK_EXIT_USAGE;
    }

    de__resolved *r = NULL;
    de__resolve_desc rdsc = {.allocator = run->a};
    if (de__manifest_resolve(m, &rdsc, &r) != DE_OK) {
        de__manifest_destroy(m);
        return ck_io_error(run->io, "out of memory while resolving", run->o->manifest);
    }
    de__validate_desc vd = {
        .known_types = run->types,
        .known_type_count = run->type_count,
        .skip_sources = run->o->no_sources,
    };
    if (de__manifest_validate(r, &vd) != DE_OK) {
        de__resolved_destroy(r);
        de__manifest_destroy(m);
        return ck_io_error(run->io, "out of memory while validating", run->o->manifest);
    }
    rc = ck_print(run, r);
    de__resolved_destroy(r);
    de__manifest_destroy(m);
    return rc;
}

int de_cook_check_main(int argc, const char *const *argv, const de_cook_io *io) {
    ck_options o;
    int rc = ck_parse_args(argc, argv, &o, io);
    if (rc >= 0) {
        if (rc == DE_COOK_EXIT_OK && o.help)
            de_cook_check_usage(io, DE_COOK_STDOUT);
        return rc;
    }
    ck_run run;
    memset(&run, 0, sizeof run);
    run.a = de_heap_allocator();
    run.o = &o;
    run.io = io;
    rc = ck_check(&run);
    ck_cleanup(&run);
    return rc;
}
