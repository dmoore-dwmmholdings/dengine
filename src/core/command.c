/* command.c: the console command registry (SPEC §18.6, DENG-96). Public API: the console
 * commands section of include/dengine/de_debug.h; internals: core/command_internal.h.
 *
 * One process-wide registry, created on first use like the cvar registry (a de_mutex has no
 * static initialiser, so the first caller creates it while others spin). Entries live in a
 * de_array from the default heap, in registration order; lookups are linear and ignore ASCII
 * case (the registry holds tens of commands and exec is not a hot path). A command function is
 * copied out under the lock and called without it, so commands may register, unregister and
 * execute other commands.
 *
 * Output (help, cvars, echo, a cvar's value) goes to the log, category `console`, Info. */
#include "core/command_internal.h"

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "core/de_array.h"
#include "core/de_compiler.h"
#include "platform/de_atomic.h"

#include <stdio.h>
#include <string.h>

typedef struct cmd_entry {
    char name[DE_COMMAND_NAME_MAX + 1];
    char *help; /* heap copy, or NULL */
    size_t help_size;
    de_command_fn fn;
    void *user;
} cmd_entry;

enum { STATE_UNINIT = 0, STATE_INITIALIZING = 1, STATE_READY = 2 };

static struct {
    de_atomic_u32 state;
    de_mutex lock;
    const de_allocator *a;
    de_array entries; /* cmd_entry, registration order */
    de__command_exec_hook_fn hook;
    void *hook_user;
    de_log_category log;
} g_cmd;

/* Nesting of de_command_exec on this thread: 0 outside, 1 in a top-level line, more inside an
 * `exec` file. */
static DE_THREAD_LOCAL int g_exec_depth;

/* ---- Helpers ----------------------------------------------------------------------------- */

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool name_eq_nocase(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; ++a, ++b)
        if (ascii_lower(*a) != ascii_lower(*b))
            return false;
    return *a == *b;
}

