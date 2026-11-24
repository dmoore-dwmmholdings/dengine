/*
 * nav_build.h: navmesh build code shared by the runtime (de_nav) and the cooker's `navmesh`
 * importer (DENG-76). Target de_nav_build: this C code, the Recast / Detour bridge and the
 * vendored library. Internal, not installed.
 *
 * Coordinate mapping (SPEC §5.5: the engine is Z up, right-handed; Recast is Y up):
 *   Recast (x, y, z) = engine (x, z, -y)      engine (x, y, z) = Recast (x, -z, y)
 * This is a rotation (determinant +1), so it keeps triangle winding: a face counter-clockwise
 * seen from +Z in the engine is counter-clockwise seen from +Y in Recast. Positions are relative
 * to the navmesh origin, an f64 engine-space point on the 256 m world cell grid, so the f32 values
 * Recast sees stay small. Tile (tx, tz) covers Recast x in [tx * T, (tx + 1) * T) and Recast z in
 * [tz * T, (tz + 1) * T), T = tile_size: engine x from origin.x + tx * T, engine y from
 * origin.y - (tz + 1) * T to origin.y - tz * T.
 */
#ifndef DE_GAMEPLAY_NAV_BUILD_H
#define DE_GAMEPLAY_NAV_BUILD_H

#include "gameplay/nav/nav_recast_bridge.h"

#include <dengine/de_base.h>
#include <dengine/de_gameplay.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>

DE_EXTERN_C_BEGIN

/** World cell size (m): tile_size must divide it into whole tiles, and origins sit on its grid. */
#define DE__NAV_CELL_M 256.0

/* ---- Coordinate mapping ---------------------------------------------------------------------- */

/** Engine-space offset from the origin -> Recast space. Exact (a permutation and a negation). */
static inline void de__nav_to_recast(de_vec3 e, f32 out[3]) {
    out[0] = e.x;
    out[1] = e.z;
    out[2] = -e.y;
}
/** Recast space -> engine-space offset from the origin. Exact inverse of de__nav_to_recast. */
static inline de_vec3 de__nav_from_recast(const f32 r[3]) {
    de_vec3 e;
    e.x = r[0];
    e.y = -r[2];
    e.z = r[1];
    return e;
}
/** A world point -> Recast space relative to `origin`. */
static inline void de__nav_world_to_recast(const de_dvec3 *origin, de_dvec3 p, f32 out[3]) {
    de_vec3 e;
    e.x = (f32)(p.x - origin->x);
    e.y = (f32)(p.y - origin->y);
    e.z = (f32)(p.z - origin->z);
    de__nav_to_recast(e, out);
}
/** Recast space relative to `origin` -> a world point. */
static inline de_dvec3 de__nav_recast_to_world(const de_dvec3 *origin, const f32 r[3]) {
    de_vec3 e = de__nav_from_recast(r);
    de_dvec3 p;
    p.x = origin->x + (f64)e.x;
    p.y = origin->y + (f64)e.y;
    p.z = origin->z + (f64)e.z;
    return p;
}

/* ---- Settings -------------------------------------------------------------------------------- */

/** A checked build desc and the Recast values derived from it. */
typedef struct de__nav_settings {
    de_navmesh_build_desc desc;
    i32 tile_cells;      /* tile_size / cell_size */
    i32 walkable_height; /* voxels */
    i32 walkable_climb;  /* voxels */
    i32 walkable_radius; /* cells */
    i32 border;          /* walkable_radius + 3 cells */
    f32 slope_deg;
} de__nav_settings;

/**
 * Checks `d` and derives the Recast values. Rules: every size finite and > 0 (agent_radius and
 * agent_max_climb >= 0), agent_max_slope_rad in (0, pi/2), tile_size / cell_size a whole number
 * of cells from 8 to 1024, and 256 / tile_size a whole number of tiles.
 * @return DE_OK or DE_ERR_INVALID_ARGUMENT (with a message in `*why`).
 */
de_result de__nav_settings_make(const de_navmesh_build_desc *d, de__nav_settings *out,
                                const char **why);

