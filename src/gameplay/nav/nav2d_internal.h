/* nav2d_internal.h: internals of 2D navigation (DENG-79; nav2d_grid.c, nav2d_mesh.c). SPEC §21.2.
 * Exposed to tests/unit/nav/ and tests/determinism/nav/ only. */
#ifndef DE_NAV2D_INTERNAL_H
#define DE_NAV2D_INTERNAL_H

#include <dengine/de_gameplay.h>
#include <dengine/de_memory.h>

DE_EXTERN_C_BEGIN

/* Grid step costs in fixed point: straight 1.0, diagonal 1.4142 (exact integer sums, so A* and
 * JPS compare costs without rounding). */
#define DE__NAV2D_COST_STRAIGHT 10000u
#define DE__NAV2D_COST_DIAGONAL 14142u

/** Statistics of one grid search. */
typedef struct de__nav2d_grid_stats {
    u32 expanded; /* nodes popped from the open set */
    u64 cost;     /* path cost in DE__NAV2D_COST_* units; 0 when no path */
} de__nav2d_grid_stats;

/** de_nav2d_find_path that also reports statistics (`stats` may be NULL). */
de_result de__nav2d_find_path_stats(const de_nav2d_grid *grid, de_vec2 from, de_vec2 to,
                                    bool jump_point_search, const de_allocator *a,
                                    de_vec2 **points, u32 *count, de__nav2d_grid_stats *stats);

/** A triangle of the 2D navmesh: CCW vertex ids, and the neighbour across edge k (v[k] to
 *  v[(k + 1) % 3]), or UINT32_MAX on the boundary. */
typedef struct de__nav2d_tri {
    u32 v[3];
    u32 adj[3];
} de__nav2d_tri;

/** Uniform grid over the mesh bounds. Each cell lists the triangle ids (`tri_*`) and the
 *  boundary-edge ids (`edge_*`) whose padded extent touches it, ascending (CSR: the cell c holds
 *  idx[off[c] .. off[c + 1])). Built with the mesh (DENG-860). */
typedef struct de__nav2d_grid_index {
    f64 x0, y0, sx, sy, pad; /* origin, cell size, conservative padding */
    u32 gx, gy;              /* cells per axis; 0 when the mesh is empty */
    u32 *tri_off, *tri_idx;
    u32 *edge_off, *edge_idx;
    u32 tri_idx_count, edge_idx_count;
} de__nav2d_grid_index;

struct de_nav2d_mesh {
    const de_allocator *alloc;
    bool brute; /* tests: linear scans instead of the grid (same results) */
    de__nav2d_grid_index gi;
    de_vec2 *verts; /* unique vertices, sorted by (x, y) */
    u32 vert_count;
    de__nav2d_tri *tris;
    u32 tri_count;
    u32 *bedges; /* boundary edges as vertex pairs, interior on the left */
    u32 bedge_count;
    u32 *corners; /* vertex ids of the corners (reflex or pinched boundary vertices) */
    u32 corner_count;
    u32 *vis_off; /* corner_count + 1 offsets into vis_nbr */
    u32 *vis_nbr; /* corners visible from each corner, ascending */
    u32 vis_count;
};

/** de_nav2d_mesh_build with the spatial index switched off (`brute`: every locate and
 *  line-of-sight query is a linear scan). The reference for the equality tests and benchmark. */
de_result de__nav2d_mesh_build_opts(const de_nav2d_mesh_desc *desc, const de_allocator *a,
                                    bool brute, de_nav2d_mesh **out);

/** Index of the first triangle that contains `p` (boundary included), or UINT32_MAX. */
u32 de__nav2d_mesh_locate(const de_nav2d_mesh *m, de_vec2 p);

/** Allocator resolution: NULL means de_heap_allocator(). */
static inline const de_allocator *de__nav2d_alloc(const de_allocator *a) {
    return a != NULL ? a : de_heap_allocator();
}

DE_EXTERN_C_END

#endif
