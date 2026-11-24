/* editor_nav.c: world.nav (DENG-582; DIDE protocol 1.25, DIDE-128, DIDE-R174; DENG-863: 1.27).
 *
 * Methods nav.info, nav.configure, nav.bake, nav.cancelBake, nav.tiles, nav.tileMesh,
 * nav.testPath, nav.testPath2d and nav.overlay, and the topic nav.bake. The service keeps one
 * editor de_nav per agent type, bound (not attached) to the engine's world, and bakes the
 * protocol's tiles one de_nav_rebuild_tiles call each on the nav's task system, a few in flight at
 * a time. The reflected components DeNavLink and DeNavModifier (src/gameplay/nav/nav_components.c)
 * go into every bake as off-mesh links and area or carve volumes. DeNavGrid2D is registered and
 * reflected here and the service attaches the `nav2d.grid` system (DENG-832,
 * de_nav2d_grid_attach) to the editor world, unless a game already did. nav.testPath2d runs
 * de_nav2d_find_path on the built grid of a DeNavGrid2D entity; NavInfo.plane2d reports the
 * system's plane (side_xz by default); NavOverlayParams.grids draws the cells (DENG-863).
 *
 * Axes: the engine is Z-up, so tile (x, y) covers bounds.min + [x, y] * tileSize along world X
 * and Y. Engine tiles lie on a world grid (the navmesh origin is on the 256 m grid), so
 * bounds.min x and y must be multiples of tileSize.
 *
 * Dirty tiles: each baked tile keeps a fingerprint of the inputs that can reach it (triangles,
 * modifiers and links whose box, grown by the agent radius and a border, meets the tile, plus the
 * agent and area settings). A tile whose fingerprint changed is DIRTY; with autoRebake on, the
 * dirty baked tiles rebake every few frames, so moving a wall rebakes only its tiles.
 *
 * Persistence (DENG-833): the settings (agents, areas, bounds, tileSize, autoRebake) live in the
 * project file `nav.toml` beside the root manifest (de_nav_settings_*, de_gameplay.h). The service
 * loads it when the project is known (register, and when the manifest path changes) and
 * nav.configure rewrites it, so a restart keeps them and the game runtime reads the same file.
 *
 * Baked tiles (DENG-864): when a bake ends (done, canceled or failed) the service writes every
 * agent type's baked tiles, with their fingerprints, polygon and link counts, to `nav.tiles`
 * beside nav.toml (docs/formats/nav_tiles.md). With a new world or project it loads them back
 * into the agent types of the same name, grid and build settings, so a restart needs no rebake;
 * tiles whose inputs differ in this world show DIRTY and rebake as usual.
 *
 * Limits: one nav.tiles per project, not per scene. tileSize / cellSize must be a whole number of
 * cells from 8 to 1024 (Recast tiles).
 */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* snprintf */
#endif
#include "host/editor/editor_internal.h"

#include "gameplay/nav/nav_build.h"
#include "gameplay/nav/nav_components.h"
#include "gameplay/nav/nav_internal.h"
#include <dengine/de_debug.h>
#include <dengine/de_platform.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define NV_CAP           "world.nav"
#define NV_MAX_AGENTS    4u
#define NV_MAX_TILES     4096u
#define NV_MAX_AREAS     16u
#define NV_MAX_LINKS     1024u
#define NV_MAX_VOLUMES   1024u
#define NV_INFLIGHT      4u    /* rebuilds in flight per agent type */
#define NV_AUTO_EVERY    6u    /* frames between automatic rebake checks */
#define NV_PATH_VALUES   3072u /* overlay path: 1024 points */
#define NV_DRAW_TRIS     4096u /* triangles read per tile for the overlay */
#define NV_DRAW_LINES    30000u
#define NV_DRAW_CELLS    16384u /* grid cells drawn per frame (4 grids of 64 x 64) */
#define NV_NAME_MAX      32u
#define NV_AREA_NAME_MAX 63u

typedef struct nv_agent {
    char name[NV_NAME_MAX + 1];
    f32 radius, height, max_slope_deg, step_height, cell_size, cell_height;
    u32 collision_mask;
} nv_agent;

typedef struct nv_area {
    u32 id;
    char name[NV_AREA_NAME_MAX + 1];
    f32 cost;
} nv_area;

typedef struct nv_tile {
    bool baked;
    bool busy; /* queued or in flight */
    u64 revision;
    u32 polys, links;
    f32 ms;
    u64 fp;
} nv_tile;

typedef struct nv_flight {
    u32 tile;
    u32 bake; /* the bake that queued it; 0 = none */
    u64 start_ns;
    u64 fp;
} nv_flight;

typedef struct nv_nav {
    de_nav *nav;
    nv_tile *tiles; /* NV_MAX_TILES, y * tiles_x + x */
    nv_flight flight[NV_INFLIGHT];
    u32 flight_count;
    u64 *fp; /* NV_MAX_TILES: current fingerprints (scratch) */
} nv_nav;

typedef struct nv_key {
    u8 agent;
    u32 tile;
} nv_key;

typedef struct nv_state {
    de_world *world; /* the world the navs read (the engine's, checked every tick) */
    de__nav_component_ids ids;
    bool has_ids;

    nv_agent agents[NV_MAX_AGENTS];
    nv_nav navs[NV_MAX_AGENTS];
    u32 agent_count;
    nv_area areas[NV_MAX_AREAS];
    u32 area_count;
    f64 bmin[3], bmax[3];
    f32 tile_size;
    u32 tiles_x, tiles_y;
    bool auto_rebake;
    char settings_path[1024]; /* the nav.toml last loaded or written; empty = no project */
    bool tiles_pending;       /* nav.tiles to load (new world or new project) */
    u64 revision;
    u64 frame;

    /* The bake. */
    bool running;
    u32 bake, bake_seq;
    bool bake_auto;
    u64 bake_start_ns;
    u32 done, total;
    nv_key queue[NV_MAX_AGENTS * NV_MAX_TILES];
    u32 qhead, qcount;
    nv_key landed[NV_MAX_AGENTS * NV_MAX_TILES]; /* since the last note */
    u32 landed_count;

    /* Off-mesh link ids: slot i holds the entity of link id i + 1. */
    u64 link_entity[NV_MAX_LINKS];
    bool link_seen[NV_MAX_LINKS];

    /* The overlay. */
    bool ov_has_agent;
    char ov_agent[NV_NAME_MAX + 1];
    bool ov_navmesh, ov_tiles, ov_links, ov_grids;
    bool ov_has_path;
    f64 ov_path[NV_PATH_VALUES];
    u32 ov_path_count;
    de_draw_category cat;
} nv_state;

static nv_state g_nv;
static de__nav_volume g_volumes[NV_MAX_VOLUMES];
static de_dvec3 g_draw_pts[NV_DRAW_TRIS * 3];
static u8 g_draw_areas[NV_DRAW_TRIS];

static const de_allocator *heap(void) {
    return de_heap_allocator();
}

static de_world *world_of(de__editor *ed) {
    return ed->e != NULL ? ed->e->world : NULL;
}

/* ---------- Strings and values ---------- */

static bool str_is(de_ep_str s, const char *c) {
    return s.len == strlen(c) && (s.len == 0 || memcmp(s.ptr, c, s.len) == 0);
}

static void str_copy(char *dst, size_t cap, de_ep_str s) {
    const size_t n = s.len < cap - 1 ? s.len : cap - 1;
    if (n > 0)
        memcpy(dst, s.ptr, n);
    dst[n] = '\0';
}

static i32 agent_index(de_ep_str s) {
    for (u32 i = 0; i < g_nv.agent_count; ++i)
        if (str_is(s, g_nv.agents[i].name))
            return (i32)i;
    return -1;
}

static bool finite3(const de_ep_list_f64 *l) {
    return l->count == 3 && isfinite(l->items[0]) && isfinite(l->items[1]) &&
           isfinite(l->items[2]);
}

/* The protocol's check_bounds: 3 finite values each, max above min on every axis. */
static bool bounds_ok(const de_ep_nav_bounds *b) {
    if (!finite3(&b->min) || !finite3(&b->max))
        return false;
    for (u32 i = 0; i < 3; ++i)
        if (!(b->max.items[i] > b->min.items[i]))
            return false;
    return true;
}

