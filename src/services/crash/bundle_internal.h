/* bundle_internal.h: the crash report bundle (SPEC §18.5, DENG-96). Internal to the engine;
 * include as "services/crash/bundle_internal.h". Users: the app host (src/host/host_crash.c) and
 * tests/unit/crash/test_bundle.c.
 *
 * DENG-251 (src/platform/crash_*.c) writes the core report `<stem>.txt` and the pending marker.
 * The bundle adds, through one chained crash handler (de_crash_add_handler), two sibling files
 * with the same stem:
 *
 *   <stem>.log_tail.txt   the last DE__CRASH_LOG_RING_BYTES of formatted log lines (log_ring.c)
 *   <stem>.cvars.toml     every cvar and its value, as TOML (cvar_snapshot.c)
 *
 * and appends `log_tail: <path>` and `cvars: <path>` to the report. The handler is
 * async-signal-safe: it only reads preallocated buffers and writes with DENG-251's file calls
 * (open/write/close, CreateFileW/WriteFile/CloseHandle); it allocates nothing and takes no lock.
 *
 * Everything is process-wide, like the crash state it extends. */
#ifndef DE_SERVICES_CRASH_BUNDLE_INTERNAL_H
#define DE_SERVICES_CRASH_BUNDLE_INTERNAL_H

#include <dengine/de_base.h>
#include <dengine/de_debug.h>

#include <stddef.h>
#include <stdint.h>

DE_EXTERN_C_BEGIN

/* ---------- Bundle (bundle.c) ---------- */

/**
 * Starts the bundle, or brings it up to date when it runs already: adds the chained crash
 * handler (once per process), (re)adds the log ring sink (de_log_shutdown removes every sink),
 * installs the command exec hook (the cvar snapshot is refreshed after every top-level
 * de_command_exec) and takes a full cvar snapshot. Works before or after de_crash_install (the
 * handler only runs once a crash is reported). Main thread.
 * @return DE_OK, or de_crash_add_handler's error (DE_ERR_BUSY: all handler slots taken).
 */
de_result de__crash_bundle_install(void);

/** True after a successful de__crash_bundle_install. */
bool de__crash_bundle_installed(void);

/**
 * Per-frame work (the host calls it from a frame-end hook): refreshes the cvar snapshot when
 * DE__CRASH_SNAPSHOT_INTERVAL_NS have passed since the last refresh and a cvar changed. Does
 * nothing before de__crash_bundle_install. Allocates nothing unless a cvar value changed.
 * `now_ns` is in the de_time_now_ns timebase. Main thread.
 */
void de__crash_bundle_tick(u64 now_ns);

/** Shortest time between two refreshes from de__crash_bundle_tick (1 s). */
#define DE__CRASH_SNAPSHOT_INTERVAL_NS 1000000000ull

/** The chained handler itself, for tests that call it with a context of their own. */
void de__crash_bundle_handler(const de_crash_context *ctx, void *user);

/* ---------- Log ring (log_ring.c) ---------- */

/** Bytes of formatted log text the ring keeps (the newest ones). */
#define DE__CRASH_LOG_RING_BYTES (64u * 1024u)

/**
 * The log sink (de_log_add_sink) that feeds the ring. Each record becomes one line,
 * `<seconds>.<ms> [level][category][thread] file:line message`, like the file sink. Sinks run
 * one at a time under the log's drain lock, so the ring has one writer at a time. Allocates
 * nothing.
 */
void de__crash_log_ring_sink(const de_log_record *rec, void *user);

/** Writes the ring's text, oldest first, to `fd` (a DENG-251 crash file handle). When the ring
 * has wrapped, the partial oldest line is skipped. Async-signal-safe. */
void de__crash_log_ring_write_fd(intptr_t fd);

/** Copies what de__crash_log_ring_write_fd would write into `out` (at most cap - 1 bytes, the
 * newest ones) and NUL-terminates it. Returns the length. For tests; call after de_log_flush. */
size_t de__crash_log_ring_copy(char *out, size_t cap);

/** Total bytes ever written to the ring. */
u64 de__crash_log_ring_written(void);

/** Tests only: empties the ring. */
void de__crash_log_ring_reset_for_tests(void);

/* ---------- Cvar snapshot (cvar_snapshot.c) ---------- */

/** Bytes of each of the two snapshot buffers. */
#define DE__CRASH_SNAPSHOT_BYTES (32u * 1024u)

/**
 * Formats every registered cvar (registration order, `name = value`, TOML) into the buffer the
 * crash handler is not reading, then makes it current with one atomic store. Without `force` it
 * first hashes the current values (lock-free getters, no allocation) and does nothing when they
 * are unchanged since the last refresh. Cvars that do not fit are counted in a closing comment.
 * Does nothing while another refresh runs or after a crash froze the snapshot. Main thread.
 * @return true when a new snapshot was published.
 */
bool de__crash_cvar_snapshot_refresh(bool force);

/** The current snapshot text (not NUL-terminated) and its length. Valid until the next refresh
 * but one; for tests and the crash handler. */
const char *de__crash_cvar_snapshot_current(size_t *len);

/** Writes the current snapshot to `fd` and stops later refreshes. Async-signal-safe. */
void de__crash_cvar_snapshot_write_fd(intptr_t fd);

/** Snapshots published so far. */
u64 de__crash_cvar_snapshot_count(void);

/** Tests only: clears both buffers, the change hash and the frozen flag. */
void de__crash_cvar_snapshot_reset_for_tests(void);

DE_EXTERN_C_END
#endif /* DE_SERVICES_CRASH_BUNDLE_INTERNAL_H */