static bool starts_with_nocase(const char *s, const char *prefix) {
    for (; *prefix != '\0'; ++s, ++prefix)
        if (*s == '\0' || ascii_lower(*s) != ascii_lower(*prefix))
            return false;
    return true;
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool name_valid(const char *name) {
    size_t n = 0;
    if (name == NULL || name[0] == '\0' || name[0] == '#')
        return false;
    for (; name[n] != '\0'; ++n) {
        unsigned char c = (unsigned char)name[n];
        if (c <= 0x20u || c == 0x7Fu || c == '"' || c == '\\')
            return false;
    }
    return n <= DE_COMMAND_NAME_MAX;
}

/* ---- Built-in commands ------------------------------------------------------------------- */

static de_result cmd_help(int argc, const char *const *argv, void *user);
static de_result cmd_cvars(int argc, const char *const *argv, void *user);
static de_result cmd_echo(int argc, const char *const *argv, void *user);
static de_result cmd_exec(int argc, const char *const *argv, void *user);

typedef struct builtin {
    const char *name, *help;
    de_command_fn fn;
} builtin;

static const builtin k_builtins[] = {
    {"help", "help [prefix]: list the commands (whose names start with prefix)", cmd_help},
    {"cvars", "cvars [prefix]: list the cvars and their values (names starting with prefix)",
     cmd_cvars},
    {"echo", "echo <words>: write the words to the log", cmd_echo},
    {"exec", "exec <vpath>: run a file of commands, one per line (# starts a comment)", cmd_exec},
};

/* Adds an entry; the caller holds the lock (or is creating the registry). */
static de_result add_locked(const char *name, const char *help, de_command_fn fn, void *user) {
    for (u32 i = 0; i < g_cmd.entries.count; ++i)
        if (name_eq_nocase(DE_ARRAY_AT(&g_cmd.entries, cmd_entry, i).name, name))
            return DE_FAIL(DE_ERR_ALREADY_EXISTS, "command '%s' is already registered", name);
    cmd_entry e;
    memset(&e, 0, sizeof e);
    memcpy(e.name, name, strlen(name) + 1);
    if (help != NULL && help[0] != '\0') {
        e.help_size = strlen(help) + 1;
        e.help = (char *)DE_ALLOC(g_cmd.a, e.help_size, 1, DE_MEM_GENERAL);
        if (e.help == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "command '%s': help text (%zu bytes)", name,
                           e.help_size);
        memcpy(e.help, help, e.help_size);
    }
    e.fn = fn;
    e.user = user;
    de_result r = de_array_push(&g_cmd.entries, &e);
    if (r != DE_OK) {
        if (e.help != NULL)
            DE_FREE(g_cmd.a, e.help, e.help_size);
        return DE_FAIL_WRAP(r, "command '%s'", name);
    }
    return DE_OK;
}

static void add_builtins_locked(void) {
    for (size_t i = 0; i < sizeof k_builtins / sizeof k_builtins[0]; ++i)
        if (add_locked(k_builtins[i].name, k_builtins[i].help, k_builtins[i].fn, NULL) != DE_OK)
            de_error_clear(); /* out of memory: that built-in is missing, the rest still work */
}

/* ---- Lifetime ---------------------------------------------------------------------------- */

static void registry_create(void) {
    de_mutex_init(&g_cmd.lock);
    g_cmd.a = de_heap_allocator();
    de_array_init(&g_cmd.entries, g_cmd.a, (u32)sizeof(cmd_entry), (u32)DE_ALIGNOF(cmd_entry),
                  DE_MEM_GENERAL);
    g_cmd.log = de_log_register_category("console", DE_LOG_INFO);
    add_builtins_locked();
}

static void registry_init_once(void) {
    if (de_atomic_load_u32(&g_cmd.state, DE_ATOMIC_ACQUIRE) == STATE_READY)
        return;
    u32 expected = STATE_UNINIT;
    if (de_atomic_cas_u32(&g_cmd.state, &expected, STATE_INITIALIZING, DE_ATOMIC_ACQ_REL,
                          DE_ATOMIC_ACQUIRE)) {
        registry_create();
        de_atomic_store_u32(&g_cmd.state, STATE_READY, DE_ATOMIC_RELEASE);
        return;
    }
    while (de_atomic_load_u32(&g_cmd.state, DE_ATOMIC_ACQUIRE) != STATE_READY)
        de_cpu_pause();
}

static void lock(void) {
    registry_init_once();
    de_mutex_lock(&g_cmd.lock);
}

static void unlock(void) {
    de_mutex_unlock(&g_cmd.lock);
}

/* ---- Registration ------------------------------------------------------------------------ */

de_result de_command_register(const char *name, const char *help, de_command_fn fn, void *user) {
    if (fn == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_command_register: NULL function for '%s'",
                       name != NULL ? name : "(null)");
    if (!name_valid(name))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_command_register: bad name '%s' (1 to %d bytes; no spaces, quotes, "
                       "backslashes or control characters; not starting with #)",
                       name != NULL ? name : "(null)", DE_COMMAND_NAME_MAX);
    lock();
    de_result r = add_locked(name, help, fn, user);
    unlock();
    return r;
}

de_result de_command_unregister(const char *name) {
    if (name == NULL)
        return DE_FAIL(DE_ERR_NOT_FOUND, "de_command_unregister: NULL name");
    lock();
    for (u32 i = 0; i < g_cmd.entries.count; ++i) {
        cmd_entry *e = &DE_ARRAY_AT(&g_cmd.entries, cmd_entry, i);
        if (!name_eq_nocase(e->name, name))
            continue;
        if (e->help != NULL)
            DE_FREE(g_cmd.a, e->help, e->help_size);
        de_array_remove_ordered(&g_cmd.entries, i);
        unlock();
        return DE_OK;
    }
    unlock();
    return DE_FAIL(DE_ERR_NOT_FOUND, "de_command_unregister: no command '%s'", name);
}

bool de__command_name_at(u32 index, char *out, size_t cap) {
    lock();
    const bool ok = index < g_cmd.entries.count && cap > 0;
    if (ok)
        snprintf(out, cap, "%s", DE_ARRAY_AT(&g_cmd.entries, cmd_entry, index).name);
    unlock();
    return ok;
}

u32 de__command_count(void) {
    lock();
    u32 n = g_cmd.entries.count;
    unlock();
    return n;
}

void de__command_set_exec_hook(de__command_exec_hook_fn fn, void *user) {
    lock();
    g_cmd.hook = fn;
    g_cmd.hook_user = user;
    unlock();
}

void de__command_reset_for_tests(void) {
    lock();
    for (u32 i = 0; i < g_cmd.entries.count; ++i) {
        cmd_entry *e = &DE_ARRAY_AT(&g_cmd.entries, cmd_entry, i);
        if (e->help != NULL)
            DE_FREE(g_cmd.a, e->help, e->help_size);
    }
    de_array_clear(&g_cmd.entries);
    g_cmd.hook = NULL;
    g_cmd.hook_user = NULL;
    add_builtins_locked();
    unlock();
}

/* ---- Tokenizer --------------------------------------------------------------------------- */

de_result de__command_tokenize(char *line, int max_args, char **argv, int *argc) {
    char *r = line; /* read position */
    char *w = line; /* write position; never ahead of r */
    int n = 0;
    *argc = 0;
    for (;;) {
        while (is_space(*r))
            ++r;
        if (*r == '\0')
            break;
        if (n >= max_args)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "command line has more than %d words",
                           max_args);
        char *start = w;
        bool quoted = false;
        while (*r != '\0') {
            char c = *r;
            if (c == '\\' && (r[1] == '"' || r[1] == '\\')) {
                *w++ = r[1];
                r += 2;
                continue;
            }
            if (c == '"') {
                quoted = !quoted;
                ++r;
                continue;
            }
            if (!quoted && is_space(c))
                break;
            *w++ = c;
            ++r;
        }
        if (quoted)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "command line has an unclosed quote");
        if (*r != '\0')
            ++r; /* the separator is consumed before the NUL may overwrite it */
        *w++ = '\0';
        argv[n++] = start;
    }
    *argc = n;
    return DE_OK;
}