/** The origin for a navmesh first built around `center`: x and y down to the 256 m cell grid,
 *  z = 0. */
de_dvec3 de__nav_origin_for(de_dvec3 center);

/** Inclusive tile range of the Recast-space xz box [rmin, rmax]. */
void de__nav_tile_range(f32 tile_size, const f32 rmin[3], const f32 rmax[3], i32 *tx0, i32 *tz0,
                        i32 *tx1, i32 *tz1);

/** An engine-space world box (f64 corners) -> a Recast-space box relative to `origin`. */
void de__nav_box_to_recast(const de_dvec3 *origin, de_dvec3 wmin, de_dvec3 wmax, f32 rmin[3],
                           f32 rmax[3]);

/* ---- Triangle soup --------------------------------------------------------------------------- */

/** Input triangles in Recast space relative to an origin. Grows from `alloc`. */
typedef struct de__nav_soup {
    const de_allocator *alloc;
    de_dvec3 origin;
    f32 *verts; /* 3 per vertex */
    i32 *tris;  /* 3 per triangle */
    u32 vert_count, vert_cap;
    u32 tri_count, tri_cap;
    bool clip;            /* keep only triangles whose xz box meets [clip_min, clip_max] */
    f32 clip_min[3], clip_max[3];
    bool failed;          /* an allocation failed; the soup is incomplete */
    f32 *obstacles;       /* DE__NAV_RC_OBSTACLE_FLOATS per obstacle */
    u32 obstacle_count, obstacle_cap;
    f32 *offmesh;         /* DE__NAV_RC_OFFMESH_FLOATS per off-mesh link (DENG-77) */
    u32 offmesh_count, offmesh_cap;
    f32 *area_vols;       /* DE__NAV_RC_AREA_VOL_FLOATS per area volume (DENG-582) */
    u32 area_vol_count, area_vol_cap;
} de__nav_soup;

/** A pose: position and unit rotation quaternion (x, y, z, w), f64. Scale is not used. */
typedef struct de__nav_pose {
    de_dvec3 pos;
    f64 q[4];
} de__nav_pose;

void de__nav_soup_init(de__nav_soup *s, const de_allocator *a, de_dvec3 origin);
void de__nav_soup_free(de__nav_soup *s);
/** Adds a world-space triangle (counter-clockwise seen from outside / above). */
void de__nav_soup_add_tri(de__nav_soup *s, de_dvec3 a, de_dvec3 b, de_dvec3 c);
/** Appends another soup's triangles (same origin). */
void de__nav_soup_append(de__nav_soup *s, const de__nav_soup *src);
/** Composes poses: world = parent * child. */
de__nav_pose de__nav_pose_mul(const de__nav_pose *parent, const de__nav_pose *child);
de__nav_pose de__nav_pose_from(const de_transform *t);
/** Applies a pose to a local point. */
de_dvec3 de__nav_pose_apply(const de__nav_pose *p, de_vec3 v);
/* Primitive surfaces, placed with `p`. Round shapes use 8 sides and contain the true shape. */
void de__nav_soup_add_box(de__nav_soup *s, const de__nav_pose *p, de_vec3 half_extents);
void de__nav_soup_add_sphere(de__nav_soup *s, const de__nav_pose *p, f32 radius);
/** Capsule along local Z: segment of half length `half_height`, then hemispheres. */
void de__nav_soup_add_capsule(de__nav_soup *s, const de__nav_pose *p, f32 radius, f32 half_height);
/** A box obstacle: its footprint (the box's xz outline in Recast space, from its 8 corners
 *  bounding rectangle when rotated off the vertical) is carved, inflated by the agent radius. */
void de__nav_soup_add_obstacle(de__nav_soup *s, const de__nav_pose *p, de_vec3 half_extents);
/** An off-mesh link between two world points (DENG-77); every tile build gets it, Detour keeps
 *  it in the tile that holds `from`. */
