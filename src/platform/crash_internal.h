/* crash_internal.h: internals of the crash handler (SPEC §18.5, DENG-251). Not a public header;
 * include as "platform/crash_internal.h". Shared by crash_common.c (every OS), crash_posix.c,
 * crash_win32.c and tests/unit/platform/test_crash.c.
 *
 * Everything reachable from a crash (signal handler, exception filter, panic hook) is
 * async-signal-safe: no malloc, no locks, no stdio. Strings are formatted with the helpers below
 * into preallocated or stack buffers.
 *
 * Files of one crash, all `<stem>.*` with `<stem>` = `<dump_dir>/crash-<unixtime>-<pid>` (plus
 * `-<n>` if that name is taken):
 *
 *   <stem>.txt      the text report (de_crash_context.report_path)
 *   <stem>.dmp      the minidump (Windows only)
 *   <stem>.pending  the pending-report marker, written right after the core report and before
 *                   the chained handlers run, so a handler that crashes does not lose it
 *   <stem>.*        sibling files written by chained handlers (S06 DENG-96: .log_tail.txt, ...)
 *
 * Pending marker format (read by the next-launch scan, S06 DENG-96): UTF-8 text, one
 * `key=value` per line, LF line ends, no quoting (paths run to the end of the line). Keys:
 *
 *   report=<full path of <stem>.txt>          always, first line
 *   dump=<full path of <stem>.dmp>            Windows, when the minidump was written
 *   build_id=<de_build_id()>                  always
 *   reason=<SIGSEGV | EXCEPTION_... | panic>  always
 *
 * Unknown keys must be ignored by readers. The next-launch scan (DENG-96,
 * de__crash_upload_pending) renames the marker to `<stem>.sent` (or deletes it) right before the
 * upload callback sees the report, so a report is never handed over twice.
 */
#ifndef DE_PLATFORM_CRASH_INTERNAL_H
#define DE_PLATFORM_CRASH_INTERNAL_H

#include <dengine/de_base.h>
#include <dengine/de_debug.h>

#include <stddef.h>
#include <stdint.h>

/** Longest dump directory (bytes, without the NUL). Leaves room for the file names. */
#define DE__CRASH_DIR_MAX 900
/** Size of every path buffer: directory + "/crash-<20 digits>-<20 digits>-<n>.pending". */
#define DE__CRASH_PATH_CAP 1024
/** Most backtrace frames in a report. */
#define DE__CRASH_MAX_FRAMES 64
/** Bytes of a crash thread name, with the NUL. */
#define DE__CRASH_THREAD_NAME_CAP 64
/** Bytes of a panic message in the report, with the NUL (de_panic_ cuts at 1023). */
#define DE__CRASH_PANIC_CAP 1024
/** Bytes of each alternate signal stack (POSIX), at least; Linux may ask for more. */
#define DE__CRASH_ALTSTACK_BYTES (64u * 1024u)

/* ---------- Formatting (async-signal-safe) ---------- */

/** Writes `v` in `base` (10 or 16, lowercase, no prefix) into `buf` (at least 21 bytes) and
 * NUL-terminates it. Returns the length. */
size_t de__crash_fmt_u64(char *buf, u64 v, unsigned base);

/** Appends `src` to the NUL-terminated `dst` of capacity `cap`, cutting at the capacity.
 * Returns false when it had to cut. */
bool de__crash_str_append(char *dst, size_t cap, const char *src);

/** Copies at most cap-1 bytes of `src` (NULL is "") into `dst`, not cutting a UTF-8 sequence,
 * and NUL-terminates. Returns the length. */
size_t de__crash_str_copy_utf8(char *dst, size_t cap, const char *src);

/* ---------- The crash being reported ---------- */

/** Everything the OS layer knows about a crash. Filled on the crashing thread. */
typedef struct de__crash_event {
    de_crash_kind kind;
    u32 code;
    const char *reason; /**< static string */
    u64 fault_address;
    const char *panic_message;  /**< NULL unless kind == DE_CRASH_PANIC */
    const char *panic_file;     /**< may be NULL */
    const char *panic_function; /**< may be NULL */
    u32 panic_line;
    char thread_name[DE__CRASH_THREAD_NAME_CAP];
    u64 thread_id;
} de__crash_event;

/** OS callbacks used while a report is written. Any of them may be NULL. */
typedef struct de__crash_os_report {
    /** Writes the minidump to `dump_path` (Windows). Returns true when it was written. */
    bool (*write_dump)(const char *dump_path, void *user);
    /** Writes backtrace lines to `fd`. */
    void (*write_backtrace)(intptr_t fd, void *user);
    void *user;
} de__crash_os_report;

