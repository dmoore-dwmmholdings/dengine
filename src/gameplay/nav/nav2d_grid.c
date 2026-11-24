/* nav2d_grid.c: 2D grid path finding, A* and Jump Point Search (DENG-79). SPEC §21.2, §5.7.
 *
 * 8-neighbour moves, no corner cutting (a diagonal step needs both orthogonal cells walkable).
 * Costs are fixed-point integers (DE__NAV2D_COST_*) and the octile heuristic is computed in the
 * same units, so every comparison is exact and the result is bit-identical on every machine.
 * The open set is a binary heap ordered by f, then h, then cell index. All scratch is one block
 * from the caller's allocator, sized from the grid and freed before return.
 *
 * JPS follows Harabor and Grastien's pruning rules for the no-corner-cutting move model: a
 * straight jump stops where a side cell is open but the cell behind it is blocked (that side
 * cell can only be reached through this one); a diagonal jump stops where one of its two
 * straight jumps finds something. Straight nodes always try their two side directions, which
 * keeps the search optimal for this move model. */
#include "gameplay/nav/nav2d_internal.h"

#include <string.h>

#define NONE UINT32_MAX

enum { ST_NEW = 0, ST_OPEN = 1, ST_CLOSED = 2 };

typedef struct grid_search {
    const de_nav2d_grid *grid;
    i32 w, h;
    i32 gx, gy; /* goal cell */
    u64 *g;
    u32 *parent;
    u32 *heap_pos; /* cell -> index in heap */
    u32 *heap;
    u32 heap_count;
    u8 *state;
} grid_search;

static bool walkable(const grid_search *s, i32 x, i32 y) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h)
        return false;
    return s->grid->walkable[(size_t)y * (size_t)s->w + (size_t)x] != 0;
}

static u64 octile(i32 ax, i32 ay, i32 bx, i32 by) {
    u64 dx = (u64)(ax > bx ? ax - bx : bx - ax);
    u64 dy = (u64)(ay > by ? ay - by : by - ay);
    const u64 lo = dx < dy ? dx : dy, hi = dx < dy ? dy : dx;
    return DE__NAV2D_COST_STRAIGHT * (hi - lo) + DE__NAV2D_COST_DIAGONAL * lo;
}

static u64 heur(const grid_search *s, u32 c) {
    return octile((i32)(c % (u32)s->w), (i32)(c / (u32)s->w), s->gx, s->gy);
}

/* Heap order: f, then h, then cell index. */
static bool before(const grid_search *s, u32 a, u32 b) {
    const u64 ha = heur(s, a), hb = heur(s, b);
    const u64 fa = s->g[a] + ha, fb = s->g[b] + hb;
    if (fa != fb)
        return fa < fb;
    if (ha != hb)
        return ha < hb;
    return a < b;
}

static void heap_set(grid_search *s, u32 i, u32 c) {
    s->heap[i] = c;
    s->heap_pos[c] = i;
}

static void sift_up(grid_search *s, u32 i) {
    const u32 c = s->heap[i];
    while (i > 0) {
        const u32 p = (i - 1u) / 2u;
        if (!before(s, c, s->heap[p]))
            break;
        heap_set(s, i, s->heap[p]);
        i = p;
    }
    heap_set(s, i, c);
}

static void sift_down(grid_search *s, u32 i) {
    const u32 c = s->heap[i];
    for (;;) {
        u32 k = 2u * i + 1u;
        if (k >= s->heap_count)
            break;
        if (k + 1u < s->heap_count && before(s, s->heap[k + 1u], s->heap[k]))
            k++;
        if (!before(s, s->heap[k], c))
            break;
        heap_set(s, i, s->heap[k]);
        i = k;
    }
    heap_set(s, i, c);
}

static u32 heap_pop(grid_search *s) {
    const u32 top = s->heap[0];
    s->heap_count--;
    if (s->heap_count > 0) {
        heap_set(s, 0, s->heap[s->heap_count]);
        sift_down(s, 0);
    }
    return top;
}