static u64 fnv(u64 h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/* ---------- Tiles ---------- */

static u32 tile_count(void) {
    return g_nv.tiles_x * g_nv.tiles_y;
}

static void grid_of(const f64 *mn, const f64 *mx, f32 ts, u32 *tx, u32 *ty) {
    const f64 x = ceil((mx[0] - mn[0]) / ts), y = ceil((mx[1] - mn[1]) / ts);
    *tx = x < 1.0 ? 1u : (u32)x;
    *ty = y < 1.0 ? 1u : (u32)y;
}

/* The inclusive tile range a world box [x0, x1] x [y0, y1] meets; false when it meets none. */
static bool tile_range(f64 x0, f64 y0, f64 x1, f64 y1, i32 *ix0, i32 *iy0, i32 *ix1, i32 *iy1) {
    const f64 ts = g_nv.tile_size;
    f64 a = floor((x0 - g_nv.bmin[0]) / ts), b = floor((y0 - g_nv.bmin[1]) / ts);
    f64 c = floor((x1 - g_nv.bmin[0]) / ts), d = floor((y1 - g_nv.bmin[1]) / ts);
    if (c < 0 || d < 0 || a >= (f64)g_nv.tiles_x || b >= (f64)g_nv.tiles_y)
        return false;
    *ix0 = a < 0 ? 0 : (i32)a;
    *iy0 = b < 0 ? 0 : (i32)b;
    *ix1 = c >= (f64)g_nv.tiles_x ? (i32)g_nv.tiles_x - 1 : (i32)c;
    *iy1 = d >= (f64)g_nv.tiles_y ? (i32)g_nv.tiles_y - 1 : (i32)d;
    return true;
}

static void tile_rect(u32 t, f64 *x0, f64 *y0, f64 *x1, f64 *y1) {
    const u32 x = t % g_nv.tiles_x, y = t / g_nv.tiles_x;
    *x0 = g_nv.bmin[0] + (f64)x * g_nv.tile_size;
    *y0 = g_nv.bmin[1] + (f64)y * g_nv.tile_size;
    *x1 = *x0 + g_nv.tile_size;
    *y1 = *y0 + g_nv.tile_size;
}

/* The engine tile (tx, tz) under protocol tile `t`; false before the navmesh has an origin. */
static bool engine_tile(const nv_nav *n, u32 t, i32 *tx, i32 *tz) {
    f64 x0, y0, x1, y1;
    tile_rect(t, &x0, &y0, &x1, &y1);
    const de_dvec3 c = {(x0 + x1) * 0.5, (y0 + y1) * 0.5, g_nv.bmin[2]};
    return n->nav != NULL && de__nav_tile_of(n->nav, c, tx, tz);
}

/* ---------- Agent navs ---------- */

static void nav_free(nv_nav *n) {
    if (n->nav != NULL) {
        de__nav_bind_world(n->nav, NULL);
        de_nav_destroy(n->nav);
    }
    if (n->tiles != NULL)
        DE_FREE(heap(), n->tiles, sizeof(nv_tile) * NV_MAX_TILES);
    if (n->fp != NULL)
        DE_FREE(heap(), n->fp, sizeof(u64) * NV_MAX_TILES);
    memset(n, 0, sizeof *n);
}

/* Makes the nav of agent slot `i` when it has none (bound to the current world). */
static bool nav_ensure(de__editor *ed, u32 i) {
    nv_nav *n = &g_nv.navs[i];
    if (n->tiles == NULL) {
        n->tiles = (nv_tile *)DE_ALLOC(heap(), sizeof(nv_tile) * NV_MAX_TILES, 16, DE_MEM_GENERAL);
        n->fp = (u64 *)DE_ALLOC(heap(), sizeof(u64) * NV_MAX_TILES, 16, DE_MEM_GENERAL);
        if (n->tiles == NULL || n->fp == NULL) {
            nav_free(n);
            return false;
        }
        memset(n->tiles, 0, sizeof(nv_tile) * NV_MAX_TILES);
        memset(n->fp, 0, sizeof(u64) * NV_MAX_TILES);
    }
    if (n->nav == NULL) {
        if (de_nav_create(heap(), ed->e != NULL ? ed->e->tasks : NULL, &n->nav) != DE_OK) {
            n->nav = NULL;
            return false;
        }
        de__nav_bind_world(n->nav, g_nv.world);
        n->flight_count = 0;
    }
    return true;
}

/* Drops the navmesh and tile records of agent slot `i` (the settings stay). */
static void nav_reset(nv_nav *n) {
    if (n->nav != NULL) {
        de__nav_bind_world(n->nav, NULL);
        de_nav_destroy(n->nav);
        n->nav = NULL;
    }
    n->flight_count = 0;
    if (n->tiles != NULL)
        memset(n->tiles, 0, sizeof(nv_tile) * NV_MAX_TILES);
}

static de_navmesh_build_desc desc_of(const nv_agent *a) {
    de_navmesh_build_desc d;
    memset(&d, 0, sizeof d);
    d.cell_size = a->cell_size;
    d.cell_height = a->cell_height;
    d.agent_radius = a->radius;
    d.agent_height = a->height;
    d.agent_max_climb = a->step_height;
    /* Recast needs a slope above 0; 0 degrees means flat ground only. */
    const f32 deg = a->max_slope_deg < 0.01f ? 0.01f : a->max_slope_deg;
    d.agent_max_slope_rad = deg * 3.14159265358979f / 180.0f;
    d.tile_size = g_nv.tile_size;
    d.collision_mask = a->collision_mask;
    return d;
}

/* ---------- Inputs: DeNavLink and DeNavModifier ---------- */

static bool ensure_ids(de__editor *ed) {
    de_world *w = world_of(ed);
    if (w == NULL)
        return false;
    if (!g_nv.has_ids) {
        if (de__nav_components_register(w, &g_nv.ids) != DE_OK)
            return false;
        g_nv.has_ids = true;
    }
    return true;
}

#define NV_MAX_SCAN 4096u
typedef struct nv_scan {
    u64 bits[NV_MAX_SCAN];
    u32 n;
} nv_scan;
static nv_scan g_scan;

static void collect(const de_query_chunk *ch, void *user) {
    nv_scan *s = (nv_scan *)user;
    for (u32 r = 0; r < ch->count && s->n < NV_MAX_SCAN; ++r)
        s->bits[s->n++] = ch->entities[r].bits;
}

static void scan(de_world *w, de_component c) {
    g_scan.n = 0;
    const de_query_term terms[1] = {{c, DE_READ}};
    const de_query_desc qd = {terms, 1, NULL, 0};
    de_query q = de_query_create(w, &qd);
    de_query_each(w, q, collect, &g_scan);
    de_query_destroy(w, q);
}

/* The link id of entity `bits`: its slot + 1, kept while the entity has a DeNavLink. */
static u32 link_id(u64 bits) {
    u32 free_slot = NV_MAX_LINKS;
    for (u32 i = 0; i < NV_MAX_LINKS; ++i) {
        if (g_nv.link_entity[i] == bits)
            return i + 1;
        if (g_nv.link_entity[i] == 0 && free_slot == NV_MAX_LINKS)
            free_slot = i;
    }
    if (free_slot == NV_MAX_LINKS)
        return 0;
    g_nv.link_entity[free_slot] = bits;
    return free_slot + 1;
}

/* Gives every nav the current links and modifier volumes. */
static void refresh_inputs(de__editor *ed) {
    de_world *w = world_of(ed);
    if (w == NULL || !ensure_ids(ed))
        return;
    for (u32 i = 0; i < g_nv.agent_count; ++i)
        if (g_nv.navs[i].nav != NULL)
            de__nav_clear_links(g_nv.navs[i].nav);

    memset(g_nv.link_seen, 0, sizeof g_nv.link_seen);
    scan(w, g_nv.ids.link);
    for (u32 k = 0; k < g_scan.n; ++k) {
        const de_entity e = {g_scan.bits[k]};
        const DeNavLink *l = (const DeNavLink *)de_entity_get_const(w, e, g_nv.ids.link);
        if (l == NULL)
            continue;
        const u32 id = link_id(e.bits);
        if (id == 0)
            continue;
        g_nv.link_seen[id - 1] = true;
        const de_transform xf = de__ed_world_xf(w, e);
        const de_dvec3 from = xf.position;
        const de_dvec3 to = {from.x + l->end.x, from.y + l->end.y, from.z + l->end.z};
        const f32 radius = l->radius > 0.0f ? l->radius : 0.5f;
        const u8 area = (u8)(l->area > 15u ? 15u : l->area);
        for (u32 i = 0; i < g_nv.agent_count; ++i)
            if (g_nv.navs[i].nav != NULL)
                (void)de__nav_add_link_id(g_nv.navs[i].nav, from, to, radius, l->bidirectional,
                                          area, id);
    }
    for (u32 i = 0; i < NV_MAX_LINKS; ++i)
        if (!g_nv.link_seen[i])
            g_nv.link_entity[i] = 0;

    scan(w, g_nv.ids.modifier);
    u32 nv = 0;
    for (u32 k = 0; k < g_scan.n && nv < NV_MAX_VOLUMES; ++k) {
        const de_entity e = {g_scan.bits[k]};
        const DeNavModifier *m =
            (const DeNavModifier *)de_entity_get_const(w, e, g_nv.ids.modifier);
        if (m == NULL)
            continue;
        const bool carve = m->mode == DE_NAV_MODIFIER_CARVE;
        if (!carve && (m->area == 0 || m->area > 15u))
            continue; /* area 0 is the ground already */
        const de_transform xf = de__ed_world_xf(w, e);
        de__nav_volume *v = &g_volumes[nv++];
        memset(v, 0, sizeof *v);
        v->pose = de__nav_pose_from(&xf);
        v->half_extents.x = fabsf(m->size.x) * 0.5f;
        v->half_extents.y = fabsf(m->size.y) * 0.5f;
        v->half_extents.z = fabsf(m->size.z) * 0.5f;
        v->carve = carve;
        v->area = carve ? 0 : (u8)m->area;
    }
    for (u32 i = 0; i < g_nv.agent_count; ++i)
        if (g_nv.navs[i].nav != NULL)
            (void)de__nav_set_volumes(g_nv.navs[i].nav, g_volumes, nv);
}

/* ---------- Fingerprints ---------- */

static u64 settings_hash(const nv_agent *a) {
    u64 h = 0xcbf29ce484222325ull;
    h = fnv(h, &a->radius, sizeof(f32) * 6);
    h = fnv(h, &a->collision_mask, sizeof a->collision_mask);
    h = fnv(h, &g_nv.tile_size, sizeof g_nv.tile_size);
    for (u32 i = 0; i < g_nv.area_count; ++i) {
        h = fnv(h, &g_nv.areas[i].id, sizeof(u32));
        h = fnv(h, &g_nv.areas[i].cost, sizeof(f32));
    }
    return h;
}

/* Folds element hash `eh` into the tiles a Recast-space xz box (origin 0) meets. */
static void fold(u64 *fp, f64 margin, f32 rx0, f32 rz0, f32 rx1, f32 rz1, u64 eh) {
    /* Recast x = world x, Recast z = -world y. */
    i32 ix0, iy0, ix1, iy1;
    if (!tile_range((f64)rx0 - margin, -(f64)rz1 - margin, (f64)rx1 + margin,
                    -(f64)rz0 + margin, &ix0, &iy0, &ix1, &iy1))
        return;
    for (i32 y = iy0; y <= iy1; ++y)
        for (i32 x = ix0; x <= ix1; ++x) {
            u64 *h = &fp[(u32)y * g_nv.tiles_x + (u32)x];
            *h = (*h ^ eh) * 0x100000001b3ull;
        }
}

static void fold_floats(u64 *fp, f64 margin, const f32 *v, u32 n, const f32 *xz, u32 corners) {
    f32 x0 = xz[0], x1 = xz[0], z0 = xz[1], z1 = xz[1];
    for (u32 c = 1; c < corners; ++c) {
        const f32 x = xz[c * 2], z = xz[c * 2 + 1];
        x0 = x < x0 ? x : x0;
        x1 = x > x1 ? x : x1;
        z0 = z < z0 ? z : z0;
        z1 = z > z1 ? z : z1;
    }
    fold(fp, margin, x0, z0, x1, z1, fnv(0xcbf29ce484222325ull, v, sizeof(f32) * n));
}

/* Fills navs[i].fp with the current fingerprint of every tile (inputs must be refreshed). */
static void fingerprints(u32 i) {
    nv_nav *n = &g_nv.navs[i];
    const nv_agent *a = &g_nv.agents[i];
    const u32 tc = tile_count();
    const u64 seed = settings_hash(a);
    for (u32 t = 0; t < tc; ++t)
        n->fp[t] = seed;
    if (n->nav == NULL)
        return;
    de__nav_soup soup;
    const de_dvec3 zero = {0, 0, 0};
    de__nav_soup_init(&soup, heap(), zero);
    de__nav_gather_input(n->nav, &soup, a->collision_mask);
    const f64 margin = (f64)a->radius + 4.0 * (f64)a->cell_size;
    for (u32 k = 0; k < soup.tri_count; ++k) {
        f32 v[9], xz[6];
        for (u32 c = 0; c < 3; ++c) {
            const f32 *p = &soup.verts[(u32)soup.tris[k * 3 + c] * 3];
            v[c * 3] = p[0], v[c * 3 + 1] = p[1], v[c * 3 + 2] = p[2];
            xz[c * 2] = p[0], xz[c * 2 + 1] = p[2];
        }
        fold_floats(n->fp, margin, v, 9, xz, 3);
    }
    for (u32 k = 0; k < soup.obstacle_count; ++k) {
        const f32 *o = &soup.obstacles[k * DE__NAV_RC_OBSTACLE_FLOATS];
        fold_floats(n->fp, margin, o, DE__NAV_RC_OBSTACLE_FLOATS, o, 4);
    }
    for (u32 k = 0; k < soup.area_vol_count; ++k) {
        const f32 *o = &soup.area_vols[k * DE__NAV_RC_AREA_VOL_FLOATS];
        fold_floats(n->fp, margin, o, DE__NAV_RC_AREA_VOL_FLOATS, o, 4);
    }
    for (u32 k = 0; k < soup.offmesh_count; ++k) {
        const f32 *o = &soup.offmesh[k * DE__NAV_RC_OFFMESH_FLOATS];
        const f32 xz[2] = {o[0], o[2]}; /* built into the tile of its start */
        fold_floats(n->fp, margin, o, DE__NAV_RC_OFFMESH_FLOATS, xz, 1);
    }
    de__nav_soup_free(&soup);
}

static de_ep_nav_tile_state tile_state(const nv_nav *n, u32 t) {
    const nv_tile *r = &n->tiles[t];
    if (r->busy)
        return DE_EP_NAV_TILE_STATE_BAKING;
    if (!r->baked)
        return DE_EP_NAV_TILE_STATE_UNBAKED;
    return r->fp != n->fp[t] ? DE_EP_NAV_TILE_STATE_DIRTY : DE_EP_NAV_TILE_STATE_BAKED;
}

/* ---------- Notes ---------- */

static de_ep_nav_tile_key g_keys[NV_MAX_AGENTS * NV_MAX_TILES];

static void send_progress(de__editor *ed, de_ep_nav_bake_state st, const char *error) {
    if (!ed->connected) {
        g_nv.landed_count = 0;
        return;
    }
    de_ep_nav_bake_progress p;
    memset(&p, 0, sizeof p);
    p.bake = g_nv.bake;
    p.state = st;
    p.done = g_nv.done;
    p.total = g_nv.total;
    for (u32 k = 0; k < g_nv.landed_count; ++k) {
        const nv_key *l = &g_nv.landed[k];
        g_keys[k].agent = de_ep_cstr(g_nv.agents[l->agent].name);
        g_keys[k].x = (i32)(l->tile % g_nv.tiles_x);
        g_keys[k].y = (i32)(l->tile / g_nv.tiles_x);
    }
    p.tiles.items = g_keys;
    p.tiles.count = g_nv.landed_count;
    p.auto_ = g_nv.bake_auto;
    p.ms = (f32)((f64)(de_time_now_ns() - g_nv.bake_start_ns) / 1e6);
    if (error != NULL) {
        p.has_error = true;
        p.error = de_ep_cstr(error);
    }
    mpack_writer_t nw;
    const u64 seq = de__ed_note_begin(ed, &nw);
    de_ep_write_notify_nav_bake(&nw, seq, &p);
    de__ed_send(ed, &nw, DE_EP_KIND_NOTIFICATION);
    g_nv.landed_count = 0;
}

/* ---------- The bake ---------- */

static void clear_queue(void) {
    for (u32 k = 0; k < g_nv.qcount; ++k) {
        const nv_key *q = &g_nv.queue[(g_nv.qhead + k) % (NV_MAX_AGENTS * NV_MAX_TILES)];
        if (g_nv.navs[q->agent].tiles != NULL)
            g_nv.navs[q->agent].tiles[q->tile].busy = false;
    }
    g_nv.qhead = g_nv.qcount = 0;
}

static void start_bake(bool auto_) {
    g_nv.running = true;
    g_nv.bake = ++g_nv.bake_seq;
    if (g_nv.bake == 0)
        g_nv.bake = ++g_nv.bake_seq;
    g_nv.bake_auto = auto_;
    g_nv.bake_start_ns = de_time_now_ns();
    g_nv.done = g_nv.total = 0;
    g_nv.qhead = g_nv.qcount = 0;
    g_nv.landed_count = 0;
}

static void enqueue(u32 agent, u32 t) {
    nv_tile *r = &g_nv.navs[agent].tiles[t];
    if (r->busy)
        return;
    r->busy = true;
    const u32 cap = NV_MAX_AGENTS * NV_MAX_TILES;
    g_nv.queue[(g_nv.qhead + g_nv.qcount) % cap] = (nv_key){(u8)agent, t};
    ++g_nv.qcount;
    ++g_nv.total;
}

static void save_tiles(de__editor *ed);

static void fail_bake(de__editor *ed, const char *why) {
    clear_queue();
    send_progress(ed, DE_EP_NAV_BAKE_STATE_FAILED, why);
    g_nv.running = false;
    save_tiles(ed); /* the tiles that landed */
}

/* Applies finished rebuilds and records their tiles. */
static void land(de__editor *ed) {
    (void)ed;
    for (u32 i = 0; i < g_nv.agent_count; ++i) {
        nv_nav *n = &g_nv.navs[i];
        if (n->nav == NULL || n->flight_count == 0)
            continue;
        de_nav_update(n->nav);
        const u32 pending = de__nav_pending_jobs(n->nav);
        const u32 finished = n->flight_count > pending ? n->flight_count - pending : 0;
        const u64 now = de_time_now_ns();
        for (u32 k = 0; k < finished; ++k) {
            const nv_flight *f = &n->flight[k];
            nv_tile *r = &n->tiles[f->tile];
            r->busy = false;
            r->baked = true;
            r->revision = ++g_nv.revision;
            r->fp = f->fp;
            r->ms = (f32)((f64)(now - f->start_ns) / 1e6);
            i32 tx, tz, polys = 0, links = 0;
            if (engine_tile(n, f->tile, &tx, &tz))
                (void)de__nav_tile_stats(n->nav, tx, tz, &polys, &links);
            r->polys = (u32)polys;
            r->links = (u32)links;
            if (g_nv.running && f->bake == g_nv.bake) {
                ++g_nv.done;
                g_nv.landed[g_nv.landed_count++] = (nv_key){(u8)i, f->tile};
            }
        }
        if (finished > 0) {
            memmove(n->flight, n->flight + finished, sizeof(nv_flight) * (n->flight_count - finished));
            n->flight_count -= finished;
        }
    }
}

static bool bake_in_flight(void) {
    for (u32 i = 0; i < g_nv.agent_count; ++i)
        for (u32 k = 0; k < g_nv.navs[i].flight_count; ++k)
            if (g_nv.navs[i].flight[k].bake == g_nv.bake)
                return true;
    return false;
}

/* Starts queued rebuilds while their agent has room. */
static void submit(de__editor *ed) {
    if (g_nv.qcount == 0)
        return;
    bool refreshed = false;
    bool fp_done[NV_MAX_AGENTS] = {false};
    const u32 cap = NV_MAX_AGENTS * NV_MAX_TILES;
    while (g_nv.qcount > 0) {
        const nv_key q = g_nv.queue[g_nv.qhead];
        nv_nav *n = &g_nv.navs[q.agent];
        if (n->flight_count >= NV_INFLIGHT)
            break;
        if (!refreshed) {
            refresh_inputs(ed);
            refreshed = true;
        }
        if (!fp_done[q.agent]) {
            fingerprints(q.agent);
            fp_done[q.agent] = true;
        }
        g_nv.qhead = (g_nv.qhead + 1) % cap;
        --g_nv.qcount;
        f64 x0, y0, x1, y1;
        tile_rect(q.tile, &x0, &y0, &x1, &y1);
        const f64 in = 0.01;
        de_aabb box;
        box.min.x = (f32)(x0 + in), box.min.y = (f32)(y0 + in), box.min.z = (f32)g_nv.bmin[2];
        box.max.x = (f32)(x1 - in), box.max.y = (f32)(y1 - in), box.max.z = (f32)g_nv.bmax[2];
        const de_navmesh_build_desc d = desc_of(&g_nv.agents[q.agent]);
        const de_result r = de_nav_rebuild_tiles(n->nav, &d, box);
        if (r != DE_OK) {
            n->tiles[q.tile].busy = false;
            char why[256];
            const de_error *e = de_error_last();
            snprintf(why, sizeof why, "tile (%u, %u) of %s: %s", q.tile % g_nv.tiles_x,
                     q.tile / g_nv.tiles_x, g_nv.agents[q.agent].name,
                     e != NULL && e->message[0] != '\0' ? e->message : "the rebuild failed");
            fail_bake(ed, why);
            return;
        }
        nv_flight *f = &n->flight[n->flight_count++];
        f->tile = q.tile;
        f->bake = g_nv.bake;
        f->start_ns = de_time_now_ns();
        f->fp = n->fp[q.tile];
    }
}

/* Every few frames with autoRebake on: rebakes the baked tiles whose inputs changed. */
static void auto_check(de__editor *ed) {
    if (!g_nv.auto_rebake || g_nv.running || g_nv.frame % NV_AUTO_EVERY != 0)
        return;
    bool any = false;
    for (u32 i = 0; i < g_nv.agent_count && !any; ++i) {
        const nv_nav *n = &g_nv.navs[i];
        for (u32 t = 0; n->tiles != NULL && t < tile_count() && !any; ++t)
            any = n->tiles[t].baked;
    }
    if (!any)
        return;
    refresh_inputs(ed);
    bool started = false;
    for (u32 i = 0; i < g_nv.agent_count; ++i) {
        nv_nav *n = &g_nv.navs[i];
        if (n->nav == NULL || n->tiles == NULL)
            continue;
        fingerprints(i);
        for (u32 t = 0; t < tile_count(); ++t)
            if (n->tiles[t].baked && !n->tiles[t].busy && n->tiles[t].fp != n->fp[t]) {
                if (!started) {
                    start_bake(true);
                    started = true;
                }
                enqueue(i, t);
            }
    }
}

/* ---------- World changes ---------- */

static void drop_all(void) {
    for (u32 i = 0; i < NV_MAX_AGENTS; ++i)
        nav_reset(&g_nv.navs[i]);
    g_nv.running = false;
    g_nv.qhead = g_nv.qcount = 0;
    g_nv.landed_count = 0;
    memset(g_nv.link_entity, 0, sizeof g_nv.link_entity);
}

static void sync_world(de__editor *ed) {
    de_world *w = world_of(ed);
    if (w == g_nv.world)
        return;
    drop_all();
    g_nv.world = w;
    g_nv.has_ids = false;
    g_nv.tiles_pending = w != NULL;
    if (w != NULL) {
        (void)ensure_ids(ed);
        /* DENG-863: the 2D grids nav.testPath2d and the grid overlay read. A no-op when the
         * world has the system already (a game that chose its own plane). */
        (void)de_nav2d_grid_attach(w, NULL);
    }
}

/* ---------- Defaults ---------- */

static void set_defaults(void) {
    memset(g_nv.agents, 0, sizeof g_nv.agents);
    nv_agent *a = &g_nv.agents[0];
    snprintf(a->name, sizeof a->name, "humanoid");
    a->radius = 0.4f;
    a->height = 1.8f;
    a->max_slope_deg = 45.0f;
    a->step_height = 0.3f;
    a->cell_size = 0.5f;
    a->cell_height = 0.2f;
    a->collision_mask = 0;
    g_nv.agent_count = 1;
    static const struct {
        const char *name;
        f32 cost;
    } k_areas[4] = {{"ground", 1.0f}, {"road", 1.0f}, {"water", 5.0f}, {"jump", 2.0f}};
    for (u32 i = 0; i < 4; ++i) {
        g_nv.areas[i].id = i;
        snprintf(g_nv.areas[i].name, sizeof g_nv.areas[i].name, "%s", k_areas[i].name);
        g_nv.areas[i].cost = k_areas[i].cost;
    }
    g_nv.area_count = 4;
    g_nv.bmin[0] = -32, g_nv.bmin[1] = -32, g_nv.bmin[2] = -2;
    g_nv.bmax[0] = 32, g_nv.bmax[1] = 32, g_nv.bmax[2] = 10;
    g_nv.tile_size = 8.0f;
    grid_of(g_nv.bmin, g_nv.bmax, g_nv.tile_size, &g_nv.tiles_x, &g_nv.tiles_y);
    g_nv.auto_rebake = true;
    g_nv.revision = 1;
}

static bool any_baked(void);

/* ---------- Project file nav.toml (DENG-833) ---------- */

static const char *g_path_override; /* tests: a vpath to use when there is no manifest */

void de__ed_nav_set_settings_path(const char *vpath) {
    g_path_override = vpath;
}

static bool settings_vpath(de__editor *ed, char *out, size_t cap) {
    if (g_path_override != NULL) {
        const int n = snprintf(out, cap, "%s", g_path_override);
        return n > 0 && (size_t)n < cap;
    }
    return de__ed_project_vpath(ed, "nav.toml", out, cap);
}

static void to_settings(de_nav_settings *s) {
    memset(s, 0, sizeof *s);
    s->agent_count = g_nv.agent_count;
    for (u32 i = 0; i < g_nv.agent_count; ++i) {
        const nv_agent *a = &g_nv.agents[i];
        de_nav_agent_settings *d = &s->agents[i];
        memcpy(d->name, a->name, sizeof d->name);
        d->radius = a->radius, d->height = a->height, d->max_slope_deg = a->max_slope_deg;
        d->step_height = a->step_height, d->cell_size = a->cell_size;
        d->cell_height = a->cell_height, d->collision_mask = a->collision_mask;
    }
    s->area_count = g_nv.area_count;
    for (u32 i = 0; i < g_nv.area_count; ++i) {
        s->areas[i].id = g_nv.areas[i].id;
        memcpy(s->areas[i].name, g_nv.areas[i].name, sizeof s->areas[i].name);
        s->areas[i].cost = g_nv.areas[i].cost;
    }
    for (u32 i = 0; i < 3; ++i)
        s->bounds_min[i] = g_nv.bmin[i], s->bounds_max[i] = g_nv.bmax[i];
    s->tile_size = g_nv.tile_size;
    s->auto_rebake = g_nv.auto_rebake;
}

static void from_settings(const de_nav_settings *s) {
    memset(g_nv.agents, 0, sizeof g_nv.agents);
    g_nv.agent_count = s->agent_count;
    for (u32 i = 0; i < s->agent_count; ++i) {
        const de_nav_agent_settings *a = &s->agents[i];
        nv_agent *d = &g_nv.agents[i];
        memcpy(d->name, a->name, sizeof d->name);
        d->radius = a->radius, d->height = a->height, d->max_slope_deg = a->max_slope_deg;
        d->step_height = a->step_height, d->cell_size = a->cell_size;
        d->cell_height = a->cell_height, d->collision_mask = a->collision_mask;
    }
    memset(g_nv.areas, 0, sizeof g_nv.areas);
    g_nv.area_count = 0;
    for (u32 id = 0; id < NV_MAX_AREAS; ++id) /* by id, as nav.configure keeps them */
        for (u32 i = 0; i < s->area_count; ++i)
            if (s->areas[i].id == id) {
                nv_area *r = &g_nv.areas[g_nv.area_count++];
                r->id = id;
                memcpy(r->name, s->areas[i].name, sizeof r->name);
                r->cost = s->areas[i].cost;
            }
    for (u32 i = 0; i < 3; ++i)
        g_nv.bmin[i] = s->bounds_min[i], g_nv.bmax[i] = s->bounds_max[i];
    g_nv.tile_size = s->tile_size;
    grid_of(g_nv.bmin, g_nv.bmax, g_nv.tile_size, &g_nv.tiles_x, &g_nv.tiles_y);
    g_nv.auto_rebake = s->auto_rebake;
}

/* Loads nav.toml when the project path is new. A missing file keeps the settings; a bad one is
 * logged and ignored (the next nav.configure rewrites it). Not while a bake runs or tiles exist:
 * the grid cannot change under them. */
static void load_project(de__editor *ed) {
    char path[1024];
    if (!settings_vpath(ed, path, sizeof path) || strcmp(path, g_nv.settings_path) == 0)
        return;
    if (g_nv.running || any_baked())
        return;
    memcpy(g_nv.settings_path, path, sizeof path);
    g_nv.tiles_pending = true;
    de_nav_settings s;
    const de_result r = de_nav_settings_load(path, &s);
    if (r == DE_OK) {
        from_settings(&s);
        ++g_nv.revision;
    } else if (r != DE_ERR_NOT_FOUND) {
        DE_WARN(ed->log, "nav: %s ignored: %s", path, de_error_last()->message);
    }
    de_error_clear();
}

static void save_project(de__editor *ed) {
    char path[1024];
    if (!settings_vpath(ed, path, sizeof path))
        return; /* no project yet: the settings stay in the session */
    memcpy(g_nv.settings_path, path, sizeof path);
    de_nav_settings s;
    to_settings(&s);
    if (de_nav_settings_save(path, &s) != DE_OK) {
        DE_WARN(ed->log, "nav: could not write %s: %s", path, de_error_last()->message);
        de_error_clear();
    }
}

/* ---------- Baked tiles nav.tiles (DENG-864) ---------- */

static bool same_desc(const de_navmesh_build_desc *a, const de_navmesh_build_desc *b) {
    return a->tile_size == b->tile_size && a->cell_size == b->cell_size &&
           a->cell_height == b->cell_height && a->agent_radius == b->agent_radius &&
           a->agent_height == b->agent_height && a->agent_max_climb == b->agent_max_climb &&
           a->agent_max_slope_rad == b->agent_max_slope_rad &&
           a->collision_mask == b->collision_mask;
}

static bool tiles_vpath(de__editor *ed, char *out, size_t cap) {
    char toml[1024];
    return settings_vpath(ed, toml, sizeof toml) && de__nav_tiles_path(toml, out, cap);
}

static de__nav_tile_record g_records[NV_MAX_AGENTS][NV_MAX_TILES];
static i32 g_txz[NV_MAX_TILES * 2];

/* Writes the baked tiles of every agent type, with their fingerprints, to nav.tiles. Called when
 * a bake ends; a tile baked empty is a record with no Detour data. */
static void save_tiles(de__editor *ed) {
    char path[1024];
    if (!tiles_vpath(ed, path, sizeof path))
        return; /* no project yet */
    de__nav_tiles_agent ags[NV_MAX_AGENTS];
    u8 *dnav[NV_MAX_AGENTS] = {NULL};
    memset(ags, 0, sizeof ags);
    bool ok = true;
    for (u32 i = 0; i < g_nv.agent_count; ++i) {
        de__nav_tiles_agent *g = &ags[i];
        const nv_nav *n = &g_nv.navs[i];
        snprintf(g->name, sizeof g->name, "%s", g_nv.agents[i].name);
        g->bmin_x = g_nv.bmin[0], g->bmin_y = g_nv.bmin[1];
        g->tile_size = g_nv.tile_size;
        g->tiles_x = g_nv.tiles_x, g->tiles_y = g_nv.tiles_y;
        g->records = g_records[i];
        if (n->tiles == NULL || n->nav == NULL)
            continue;
        u32 pairs = 0;
        for (u32 t = 0; t < tile_count(); ++t) {
            const nv_tile *r = &n->tiles[t];
            if (!r->baked)
                continue;
            g_records[i][g->record_count++] = (de__nav_tile_record){t, r->polys, r->links, r->fp};
            i32 tx, tz;
            if (engine_tile(n, t, &tx, &tz)) {
                g_txz[pairs * 2u] = tx;
                g_txz[pairs * 2u + 1u] = tz;
                ++pairs;
            }
        }
        if (de__nav_write_container(n->nav, heap(), g_txz, pairs, &dnav[i], &g->dnav_size) !=
            DE_OK) {
            ok = false;
            break;
        }
        g->dnav = dnav[i];
    }
    u8 *file = NULL;
    u64 size = 0;
    if (ok && de__nav_tiles_write(heap(), ags, g_nv.agent_count, &file, &size) == DE_OK) {
        if (de_file_write_all_atomic(path, file, size) != DE_OK) {
            DE_WARN(ed->log, "nav: could not write %s: %s", path, de_error_last()->message);
            de_error_clear();
        }
        DE_FREE(heap(), file, (size_t)size);
    } else {
        DE_WARN(ed->log, "nav: could not encode %s", path);
    }
    for (u32 i = 0; i < NV_MAX_AGENTS; ++i)
        if (dnav[i] != NULL)
            DE_FREE(heap(), dnav[i], (size_t)ags[i].dnav_size);
}

/* Loads nav.tiles into agent types of the same name, grid and build settings, so a restart keeps
 * the bake. Only into a world with no tile baked and no bake running. The tiles keep their saved
 * fingerprints, so the ones whose inputs differ in this world are DIRTY (and auto rebake redoes
 * them). A missing file is quiet; a bad one is logged and ignored (the next bake rewrites it). */
static void load_tiles(de__editor *ed) {
    g_nv.tiles_pending = false;
    char path[1024];
    if (g_nv.world == NULL || g_nv.running || any_baked() || !tiles_vpath(ed, path, sizeof path))
        return;
    void *data = NULL;
    u64 size = 0;
    if (de_file_read_all(path, heap(), &data, &size) != DE_OK) {
        de_error_clear();
        return;
    }
    const u8 *bytes = (const u8 *)data;
    u32 count = 0;
    const char *why = NULL;
    if (!de__nav_tiles_read(bytes, size, &count, &why)) {
        DE_WARN(ed->log, "nav: %s ignored: %s", path, why != NULL ? why : "malformed");
        count = 0;
    }
    for (u32 k = 0; k < count; ++k) {
        de__nav_tiles_agent g;
        if (!de__nav_tiles_agent_at(bytes, size, k, &g))
            continue;
        const i32 i = agent_index(de_ep_cstr(g.name));
        if (i < 0 || g.bmin_x != g_nv.bmin[0] || g.bmin_y != g_nv.bmin[1] ||
            g.tile_size != g_nv.tile_size || g.tiles_x != g_nv.tiles_x || g.tiles_y != g_nv.tiles_y)
            continue; /* another grid or agent type: the tiles do not apply */
        if (g.dnav_size > 0) {
            de__nav_container_info info;
            const de_navmesh_build_desc d = desc_of(&g_nv.agents[i]);
            if (!de__nav_container_read(g.dnav, g.dnav_size, &info, NULL) ||
                !same_desc(&info.desc, &d))
                continue; /* baked with other agent settings */
        }
        if (!nav_ensure(ed, (u32)i))
            continue;
        nv_nav *n = &g_nv.navs[i];
        if (g.dnav_size > 0 && de__nav_add_container(n->nav, g.dnav, g.dnav_size) != DE_OK) {
            DE_WARN(ed->log, "nav: %s: tiles of %s skipped: %s", path, g.name,
                    de_error_last()->message);
            de_error_clear();
            nav_reset(n);
            continue;
        }
        for (u32 r = 0; r < g.record_count; ++r) {
            de__nav_tile_record rec;
            de__nav_tiles_record(&g, r, &rec);
            nv_tile *t = &n->tiles[rec.tile];
            t->baked = true;
            t->revision = ++g_nv.revision;
            t->polys = rec.polys;
            t->links = rec.links;
            t->fp = rec.fp;
            t->ms = 0.0f;
        }
    }
    DE_FREE(heap(), data, (size_t)size + 1);
}

/* ---------- nav.info ---------- */

static bool f64_list(de_ep_arena *a, de_ep_list_f64 *out, const f64 *v, u32 n) {
    out->count = n;
    out->items = n > 0 ? (double *)de__ed_alloc(a, sizeof(double) * n) : NULL;
    if (n > 0 && out->items == NULL)
        return false;
    for (u32 i = 0; i < n; ++i)
        out->items[i] = v[i];
    return true;
}

/* The plane of the world's nav2d.grid system; side_xz when it has none. */
static de_plane_2d plane2d_of(de_world *w) {
    de_plane_2d p = DE_PLANE_SIDE_XZ;
    if (w != NULL)
        (void)de_nav2d_grid_plane(w, &p);
    return p;
}

static bool fill_info(de_ep_arena *a, de_ep_nav_info *o) {
    memset(o, 0, sizeof *o);
    static const f64 up[3] = {0, 0, 1};
    o->revision = g_nv.revision;
    if (!f64_list(a, &o->up, up, 3) || !f64_list(a, &o->bounds.min, g_nv.bmin, 3) ||
        !f64_list(a, &o->bounds.max, g_nv.bmax, 3))
        return false;
    o->tile_size = g_nv.tile_size;
    o->tiles_x = g_nv.tiles_x;
    o->tiles_y = g_nv.tiles_y;
    o->auto_rebake = g_nv.auto_rebake;
    o->agents.count = g_nv.agent_count;
    o->agents.items =
        (de_ep_nav_agent *)de__ed_alloc(a, sizeof(de_ep_nav_agent) * g_nv.agent_count);
    if (o->agents.items == NULL)
        return false;
    for (u32 i = 0; i < g_nv.agent_count; ++i) {
        const nv_agent *s = &g_nv.agents[i];
        de_ep_nav_agent *d = &o->agents.items[i];
        d->name = de_ep_cstr(s->name);
        d->radius = s->radius;
        d->height = s->height;
        d->max_slope_deg = s->max_slope_deg;
        d->step_height = s->step_height;
        d->cell_size = s->cell_size;
        d->cell_height = s->cell_height;
        d->collision_mask = s->collision_mask;
    }
    o->areas.count = g_nv.area_count;
    o->areas.items = (de_ep_nav_area *)de__ed_alloc(a, sizeof(de_ep_nav_area) * g_nv.area_count);
    if (o->areas.items == NULL)
        return false;
    for (u32 i = 0; i < g_nv.area_count; ++i) {
        o->areas.items[i].id = g_nv.areas[i].id;
        o->areas.items[i].name = de_ep_cstr(g_nv.areas[i].name);
        o->areas.items[i].cost = g_nv.areas[i].cost;
    }
    o->max_agents = NV_MAX_AGENTS;
    o->link_component = de_ep_cstr("DeNavLink");
    o->modifier_component = de_ep_cstr("DeNavModifier");
    o->grid_component = de_ep_cstr("DeNavGrid2D");
    o->has_bake = g_nv.running;
    o->bake = g_nv.running ? g_nv.bake : 0;
    de_ep_nav_overlay_params *ov = &o->overlay;
    ov->has_agent = g_nv.ov_has_agent;
    if (ov->has_agent)
        ov->agent = de_ep_cstr(g_nv.ov_agent);
    ov->navmesh = g_nv.ov_navmesh;
    ov->tiles = g_nv.ov_tiles;
    ov->links = g_nv.ov_links;
    ov->has_grids = true;
    ov->grids = g_nv.ov_grids;
    ov->has_path = g_nv.ov_has_path;
    if (ov->has_path && !f64_list(a, &ov->path, g_nv.ov_path, g_nv.ov_path_count))
        return false;
    o->has_plane2d = true;
    o->plane2d = plane2d_of(g_nv.world) == DE_PLANE_TOP_XY ? DE_EP_NAV_PLANE2D_TOP_XY
                                                           : DE_EP_NAV_PLANE2D_SIDE_XZ;
    return true;
}

static void on_info(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    sync_world(ed);
    de_ep_nav_info o;
    if (!fill_info(a, &o)) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "nav.info: the response does not fit");
        return;
    }
    de_ep_write_response_nav_info(w, req->id, &o);
}

