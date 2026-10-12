/* bundle.c: the crash report bundle (SPEC §18.5, DENG-96): the chained crash handler that
 * writes the log tail and the cvar dump beside DENG-251's report, and the setup the app host
 * calls. See bundle_internal.h for the files and the rules. */
#include "services/crash/bundle_internal.h"

#include "core/command_internal.h"
#include "platform/crash_internal.h"
#include "platform/de_atomic.h"

#include <string.h>

static de_atomic_u32 g_handler_added;
static de_atomic_u32 g_installed;
static u64 g_last_tick_ns; /* main thread */

/* Static, not on the (alternate) signal stack: only the crash owner runs the handler. */
static char g_path[DE__CRASH_PATH_CAP];

/* ---- The chained handler (async-signal-safe) ------------------------------------------- */

static void put(const de_crash_context *ctx, const char *s) {
    de_crash_write(ctx, s, strlen(s));
}

/* Creates `<stem><suffix>`, lets `body` fill it and closes it; appends `key: <path>` (or
 * `key: (failed)`) to the report. */
static void write_sibling(const de_crash_context *ctx, const char *suffix, const char *key,
                          void (*body)(intptr_t fd)) {
    g_path[0] = '\0';
    bool fits = de__crash_str_append(g_path, sizeof g_path, ctx->report_stem) &&
                de__crash_str_append(g_path, sizeof g_path, suffix);
    bool exists = false;
    intptr_t fd = fits ? de__crash_os_create(g_path, &exists) : -1;
    put(ctx, key);
    put(ctx, ": ");
    if (fd == -1) {
        put(ctx, "(failed)\n");
        return;
    }
    body(fd);
    de__crash_os_close(fd);
    put(ctx, g_path);
    put(ctx, "\n");
}

static void log_tail_body(intptr_t fd) {
    static const char header[] =
        "# Log tail (crash bundle): the newest drained log lines, oldest first. Lines still "
        "queued in a thread's log buffer at the crash are missing.\n";
    de__crash_os_write(fd, header, sizeof header - 1);
    de__crash_log_ring_write_fd(fd);
}

void de__crash_bundle_handler(const de_crash_context *ctx, void *user) {
    (void)user;
    if (ctx == NULL || ctx->report_stem == NULL)
        return;
    write_sibling(ctx, ".log_tail.txt", "log_tail", log_tail_body);
    write_sibling(ctx, ".cvars.toml", "cvars", de__crash_cvar_snapshot_write_fd);
}

/* ---- Setup and per-frame work (main thread) ------------------------------------------- */

static void exec_hook(void *user) {
    (void)user;
    (void)de__crash_cvar_snapshot_refresh(false);
}

de_result de__crash_bundle_install(void) {
    if (de_atomic_load_u32(&g_handler_added, DE_ATOMIC_ACQUIRE) == 0) {
        de_result r = de_crash_add_handler(de__crash_bundle_handler, NULL);
        if (r != DE_OK)
            return DE_FAIL_WRAP(r, "crash bundle: chained handler");
        de_atomic_store_u32(&g_handler_added, 1u, DE_ATOMIC_RELEASE);
    }
    /* de_log_shutdown drops every sink, so a later engine in the same process adds it again;
     * removing first keeps it from being added twice. */
    de_log_remove_sink(de__crash_log_ring_sink, NULL);
    de_log_add_sink(de__crash_log_ring_sink, NULL);
    de__command_set_exec_hook(exec_hook, NULL);
    (void)de__crash_cvar_snapshot_refresh(true);
    de_atomic_store_u32(&g_installed, 1u, DE_ATOMIC_RELEASE);
    return DE_OK;
}

bool de__crash_bundle_installed(void) {
    return de_atomic_load_u32(&g_installed, DE_ATOMIC_ACQUIRE) != 0;
}

void de__crash_bundle_tick(u64 now_ns) {
    if (!de__crash_bundle_installed())
        return;
    if (g_last_tick_ns != 0 && now_ns - g_last_tick_ns < DE__CRASH_SNAPSHOT_INTERVAL_NS)
        return;
    g_last_tick_ns = now_ns;
    (void)de__crash_cvar_snapshot_refresh(false);
}