static void relax(grid_search *s, u32 from, u32 to, u64 step) {
    if (s->state[to] == ST_CLOSED)
        return;
    const u64 ng = s->g[from] + step;
    if (s->state[to] == ST_OPEN) {
        if (ng >= s->g[to])
            return;
        s->g[to] = ng;
        s->parent[to] = from;
        sift_up(s, s->heap_pos[to]);
        return;
    }
    s->state[to] = ST_OPEN;
    s->g[to] = ng;
    s->parent[to] = from;
    heap_set(s, s->heap_count, to);
    s->heap_count++;
    sift_up(s, s->heap_count - 1u);
}

static u32 cell_of(const grid_search *s, i32 x, i32 y) {
    return (u32)y * (u32)s->w + (u32)x;
}

/* ---------- A* ---------- */

static void astar_expand(grid_search *s, u32 c) {
    const i32 x = (i32)(c % (u32)s->w), y = (i32)(c / (u32)s->w);
    static const i8 dirs[8][2] = {{1, 0},  {-1, 0}, {0, 1},  {0, -1},
                                  {1, 1},  {-1, 1}, {1, -1}, {-1, -1}};
    for (u32 d = 0; d < 8; ++d) {
        const i32 dx = dirs[d][0], dy = dirs[d][1];
        const i32 nx = x + dx, ny = y + dy;
        if (!walkable(s, nx, ny))
            continue;
        if (dx != 0 && dy != 0 && !(walkable(s, x + dx, y) && walkable(s, x, y + dy)))
            continue;
        relax(s, c, cell_of(s, nx, ny),
              (dx != 0 && dy != 0) ? DE__NAV2D_COST_DIAGONAL : DE__NAV2D_COST_STRAIGHT);
    }
}

/* ---------- JPS ---------- */

static u32 jump(const grid_search *s, i32 px, i32 py, i32 dx, i32 dy);

static bool straight_finds(const grid_search *s, i32 x, i32 y, i32 dx, i32 dy) {
    return jump(s, x, y, dx, dy) != NONE;
}

/* From (px, py), steps in (dx, dy) until a jump point (returned) or a wall (NONE). */
static u32 jump(const grid_search *s, i32 px, i32 py, i32 dx, i32 dy) {
    i32 x = px + dx, y = py + dy;
    for (;;) {
        if (!walkable(s, x, y))
            return NONE;
        if (dx != 0 && dy != 0 && !(walkable(s, x - dx, y) && walkable(s, x, y - dy)))
            return NONE;
        if (x == s->gx && y == s->gy)
            return cell_of(s, x, y);
        if (dx != 0 && dy != 0) {
            if (straight_finds(s, x, y, dx, 0) || straight_finds(s, x, y, 0, dy))
                return cell_of(s, x, y);
        } else if (dx != 0) {
            if ((walkable(s, x, y - 1) && !walkable(s, x - dx, y - 1)) ||
                (walkable(s, x, y + 1) && !walkable(s, x - dx, y + 1)))
                return cell_of(s, x, y);
        } else {
            if ((walkable(s, x - 1, y) && !walkable(s, x - 1, y - dy)) ||
                (walkable(s, x + 1, y) && !walkable(s, x + 1, y - dy)))
                return cell_of(s, x, y);
        }
        x += dx;
        y += dy;
    }
}

static i32 sgn(i32 v) {
    return (v > 0) - (v < 0);
}

static void jps_try(grid_search *s, u32 c, i32 x, i32 y, i32 dx, i32 dy) {
    const u32 j = jump(s, x, y, dx, dy);
    if (j == NONE)
        return;
    relax(s, c, j, octile(x, y, (i32)(j % (u32)s->w), (i32)(j / (u32)s->w)));
}

