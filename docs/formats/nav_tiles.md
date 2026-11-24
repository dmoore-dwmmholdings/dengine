# `nav.tiles` baked navmesh tiles format ("DNVT")

The editor's baked navmesh tiles, per agent type (DENG-864), one file per scene (DENG-877). This file is the byte-level
reference. The code must match it byte for byte:

- Format header (structs, constants, functions): `src/gameplay/nav/nav_build.h`, section
  "Editor bake file DNVT". It is internal and shared by the editor and the runtime.
- Encoder and decoder: `src/gameplay/nav/nav_tiles.c` (target `de_nav_build`).
- Editor writer and loader: `src/host/editor/editor_nav.c` (`save_tiles`, `load_tiles`).
- Runtime loader: `de_nav_settings_load_tiles` (`include/dengine/de_gameplay.h`).
- Runtime check and file name: `de_nav_settings_check_tiles`, `de_nav_scene_tiles_path`
  (`include/dengine/de_gameplay.h`). Fingerprints: `de__nav_fp_seed`, `de__nav_fp_tiles`
  (`nav_tiles.c`), shared by the editor and the runtime check.

Current version: **1**.

## File name and location

The file is keyed by scene (DENG-877). A scene opened from a file has its tiles file beside it:
the scene path with `.scene` replaced by `.nav.tiles` (`de_nav_scene_tiles_path`). A path without
`.scene` gets `.nav.tiles` appended. The editor places a project-relative scene path (as DIDE
sends it, for example `scenes/main.scene`) below the root manifest's folder.

The legacy project-level file (DENG-864) sits beside the project nav settings file `nav.toml`
(DENG-833). Its name is the name of the settings file with `.toml` replaced by `.tiles`
(`de__nav_tiles_path`). A settings path without `.toml` gets `.tiles` appended. It is the file of
the game scene (no scene file), and the fallback for a scene that has no tiles file of its own.
For the default project layout the files are:

```
<project>/nav.toml               settings: agent types, areas, bounds, tile size, auto rebake
<project>/nav.tiles              legacy: baked tiles of the game scene, and the fallback
<project>/scenes/main.scene      a scene
<project>/scenes/main.nav.tiles  baked tiles of scenes/main.scene
```

The settings (`nav.toml`) stay one per project, so every scene's file uses the same grid and
agent types. The format of the two kinds of file is the same.

## When it is written and read

- The editor keys the file by the active scene: the active scene's tiles file, or the legacy
  file when the active scene is the game scene. Additive scenes share one world, so the file of
  the active scene holds the tiles of every loaded scene.
- The editor writes the file of the current key when a bake ends: when it is done, canceled or
  failed. The file holds every baked tile of every agent type at that time. The write is atomic
  (`de_file_write_all_atomic`). The editor never deletes or rewrites the legacy file for a scene
  that has a file.
- When the active scene changes and no bake runs, the editor drops the baked tiles (the bake
  already saved them) and reads the new key's file. During a bake the switch waits, so the bake
  saves to the key it started under.
- The editor reads the file when it gets a new world, a new project (register, a manifest path
  change) or a new active scene. It reads only into a world with no baked tile and no bake
  running. When the scene's file is missing it reads the legacy file instead; its tiles whose
  inputs differ show DIRTY, and the next bake writes the scene's file.
  - It loads an agent block only into the agent type with the same name, the same grid (`bmin_x`,
    `bmin_y`, `tile_size`, `tiles_x`, `tiles_y`) and the same build settings (the DNAV header
    desc). Other blocks are ignored.
  - Each loaded tile keeps its saved fingerprint. A tile whose inputs differ in this world shows
    DIRTY, and auto rebake (or a dirty-only bake) rebakes it as usual.
  - A missing file is quiet. A malformed file is logged and ignored; the next bake rewrites it.
- The game runtime reads the file with `de_nav_settings_load_tiles(nav, vpath, agent)`. This
  adds the DNAV tiles of the named agent type to `nav`, in place of a runtime bake. It returns
  the read error (`DE_ERR_NOT_FOUND`) for a missing file, `DE_ERR_PARSE` for a malformed file,
  and `DE_ERR_NOT_FOUND` for an unknown agent type. A game loads the file of the scene it opens
  (`de_nav_scene_tiles_path`), and the legacy file when that one is missing.
- The runtime check `de_nav_settings_check_tiles(nav, settings, vpath, agent, &check)` compares
  the file with the settings and with the inputs of `nav`'s world (DENG-877):
  - `grid_match`: the block's grid equals the grid of `settings` (bounds min x and y, tile size,
    `ceil(extent / tile_size)` tiles per axis) and the DNAV desc equals
    `de_nav_settings_build_desc`.
  - `stale`: saved tiles whose fingerprint differs from the fingerprint of the current inputs
    (every saved tile when the grid does not match). `missing`: grid tiles with no record.
  - The file matches the world when `grid_match` is true and `stale` is 0. The check reads the
    inputs that `nav` has (world colliders, obstacles, volumes, links): a game that adds other
    links or modifiers than the editor did sees those tiles as stale.
  - It returns the read error for a missing file, `DE_ERR_PARSE` for a malformed file,
    `DE_ERR_NOT_FOUND` when the file has no block for the agent type, and
    `DE_ERR_INVALID_ARGUMENT` when the settings are invalid or have no such agent type.