/* ---------- nav.configure ---------- */

static bool agent_ok(const de_ep_nav_agent *g) {
    return g->radius > 0 && g->height > 0 && g->max_slope_deg >= 0 && g->max_slope_deg <= 89 &&
           g->step_height >= 0 && g->cell_size >= 0.05f && g->cell_size <= 2 &&
           g->cell_height >= 0.05f && g->cell_height <= 2 && isfinite(g->radius) &&
           isfinite(g->height) && isfinite(g->step_height);
}

static bool same_agent(const nv_agent *a, const de_ep_nav_agent *g) {
    return a->radius == g->radius && a->height == g->height &&
           a->max_slope_deg == g->max_slope_deg && a->step_height == g->step_height &&
           a->cell_size == g->cell_size && a->cell_height == g->cell_height &&
           a->collision_mask == g->collision_mask;
}

static bool any_baked(void) {
    for (u32 i = 0; i < g_nv.agent_count; ++i)
        for (u32 t = 0; g_nv.navs[i].tiles != NULL && t < tile_count(); ++t)
            if (g_nv.navs[i].tiles[t].baked)
                return true;
    return false;
}

static void on_configure(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                         mpack_writer_t *w) {
    DE__ED_PARAMS(nav_configure, de_ep_nav_configure_params, q);
    sync_world(ed);
    if (g_nv.running) {
        de__ed_error(w, req->id, DE_EP_ERR_CONFLICT, "nav.configure: a bake is running");
        return;
    }
    if (q.has_agents) {
        if (q.agents.count < 1 || q.agents.count > NV_MAX_AGENTS) {
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                         "nav.configure: 1 to %u agent types", NV_MAX_AGENTS);
            return;
        }
        for (u32 i = 0; i < q.agents.count; ++i) {
            const de_ep_nav_agent *g = &q.agents.items[i];
            if (g->name.len < 1 || g->name.len > NV_NAME_MAX) {
                de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                             "nav.configure: an agent name must have 1 to 32 characters");
                return;
            }
            for (u32 j = 0; j < i; ++j)
                if (q.agents.items[j].name.len == g->name.len &&
                    memcmp(q.agents.items[j].name.ptr, g->name.ptr, g->name.len) == 0) {
                    de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                                 "nav.configure: agent %.*s is listed twice", (int)g->name.len,
                                 g->name.ptr);
                    return;
                }
            if (!agent_ok(g)) {
                de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                             "nav.configure: agent \"%.*s\" has a value out of range",
                             (int)g->name.len, g->name.ptr);
                return;
            }
        }
    }
    if (q.has_areas) {
        bool has0 = false;
        for (u32 i = 0; i < q.areas.count; ++i) {
            const de_ep_nav_area *r = &q.areas.items[i];
            has0 = has0 || r->id == 0;
            bool dup = false;
            for (u32 j = 0; j < i; ++j)
                dup = dup || q.areas.items[j].id == r->id;
            if (r->id > 15 || dup || !(r->cost >= 1.0f) || !isfinite(r->cost) || r->name.len == 0 ||
                r->name.len > NV_AREA_NAME_MAX) {
                de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                             "nav.configure: area %u is out of range or listed twice", r->id);
                return;
            }
        }
        if (!has0) {
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                         "nav.configure: the areas must hold area 0");
            return;
        }
    }
    if (q.has_bounds && !bounds_ok(&q.bounds)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "nav.configure: bounds need 3 finite values each, max above min");
        return;
    }
    const f32 ts = q.has_tile_size ? q.tile_size : g_nv.tile_size;
    if (q.has_tile_size) {
        const f64 k = 256.0 / (f64)ts;
        if (!(ts > 0) || !isfinite(ts) || k != floor(k)) {
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                         "nav.configure: tileSize %g does not divide 256 m", (f64)ts);
            return;
        }
        if (ts != g_nv.tile_size && any_baked()) {
            de__ed_error(w, req->id, DE_EP_ERR_CONFLICT,
                         "nav.configure: tileSize is fixed by the first bake");
            return;
        }
    }
    f64 mn[3], mx[3];
    for (u32 i = 0; i < 3; ++i) {
        mn[i] = q.has_bounds ? q.bounds.min.items[i] : g_nv.bmin[i];
        mx[i] = q.has_bounds ? q.bounds.max.items[i] : g_nv.bmax[i];
    }
    if (fmod(mn[0], (f64)ts) != 0.0 || fmod(mn[1], (f64)ts) != 0.0) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "nav.configure: bounds.min x and y must be multiples of tileSize (%g m): "
                     "engine tiles lie on a world grid",
                     (f64)ts);
        return;
    }
    u32 tx, ty;
    grid_of(mn, mx, ts, &tx, &ty);
    if ((u64)tx * ty > NV_MAX_TILES) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "nav.configure: %u x %u tiles is more than %u", tx, ty, NV_MAX_TILES);
        return;
    }
    /* The engine's own limits (de__nav_settings_make), with the new tile size. */
    {
        const f32 keep_ts = g_nv.tile_size;
        g_nv.tile_size = ts;
        const u32 n = q.has_agents ? q.agents.count : g_nv.agent_count;
        for (u32 i = 0; i < n; ++i) {
            nv_agent t;
            memset(&t, 0, sizeof t);
            if (q.has_agents) {
                const de_ep_nav_agent *g = &q.agents.items[i];
                t.radius = g->radius, t.height = g->height, t.max_slope_deg = g->max_slope_deg;
                t.step_height = g->step_height, t.cell_size = g->cell_size;
                t.cell_height = g->cell_height, t.collision_mask = g->collision_mask;
            } else {
                t = g_nv.agents[i];
            }
            const de_navmesh_build_desc d = desc_of(&t);
            de__nav_settings st;
            const char *why = NULL;
            if (de__nav_settings_make(&d, &st, &why) != DE_OK) {
                g_nv.tile_size = keep_ts;
                de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "nav.configure: %s",
                             why != NULL ? why : "settings out of range");
                return;
            }
        }
        g_nv.tile_size = keep_ts;
    }

    /* Apply. A changed grid origin or tile size drops every navmesh. */
    const bool regrid = ts != g_nv.tile_size || mn[0] != g_nv.bmin[0] || mn[1] != g_nv.bmin[1];
    if (regrid) {
        for (u32 i = 0; i < NV_MAX_AGENTS; ++i)
            nav_reset(&g_nv.navs[i]);
    } else if (tx != g_nv.tiles_x || ty != g_nv.tiles_y) {
        /* Same origin: keep the records of tiles still in the grid. */
        for (u32 i = 0; i < g_nv.agent_count; ++i) {
            nv_nav *n = &g_nv.navs[i];
            if (n->tiles == NULL)
                continue;
            static nv_tile keep[NV_MAX_TILES];
            memcpy(keep, n->tiles, sizeof keep);
            memset(n->tiles, 0, sizeof(nv_tile) * NV_MAX_TILES);
            for (u32 y = 0; y < ty && y < g_nv.tiles_y; ++y)
                for (u32 x = 0; x < tx && x < g_nv.tiles_x; ++x)
                    n->tiles[y * tx + x] = keep[y * g_nv.tiles_x + x];
        }
    }
    if (q.has_agents) {
        nv_agent na[NV_MAX_AGENTS];
        nv_nav nn[NV_MAX_AGENTS];
        memset(na, 0, sizeof na);
        memset(nn, 0, sizeof nn);
        bool kept[NV_MAX_AGENTS] = {false};
        for (u32 i = 0; i < q.agents.count; ++i) {
            const de_ep_nav_agent *g = &q.agents.items[i];
            str_copy(na[i].name, sizeof na[i].name, g->name);
            na[i].radius = g->radius, na[i].height = g->height;
            na[i].max_slope_deg = g->max_slope_deg, na[i].step_height = g->step_height;
            na[i].cell_size = g->cell_size, na[i].cell_height = g->cell_height;
            na[i].collision_mask = g->collision_mask;
            const i32 old = agent_index(g->name);
            if (old >= 0 && same_agent(&g_nv.agents[old], g)) {
                nn[i] = g_nv.navs[old];
                kept[old] = true;
            }
        }
        for (u32 i = 0; i < NV_MAX_AGENTS; ++i)
            if (!kept[i])
                nav_free(&g_nv.navs[i]);
        memcpy(g_nv.agents, na, sizeof na);
        memcpy(g_nv.navs, nn, sizeof nn);
        g_nv.agent_count = q.agents.count;
        if (g_nv.ov_has_agent && agent_index(de_ep_cstr(g_nv.ov_agent)) < 0)
            g_nv.ov_has_agent = false;
    }
    if (q.has_areas) {
        g_nv.area_count = 0;
        for (u32 id = 0; id < NV_MAX_AREAS; ++id)
            for (u32 i = 0; i < q.areas.count; ++i)
                if (q.areas.items[i].id == id) {
                    nv_area *r = &g_nv.areas[g_nv.area_count++];
                    r->id = id;
                    str_copy(r->name, sizeof r->name, q.areas.items[i].name);
                    r->cost = q.areas.items[i].cost;
                }
    }
    for (u32 i = 0; i < 3; ++i)
        g_nv.bmin[i] = mn[i], g_nv.bmax[i] = mx[i];
    g_nv.tile_size = ts;
    g_nv.tiles_x = tx;
    g_nv.tiles_y = ty;
    if (q.has_auto_rebake)
        g_nv.auto_rebake = q.auto_rebake;
    ++g_nv.revision;
    save_project(ed);

    de_ep_nav_info o;
    if (!fill_info(a, &o)) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "nav.configure: the response does not fit");
        return;
    }
    de_ep_write_response_nav_configure(w, req->id, &o);
}