static void jps_expand(grid_search *s, u32 c, u32 start) {
    const i32 x = (i32)(c % (u32)s->w), y = (i32)(c / (u32)s->w);
    if (c == start) {
        static const i8 dirs[8][2] = {{1, 0},  {-1, 0}, {0, 1},  {0, -1},
                                      {1, 1},  {-1, 1}, {1, -1}, {-1, -1}};
        for (u32 d = 0; d < 8; ++d)
            jps_try(s, c, x, y, dirs[d][0], dirs[d][1]);
        return;
    }
    const u32 p = s->parent[c];
    const i32 dx = sgn(x - (i32)(p % (u32)s->w)), dy = sgn(y - (i32)(p / (u32)s->w));
    if (dx != 0 && dy != 0) {
        jps_try(s, c, x, y, dx, 0);
        jps_try(s, c, x, y, 0, dy);
        jps_try(s, c, x, y, dx, dy);
    } else if (dx != 0) {
        jps_try(s, c, x, y, dx, 0);
        jps_try(s, c, x, y, dx, 1);
        jps_try(s, c, x, y, dx, -1);
        jps_try(s, c, x, y, 0, 1);
        jps_try(s, c, x, y, 0, -1);
    } else {
        jps_try(s, c, x, y, 0, dy);
        jps_try(s, c, x, y, 1, dy);
        jps_try(s, c, x, y, -1, dy);
        jps_try(s, c, x, y, 1, 0);
        jps_try(s, c, x, y, -1, 0);
    }
}

/* ---------- entry ---------- */

static bool point_cell(const de_nav2d_grid *grid, de_vec2 p, i32 *cx, i32 *cy) {
    const f32 fx = (p.x - grid->origin.x) / grid->cell_size;
    const f32 fy = (p.y - grid->origin.y) / grid->cell_size;
    if (!(fx >= 0.0f) || !(fy >= 0.0f) || !(fx < (f32)grid->width) || !(fy < (f32)grid->height))
        return false;
    *cx = (i32)fx;
    *cy = (i32)fy;
    if (*cx >= (i32)grid->width)
        *cx = (i32)grid->width - 1;
    if (*cy >= (i32)grid->height)
        *cy = (i32)grid->height - 1;
    return true;
}

static de_vec2 cell_centre(const de_nav2d_grid *grid, u32 c) {
    de_vec2 v;
    v.x = grid->origin.x + ((f32)(c % grid->width) + 0.5f) * grid->cell_size;
    v.y = grid->origin.y + ((f32)(c / grid->width) + 0.5f) * grid->cell_size;
    return v;
}

