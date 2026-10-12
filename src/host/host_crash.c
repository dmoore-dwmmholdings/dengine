/*
 * host_crash.c: crash reporting and the host's console commands (SPEC §18.5, §18.6; DENG-96).
 * See host_crash.h.
 *
 * The crash API is DENG-251's (src/platform/crash_*.c, de_debug.h crash section); the bundle is
 * src/services/crash (log tail, cvar dump). This file only glues them to the engine: where the
 * reports go (cvars), what the host annotates, and the per-frame updates.
 *
 * Host console commands: `quit` and `reload` (DENG-93 hot reload). Commands are process-wide
 * and outlive engines, so each only raises a flag that the frame-end hook of the running main
 * loop carries out.
 *
 * A game that wants its crash reports uploaded calls de_crash_install(dir, upload, user) again
 * from its `init`: the second install only changes the directory and the callback, and with a
 * callback it hands the reports of earlier crashes to it (next-launch upload).
 */
#include "host/host_crash.h"

#include "host/host_internal.h"
#include "host/hot_reload.h" /* DENG-93: de__hot_reload_request for the `reload` command */
#include "platform/crash_internal.h"
#include "platform/de_atomic.h"
#include "services/crash/bundle_internal.h"

#include <dengine/de_debug.h>

#include <string.h>

#if defined(DE_DEBUG)
#define DE__HOST_BUILD_TYPE "Debug"
#elif defined(DE_DEV)
#define DE__HOST_BUILD_TYPE "Dev"
#elif defined(DE_SHIPPING)
#define DE__HOST_BUILD_TYPE "Shipping"
#else
#define DE__HOST_BUILD_TYPE "Release"
#endif

static de_atomic_u32 g_quit_command;   /* `quit` and `reload` registered (once per process) */
static de_atomic_u32 g_quit_pending;   /* `quit` ran; the next frame-end hook asks to quit */
static de_atomic_u32 g_reload_pending; /* `reload` ran; the next frame-end hook requests it */
static de_atomic_u32 g_active;         /* the last engine init installed crash handling */
static de_atomic_u32 g_warned;         /* the "crash reports off" warning was logged */
static de_timing g_timing_annotated;   /* main thread: what host.timing says */

/* ---- The `quit` and `reload` commands
 * ------------------------------------------------------------------- */

/* Commands are process-wide and outlive engines, so `quit` holds no engine pointer: it raises a
 * flag that the frame-end hook of the running main loop turns into de_engine_request_quit. */
static de_result cmd_quit(int argc, const char *const *argv, void *user) {
    (void)argc;
    (void)argv;
    (void)user;
    de_atomic_store_u32(&g_quit_pending, 1u, DE_ATOMIC_RELEASE);
    return DE_OK;
}

/* Same pattern for `reload`: the hook passes it to the hot reload of the running host (DENG-93),
 * which reloads the game library at the next frame boundary (nothing when hot reload is off). */
static de_result cmd_reload(int argc, const char *const *argv, void *user) {
    (void)argc;
    (void)argv;
    (void)user;
    de_atomic_store_u32(&g_reload_pending, 1u, DE_ATOMIC_RELEASE);
    return DE_OK;
}

static void register_host_commands(void) {
    if (de_atomic_load_u32(&g_quit_command, DE_ATOMIC_ACQUIRE) != 0)
        return;
    /* A game's own `quit` or `reload` takes precedence (DE_ERR_ALREADY_EXISTS is fine). */
    (void)de_command_register("quit", "quit: end the game after the current frame", cmd_quit, NULL);
    (void)de_command_register("reload",
                              "reload: reload the game library at the next frame boundary "
                              "(Debug and Dev, game libraries only)",
                              cmd_reload, NULL);
    de_atomic_store_u32(&g_quit_command, 1u, DE_ATOMIC_RELEASE);
    de_error_clear();
}

/* ---- Annotations -------------------------------------------------------------------------- */

static void annotate_timing(const de_timing *t) {
    char text[48];
    char num[24];
    text[0] = '\0';
    de__crash_str_append(text, sizeof text, t->mode == DE_TIMING_VARIABLE ? "variable " : "fixed ");
    de__crash_fmt_u64(num, t->fixed_hz, 10);
    de__crash_str_append(text, sizeof text, num);
    de__crash_str_append(text, sizeof text, " Hz");
    de_crash_set_annotation("host.timing", text);
    g_timing_annotated = *t;
}

static void annotate_u64(const char *key, u64 v) {
    char num[24];
    de__crash_fmt_u64(num, v, 10);
    de_crash_set_annotation(key, num);
}

/* ---- Engine init -------------------------------------------------------------------------- */

static void warn_once(de__engine *e, const char *what, const char *dir) {
    u32 expected = 0;
    if (!de_atomic_cas_u32(&g_warned, &expected, 1u, DE_ATOMIC_ACQ_REL, DE_ATOMIC_RELAXED))
        return;
    const de_error *err = de_error_last();
    DE_WARN(e->log_host, "crash reports are off: %s '%s'%s%s", what, dir, err != NULL ? ": " : "",
            err != NULL ? err->message : "");
}