/* ---------- nav.bake and nav.cancelBake ---------- */

static void on_bake(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(nav_bake, de_ep_nav_bake_params, q);
    sync_world(ed);
    if (g_nv.running) {
        de__ed_error(w, req->id, DE_EP_ERR_CONFLICT, "nav.bake: bake %u is running", g_nv.bake);
        return;
    }
    i32 only = -1;
    if (q.has_agent) {
        only = agent_index(q.agent);
        if (only < 0) {
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "nav.bake: no agent type %.*s",
                         (int)q.agent.len, q.agent.ptr);
            return;
        }
    }
    if (q.has_region && !bounds_ok(&q.region)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "nav.bake: region needs 3 finite values each, max above min");
        return;
    }
    if (g_nv.world == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "nav.bake: no world");
        return;
    }
    i32 ix0 = 0, iy0 = 0, ix1 = (i32)g_nv.tiles_x - 1, iy1 = (i32)g_nv.tiles_y - 1;
    bool meets = true;
    if (q.has_region)
        meets = tile_range(q.region.min.items[0], q.region.min.items[1], q.region.max.items[0],
                           q.region.max.items[1], &ix0, &iy0, &ix1, &iy1);
    for (u32 i = 0; i < g_nv.agent_count; ++i)
        if ((only < 0 || (u32)only == i) && !nav_ensure(ed, i)) {
            de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "nav.bake: cannot create the navmesh");
            return;
        }
    const bool dirty_only = q.has_dirty_only && q.dirty_only;
    if (dirty_only) {
        refresh_inputs(ed);
        for (u32 i = 0; i < g_nv.agent_count; ++i)
            if (only < 0 || (u32)only == i)
                fingerprints(i);
    }
    start_bake(false);
    for (u32 i = 0; i < g_nv.agent_count && meets; ++i) {
        if (only >= 0 && (u32)only != i)
            continue;
        const nv_nav *n = &g_nv.navs[i];
        for (i32 y = iy0; y <= iy1; ++y)
            for (i32 x = ix0; x <= ix1; ++x) {
                const u32 t = (u32)y * g_nv.tiles_x + (u32)x;
                if (dirty_only && n->tiles[t].baked && n->tiles[t].fp == n->fp[t])
                    continue;
                enqueue(i, t);
            }
    }
    de_ep_nav_bake_started o = {g_nv.bake, g_nv.total};
    de_ep_write_response_nav_bake(w, req->id, &o);
    (void)a;
}

