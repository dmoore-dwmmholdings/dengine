/*
 * host_crash.h: crash reporting and console wiring of the app host (internal to src/host/, not
 * installed). SPEC §18.5, §18.6. Owner: DENG-96.
 *
 * Include as "host/host_crash.h". Two calls, kept out of engine.c and host_run.c so those files
 * change by one line each: de__host_crash_engine_init (engine create) and de__host_crash_attach
 * (de_host_run, after the game is attached).
 */
#ifndef DE_HOST_HOST_CRASH_H
#define DE_HOST_HOST_CRASH_H

#include "host/engine_internal.h"

DE_EXTERN_C_BEGIN

struct de__host;

/** Default of the cvar `crash.dir`. */
#define DE__HOST_CRASH_DIR_DEFAULT "user:/crashes"

/**
 * Crash handling and the built-in host commands for engine `e`. de__engine_create calls it right
 * after the platform is up. In order:
 *   - registers the cvars `crash.enabled` (bool, true) and `crash.dir` (string, `user:/crashes`;
 *     a virtual path is resolved with de_fs_resolve, anything else is a native path), and the
 *     console commands `quit` and `reload` (once per process; they ask the running main loop to
 *     quit, or its hot reload (DENG-93) to reload the game library);
 *   - unless `crash.enabled` is false: de_crash_install into `crash.dir` (keeping an upload
 *     callback a game installed earlier in this process), the crash bundle
 *     (de__crash_bundle_install: log tail, cvar dump) and the annotations `host.game` (game.toml
 *     name until the game attaches), `host.game_library`, `host.headless`, `host.timing`,
 *     `host.build_type` and `host.frame` (0).
 * Crash reporting is best effort: nothing here fails the engine. A directory that cannot be
 * resolved or created, or a bundle that cannot be added, logs a warning (once per process) and
 * the engine runs without it. Main thread.
 */
void de__host_crash_engine_init(de__engine *e);

/**
 * After de__game_attach: sets `host.game` to the game's API name and `host.game_library` to the
 * library path (or "static"), and adds the crash frame-end hook. The hook, once per frame,
 * carries out a `quit` or `reload` command, writes `host.frame` (the index of the frame in
 * progress: after frame N it says N + 1, so a crash in a schedule reports its own frame; formatted
 * on the stack), updates `host.timing` when the timing changed, and lets the bundle refresh its
 * cvar snapshot (at most once per second, only when a cvar changed). It allocates nothing in steady
 * state. Main thread.
 * @return DE_OK or DE_ERR_BUSY (the frame-end hook list is full; logged as a warning, and the
 *         game runs without the hook).
 */
de_result de__host_crash_attach(struct de__host *h);

/** Tests: true when the last de__host_crash_engine_init installed crash handling. */
bool de__host_crash_active(void);

DE_EXTERN_C_END
#endif /* DE_HOST_HOST_CRASH_H */
