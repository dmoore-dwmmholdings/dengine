/*
 * cook_cmd.h: `de-cook cook`, the full cook of a project into a .dpak pack (SPEC §11.5, §11.7;
 * DENG-213). Tools only: the runtime never links it.
 *
 *   de-cook cook <assets.toml> --platform <windows|linux|macos> --out <dir> [options]
 *
 * Steps:
 *   1. The manifest is checked with the `--check` code (check.h), with the cooker's registered
 *      type names as extra known types. Errors print exactly as `--check` prints them (stdout)
 *      and the command exits 1 without cooking; warnings and notes go to stderr.
 *   2. The tool builds what the importers need: a type registry with the ECS built-in components
 *      (a world registers them), an asset system with `types` set to that registry (the scene and
 *      prefab importer reads component types from it, DENG-215), and a cooker that mirrors every
 *      asset type of that asset system. `--game <library>` (DENG-394) loads a game library with
 *      the host's loader (de_game_get_api) and calls only its `register_types`, with a de_engine
 *      that holds the tool's allocator, task system (NULL with `--jobs 1`), registry, world and
 *      asset system (every other field NULL). de_cook_hooks then lets a caller add more component
 *      and asset types (the tests do). The library stays loaded until the command returns.
 *   3. Every manifest asset is cooked through the cooker and its `.decache` (`.decache/` next to
 *      the manifest unless `--cache` or `--no-cache`), in parallel on the task system
 *      (`de_task_parallel_for`, one asset per chunk). Each result lands in the slot of its asset,
 *      so the order in which cooks finish never matters. Sub-assets become pack entries
 *      `<id>#<name>`.
 *   4. The pack is built in memory with dpak_writer (entries sorted by ID, bundles from the
 *      manifest), the build ID is filled in, and `<out>/<pack-name>.dpak` is written atomically.
 *
 * Determinism: the same manifest, sources, importers, platform and options give a byte-identical
 * pack, with a cold or a warm cache and with any `--jobs`. Without `--build-id` the build ID is
 * XXH3-128 (seed 0, canonical byte order) of the whole pack with a zero build ID field. A pack
 * never contains a host path, a user name or a time.
 *
 * Output: progress lines (`[k/n] <asset id>`) and diagnostics on stderr, the summary on stdout.
 * Exit codes (as `--check`): 0 cooked, 1 manifest errors or failed cooks (no pack is written),
 * 2 usage or I/O error.
 *
 * A game library binds to the engine in the host process (cmake/DeAddGame.cmake), so `de-cook`
 * links the whole engine and exports it where de_add_game() builds libraries with a static
 * engine (macOS, Linux). On Windows without DE_BUILD_SHARED it can load only libraries that need
 * nothing from the engine, like the `dengine` host. A DE_BUILD_SHARED build refuses `--game`
 * (exit 2): the library would bind to the shared dengine, a second engine without the tool's
 * VFS mounts.
 *
 * `cook` needs the runtime importers, which exist only where the cooker is linked into the
 * runtime (DE_HAS_COOKER: Debug and Dev). Built from a Release or Shipping tree it refuses to run
 * (exit 2): the built-in types have no importer there and would be copied raw.
 */
#ifndef DE_COOK_COOK_CMD_H
#define DE_COOK_COOK_CMD_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_reflect.h>

#include "check.h"

DE_EXTERN_C_BEGIN

/** Lets a caller of de_cook_cook_run register types before anything is cooked. */
typedef struct de_cook_hooks {
    /**
     * Called once, after the tool's type registry (with the ECS built-in components) and asset
     * system (with the built-in asset types) exist, after the `--game` library's register_types
     * (if any), and before the cooker copies the asset types:
     * register component types into `types` and asset types (with their importers) into `s`.
     * A result other than DE_OK stops the command with exit code 2.
     */
    de_result (*register_types)(void *user, de_asset_system *s, de_type_registry *types);
    void *user;
} de_cook_hooks;

/**
 * Runs `de-cook cook` with the arguments that follow `cook` (`argv[0]` is the manifest or the
 * first option; `argc` may be 0). Writes through `io`. `hooks` may be NULL.
 * Not reentrant: it mounts the process-wide VFS roots `cook`, `cookinc`, `cookcache`, `cookout`
 * and `cookmk` (and removes them before it returns), and the calling thread becomes the main
 * thread of a task system for the run (so it must not be one already) unless `--jobs 1`.
 * @return a DE_COOK_EXIT_* code.
 */
int de_cook_cook_run(int argc, const char *const *argv, const de_cook_io *io,
                     const de_cook_hooks *hooks);

/** de_cook_cook_run without hooks: the command-table entry of main.c. */
int de_cook_cook_main(int argc, const char *const *argv, const de_cook_io *io);

/** Writes the usage text of `cook` to `stream`. */
void de_cook_cook_usage(const de_cook_io *io, de_cook_stream stream);

DE_EXTERN_C_END

#endif /* DE_COOK_COOK_CMD_H */
