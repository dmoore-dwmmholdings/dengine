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
 *   cell_<cx>_<cy>.proxy.gltf   the cell's far proxy mesh (DENG-830), when the world's
 *                               [worlds.<name>.proxy] is enabled (the default) and the cell
 *                               draws at least one mesh; see world_proxy.h
 *   terrain_<id>_<tx>_<ty>.tile one per tile of each DeTerrain (DENG-857): its heights, holes
 *                               and splat (format in de_asset.h, "World streaming")
 *   xref_<cx>_<cy>.xref         the cell's cross-cell reference table (DENG-858), when one of
 *                               its entities references an entity of another cell or is
 *                               referenced from one (format in streaming_internal.h)
 *   cells.toml                  [assets] and [bundles] for every cell (and, per proxy, a model
 *                               asset and bundle "world/<name>/proxy_<cx>_<cy>" whose mesh is
 *                               "#proxy"; per tile, a blob asset and bundle
 *                               "world/<name>/terrain_<id>_<tx>_<ty>", in the `depends` of every
 *                               cell bundle it overlaps); include it from the project manifest
 *
 * A component field that holds the scene ID (a "0x.." string) of a placed entity in another cell
 * is a cross-cell reference: the cook reports it (a note), writes "0x0" in the cell scene and
 * records it, with the world scene IDs of both entities, in the source cell's reference table, a
 * blob asset "world/<name>/xref_<cx>_<cy>" in the cell's bundle. The world partition resolves it
 * while both cells are loaded. A reference nested in an array or table is warned and dropped.
 *
 * A cell that a terrain tile overlaps but no entity is in gets an empty scene. A malformed
 * DeTerrain (resolution not 33, arrays of the wrong length or range) is an error; a tile wider
 * than 9 cells is refused.
 *
 * The proxy keeps `triangle_ratio` of the cell's triangles; the partition draws it while the cell
 * is not loaded and crossfades it over `fade_seconds` (world_partition.c). A mesh the baker
 * cannot read is left out of the proxy with a warning.
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