## Conventions

- All integers and floats are little-endian. Floats are IEEE 754.
- Records have fixed sizes and no implicit padding. Every padding and reserved byte is zero.
- The file has no trailing bytes after the last agent block.

## File layout

```
offset 0      header (16 bytes)
16            agent_count agent blocks, back to back; each starts on an 8-byte boundary
file_size     end of the last agent block
```

## Header (16 bytes)

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | u32 | magic | `'DNVT'`, 0x54564E44 |
| 4 | u32 | version | 1 |
| 8 | u32 | agent_count | number of agent blocks, at most 16 |
| 12 | u32 | reserved | 0 |

## Agent block

An 80-byte head, then `record_count` tile records, then the DNAV container.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | char[40] | name | agent type name; not empty, NUL-terminated, NUL-padded, unique in the file |
| 40 | f64 | bmin_x | world X of the grid's minimum corner (m) |
| 48 | f64 | bmin_y | world Y of the grid's minimum corner (m) |
| 56 | f32 | tile_size | tile edge (m); greater than 0 |
| 60 | u32 | tiles_x | grid width in tiles; at least 1 |
| 64 | u32 | tiles_y | grid height in tiles; at least 1; `tiles_x * tiles_y` at most 4096 |
| 68 | u32 | record_count | number of tile records; at most 4096 |
| 72 | u64 | dnav_size | bytes of the DNAV container; 0 when no tile has Detour data |

### Tile record (24 bytes)

One record per baked tile of the editor's grid. Records are sorted by `tile`, strictly
increasing.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | u32 | tile | grid index `y * tiles_x + x`; less than `tiles_x * tiles_y` |
| 4 | u32 | polys | polygon count of the baked tile |
| 8 | u32 | links | off-mesh link count of the baked tile |
| 12 | u32 | reserved | 0 |
| 16 | u64 | fp | the editor's input fingerprint of the tile when it was baked |

A tile that baked with no walkable polygon has a record but no tile in the DNAV container.

The fingerprint is a hash of the inputs that can reach the tile (triangles, modifiers and links
whose box meets the tile, plus the agent and area settings), computed by `de__nav_fp_tiles`:

- The seed is FNV-1a 64 over the agent's six f32 settings (radius, height, max slope, step
  height, cell size, cell height), its u32 collision mask, the f32 tile size, then the u32 id and
  f32 cost of each area in id order. Every tile starts at the seed.
- Each input (in Recast space, origin 0) is hashed with FNV-1a 64 over all its floats (a
  triangle's 9 vertex floats; an obstacle's, area volume's or link's soup record). The hash is
  folded (`h = (h ^ e) * 0x100000001b3`) into every tile that its xz box meets, grown by
  `radius + 4 * cell_size`. The box of a triangle is its 3 corners, of an obstacle or area
  volume its 4 corners, and of a link the xz of its start. The order is triangles,
  obstacles, area volumes, links.

The editor and the runtime check share this code, so files written by DENG-864 keep their
fingerprints. It is still not a stable format: when the computation changes, the saved tiles
only show DIRTY and rebake.

### DNAV container

Then `dnav_size` bytes of a DNAV v1 container: the same format as the cooked `navmesh` asset
(`nav_build.h`, section "Cooked container DNAV"). It holds the Detour data of the agent's tiles
that have data, with the agent's build settings and the navmesh origin in its header. Its
`tile_size` must equal the block's `tile_size`. Zero padding follows, to the next 8-byte
boundary.

Detour tile data is the target's native layout, so the file is not portable between targets
with a different Detour layout (every current target is little-endian with 64-bit poly refs).

## Validation

`de__nav_tiles_read` refuses a file, with a message that starts with `nav.tiles: `, when:

- the header is truncated, the magic or the version is wrong, or `agent_count` is over 16;
- an agent block is truncated, or its name is empty, not NUL-terminated or a duplicate;
- the grid is empty or over 4096 tiles, or `tile_size` is not greater than 0;
- a record is out of the grid or out of order;
- a DNAV container is malformed (the DNAV reader's message), or its `tile_size` differs;
- bytes follow the last agent block.

## Version history

- 1 (DENG-864): first version.
- 1 (DENG-877): no byte change. The file is keyed by scene (`<scene>.nav.tiles`); the
  project-level `nav.tiles` stays as the legacy file and the fallback. The runtime check
  `de_nav_settings_check_tiles` reads the fingerprints.