/* ---- Cvar pass-through ------------------------------------------------------------------- */

/* The value of `cv` as text, for the log. */
static void cvar_value_text(de_cvar cv, char *buf, size_t cap) {
    switch (de_cvar_get_type(cv)) {
    case DE_CVAR_BOOL:
        snprintf(buf, cap, "%s", de_cvar_bool(cv) ? "true" : "false");
        break;
    case DE_CVAR_INT:
        snprintf(buf, cap, "%lld", (long long)de_cvar_int(cv));
        break;
    case DE_CVAR_FLOAT:
        snprintf(buf, cap, "%.17g", de_cvar_float(cv));
        break;
    case DE_CVAR_STRING:
        snprintf(buf, cap, "\"%s\"", de_cvar_string(cv) != NULL ? de_cvar_string(cv) : "");
        break;
    case DE_CVAR_ENUM:
        snprintf(buf, cap, "%s", de_cvar_string(cv) != NULL ? de_cvar_string(cv) : "");
        break;
    default:
        snprintf(buf, cap, "?");
        break;
    }
}

/* argv[0] names no command: a cvar get or set. */
static de_result exec_cvar(int argc, char **argv) {
    char name[DE_CVAR_NAME_MAX + 1];
    size_t n = strlen(argv[0]);
    if (n > DE_CVAR_NAME_MAX)
        return DE_FAIL(DE_ERR_NOT_FOUND, "unknown command or cvar '%s'", argv[0]);
    for (size_t i = 0; i <= n; ++i)
        name[i] = ascii_lower(argv[0][i]);
    de_cvar cv = de_cvar_find(name);
    if (!DE_HANDLE_VALID(cv))
        return DE_FAIL(DE_ERR_NOT_FOUND, "unknown command or cvar '%s'", argv[0]);
    if (argc == 1) {
        char value[DE_LOG_MESSAGE_MAX / 2];
        cvar_value_text(cv, value, sizeof value);
        DE_INFO(g_cmd.log, "%s = %s", name, value);
        return DE_OK;
    }
    /* Several value words are joined with single spaces (quote a value to keep its spacing). The
     * line was at most DE_COMMAND_LINE_MAX bytes, so the join fits. */
    char value[DE_COMMAND_LINE_MAX + 1];
    size_t len = 0;
    for (int i = 1; i < argc; ++i) {
        size_t wl = strlen(argv[i]);
        if (i > 1)
            value[len++] = ' ';
        memcpy(value + len, argv[i], wl);
        len += wl;
    }
    value[len] = '\0';
    return de_cvar_set(cv, value);
}

