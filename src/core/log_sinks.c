/* log_sinks.c: the console and file log sinks (SPEC §5.2, DENG-150).
 *
 * Both use stdio, so logging works before the S03 VFS exists. Sinks run under the drain lock, one
 * record at a time, so they need no locking of their own. */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* fopen */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE /* fileno, isatty under -std=c11 */
#else
#define _POSIX_C_SOURCE 200809L /* fileno, isatty under -std=c11 */
#endif

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "core/log_internal.h"
#include "platform/de_atomic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h> /* _fileno, _get_osfhandle */
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING /* SDKs before Windows 10 1511 */
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <unistd.h>
#endif

/* Longest console or file line; a longer one is cut (it keeps its newline). */
#define DE__LOG_LINE_MAX (DE_LOG_MESSAGE_MAX + 512)

const char *de__log_basename(const char *path) {
    if (path == NULL)
        return "?";
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    return base;
}

/* ---------- Console ---------- */

/* 0 = not checked yet, 1 = plain, 2 = colour. Whether stderr is a terminal does not change while
 * the process runs, so it is checked once. */
static de_atomic_u32 de__console_mode;

#if defined(_WIN32)
/* Windows consoles show ANSI colours only with ENABLE_VIRTUAL_TERMINAL_PROCESSING (Windows 10
 * 1511 and later). GetConsoleMode fails when stderr is a file or a pipe, and SetConsoleMode fails
 * on an older console: both give plain output, so redirected output has no escape codes. The flag
 * is set on stderr's own handle (the CRT fd 2, which follows _dup2), and is left on at exit, as
 * Windows Terminal has it on by default anyway. */
static bool de__stderr_is_vt_console(void) {
    int fd = _fileno(stderr);
    if (fd < 0) /* a GUI-subsystem process without a console gives -2 */
        return false;
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE || h == NULL)
        return false;
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode))
        return false;
    if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)
        return true;
    return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}
#endif

bool de__log_console_detect(void) {
    /* NO_COLOR first, so a console whose user opted out keeps its mode untouched. */
    const char *no_color = getenv("NO_COLOR");
    if (no_color != NULL && no_color[0] != '\0')
        return false;
#if defined(_WIN32)
    return de__stderr_is_vt_console();
#else
    return isatty(fileno(stderr)) != 0;
#endif
}

static bool de__console_color(void) {
    u32 mode = de_atomic_load_u32(&de__console_mode, DE_ATOMIC_RELAXED);
    if (mode == 0) {
        /* Two threads may both detect on their first line; the answer and the console mode they
         * set are the same, so the race is harmless. */
        mode = de__log_console_detect() ? 2u : 1u;
        de_atomic_store_u32(&de__console_mode, mode, DE_ATOMIC_RELAXED);
    }
    return mode == 2;
}

static const char *de__level_color(de_log_level lvl) {
    switch (lvl) {
    case DE_LOG_TRACE:
        return "\x1b[90m";
    case DE_LOG_DEBUG:
        return "\x1b[36m";
    case DE_LOG_INFO:
        return "\x1b[32m";
    case DE_LOG_WARN:
        return "\x1b[33m";
    case DE_LOG_ERROR:
        return "\x1b[31m";
    case DE_LOG_FATAL:
        return "\x1b[1;35m";
    }
    return "";
}

/* snprintf returns the untruncated length; clamp it to what is in the buffer. */
static size_t de__clamp(int n, size_t cap) {
    if (n < 0)
        return 0;
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

void de__log_write_console(const de_log_record *rec) {
    char line[DE__LOG_LINE_MAX];
    bool color = de__console_color();
    int msg_len =
        rec->message.len > (size_t)DE_LOG_MESSAGE_MAX ? DE_LOG_MESSAGE_MAX : (int)rec->message.len;
    int n = snprintf(line, sizeof line - 1, "%s[%s]%s[%s] %s:%u %.*s",
                     color ? de__level_color(rec->level) : "", de_log_level_name(rec->level),
                     color ? "\x1b[0m" : "", de_log_category_name(rec->category),
                     de__log_basename(rec->file), (unsigned)rec->line, msg_len,
                     rec->message.ptr != NULL ? rec->message.ptr : "");
    size_t len = de__clamp(n, sizeof line - 1);
    line[len++] = '\n';
    /* One write per line, so lines from different processes or the panic path do not mix
     * mid-line (stderr is unbuffered). */
    fwrite(line, 1, len, stderr);
}

void de_log_console_sink(const de_log_record *rec, void *user) {
    (void)user;
    if (rec != NULL)
        de__log_write_console(rec);
}

/* ---------- File ---------- */

struct de_log_file_sink {
    FILE *file;
    de_allocator alloc; /* the allocator that made this struct */
};

static void de__file_sink_write(const de_log_record *rec, void *user) {
    de_log_file_sink *s = (de_log_file_sink *)user;
    int msg_len =
        rec->message.len > (size_t)DE_LOG_MESSAGE_MAX ? DE_LOG_MESSAGE_MAX : (int)rec->message.len;
    fprintf(s->file, "%llu.%06llu [%s][%s][%u] %s:%u %.*s\n",
            (unsigned long long)(rec->timestamp_ns / 1000000000ull),
            (unsigned long long)(rec->timestamp_ns % 1000000000ull / 1000ull),
            de_log_level_name(rec->level), de_log_category_name(rec->category),
            (unsigned)rec->thread_id, de__log_basename(rec->file), (unsigned)rec->line, msg_len,
            rec->message.ptr != NULL ? rec->message.ptr : "");
}

static void de__file_sink_batch_end(void *user) {
    de_log_file_sink *s = (de_log_file_sink *)user;
    fflush(s->file);
}

de_result de_log_file_sink_open(const char *native_path, de_log_file_sink **out) {
    if (out != NULL)
        *out = NULL;
    if (native_path == NULL || native_path[0] == '\0' || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_log_file_sink_open: no path or no out");

    de_allocator a;
    de__log_allocator(&a);
    de_log_file_sink *s = (de_log_file_sink *)a.alloc(a.user, sizeof *s,
                                                      DE_ALIGNOF(de_log_file_sink), DE_MEM_GENERAL);
    if (s == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_log_file_sink_open: %s", native_path);
    s->alloc = a;
    s->file = fopen(native_path, "w");
    if (s->file == NULL) {
        a.free(a.user, s, sizeof *s);
        return DE_FAIL(DE_ERR_IO, "de_log_file_sink_open: cannot open %s", native_path);
    }
    if (!de__log_add_sink_ex(de__file_sink_write, de__file_sink_batch_end, s)) {
        fclose(s->file);
        a.free(a.user, s, sizeof *s);
        return DE_FAIL(DE_ERR_BUSY, "de_log_file_sink_open: sink table full (%d)",
                       DE_LOG_MAX_SINKS);
    }
    *out = s;
    return DE_OK;
}

void de_log_file_sink_close(de_log_file_sink *sink) {
    if (sink == NULL)
        return;
    de_log_flush();
    de_log_remove_sink(de__file_sink_write, sink);
    fclose(sink->file);
    de_allocator a = sink->alloc;
    a.free(a.user, sink, sizeof *sink);
}
