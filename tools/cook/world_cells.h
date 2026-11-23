/*
 * world_cells.h: `de-cook world` (DENG-271). Splits the scenes of one [worlds.<name>] world into
 * 256 m streaming cells.
 *
 *   de-cook world <assets.toml> --world <name> [--out <dir>]
 *
 * Reads the manifest (and its includes), then every scene the world lists, in the scene TOML
 * source format (keyed `[entity.0x..]` or `[[entity]]` layout). Each root entity goes to the
 * cell of its DeTransform position, its position made relative to the cell origin; children go
 * with their root. A root without DeTransform is left out with a warning, and so is the editor
 * `[prefab]` table. Writes, into `--out` (default: <manifest dir>/<asset_root>/<cells_dir>):
 *
 *   cell_<cx>_<cy>.scene.toml   one per non-empty cell, keyed layout, entities by ID, keys sorted
 *   cells.toml                  [assets] and [bundles] for every cell; include it from the
 *                               project manifest
 *
 * The output depends only on the input bytes (no time, no absolute path): cooking twice gives
 * byte-identical files. Sources in cells.toml are `<cells_dir>/cell_<cx>_<cy>.scene.toml`, so
 * `--out` should be the default for a project build (tests may write elsewhere).
 *
 * A missing include (for example the cells.toml of the first cook) is not an error here.
 */
#ifndef DE_COOK_WORLD_CELLS_H
#define DE_COOK_WORLD_CELLS_H

#include "check.h"

/** The `world` command. `argv` holds the arguments after "world". Returns a DE_COOK_EXIT_* code. */
int de_cook_world_main(int argc, const char *const *argv, const de_cook_io *io);

#endif /* DE_COOK_WORLD_CELLS_H */