void de__nav_soup_add_offmesh(de__nav_soup *s, de_dvec3 from, de_dvec3 to, f32 radius,
                              bool bidirectional, u8 area, u32 id);
/** An area volume (DENG-582): the walkable cells in the box's footprint (as an obstacle's) and
 *  height range get `area` (1..15). */
void de__nav_soup_add_area_volume(de__nav_soup *s, const de__nav_pose *p, de_vec3 half_extents,
                                  u8 area);
/** The convex hull of `count` local points (brute force; at most 64 points). */
void de__nav_soup_add_hull(de__nav_soup *s, const de__nav_pose *p, const de_vec3 *points,
                           u32 count);

/* ---- Tile build ------------------------------------------------------------------------------ */

/**
 * Builds tile (tx, tz) of `soup` (the triangles that meet the tile and its border). `*data` NULL
 * means the tile has no walkable polygon. Free a tile with de__nav_rc_free.
 */
de_result de__nav_build_tile(const de_allocator *a, const de__nav_settings *s,
                             const de__nav_soup *soup, i32 tx, i32 tz, u8 **data, i32 *size);

/* ---- Cooked container "DNAV" ----------------------------------------------------------------- */
/*
 * Little-endian:
 *   u32 magic 'DNAV' (0x56414E44), u32 version (1), u32 tile_count, u32 reserved (0)
 *   f64 origin x, y, z
 *   f32 tile_size, cell_size, cell_height, agent_radius, agent_height, agent_max_climb,
 *       agent_max_slope_rad
 *   u32 collision_mask
 *   tile_count x { i32 tx, i32 tz, u32 size, size bytes of Detour tile data, zero padding to 4 }
 * Tiles are sorted by (tz, tx). Detour tile data is the target's native layout (every target is
 * little-endian, 64-bit poly refs: DT_POLYREF64).
 */
#define DE__NAV_MAGIC   0x56414E44u
#define DE__NAV_VERSION 1u
#define DE__NAV_HEADER_SIZE 72u

typedef struct de__nav_container_info {
    de_dvec3 origin;
    de_navmesh_build_desc desc;
    u32 tile_count;
} de__nav_container_info;

typedef struct de__nav_tile_blob {
    i32 tx, tz;
    const u8 *data;
    u32 size;
} de__nav_tile_blob;

/** Writes a container; `*out` comes from `a` (free with size `*out_size`). Sorts `tiles`. */
de_result de__nav_container_write(const de_allocator *a, const de__nav_container_info *info,
                                  de__nav_tile_blob *tiles, u32 tile_count, u8 **out,
                                  u64 *out_size);
/** Checks a container and reads its header. false with a message in `*why` when malformed. */
bool de__nav_container_read(const u8 *data, u64 size, de__nav_container_info *info,
                            const char **why);
/** Tile `index` of a container that de__nav_container_read accepted (walks from the start). */
bool de__nav_container_tile(const u8 *data, u64 size, u32 index, de__nav_tile_blob *out);

/* ---- Editor bake file "DNVT" (DENG-864, docs/formats/nav_tiles.md) ------------------------- */
/*
 * The editor's baked tiles, one file `nav.tiles` beside the project's `nav.toml`. Little-endian:
 *   u32 magic 'DNVT' (0x54564E44), u32 version (1), u32 agent_count, u32 reserved (0)
 *   agent_count x {
 *     char name[40] (NUL-terminated, NUL padded), f64 bmin_x, f64 bmin_y, f32 tile_size,
 *     u32 tiles_x, u32 tiles_y, u32 record_count, u64 dnav_size
 *     record_count x { u32 tile (y * tiles_x + x), u32 polys, u32 links, u32 reserved, u64 fp }
 *     dnav_size bytes of a DNAV container (0 = no tile has data), zero padding to 8
 *   }
 * Records are sorted by tile, one per baked tile of the editor's grid (tiles_x * tiles_y <= 4096).
 */
#define DE__NAV_TILES_MAGIC      0x54564E44u
#define DE__NAV_TILES_VERSION    1u
#define DE__NAV_TILES_NAME       40u
#define DE__NAV_TILES_MAX_TILES  4096u
#define DE__NAV_TILES_MAX_AGENTS 16u