void de__host_crash_engine_init(de__engine *e) {
    de_atomic_store_u32(&g_active, 0, DE_ATOMIC_RELEASE);

    /* Re-registering returns the same handle (the cvars are process-wide). */
    de_cvar enabled = de_cvar_register(&(const de_cvar_desc){
        .name = "crash.enabled",
        .help = "Write a crash report (with log tail and cvars) when the game crashes; read at "
                "engine start.",
        .type = DE_CVAR_BOOL,
        .default_number = 1.0,
    });
    de_cvar dir_cv = de_cvar_register(&(const de_cvar_desc){
        .name = "crash.dir",
        .help = "Where crash reports go: a virtual path (user:/crashes) or a native directory; "
                "read at engine start.",
        .type = DE_CVAR_STRING,
        .default_string = DE__HOST_CRASH_DIR_DEFAULT,
    });
    de_error_clear(); /* a failed registration leaves an invalid handle: the defaults apply */
    register_host_commands();

    if (DE_HANDLE_VALID(enabled) && !de_cvar_bool(enabled)) {
        DE_LOG(DE_LOG_DEBUG, e->log_host, "crash reports are off (crash.enabled = false)");
        return;
    }

    const char *dir = DE_HANDLE_VALID(dir_cv) ? de_cvar_string(dir_cv) : NULL;
    if (dir == NULL || dir[0] == '\0')
        dir = DE__HOST_CRASH_DIR_DEFAULT;
    char native[DE_FS_PATH_MAX];
    if (strstr(dir, ":/") != NULL) {
        de_result r = de_fs_resolve(dir, native, sizeof native);
        if (r != DE_OK) {
            warn_once(e, "cannot resolve crash.dir", dir);
            de_error_clear();
            return;
        }
    } else {
        size_t n = strlen(dir);
        if (n >= sizeof native) {
            warn_once(e, "crash.dir is too long", dir);
            return;
        }
        memcpy(native, dir, n + 1);
    }

    /* An upload callback a game installed earlier in this process stays. */
    void *upload_user = NULL;
    de_crash_upload_fn upload = de__crash_upload(&upload_user);
    de_result r = de_crash_install(native, upload, upload_user);
    if (r != DE_OK) {
        warn_once(e, "cannot install the crash handler for", native);
        de_error_clear();
        return;
    }
    r = de__crash_bundle_install();
    if (r != DE_OK) {
        /* The core report still works; only the log tail and the cvar dump are missing. */
        warn_once(e, "no log tail or cvar dump in", native);
        de_error_clear();
    }

    de_crash_set_annotation("host.game", e->config.name);
    de_crash_set_annotation("host.game_library",
                            e->config.library[0] != '\0' ? e->config.library : "static");
    de_crash_set_annotation("host.headless", e->headless ? "true" : "false");
    annotate_timing(&e->timing);
    de_crash_set_annotation("host.build_type", DE__HOST_BUILD_TYPE);
    annotate_u64("host.frame", 0);
    de_atomic_store_u32(&g_active, 1u, DE_ATOMIC_RELEASE);
    DE_LOG(DE_LOG_DEBUG, e->log_host, "crash reports go to %s", native);
}

bool de__host_crash_active(void) {
    return de_atomic_load_u32(&g_active, DE_ATOMIC_ACQUIRE) != 0;
}

/* ---- After attach: game annotations and the frame-end hook ------------------------------ */

static de_result crash_frame_end(de__host *h, void *user) {
    (void)user;
    if (de_atomic_exchange_u32(&g_quit_pending, 0, DE_ATOMIC_ACQ_REL) != 0)
        de_engine_request_quit(h->engine);
    if (de_atomic_exchange_u32(&g_reload_pending, 0, DE_ATOMIC_ACQ_REL) != 0)
        de__hot_reload_request(h);
    if (!de__host_crash_active())
        return DE_OK;
    /* After frame N the next frame, N + 1, is the one in progress until the next update: a
     * crash in its schedules is reported with its own index (0 = the first frame). */
    annotate_u64("host.frame", h->loop_frames + 1u);
    const de__engine *e = de__engine_from(h->engine);
    if (e->timing.mode != g_timing_annotated.mode ||
        e->timing.fixed_hz != g_timing_annotated.fixed_hz)
        annotate_timing(&e->timing);
    de__crash_bundle_tick(de_time_now_ns());
    return DE_OK;
}

de_result de__host_crash_attach(de__host *h) {
    if (h == NULL || h->engine == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__host_crash_attach: no engine");
    /* A `quit` or `reload` typed before this run (or left from an earlier one) does not act. */
    de_atomic_store_u32(&g_quit_pending, 0, DE_ATOMIC_RELEASE);
    de_atomic_store_u32(&g_reload_pending, 0, DE_ATOMIC_RELEASE);
    if (de__host_crash_active() && h->api != NULL) {
        if (h->api->name != NULL)
            de_crash_set_annotation("host.game", h->api->name);
        de_crash_set_annotation("host.game_library", h->game.lib != NULL ? h->game.path : "static");
    }
    de_result r = de__loop_add_frame_end_hook(h, crash_frame_end, NULL);
    if (r != DE_OK) {
        /* Not fatal: the game runs, without host.frame updates and without the `quit` command. */
        r = DE_FAIL_WRAP(r, "crash frame-end hook");
        DE_WARN(h->log, "%s", de_error_last() != NULL ? de_error_last()->message : "hook failed");
    }
    return r;
}