static void on_cancel(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                      mpack_writer_t *w) {
    DE__ED_PARAMS(nav_cancel_bake, de_ep_nav_cancel_bake_params, q);
    (void)a;
    if (!g_nv.running || q.bake != g_nv.bake) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "nav.cancelBake: bake %u is not running",
                     q.bake);
        return;
    }
    clear_queue();
    g_nv.landed_count = 0;
    de_ep_empty o;
    memset(&o, 0, sizeof o);
    de_ep_write_response_nav_cancel_bake(w, req->id, &o);
    send_progress(ed, DE_EP_NAV_BAKE_STATE_CANCELED, NULL);
    g_nv.running = false;
    save_tiles(ed); /* the tiles that landed */
}

/* ---------- nav.tiles and nav.tileMesh ---------- */

static void on_tiles(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(nav_tiles, de_ep_nav_tiles_params, q);
    sync_world(ed);
    const i32 ai = agent_index(q.agent);
    if (ai < 0) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "nav.tiles: no agent type %.*s",
                     (int)q.agent.len, q.agent.ptr);
        return;
    }
    if (q.has_region && !bounds_ok(&q.region)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "nav.tiles: region needs 3 finite values each, max above min");
        return;
    }
    i32 ix0 = 0, iy0 = 0, ix1 = (i32)g_nv.tiles_x - 1, iy1 = (i32)g_nv.tiles_y - 1;
    bool meets = true;
    if (q.has_region)
        meets = tile_range(q.region.min.items[0], q.region.min.items[1], q.region.max.items[0],
                           q.region.max.items[1], &ix0, &iy0, &ix1, &iy1);
    nv_nav *n = &g_nv.navs[ai];
    if (n->tiles != NULL && n->nav != NULL) {
        refresh_inputs(ed);
        fingerprints((u32)ai);
    }
    de_ep_nav_tiles o;
    memset(&o, 0, sizeof o);
    o.agent = q.agent;
    o.revision = g_nv.revision;
    const u32 count = meets ? (u32)((ix1 - ix0 + 1) * (iy1 - iy0 + 1)) : 0;
    o.tiles.items = count > 0 ? (de_ep_nav_tile *)de__ed_alloc(a, sizeof(de_ep_nav_tile) * count)
                              : NULL;
    if (count > 0 && o.tiles.items == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "nav.tiles: the response does not fit");
        return;
    }
    for (i32 y = iy0; meets && y <= iy1; ++y)
        for (i32 x = ix0; x <= ix1; ++x) {
            const u32 t = (u32)y * g_nv.tiles_x + (u32)x;
            de_ep_nav_tile *d = &o.tiles.items[o.tiles.count++];
            memset(d, 0, sizeof *d);
            d->x = x;
            d->y = y;
            if (n->tiles == NULL) {
                d->state = DE_EP_NAV_TILE_STATE_UNBAKED;
                continue;
            }
            const nv_tile *r = &n->tiles[t];
            d->state = tile_state(n, t);
            d->revision = r->baked ? r->revision : 0;
            d->polys = r->polys;
            d->links = r->links;
            d->ms = r->ms;
        }
    de_ep_write_response_nav_tiles(w, req->id, &o);
}