de_result de__nav2d_find_path_stats(const de_nav2d_grid *grid, de_vec2 from, de_vec2 to,
                                    bool jump_point_search, const de_allocator *a,
                                    de_vec2 **points, u32 *count, de__nav2d_grid_stats *stats) {
    if (points != NULL)
        *points = NULL;
    if (count != NULL)
        *count = 0;
    if (stats != NULL) {
        stats->expanded = 0;
        stats->cost = 0;
    }
    if (grid == NULL || points == NULL || count == NULL || grid->walkable == NULL ||
        grid->width == 0 || grid->height == 0 || !(grid->cell_size > 0.0f) ||
        grid->width > 0x7fffu || grid->height > 0x7fffu)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_nav2d_find_path: bad grid");
    a = de__nav2d_alloc(a);

    grid_search s;
    memset(&s, 0, sizeof s);
    s.grid = grid;
    s.w = (i32)grid->width;
    s.h = (i32)grid->height;
    i32 sx, sy;
    if (!point_cell(grid, from, &sx, &sy) || !point_cell(grid, to, &s.gx, &s.gy))
        return DE_FAIL(DE_ERR_NOT_FOUND, "de_nav2d_find_path: point outside the grid");
    if (!walkable(&s, sx, sy) || !walkable(&s, s.gx, s.gy))
        return DE_FAIL(DE_ERR_NOT_FOUND, "de_nav2d_find_path: start or goal cell is blocked");

    const size_t n = (size_t)grid->width * (size_t)grid->height;
    const size_t bytes = n * (sizeof(u64) + 3u * sizeof(u32) + 1u);
    u8 *block = (u8 *)DE_ALLOC(a, bytes, 8, DE_MEM_SCRATCH);
    if (block == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav2d_find_path: scratch");
    s.g = (u64 *)(void *)block;
    s.parent = (u32 *)(void *)(block + n * sizeof(u64));
    s.heap_pos = s.parent + n;
    s.heap = s.heap_pos + n;
    s.state = (u8 *)(s.heap + n);
    memset(s.state, 0, n);

    const u32 start = cell_of(&s, sx, sy), goal = cell_of(&s, s.gx, s.gy);
    s.g[start] = 0;
    s.parent[start] = NONE;
    s.state[start] = ST_OPEN;
    heap_set(&s, 0, start);
    s.heap_count = 1;
    u32 expanded = 0;
    bool found = false;
    while (s.heap_count > 0) {
        const u32 c = heap_pop(&s);
        s.state[c] = ST_CLOSED;
        expanded++;
        if (c == goal) {
            found = true;
            break;
        }
        if (jump_point_search)
            jps_expand(&s, c, start);
        else
            astar_expand(&s, c);
    }
    if (stats != NULL)
        stats->expanded = expanded;
    if (!found) {
        DE_FREE(a, block, bytes);
        return DE_FAIL(DE_ERR_NOT_FOUND, "de_nav2d_find_path: no path");
    }
    if (stats != NULL)
        stats->cost = s.g[goal];

    /* Chain goal -> start into heap[] (free now), reversed, keeping only turning points. */
    u32 len = 0;
    for (u32 c = goal; c != NONE; c = s.parent[c])
        s.heap[len++] = c;
    for (u32 i = 0; i < len / 2u; ++i) {
        const u32 t = s.heap[i];
        s.heap[i] = s.heap[len - 1u - i];
        s.heap[len - 1u - i] = t;
    }
    u32 out = 0;
    for (u32 i = 0; i < len; ++i) {
        if (out >= 2u) {
            const u32 p0 = s.heap_pos[out - 2u], p1 = s.heap_pos[out - 1u], p2 = s.heap[i];
            const i32 w = s.w;
            const i32 ax = sgn((i32)(p1 % (u32)w) - (i32)(p0 % (u32)w));
            const i32 ay = sgn((i32)(p1 / (u32)w) - (i32)(p0 / (u32)w));
            const i32 bx = sgn((i32)(p2 % (u32)w) - (i32)(p1 % (u32)w));
            const i32 by = sgn((i32)(p2 / (u32)w) - (i32)(p1 / (u32)w));
            if (ax == bx && ay == by) {
                s.heap_pos[out - 1u] = p2;
                continue;
            }
        }
        s.heap_pos[out++] = s.heap[i]; /* heap_pos reused as the output cell list */
    }
    de_vec2 *pts = (de_vec2 *)DE_ALLOC(a, sizeof(de_vec2) * (size_t)out, DE_ALIGNOF(de_vec2),
                                       DE_MEM_GENERAL);
    if (pts == NULL) {
        DE_FREE(a, block, bytes);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_nav2d_find_path: path points");
    }
    for (u32 i = 0; i < out; ++i)
        pts[i] = cell_centre(grid, s.heap_pos[i]);
    DE_FREE(a, block, bytes);
    *points = pts;
    *count = out;
    return DE_OK;
}

de_result de_nav2d_find_path(const de_nav2d_grid *grid, de_vec2 from, de_vec2 to,
                             bool jump_point_search, const de_allocator *a, de_vec2 **points,
                             u32 *count) {
    return de__nav2d_find_path_stats(grid, from, to, jump_point_search, a, points, count, NULL);
}

void de_nav2d_path_free(const de_allocator *a, de_vec2 *points, u32 count) {
    if (points == NULL)
        return;
    a = de__nav2d_alloc(a);
    DE_FREE(a, points, sizeof(de_vec2) * (size_t)count);
}

de_dvec3 de_nav2d_to_world(de_plane_2d plane, de_vec2 p, f64 depth) {
    de_dvec3 w;
    w.x = (f64)p.x;
    if (plane == DE_PLANE_SIDE_XZ) {
        w.y = depth;
        w.z = (f64)p.y;
    } else {
        w.y = (f64)p.y;
        w.z = depth;
    }
    return w;
}

de_vec2 de_nav2d_from_world(de_plane_2d plane, de_dvec3 p) {
    de_vec2 v;
    v.x = (f32)p.x;
    v.y = (f32)(plane == DE_PLANE_SIDE_XZ ? p.z : p.y);
    return v;
}