/* ---- Exec -------------------------------------------------------------------------------- */

static de_result exec_line(const char *line) {
    size_t len = strlen(line);
    if (len > DE_COMMAND_LINE_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "command line is %zu bytes (at most %d)", len,
                       DE_COMMAND_LINE_MAX);
    char buf[DE_COMMAND_LINE_MAX + 1];
    memcpy(buf, line, len + 1);
    char *argv[DE_COMMAND_MAX_ARGS + 1];
    int argc = 0;
    DE_TRY(de__command_tokenize(buf, DE_COMMAND_MAX_ARGS, argv, &argc));
    if (argc == 0 || argv[0][0] == '#')
        return DE_OK;
    argv[argc] = NULL;

    de_command_fn fn = NULL;
    void *user = NULL;
    lock();
    for (u32 i = 0; i < g_cmd.entries.count; ++i) {
        const cmd_entry *e = &DE_ARRAY_AT(&g_cmd.entries, cmd_entry, i);
        if (name_eq_nocase(e->name, argv[0])) {
            fn = e->fn;
            user = e->user;
            break;
        }
    }
    unlock();
    if (fn == NULL)
        return exec_cvar(argc, argv);

    de_error_clear();
    de_result r = fn(argc, (const char *const *)argv, user);
    if (r != DE_OK && de_error_last() == NULL)
        r = DE_FAIL(r, "command '%s' failed (%s)", argv[0], de_result_str(r));
    return r;
}