static void on_tile_mesh(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                         mpack_writer_t *w) {
    DE__ED_PARAMS(nav_tile_mesh, de_ep_nav_tile_mesh_params, q);
    sync_world(ed);
    const i32 ai = agent_index(q.agent);
    if (ai < 0) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "nav.tileMesh: no agent type %.*s",
                     (int)q.agent.len, q.agent.ptr);
        return;
    }
    if (q.x < 0 || q.y < 0 || (u32)q.x >= g_nv.tiles_x || (u32)q.y >= g_nv.tiles_y) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "nav.tileMesh: tile (%d, %d)", q.x, q.y);
        return;
    }
    const nv_nav *n = &g_nv.navs[ai];
    const u32 t = (u32)q.y * g_nv.tiles_x + (u32)q.x;
    de_ep_nav_tile_mesh o;
    memset(&o, 0, sizeof o);
    i32 tx, tz;
    if (n->tiles != NULL && n->tiles[t].baked && engine_tile(n, t, &tx, &tz)) {
        o.revision = n->tiles[t].revision;
        const u32 count = de__nav_tile_tris(n->nav, tx, tz, NULL, NULL, 0);
        if (count > 0) {
            de_dvec3 *pts = (de_dvec3 *)DE_ALLOC(heap(), sizeof(de_dvec3) * 3 * count, 16,
                                                 DE_MEM_GENERAL);
            u8 *ar = (u8 *)DE_ALLOC(heap(), count, 16, DE_MEM_GENERAL);
            o.positions.items = (float *)de__ed_alloc(a, sizeof(float) * 9 * count);
            o.triangles.items = (uint32_t *)de__ed_alloc(a, sizeof(uint32_t) * 3 * count);
            o.areas.items = (uint32_t *)de__ed_alloc(a, sizeof(uint32_t) * count);
            const bool ok = pts != NULL && ar != NULL && o.positions.items != NULL &&
                            o.triangles.items != NULL && o.areas.items != NULL;
            if (ok) {
                (void)de__nav_tile_tris(n->nav, tx, tz, pts, ar, count);
                for (u32 k = 0; k < count * 3; ++k) {
                    o.positions.items[k * 3] = (float)pts[k].x;
                    o.positions.items[k * 3 + 1] = (float)pts[k].y;
                    o.positions.items[k * 3 + 2] = (float)pts[k].z;
                    o.triangles.items[k] = k;
                }
                for (u32 k = 0; k < count; ++k)
                    o.areas.items[k] = ar[k];
                o.positions.count = count * 9;
                o.triangles.count = count * 3;
                o.areas.count = count;
            }
            if (pts != NULL)
                DE_FREE(heap(), pts, sizeof(de_dvec3) * 3 * count);
            if (ar != NULL)
                DE_FREE(heap(), ar, count);
            if (!ok) {
                de__ed_error(w, req->id, DE_EP_ERR_BUSY, "nav.tileMesh: the response does not fit");
                return;
            }
        }
    }
    de_ep_write_response_nav_tile_mesh(w, req->id, &o);
}

