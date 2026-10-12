/*
 * hot_reload.h: game library hot reload (DENG-93, SPEC §10.1), internal to src/host/ (not
 * installed). Owner: DENG-93.
 *
 * Include as "host/hot_reload.h". host_internal.h includes it so de__host can hold the state;
 * the functions take the de__host.
 *
 * In Debug and Dev builds, a game that was loaded from a library (not linked statically) is hot
 * reloaded when its library file changes:
 *   - de__hot_reload_init (before the game is attached) mounts the library's directory as
 *     DE__HOT_RELOAD_ROOT, watches it (de_file_watch_create) and makes the game attach from a
 *     copy in `cache:/hot/`, so the build can overwrite the original (Windows locks a loaded DLL;
 *     macOS dlopen caches images by path). It adds de__hot_reload_frame_end as a frame-end hook
 *     of the main loop.
 *   - Each frame, after `Last` and before the frame arena swap, the hook polls the watch. A change
 *     of the library file marks a reload as pending, and the reload runs at once, at this frame
 *     boundary (de__hot_reload_reload has the order).
 *   - de__hot_reload_shutdown (after the game is detached) stops watching and deletes the copy.
 * Release and Shipping builds compile the functions as no-ops (DE__HOT_RELOAD is 0). The cvar
 * `host.hot_reload` (bool, default true, Dev only) turns it off for a run.
 *
 * Contract for games: all game state lives in the ECS world or in `de_engine.game_state`, which
 * the game allocates from `de_engine.allocator` (never static storage in the library: a reload
 * brings new, zeroed statics). Systems belong in `register_systems` and types in
 * `register_types`: both run on every load. A system the game adds elsewhere (in `init`) is
 * removed by a reload and not added again.
 */
#ifndef DE_HOST_HOT_RELOAD_H
#define DE_HOST_HOT_RELOAD_H

#include <dengine/de_platform.h>

#include "ecs/hot_reload_internal.h"

DE_EXTERN_C_BEGIN

struct de__host;

/** 1 when this build hot-reloads game libraries (Debug and Dev), else 0. */
#if defined(DE_DEBUG) || defined(DE_DEV)
#define DE__HOT_RELOAD 1
#else
#define DE__HOT_RELOAD 0
#endif

/** The system owner tag of the game's systems (de__world_set_system_owner). */
#define DE__HOST_GAME_SYSTEM_OWNER 1u

/** The VFS root under which the game library's directory is mounted and watched. */
#define DE__HOT_RELOAD_ROOT "gamelib"

/** The directory the loaded copies go to. */
#define DE__HOT_RELOAD_DIR "cache:/hot"

/** Longest library file name (bytes, NUL included). */
#define DE__HOT_RELOAD_NAME_MAX 256u

/** Hot reload state of one de_host_run. Zeroed = off. Fixed size: no allocation. */
typedef struct de__hot_reload {
    /** The game is a library and hot reload runs (Debug/Dev, cvar on, set up without error). */
    bool enabled;
    /** A change (or de__hot_reload_request) was seen: reload at the next frame boundary. */
    bool pending;
    /** DE__HOT_RELOAD_ROOT was mounted by de__hot_reload_init. */
    bool mounted;
    /** The watch; NULL when watching failed (de__hot_reload_request still works). */
    de_file_watch *watch;
    /** Native path of the library the build writes (the watched file). */
    char source[DE_FS_PATH_MAX];
    /** Native directory of `source` (mounted as DE__HOT_RELOAD_ROOT). */
    char source_dir[DE_FS_PATH_MAX];
    /** File name of `source`, e.g. "libgame.dylib". */
    char file_name[DE__HOT_RELOAD_NAME_MAX];
    /** Virtual path of the copy that is loaded now ("" when hot reload is off). */
    char current_copy[DE_FS_PATH_MAX];
    /** Native path of the first copy; de__hot_reload_init points de_host_desc.game_library
     * here, so de__game_attach loads it. */
    char loaded[DE_FS_PATH_MAX];
    /** The <n> of the next `<lib>.reload-<n>.<ext>`. */
    u32 next_copy;
    /** Successful reloads. */
    u32 reloads;
    /** Reloads that were refused; the game kept running on the library it had. */
    u32 failures;
    /** Systems removed by the last reload (the old library's). */
    u32 last_systems_removed;
    /** What the ECS did in the last successful reload. */
    de__world_sync_stats last_sync;
} de__hot_reload;

