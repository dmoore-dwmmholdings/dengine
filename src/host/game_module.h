/*
 * game_module.h: the attached game (SPEC §10.1), internal to src/host/ (not installed).
 *
 * Include as "host/game_module.h". A game is either a library that exports
 *   DE_GAME_EXPORT const de_game_api *de_game_get_api(void);
 * (Debug, Dev and Release: `dengine --game <lib>`, de_add_game()) or the same code linked
 * statically into the game's own executable (Shipping), which passes de_game_get_api() as
 * de_host_desc.static_api. The host never calls the game's table directly: it calls a copy in
 * de__game_module, where the optional callbacks are already filled with no-ops.
 *
 * DENG-93 (hot reload) loads a new copy of the library with de__game_module_load into a second
 * de__game_module and swaps it in; `generation` counts the loads.
 */
#ifndef DE_HOST_GAME_MODULE_H
#define DE_HOST_GAME_MODULE_H

#include <dengine/de_game.h>

DE_EXTERN_C_BEGIN

/** The symbol every game library exports. */
#define DE__GAME_API_SYMBOL "de_game_get_api"

/** The signature of DE__GAME_API_SYMBOL. */
typedef const de_game_api *(*de__game_get_api_fn)(void);

/** One attached game. Zeroed = nothing attached. Fixed size: no allocation. */
typedef struct de__game_module {
    /** The open library; NULL for a statically linked game. */
    de_dylib *lib;
    /** The game's own table (inside `lib`, or the static one). Do not call through it: use
     * `table`. NULL when nothing is attached. */
    const de_game_api *api;
    /** A copy of `*api` with `on_unload` / `on_reload` defaulted to no-ops. */
    de_game_api table;
    /** The native path `lib` was opened from; "" for a statically linked game. */
    char path[DE_FS_PATH_MAX];
    /** Successful loads into this module, the static attach included (1 after the first). */
    u32 generation;
} de__game_module;

/**
 * Checks a game API table completely: not NULL, `api_version == DE_GAME_API_VERSION` (first,
 * because another version may lay the table out differently), a `name`, and the required
 * callbacks `init`, `shutdown`, `register_types` and `register_systems`. `on_unload` and
 * `on_reload` are optional. `source` names the game in messages (a library path, or "static").
 * @return DE_OK, DE_ERR_VERSION_MISMATCH, or DE_ERR_INVALID_ARGUMENT (NULL table, no name, a
 *         missing callback; the message names the field).
 */
de_result de__game_api_validate(const de_game_api *api, const char *source);

/**
 * Opens the game library at `path` (a native path), resolves DE__GAME_API_SYMBOL, calls it and
 * validates the table. On success `*m` holds the library and `generation` is incremented; on
 * failure the library is closed again and `*m` is unchanged.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL argument, path longer than DE_FS_PATH_MAX - 1,
 *         a NULL table or a missing callback), DE_ERR_NOT_FOUND (no such library, or the library
 *         does not export DE__GAME_API_SYMBOL: the message names the symbol), DE_ERR_IO (the OS
 *         could not load it) or DE_ERR_VERSION_MISMATCH.
 */
de_result de__game_module_load(de__game_module *m, const char *path);

/** Attaches a statically linked game (no library). Same checks and result as
 * de__game_module_load. */
de_result de__game_module_set_static(de__game_module *m, const de_game_api *api);

/** Closes the library (if any) and zeroes `*m` except `generation`. Pointers into the library,
 * the table's callbacks included, are invalid afterwards. NULL is ignored. */
void de__game_module_unload(de__game_module *m);

/**
 * Turns game.toml's `[game] library` into the native path to load, in `buf`. An absolute path
 * stays as it is; a virtual path (`root:/...`) and a relative path (taken as relative to
 * `game:/`, the directory of the game) are resolved with de_fs_resolve. When the file name has
 * no extension, the platform name is used: "bin/mygame" → "bin/libmygame.dylib" (macOS),
 * "bin/libmygame.so" (Linux), "bin/mygame.dll" (Windows).
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (empty value, `buf` too small) or DE_ERR_NOT_FOUND
 *         (the root is not mounted).
 */
de_result de__game_library_resolve(const char *library, char *buf, size_t cap);

DE_EXTERN_C_END
#endif /* DE_HOST_GAME_MODULE_H */