/* ---------- nav.testPath ---------- */

static void on_test_path(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                         mpack_writer_t *w) {
    DE__ED_PARAMS(nav_test_path, de_ep_nav_test_path_params, q);
    sync_world(ed);
    const i32 ai = agent_index(q.agent);
    if (ai < 0) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "nav.testPath: no agent type %.*s",
                     (int)q.agent.len, q.agent.ptr);
        return;
    }
    if (!finite3(&q.from) || !finite3(&q.to)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "nav.testPath: %s needs 3 finite values",
                     finite3(&q.from) ? "to" : "from");
        return;
    }
    nv_nav *n = &g_nv.navs[ai];
    const de_dvec3 from = {q.from.items[0], q.from.items[1], q.from.items[2]};
    const de_dvec3 to = {q.to.items[0], q.to.items[1], q.to.items[2]};
    const f32 radius = g_nv.agents[ai].radius * 2.0f > 1.0f ? g_nv.agents[ai].radius * 2.0f : 1.0f;
    de_dvec3 snap;
    if (n->nav == NULL || !de_nav_nearest_point(n->nav, from, radius, &snap)) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "nav.testPath: from is not near the navmesh");
        return;
    }
    if (!de_nav_nearest_point(n->nav, to, radius, &snap)) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "nav.testPath: to is not near the navmesh");
        return;
    }
    f32 cost[16];
    for (u32 i = 0; i < 16; ++i)
        cost[i] = 1.0f;
    for (u32 i = 0; i < g_nv.area_count; ++i)
        cost[g_nv.areas[i].id] = g_nv.areas[i].cost;
    enum { CORRIDOR = 2048, LINKS = 256 };
    de_dvec3 *corridor = (de_dvec3 *)de__ed_alloc(a, sizeof(de_dvec3) * CORRIDOR);
    u32 *link_ids = (u32 *)de__ed_alloc(a, sizeof(u32) * LINKS);
    if (corridor == NULL || link_ids == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "nav.testPath: the response does not fit");
        return;
    }
    de__nav_path_detail d;
    memset(&d, 0, sizeof d);
    d.area_cost = cost;
    d.corridor = corridor;
    d.corridor_cap = CORRIDOR;
    d.link_ids = link_ids;
    d.link_cap = LINKS;
    de_nav_path path;
    memset(&path, 0, sizeof path);
    const u32 mask = q.has_area_mask ? q.area_mask : 0;
    const de_result r = de__nav_find_path_detail(n->nav, from, to, mask, heap(), &path, &d);
    if (r != DE_OK) {
        de__ed_error(w, req->id, r == DE_ERR_NOT_FOUND ? DE_EP_ERR_NOT_FOUND : DE_EP_ERR_INTERNAL,
                     "nav.testPath: no path");
        return;
    }
    de_ep_nav_path o;
    memset(&o, 0, sizeof o);
    o.points.items = path.count > 0 ? (double *)de__ed_alloc(a, sizeof(double) * 3 * path.count)
                                    : NULL;
    const u32 cc = d.corridor_count < CORRIDOR ? d.corridor_count : CORRIDOR;
    o.corridor.items = cc > 0 ? (double *)de__ed_alloc(a, sizeof(double) * 3 * cc) : NULL;
    const u32 lc = d.link_count < LINKS ? d.link_count : LINKS;
    o.links.items = lc > 0 ? (uint64_t *)de__ed_alloc(a, sizeof(uint64_t) * lc) : NULL;
    if ((path.count > 0 && o.points.items == NULL) || (cc > 0 && o.corridor.items == NULL) ||
        (lc > 0 && o.links.items == NULL)) {
        de_nav_path_free(heap(), &path);
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "nav.testPath: the response does not fit");
        return;
    }
    for (u32 k = 0; k < path.count; ++k) {
        o.points.items[k * 3] = path.points[k].x;
        o.points.items[k * 3 + 1] = path.points[k].y;
        o.points.items[k * 3 + 2] = path.points[k].z;
        if (k > 0) {
            const f64 dx = path.points[k].x - path.points[k - 1].x;
            const f64 dy = path.points[k].y - path.points[k - 1].y;
            const f64 dz = path.points[k].z - path.points[k - 1].z;
            o.length += sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    o.points.count = path.count * 3;
    for (u32 k = 0; k < cc; ++k) {
        o.corridor.items[k * 3] = corridor[k].x;
        o.corridor.items[k * 3 + 1] = corridor[k].y;
        o.corridor.items[k * 3 + 2] = corridor[k].z;
    }
    o.corridor.count = cc * 3;
    for (u32 k = 0; k < lc; ++k) {
        const u32 id = link_ids[k];
        if (id >= 1 && id <= NV_MAX_LINKS && g_nv.link_entity[id - 1] != 0)
            o.links.items[o.links.count++] = g_nv.link_entity[id - 1];
    }
    o.cost = d.cost;
    o.partial = path.partial;
    o.revision = g_nv.revision;
    de_nav_path_free(heap(), &path);
    de_ep_write_response_nav_test_path(w, req->id, &o);
}

/* ---------- nav.testPath2d ---------- */

/* The cell of plane coordinate `p` on `g`: false when outside the grid. */
static bool cell_of(const de_nav2d_grid *g, de_vec2 p, u32 *cx, u32 *cy) {
    const f64 fx = floor(((f64)p.x - (f64)g->origin.x) / (f64)g->cell_size);
    const f64 fy = floor(((f64)p.y - (f64)g->origin.y) / (f64)g->cell_size);
    if (!(fx >= 0.0 && fx < (f64)g->width && fy >= 0.0 && fy < (f64)g->height))
        return false;
    *cx = (u32)fx;
    *cy = (u32)fy;
    return true;
}

/* The grid entity's coordinate across the plane: the points keep it (the protocol's "off-plane
 * coordinate is the grid entity's"). Read as the system reads the origin: global, else local. */
static f64 depth_of(de_world *w, de_entity e, de_plane_2d plane) {
    const de_builtin_component_ids b = de_builtin_components(w);
    const de_transform *t = NULL;
    if (DE_HANDLE_VALID(b.global_transform))
        t = (const de_transform *)de_entity_get_const(w, e, b.global_transform);
    if (t == NULL && DE_HANDLE_VALID(b.transform))
        t = (const de_transform *)de_entity_get_const(w, e, b.transform);
    if (t == NULL)
        return 0.0;
    return plane == DE_PLANE_SIDE_XZ ? t->position.y : t->position.z;
}

/* The built grid of `e`. The system builds grids in PreUpdate; when the component is valid but
 * has no grid yet (added since the last run, or the schedule is not running), build now. A flush
 * builds every grid again, so it runs only for a valid grid that is missing. */
static bool built_grid(de_world *w, de_entity e, de_nav2d_grid *out, u32 *revision) {
    if (de_nav2d_grid_get(w, e, out, revision))
        return true;
    const DeNavGrid2D *c = g_nv.has_ids ? (const DeNavGrid2D *)de_entity_get_const(w, e, g_nv.ids.grid)
                                         : NULL;
    if (c == NULL || c->width < 1 || c->width > 64 || c->height < 1 || c->height > 64 ||
        !(c->cell_size > 0.0f))
        return false;
    de_nav2d_grid_flush(w);
    return de_nav2d_grid_get(w, e, out, revision);
}

static void on_test_path2d(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                           mpack_writer_t *w) {
    DE__ED_PARAMS(nav_test_path2d, de_ep_nav_test_path2d_params, q);
    sync_world(ed);
    de_world *world = g_nv.world;
    de_entity e;
    e.bits = q.grid;
    de_nav2d_grid grid;
    u32 revision = 0;
    const bool have = world != NULL && de_entity_alive(world, e) &&
                      built_grid(world, e, &grid, &revision);
    if (!have) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "nav.testPath2d: entity %llu has no valid DeNavGrid2D grid",
                     (unsigned long long)q.grid);
        return;
    }
    if (!finite3(&q.from) || !finite3(&q.to)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "nav.testPath2d: %s needs 3 finite values", finite3(&q.from) ? "to" : "from");
        return;
    }
    const de_plane_2d plane = plane2d_of(world);
    const de_dvec3 fw = {q.from.items[0], q.from.items[1], q.from.items[2]};
    const de_dvec3 tw = {q.to.items[0], q.to.items[1], q.to.items[2]};
    const de_vec2 from = de_nav2d_from_world(plane, fw);
    const de_vec2 to = de_nav2d_from_world(plane, tw);
    const char *name[2] = {"from", "to"};
    const de_vec2 ends[2] = {from, to};
    for (u32 k = 0; k < 2; ++k) {
        u32 cx, cy;
        if (!cell_of(&grid, ends[k], &cx, &cy)) {
            de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND,
                         "nav.testPath2d: %s is outside the grid", name[k]);
            return;
        }
        if (grid.walkable[cy * grid.width + cx] == 0) {
            de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND,
                         "nav.testPath2d: %s is on a blocked cell", name[k]);
            return;
        }
    }
    const bool jps = q.has_jps ? q.jps : true;
    de_vec2 *pts = NULL;
    u32 count = 0;
    const de_result r = de_nav2d_find_path(&grid, from, to, jps, heap(), &pts, &count);
    if (r != DE_OK) {
        de__ed_error(w, req->id, r == DE_ERR_NOT_FOUND ? DE_EP_ERR_NOT_FOUND : DE_EP_ERR_INTERNAL,
                     r == DE_ERR_NOT_FOUND ? "nav.testPath2d: from and to are not connected"
                                           : "nav.testPath2d: the path search failed");
        return;
    }
    de_ep_nav_path2d o;
    memset(&o, 0, sizeof o);
    o.points.items = count > 0 ? (double *)de__ed_alloc(a, sizeof(double) * 3 * count) : NULL;
    if (count > 0 && o.points.items == NULL) {
        de_nav2d_path_free(heap(), pts, count);
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "nav.testPath2d: the response does not fit");
        return;
    }
    const f64 depth = depth_of(world, e, plane);
    for (u32 k = 0; k < count; ++k) {
        const de_dvec3 p = de_nav2d_to_world(plane, pts[k], depth);
        o.points.items[k * 3] = p.x;
        o.points.items[k * 3 + 1] = p.y;
        o.points.items[k * 3 + 2] = p.z;
        if (k > 0) {
            const f64 dx = (f64)pts[k].x - (f64)pts[k - 1].x;
            const f64 dy = (f64)pts[k].y - (f64)pts[k - 1].y;
            o.length += sqrt(dx * dx + dy * dy);
        }
    }
    o.points.count = count * 3;
    o.revision = revision;
    de_nav2d_path_free(heap(), pts, count);
    de_ep_write_response_nav_test_path2d(w, req->id, &o);
}