/**
 * Sets up hot reload (see the file comment). Call it after the engine is created and before
 * de__game_attach: it finds the library attach is going to load (the same rules), copies it to
 * `cache:/hot/` and points `h->desc.game_library` at the copy, so the build output itself is
 * never loaded. Does nothing (and returns DE_OK) in Release and Shipping builds, for a
 * statically linked game, or when the cvar `host.hot_reload` is false. A failure to set up (no
 * `cache:/` mount, the copy cannot be written) is logged as a warning and the game is attached
 * from its own library, without hot reload.
 * @return DE_OK, or DE_ERR_BUSY when the frame-end hook list is full.
 */
de_result de__hot_reload_init(struct de__host *h);

/** Stops watching, unmounts DE__HOT_RELOAD_ROOT and deletes the loaded copy. Call it after
 * de__game_detach. Safe when hot reload is off or was never set up. */
void de__hot_reload_shutdown(struct de__host *h);

/** Marks a reload as pending, as a change of the library file does. The next
 * de__hot_reload_poll (the frame-end hook) runs it. */
void de__hot_reload_request(struct de__host *h);

/**
 * Polls the watch, then runs a pending reload (de__hot_reload_reload). Inside a schedule run (a
 * system calls it) it only polls: the reload stays pending until the frame boundary. A refused
 * library is logged and counted in `failures`; the game keeps running on the library it had, and
 * the next change is tried again.
 * @return DE_OK, or the error of a reload that left the world unusable (an allocation failure
 *         while re-laying out components); the main loop then stops.
 */
de_result de__hot_reload_poll(struct de__host *h);

/** The frame-end hook (de__loop_hook_fn): de__hot_reload_poll. */
de_result de__hot_reload_frame_end(struct de__host *h, void *user);

/**
 * Reloads the game library now. Main thread, outside every schedule run. In order:
 *   1. copy the library to `cache:/hot/<lib>.reload-<n>.<ext>` (and its `.pdb` on Windows);
 *   2. open the copy and check its API table (de__game_module_load: version, name, callbacks);
 *      a library that fails here is refused before the running game is touched;
 *   3. `on_unload` of the old game;
 *   4. remove every system of the game (owner DE__HOST_GAME_SYSTEM_OWNER);
 *   5. `register_types` of the new game; an error it leaves in the error context refuses the
 *      library;
 *   6. de__world_sync_types: components, resources and events whose layout changed are re-laid
 *      out (fields copied by name and kind, or the type's `migrate` hook), the others take the
 *      new hook pointers;
 *   7. `register_systems`, then `on_reload` of the new game;
 *   8. close the old library (the log is flushed first) and delete its copy.
 * A library refused in step 5 or 6 is undone: the old game's `register_types` runs again (the
 * registry gets the old descriptions and hooks back), then its `register_systems` and
 * `on_reload`, and the new library is closed. Steps 3 to 7 run with the game's system owner tag.
 * The error context of the main thread is cleared after a successful reload, so no error record
 * points into the closed library.
 * @return DE_OK; the refusal's error (the game runs on, unchanged); DE_ERR_UNSUPPORTED (Release,
 *         Shipping, or hot reload is off); DE_ERR_BUSY (called inside a schedule run). When
 *         `*fatal` (may be NULL) is set, the world is partly re-laid out and the game cannot go on.
 */
de_result de__hot_reload_reload(struct de__host *h, bool *fatal);

DE_EXTERN_C_END
#endif /* DE_HOST_HOT_RELOAD_H */