de_result de_command_exec(const char *line) {
    if (line == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_command_exec: NULL line");
    registry_init_once();
    ++g_exec_depth;
    de_result r = exec_line(line);
    if (--g_exec_depth == 0) {
        lock();
        de__command_exec_hook_fn hook = g_cmd.hook;
        void *hook_user = g_cmd.hook_user;
        unlock();
        if (hook != NULL)
            hook(hook_user);
    }
    return r;
}

/* ---- Built-in bodies --------------------------------------------------------------------- */

static de_result cmd_help(int argc, const char *const *argv, void *user) {
    (void)user;
    const char *prefix = argc > 1 ? argv[1] : "";
    u32 shown = 0;
    /* One entry per lock, so the log call (which may run sinks) is outside the lock. */
    for (u32 i = 0;; ++i) {
        char name[DE_COMMAND_NAME_MAX + 1];
        char help[256];
        lock();
        if (i >= g_cmd.entries.count) {
            unlock();
            break;
        }
        const cmd_entry *e = &DE_ARRAY_AT(&g_cmd.entries, cmd_entry, i);
        memcpy(name, e->name, sizeof name);
        snprintf(help, sizeof help, "%s", e->help != NULL ? e->help : "");
        unlock();
        if (!starts_with_nocase(name, prefix))
            continue;
        DE_INFO(g_cmd.log, "  %-20s %s", name, help);
        ++shown;
    }
    DE_INFO(g_cmd.log, "%u command%s%s%s", shown, shown == 1 ? "" : "s",
            prefix[0] != '\0' ? " starting with " : "", prefix);
    return DE_OK;
}

typedef struct cvars_visit {
    const char *prefix;
    u32 shown;
} cvars_visit;

static void cvars_visit_fn(de_cvar cv, void *user) {
    cvars_visit *v = (cvars_visit *)user;
    const char *name = de_cvar_name(cv);
    if (!starts_with_nocase(name, v->prefix))
        return;
    char value[DE_LOG_MESSAGE_MAX / 2];
    cvar_value_text(cv, value, sizeof value);
    const char *help = de_cvar_help(cv);
    if (help[0] != '\0')
        DE_INFO(g_cmd.log, "  %s = %s  # %s", name, value, help);
    else
        DE_INFO(g_cmd.log, "  %s = %s", name, value);
    ++v->shown;
}

static de_result cmd_cvars(int argc, const char *const *argv, void *user) {
    (void)user;
    cvars_visit v = {argc > 1 ? argv[1] : "", 0};
    de_cvar_foreach_flag(0, cvars_visit_fn, &v);
    DE_INFO(g_cmd.log, "%u cvar%s%s%s", v.shown, v.shown == 1 ? "" : "s",
            v.prefix[0] != '\0' ? " starting with " : "", v.prefix);
    return DE_OK;
}

static de_result cmd_echo(int argc, const char *const *argv, void *user) {
    (void)user;
    char text[DE_COMMAND_LINE_MAX + 1];
    size_t len = 0;
    for (int i = 1; i < argc; ++i) {
        size_t wl = strlen(argv[i]);
        if (i > 1)
            text[len++] = ' ';
        memcpy(text + len, argv[i], wl); /* the words came from one line: they fit */
        len += wl;
    }
    text[len] = '\0';
    DE_INFO(g_cmd.log, "%s", text);
    return DE_OK;
}

static de_result cmd_exec(int argc, const char *const *argv, void *user) {
    (void)user;
    if (argc != 2)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "usage: exec <vpath>");
    /* g_exec_depth counts this exec's own line; each level of nesting adds one. */
    if (g_exec_depth > DE__COMMAND_EXEC_DEPTH_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "exec %s: nested more than %d files deep", argv[1],
                       DE__COMMAND_EXEC_DEPTH_MAX);
    const char *vpath = argv[1];
    void *data = NULL;
    u64 size = 0;
    de_result r = de_file_read_all(vpath, g_cmd.a, &data, &size);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "exec %s", vpath);

    char *text = (char *)data; /* NUL-terminated by de_file_read_all */
    de_result first = DE_OK;
    char first_msg[DE_ERROR_MESSAGE_MAX];
    first_msg[0] = '\0';
    u32 first_line = 0, lines = 0, failed = 0;
    char *p = text;
    while (*p != '\0') {
        char *end = strchr(p, '\n');
        char *next = end != NULL ? end + 1 : p + strlen(p);
        if (end != NULL)
            *end = '\0';
        size_t len = strlen(p);
        if (len > 0 && p[len - 1] == '\r')
            p[len - 1] = '\0';
        ++lines;
        de_result lr = de_command_exec(p);
        if (lr != DE_OK) {
            const de_error *err = de_error_last();
            const char *msg = err != NULL ? err->message : de_result_str(lr);
            DE_WARN(g_cmd.log, "%s:%u: %s", vpath, lines, msg);
            ++failed;
            if (first == DE_OK) {
                first = lr;
                first_line = lines;
                snprintf(first_msg, sizeof first_msg, "%s", msg);
            }
        }
        p = next;
    }
    DE_FREE(g_cmd.a, data, size + 1);
    if (first != DE_OK)
        return DE_FAIL(first, "exec %s: line %u: %s (%u of %u lines failed)", vpath, first_line,
                       first_msg, failed, lines);
    return DE_OK;
}