/* ---------- nav.overlay ---------- */

static void on_overlay(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                       mpack_writer_t *w) {
    DE__ED_PARAMS(nav_overlay, de_ep_nav_overlay_params, q);
    (void)ed;
    (void)a;
    if (q.has_agent && agent_index(q.agent) < 0) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "nav.overlay: no agent type %.*s",
                     (int)q.agent.len, q.agent.ptr);
        return;
    }
    if (q.has_path) {
        bool ok = q.path.count % 3 == 0 && q.path.count <= NV_PATH_VALUES;
        for (u32 i = 0; ok && i < q.path.count; ++i)
            ok = isfinite(q.path.items[i]);
        if (!ok) {
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                         "nav.overlay: path needs 3 finite values per point (at most %u points)",
                         NV_PATH_VALUES / 3);
            return;
        }
    }
    g_nv.ov_has_agent = q.has_agent;
    if (q.has_agent)
        str_copy(g_nv.ov_agent, sizeof g_nv.ov_agent, q.agent);
    g_nv.ov_navmesh = q.navmesh;
    g_nv.ov_tiles = q.tiles;
    g_nv.ov_links = q.links;
    g_nv.ov_grids = q.has_grids && q.grids;
    g_nv.ov_has_path = q.has_path;
    g_nv.ov_path_count = q.has_path ? q.path.count : 0;
    for (u32 i = 0; i < g_nv.ov_path_count; ++i)
        g_nv.ov_path[i] = q.path.items[i];
    de_ep_empty o;
    memset(&o, 0, sizeof o);
    de_ep_write_response_nav_overlay(w, req->id, &o);
}

static de_vec4 rgba(f32 r, f32 g, f32 b) {
    de_vec4 c;
    c.x = r, c.y = g, c.z = b, c.w = 1.0f;
    return c;
}

/* DENG-863: the cells of every DeNavGrid2D grid as flat boxes in the navigation category:
 * walkable cells by area colour, blocked cells (255) dark. Cells of a grid the system has not
 * built (an invalid grid) are skipped. */
static void draw_grids(void) {
    de_world *w = g_nv.world;
    if (w == NULL || !g_nv.has_ids)
        return;
    scan(w, g_nv.ids.grid);
    const de_plane_2d plane = plane2d_of(w);
    const de_draw_category c = g_nv.cat;
    u32 cells = 0;
    for (u32 i = 0; i < g_scan.n && cells < NV_DRAW_CELLS; ++i) {
        de_entity e;
        e.bits = g_scan.bits[i];
        de_nav2d_grid g;
        const DeNavGrid2D *comp = (const DeNavGrid2D *)de_entity_get_const(w, e, g_nv.ids.grid);
        if (comp == NULL || !built_grid(w, e, &g, NULL))
            continue;
        const f64 depth = depth_of(w, e, plane);
        for (u32 y = 0; y < g.height && cells < NV_DRAW_CELLS; ++y)
            for (u32 x = 0; x < g.width && cells < NV_DRAW_CELLS; ++x, ++cells) {
                const u8 area = comp->cells[y * g.width + x];
                const de_vec4 col = area == DE_NAV_GRID2D_BLOCKED
                                        ? rgba(0.12f, 0.12f, 0.15f)
                                        : rgba(0.2f + 0.05f * (f32)(area % 16), 0.7f,
                                               0.9f - 0.05f * (f32)(area % 16));
                const de_vec2 lo = {g.origin.x + (f32)x * g.cell_size,
                                    g.origin.y + (f32)y * g.cell_size};
                const de_vec2 hi = {lo.x + g.cell_size, lo.y + g.cell_size};
                const de_dvec3 a = de_nav2d_to_world(plane, lo, depth);
                const de_dvec3 b = de_nav2d_to_world(plane, hi, depth);
                const de_dvec3 mn = {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y,
                                     a.z < b.z ? a.z : b.z};
                const de_dvec3 mx = {a.x < b.x ? b.x : a.x, a.y < b.y ? b.y : a.y,
                                     a.z < b.z ? b.z : a.z};
                de_draw_box(c, mn, mx, col, 0.0f);
            }
    }
}

static void draw_overlay(void) {
    if (g_nv.ov_grids)
        draw_grids();
    if (!g_nv.ov_has_agent)
        return;
    const i32 ai = agent_index(de_ep_cstr(g_nv.ov_agent));
    if (ai < 0)
        return;
    const nv_nav *n = &g_nv.navs[ai];
    const de_draw_category c = g_nv.cat;
    u32 lines = 0;
    const f64 z = g_nv.bmin[2];
    for (u32 t = 0; n->tiles != NULL && t < tile_count(); ++t) {
        if (g_nv.ov_tiles) {
            const de_ep_nav_tile_state st = tile_state(n, t);
            const de_vec4 col = st == DE_EP_NAV_TILE_STATE_BAKED   ? rgba(0.2f, 0.8f, 0.3f)
                                : st == DE_EP_NAV_TILE_STATE_DIRTY ? rgba(0.9f, 0.7f, 0.1f)
                                : st == DE_EP_NAV_TILE_STATE_BAKING ? rgba(0.2f, 0.6f, 1.0f)
                                                                     : rgba(0.4f, 0.4f, 0.4f);
            f64 x0, y0, x1, y1;
            tile_rect(t, &x0, &y0, &x1, &y1);
            const de_dvec3 p[4] = {{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}};
            for (u32 k = 0; k < 4; ++k)
                de_draw_line(c, p[k], p[(k + 1) % 4], col, 0.0f);
            lines += 4;
        }
        i32 tx, tz;
        if (g_nv.ov_navmesh && n->tiles[t].baked && lines < NV_DRAW_LINES &&
            engine_tile(n, t, &tx, &tz)) {
            u32 tris = de__nav_tile_tris(n->nav, tx, tz, g_draw_pts, g_draw_areas, NV_DRAW_TRIS);
            tris = tris < NV_DRAW_TRIS ? tris : NV_DRAW_TRIS;
            for (u32 k = 0; k < tris && lines < NV_DRAW_LINES; ++k) {
                const u8 ar = g_draw_areas[k];
                const de_vec4 col = rgba(0.2f + 0.05f * (f32)(ar % 16), 0.7f,
                                         0.9f - 0.05f * (f32)(ar % 16));
                const de_dvec3 *p = &g_draw_pts[k * 3];
                de_draw_line(c, p[0], p[1], col, 0.0f);
                de_draw_line(c, p[1], p[2], col, 0.0f);
                de_draw_line(c, p[2], p[0], col, 0.0f);
                lines += 3;
            }
        }
    }
    if (g_nv.ov_links && n->nav != NULL)
        for (u32 k = 0; k < n->nav->link_count; ++k) {
            const de__nav_link *l = &n->nav->links[k];
            de_draw_arrow(c, l->from, l->to, rgba(1.0f, 0.5f, 0.1f), 0.0f);
            if (l->bidirectional)
                de_draw_arrow(c, l->to, l->from, rgba(1.0f, 0.5f, 0.1f), 0.0f);
        }
    if (g_nv.ov_has_path)
        for (u32 k = 3; k + 2 < g_nv.ov_path_count; k += 3) {
            const de_dvec3 p0 = {g_nv.ov_path[k - 3], g_nv.ov_path[k - 2], g_nv.ov_path[k - 1]};
            const de_dvec3 p1 = {g_nv.ov_path[k], g_nv.ov_path[k + 1], g_nv.ov_path[k + 2]};
            de_draw_line(c, p0, p1, rgba(1.0f, 1.0f, 0.2f), 0.0f);
        }
}

/* ---------- Tick, reset, register ---------- */

static void tick(de__editor *ed) {
    sync_world(ed);
    load_project(ed);
    if (g_nv.world == NULL)
        return;
    if (g_nv.tiles_pending)
        load_tiles(ed);
    ++g_nv.frame;
    land(ed);
    if (g_nv.running) {
        submit(ed);
        if (!g_nv.running) /* failed */
            return;
        if (g_nv.qcount == 0 && !bake_in_flight()) {
            send_progress(ed, DE_EP_NAV_BAKE_STATE_DONE, NULL);
            g_nv.running = false;
            save_tiles(ed);
        } else if (g_nv.landed_count > 0) {
            send_progress(ed, DE_EP_NAV_BAKE_STATE_RUNNING, NULL);
        }
    } else {
        auto_check(ed);
    }
    draw_overlay();
}

static void reset(de__editor *ed) {
    (void)ed;
    if (g_nv.running) {
        clear_queue();
        g_nv.running = false;
    }
    g_nv.landed_count = 0;
    g_nv.ov_has_agent = false;
    g_nv.ov_has_path = false;
    g_nv.ov_path_count = 0;
    g_nv.ov_navmesh = g_nv.ov_tiles = g_nv.ov_links = g_nv.ov_grids = false;
}

void de__ed_nav_register(de__editor *ed) {
    for (u32 i = 0; i < NV_MAX_AGENTS; ++i)
        nav_free(&g_nv.navs[i]);
    memset(&g_nv, 0, sizeof g_nv);
    set_defaults();
    load_project(ed);
    g_nv.cat = de_draw_category_register("navigation", 0x60e060, false);
    sync_world(ed);
    if (g_nv.tiles_pending)
        load_tiles(ed);
    de__ed_on(ed, DE_EP_METHOD_NAV_INFO, on_info);
    de__ed_on(ed, DE_EP_METHOD_NAV_CONFIGURE, on_configure);
    de__ed_on(ed, DE_EP_METHOD_NAV_BAKE, on_bake);
    de__ed_on(ed, DE_EP_METHOD_NAV_CANCEL_BAKE, on_cancel);
    de__ed_on(ed, DE_EP_METHOD_NAV_TILES, on_tiles);
    de__ed_on(ed, DE_EP_METHOD_NAV_TILE_MESH, on_tile_mesh);
    de__ed_on(ed, DE_EP_METHOD_NAV_TEST_PATH, on_test_path);
    de__ed_on(ed, DE_EP_METHOD_NAV_TEST_PATH2D, on_test_path2d);
    de__ed_on(ed, DE_EP_METHOD_NAV_OVERLAY, on_overlay);
    de__ed_on_tick(ed, tick);
    de__ed_on_reset(ed, reset);
    de__ed_add_capability(ed, NV_CAP);
}

void de__ed_nav_shutdown(de__editor *ed) {
    (void)ed;
    for (u32 i = 0; i < NV_MAX_AGENTS; ++i)
        nav_free(&g_nv.navs[i]);
    g_nv.world = NULL;
    g_nv.running = false;
    g_nv.has_ids = false;
}