typedef struct de__nav_tile_record {
    u32 tile, polys, links;
    u64 fp;
} de__nav_tile_record;

typedef struct de__nav_tiles_agent {
    char name[DE__NAV_TILES_NAME];
    f64 bmin_x, bmin_y;
    f32 tile_size;
    u32 tiles_x, tiles_y;
    u32 record_count;
    const de__nav_tile_record *records; /* write: the records, sorted by tile */
    const u8 *record_data;              /* read: the records in the file (de__nav_tiles_record) */
    const u8 *dnav;                     /* the DNAV container, or NULL */
    u64 dnav_size;
} de__nav_tiles_agent;

/** Writes a DNVT file; `*out` comes from `a` (free with size `*out_size`). */
de_result de__nav_tiles_write(const de_allocator *a, const de__nav_tiles_agent *agents, u32 count,
                              u8 **out, u64 *out_size);
/** Checks a DNVT file (each DNAV container included). false with a message in `*why`. */
bool de__nav_tiles_read(const u8 *data, u64 size, u32 *agent_count, const char **why);
/** Agent `index` of a file that de__nav_tiles_read accepted (points into `data`). */
bool de__nav_tiles_agent_at(const u8 *data, u64 size, u32 index, de__nav_tiles_agent *out);
/** Record `k` of an agent from de__nav_tiles_agent_at. */
void de__nav_tiles_record(const de__nav_tiles_agent *ag, u32 k, de__nav_tile_record *out);
/** The tiles file beside settings file `toml_vpath`: ".toml" becomes ".tiles" (else appended).
 *  DENG-877: the legacy project-level file, read when a scene has no tiles file of its own. */
bool de__nav_tiles_path(const char *toml_vpath, char *out, size_t cap);

/* ---- DENG-877: tile input fingerprints (the editor's and de_nav_settings_check_tiles') ---- */

/** The editor's tile grid: tile (x, y) covers bmin + [x, y] * tile_size along world X and Y. */
typedef struct de__nav_fp_grid {
    f64 bmin_x, bmin_y;
    f32 tile_size;
    u32 tiles_x, tiles_y;
} de__nav_fp_grid;

/** The grid of settings `s`: bounds min x and y, the tile size, ceil(extent / tile size) tiles
 *  (at least 1) per axis, as the editor makes it. */
void de__nav_fp_grid_of(const de_nav_settings *s, de__nav_fp_grid *out);
/** The fingerprint every tile starts from: agent `a`'s settings, the tile size and the areas
 *  (`areas`, `area_count`, sorted by id, as the editor keeps them). */
u64 de__nav_fp_seed(const de_nav_agent_settings *a, f32 tile_size,
                    const de_nav_area_settings *areas, u32 area_count);
/** Fills `fp` (tiles_x * tiles_y entries, y * tiles_x + x) with the input fingerprint of every
 *  tile of `g`: `seed`, then each triangle, obstacle, area volume and off-mesh link of `soup`
 *  (origin 0, unclipped) whose xz box, grown by `a`'s radius and 4 cells, meets the tile. This is
 *  the `fp` of the nav.tiles records (docs/formats/nav_tiles.md). */
void de__nav_fp_tiles(const de__nav_fp_grid *g, const de_nav_agent_settings *a, u64 seed,
                      const de__nav_soup *soup, u64 *fp);

/* ---- Source format (the `navmesh` importer) -------------------------------------------------- */

/**
 * Cooks a `.navmesh` TOML source (see tools/cook/importers/navmesh/navmesh_import.h) to a DNAV
 * container from `a`. On failure `*why` names the problem (static text or `why_buf`).
 */
de_result de__nav_cook_source(const de_allocator *a, const char *src, u64 len, u8 **out,
                              u64 *out_size, char *why_buf, size_t why_cap);

DE_EXTERN_C_END

#endif /* DE_GAMEPLAY_NAV_BUILD_H */
