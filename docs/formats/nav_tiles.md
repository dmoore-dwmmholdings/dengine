# `nav.tiles` baked navmesh tiles format ("DNVT")

The editor's baked navmesh tiles, per agent type (DENG-864). This file is the byte-level
reference. The code must match it byte for byte:

- Format header (structs, constants, functions): `src/gameplay/nav/nav_build.h`, section
  "Editor bake file DNVT". It is internal and shared by the editor and the runtime.
- Encoder and decoder: `src/gameplay/nav/nav_tiles.c` (target `de_nav_build`).
- Editor writer and loader: `src/host/editor/editor_nav.c` (`save_tiles`, `load_tiles`).
- Runtime loader: `de_nav_settings_load_tiles` (`include/dengine/de_gameplay.h`).

Current version: **1**.

## File name and location

The file sits beside the project nav settings file `nav.toml` (DENG-833). Its name is the name of
the settings file with `.toml` replaced by `.tiles` (`de__nav_tiles_path`). A settings path
without `.toml` gets `.tiles` appended. For the default project layout the files are:

```
<project>/nav.toml     settings: agent types, areas, bounds, tile size, auto rebake
<project>/nav.tiles    baked tiles of every agent type, with their fingerprints
```

There is one `nav.tiles` per project, not one per scene.

## When it is written and read

- The editor writes the file when a bake ends: when it is done, canceled or failed. The file
  holds every baked tile of every agent type at that time. The write is atomic
  (`de_file_write_all_atomic`).
- The editor reads the file when it gets a new world or a new project (register, a manifest path
  change). It reads only into a world with no baked tile and no bake running.
  - It loads an agent block only into the agent type with the same name, the same grid (`bmin_x`,
    `bmin_y`, `tile_size`, `tiles_x`, `tiles_y`) and the same build settings (the DNAV header
    desc). Other blocks are ignored.
  - Each loaded tile keeps its saved fingerprint. A tile whose inputs differ in this world shows
    DIRTY, and auto rebake (or a dirty-only bake) rebakes it as usual.
  - A missing file is quiet. A malformed file is logged and ignored; the next bake rewrites it.
- The game runtime reads the file with `de_nav_settings_load_tiles(nav, vpath, agent)`. This
  adds the DNAV tiles of the named agent type to `nav`, in place of a runtime bake. It returns
  the read error (`DE_ERR_NOT_FOUND`) for a missing file, `DE_ERR_PARSE` for a malformed file,
  and `DE_ERR_NOT_FOUND` for an unknown agent type. The fingerprints are not used at runtime.

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

The fingerprint is the editor's hash of the inputs that can reach the tile (triangles, modifiers
and links whose box meets the tile, plus the agent and area settings). It is not a stable
format: when the editor changes how it computes fingerprints, the saved tiles only show DIRTY
and rebake.

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