/** Outcome of de__crash_enter. */
typedef enum de__crash_entry {
    DE__CRASH_ENTER_OWNER, /**< this thread reports the crash */
    DE__CRASH_ENTER_SAME,  /**< this thread already reported it (the abort after a panic) */
    DE__CRASH_ENTER_OTHER, /**< another thread is reporting or has reported */
} de__crash_entry;

/** Claims the one report of this process for thread `tid`. Async-signal-safe. */
de__crash_entry de__crash_enter(u64 tid);

/** Marks the report as finished (after the chain ran). */
void de__crash_leave(void);

/** True once a report is finished (other crashing threads wait for this, then die). */
bool de__crash_finished(void);

/**
 * Writes the whole report for `ev`: picks the stem, writes `<stem>.dmp` through os->write_dump,
 * then `<stem>.txt` (header, annotations, backtrace) and `<stem>.pending`, then runs the chained
 * handlers, then closes the report. Async-signal-safe. The caller must own the crash
 * (de__crash_enter). Returns the report path (static storage), or NULL when nothing could be
 * written (no install, or the directory is not writable).
 */
const char *de__crash_write_report(const de__crash_event *ev, const de__crash_os_report *os);

/** The installed dump directory ("" before de_crash_install). */
const char *de__crash_dump_dir(void);

/** The upload callback stored by de_crash_install, and its user pointer (S06 DENG-96). */
de_crash_upload_fn de__crash_upload(void **user);

/** Name of a POSIX signal ("SIGSEGV"), or "signal" for one not handled here. */
const char *de__crash_signal_name(int sig);

/* ---------- OS layer (crash_posix.c / crash_win32.c) ---------- */

/** Installs the OS handlers. Called once, by the first successful de_crash_install. */
de_result de__crash_os_install(void);

/** Creates `path` (and missing parents) as a directory. Not called from a crash. */
de_result de__crash_os_mkdirs(const char *path);

/** Creates `path` for writing, failing if it exists. Returns -1 on failure; sets *exists when
 * the failure was that the file exists. */
intptr_t de__crash_os_create(const char *path, bool *exists);

/** Writes all of `n` bytes (retrying short writes). */
void de__crash_os_write(intptr_t fd, const char *s, size_t n);

void de__crash_os_close(intptr_t fd);

/** Seconds since 1970 (UTC). */
u64 de__crash_os_unix_time(void);

u64 de__crash_os_pid(void);

/** Handles a DE_PANIC (called from the panic hook): fills the event and writes the report the
 * OS way (on Windows through the dump thread). Returns the report path, or NULL when no report
 * was written (another thread is already reporting a crash, or the write failed). */
const char *de__crash_os_panic(const de_panic_info *info);

/** The directory separator of native paths: '/' or '\\'. */
char de__crash_os_separator(void);

/* ---------- Test access (tests/unit/platform/test_crash.c) ---------- */

/** Copies the value of `key` (cut like de_crash_set_annotation cuts it) into `out`. Returns
 * false when the key is not set. Unlike the report, it retries until it reads a consistent
 * entry, so it never returns a torn value. */
bool de__crash_annotation_get(const char *key, char *out, size_t cap);

/** Number of set annotations. */
u32 de__crash_annotation_count(void);

/** New keys dropped because the table was full, since the start (or the last reset). */
u64 de__crash_annotation_dropped(void);

/** Clears annotations, handlers and the dropped count. Tests only; not thread-safe. */
void de__crash_reset_for_tests(void);

/* ---------- Next-launch upload (crash_common.c; S06 DENG-96) ---------- */

/**
 * Hands every pending report in the dump directory to the installed upload callback, once each:
 * renames `<stem>.pending` to `<stem>.sent` (deletes it when the rename fails) and then calls
 * `upload` on the calling thread with `dump_path` = the minidump (Windows) or the text report,
 * `log_path` = `<stem>.log_tail.txt` when it exists (else the text report) and `build_id` from
 * the marker; DENG-328: `gpu_breadcrumbs` = the text of `<stem>.gpu_report.txt` (a GPU
 * device-lost report, at most 16 KiB) and `gpu_fault` = true when that file exists.
 * de_crash_install calls it when it gets an `upload`. Never called from a crash. Returns the number
 * of reports handed over (0 without an upload callback).
 */
u32 de__crash_upload_pending(void);

#endif /* DE_PLATFORM_CRASH_INTERNAL_H */
