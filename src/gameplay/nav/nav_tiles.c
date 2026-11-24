/* nav_tiles.c: the editor bake file "DNVT", `nav.tiles` (DENG-864). See nav_build.h and
 * docs/formats/nav_tiles.md. Pure byte code: no libm (the nav module is deterministic). */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* snprintf */
#endif
#include "gameplay/nav/nav_build.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include <stdio.h>
#include <string.h>

#define NT_HEADER 16u
#define NT_AGENT  80u
#define NT_RECORD 24u

static void put_u32(u8 *p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}
static u32 get_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void put_u64(u8 *p, u64 v) {
    put_u32(p, (u32)v);
    put_u32(p + 4, (u32)(v >> 32));
}
static u64 get_u64(const u8 *p) {
    return (u64)get_u32(p) | ((u64)get_u32(p + 4) << 32);
}
static void put_f32(u8 *p, f32 v) {
    u32 u;
    memcpy(&u, &v, 4);
    put_u32(p, u);
}
static f32 get_f32(const u8 *p) {
    const u32 u = get_u32(p);
    f32 v;
    memcpy(&v, &u, 4);
    return v;
}
static void put_f64(u8 *p, f64 v) {
    u64 u;
    memcpy(&u, &v, 8);
    put_u64(p, u);
}
static f64 get_f64(const u8 *p) {
    const u64 u = get_u64(p);
    f64 v;
    memcpy(&v, &u, 8);
    return v;
}

static u64 pad8(u64 n) {
    return (n + 7u) & ~(u64)7u;
}

static u64 agent_bytes(const de__nav_tiles_agent *g) {
    return NT_AGENT + (u64)g->record_count * NT_RECORD + pad8(g->dnav_size);
}

de_result de__nav_tiles_write(const de_allocator *a, const de__nav_tiles_agent *agents, u32 count,
                              u8 **out, u64 *out_size) {
    *out = NULL;
    *out_size = 0;
    if (count > DE__NAV_TILES_MAX_AGENTS || (count > 0 && agents == NULL))
        return DE_ERR_INVALID_ARGUMENT;
    u64 total = NT_HEADER;
    for (u32 i = 0; i < count; ++i) {
        const de__nav_tiles_agent *g = &agents[i];
        if (g->record_count > DE__NAV_TILES_MAX_TILES || (g->record_count > 0 && !g->records) ||
            (g->dnav_size > 0 && g->dnav == NULL))
            return DE_ERR_INVALID_ARGUMENT;
        total += agent_bytes(g);
    }
    if (total > (u64)SIZE_MAX)
        return DE_ERR_OUT_OF_MEMORY;
    u8 *buf = (u8 *)DE_ALLOC(a, (size_t)total, 16, DE_MEM_ASSET);
    if (buf == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    memset(buf, 0, (size_t)total);
    put_u32(buf, DE__NAV_TILES_MAGIC);
    put_u32(buf + 4, DE__NAV_TILES_VERSION);
    put_u32(buf + 8, count);
    u64 off = NT_HEADER;
    for (u32 i = 0; i < count; ++i) {
        const de__nav_tiles_agent *g = &agents[i];
        u8 *p = buf + off;
        size_t nl = strnlen(g->name, DE__NAV_TILES_NAME - 1u);
        memcpy(p, g->name, nl);
        put_f64(p + 40, g->bmin_x);
        put_f64(p + 48, g->bmin_y);
        put_f32(p + 56, g->tile_size);
        put_u32(p + 60, g->tiles_x);
        put_u32(p + 64, g->tiles_y);
        put_u32(p + 68, g->record_count);
        put_u64(p + 72, g->dnav_size);
        p += NT_AGENT;
        for (u32 k = 0; k < g->record_count; ++k, p += NT_RECORD) {
            put_u32(p, g->records[k].tile);
            put_u32(p + 4, g->records[k].polys);
            put_u32(p + 8, g->records[k].links);
            put_u64(p + 16, g->records[k].fp);
        }
        if (g->dnav_size > 0)
            memcpy(p, g->dnav, (size_t)g->dnav_size);
        off += agent_bytes(g);
    }
    *out = buf;
    *out_size = total;
    return DE_OK;
}

/* Reads the agent at `off`; false when it runs past `size`. */
static bool agent_head(const u8 *data, u64 size, u64 off, de__nav_tiles_agent *g) {
    if (size - off < NT_AGENT)
        return false;
    const u8 *p = data + off;
    memset(g, 0, sizeof *g);
    memcpy(g->name, p, DE__NAV_TILES_NAME);
    g->bmin_x = get_f64(p + 40);
    g->bmin_y = get_f64(p + 48);
    g->tile_size = get_f32(p + 56);
    g->tiles_x = get_u32(p + 60);
    g->tiles_y = get_u32(p + 64);
    g->record_count = get_u32(p + 68);
    g->dnav_size = get_u64(p + 72);
    if (g->record_count > DE__NAV_TILES_MAX_TILES || g->dnav_size > size)
        return false;
    const u64 body = (u64)g->record_count * NT_RECORD + pad8(g->dnav_size);
    if (size - off - NT_AGENT < body)
        return false;
    g->record_data = p + NT_AGENT;
    g->dnav = g->dnav_size > 0 ? g->record_data + (u64)g->record_count * NT_RECORD : NULL;
    return true;
}

bool de__nav_tiles_read(const u8 *data, u64 size, u32 *agent_count, const char **why) {
    const char *dummy = NULL;
    if (why == NULL)
        why = &dummy;
    *agent_count = 0;
    if (data == NULL || size < NT_HEADER) {
        *why = "nav.tiles: truncated header";
        return false;
    }
    if (get_u32(data) != DE__NAV_TILES_MAGIC) {
        *why = "nav.tiles: not a DNVT file";
        return false;
    }
    if (get_u32(data + 4) != DE__NAV_TILES_VERSION) {
        *why = "nav.tiles: unsupported version";
        return false;
    }
    const u32 count = get_u32(data + 8);
    if (count > DE__NAV_TILES_MAX_AGENTS) {
        *why = "nav.tiles: too many agent types";
        return false;
    }
    u64 off = NT_HEADER;
    for (u32 i = 0; i < count; ++i) {
        de__nav_tiles_agent g;
        if (!agent_head(data, size, off, &g)) {
            *why = "nav.tiles: truncated agent";
            return false;
        }
        if (g.name[0] == '\0' || g.name[DE__NAV_TILES_NAME - 1u] != '\0') {
            *why = "nav.tiles: bad agent name";
            return false;
        }
        for (u32 j = 0; j < i; ++j) {
            de__nav_tiles_agent o;
            (void)de__nav_tiles_agent_at(data, size, j, &o);
            if (strcmp(o.name, g.name) == 0) {
                *why = "nav.tiles: duplicate agent name";
                return false;
            }
        }
        if (!(g.tile_size > 0.0f) || g.tiles_x == 0 || g.tiles_y == 0 ||
            (u64)g.tiles_x * g.tiles_y > DE__NAV_TILES_MAX_TILES) {
            *why = "nav.tiles: bad grid";
            return false;
        }
        for (u32 k = 0; k < g.record_count; ++k) {
            const u32 t = get_u32(g.record_data + (u64)k * NT_RECORD);
            if (t >= g.tiles_x * g.tiles_y ||
                (k > 0 && t <= get_u32(g.record_data + (u64)(k - 1u) * NT_RECORD))) {
                *why = "nav.tiles: bad tile record";
                return false;
            }
        }
        if (g.dnav_size > 0) {
            de__nav_container_info info;
            if (!de__nav_container_read(g.dnav, g.dnav_size, &info, why))
                return false;
            if (info.desc.tile_size != g.tile_size) {
                *why = "nav.tiles: navmesh tile_size differs from the grid's";
                return false;
            }
        }
        off += agent_bytes(&g);
    }
    if (off != size) {
        *why = "nav.tiles: trailing bytes";
        return false;
    }
    *agent_count = count;
    return true;
}

bool de__nav_tiles_agent_at(const u8 *data, u64 size, u32 index, de__nav_tiles_agent *out) {
    if (data == NULL || size < NT_HEADER || index >= get_u32(data + 8))
        return false;
    u64 off = NT_HEADER;
    for (u32 i = 0;; ++i) {
        if (!agent_head(data, size, off, out))
            return false;
        if (i == index)
            return true;
        off += agent_bytes(out);
    }
}

void de__nav_tiles_record(const de__nav_tiles_agent *ag, u32 k, de__nav_tile_record *out) {
    const u8 *p = ag->record_data + (u64)k * NT_RECORD;
    out->tile = get_u32(p);
    out->polys = get_u32(p + 4);
    out->links = get_u32(p + 8);
    out->fp = get_u64(p + 16);
}

bool de__nav_tiles_path(const char *toml_vpath, char *out, size_t cap) {
    if (toml_vpath == NULL || out == NULL || cap == 0)
        return false;
    const size_t n = strlen(toml_vpath);
    const size_t keep = n >= 5 && strcmp(toml_vpath + n - 5, ".toml") == 0 ? n - 5 : n;
    const int w = snprintf(out, cap, "%.*s.tiles", (int)keep, toml_vpath);
    return w > 0 && (size_t)w < cap;
}

/* ---- DENG-877: per-scene file name ----------------------------------------------------------- */

bool de_nav_scene_tiles_path(const char *scene_vpath, char *out, size_t cap) {
    if (scene_vpath == NULL || scene_vpath[0] == '\0' || out == NULL || cap == 0)
        return false;
    const size_t n = strlen(scene_vpath);
    const size_t keep = n > 6 && strcmp(scene_vpath + n - 6, ".scene") == 0 ? n - 6 : n;
    const int w = snprintf(out, cap, "%.*s.nav.tiles", (int)keep, scene_vpath);
    return w > 0 && (size_t)w < cap;
}

/* ---- DENG-877: fingerprints ------------------------------------------------------------------ */

static u64 fp_fnv(u64 h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/* floor without libm: exact (a whole f64 above 2^52 in magnitude is its own floor; NaN stays). */
static f64 fp_floor(f64 v) {
    if (!(v > -4503599627370496.0 && v < 4503599627370496.0))
        return v;
    const f64 t = (f64)(i64)v;
    return t > v ? t - 1.0 : t;
}

static f64 fp_ceil(f64 v) {
    if (!(v > -4503599627370496.0 && v < 4503599627370496.0))
        return v;
    const f64 t = (f64)(i64)v;
    return t < v ? t + 1.0 : t;
}

void de__nav_fp_grid_of(const de_nav_settings *s, de__nav_fp_grid *out) {
    const f64 ts = s->tile_size;
    const f64 x = fp_ceil((s->bounds_max[0] - s->bounds_min[0]) / ts);
    const f64 y = fp_ceil((s->bounds_max[1] - s->bounds_min[1]) / ts);
    out->bmin_x = s->bounds_min[0];
    out->bmin_y = s->bounds_min[1];
    out->tile_size = s->tile_size;
    out->tiles_x = x < 1.0 ? 1u : (u32)x;
    out->tiles_y = y < 1.0 ? 1u : (u32)y;
}

/* The inclusive tile range a world box [x0, x1] x [y0, y1] meets; false when it meets none. */
static bool fp_range(const de__nav_fp_grid *g, f64 x0, f64 y0, f64 x1, f64 y1, i32 *ix0, i32 *iy0,
                     i32 *ix1, i32 *iy1) {
    const f64 ts = g->tile_size;
    const f64 a = fp_floor((x0 - g->bmin_x) / ts), b = fp_floor((y0 - g->bmin_y) / ts);
    const f64 c = fp_floor((x1 - g->bmin_x) / ts), d = fp_floor((y1 - g->bmin_y) / ts);
    if (c < 0 || d < 0 || a >= (f64)g->tiles_x || b >= (f64)g->tiles_y)
        return false;
    *ix0 = a < 0 ? 0 : (i32)a;
    *iy0 = b < 0 ? 0 : (i32)b;
    *ix1 = c >= (f64)g->tiles_x ? (i32)g->tiles_x - 1 : (i32)c;
    *iy1 = d >= (f64)g->tiles_y ? (i32)g->tiles_y - 1 : (i32)d;
    return true;
}

/* Folds element hash `eh` into the tiles a Recast-space xz box (origin 0) meets. */
static void fp_fold(const de__nav_fp_grid *g, u64 *fp, f64 margin, f32 rx0, f32 rz0, f32 rx1,
                    f32 rz1, u64 eh) {
    /* Recast x = world x, Recast z = -world y. */
    i32 ix0, iy0, ix1, iy1;
    if (!fp_range(g, (f64)rx0 - margin, -(f64)rz1 - margin, (f64)rx1 + margin, -(f64)rz0 + margin,
                  &ix0, &iy0, &ix1, &iy1))
        return;
    for (i32 y = iy0; y <= iy1; ++y)
        for (i32 x = ix0; x <= ix1; ++x) {
            u64 *h = &fp[(u32)y * g->tiles_x + (u32)x];
            *h = (*h ^ eh) * 0x100000001b3ull;
        }
}

static void fp_floats(const de__nav_fp_grid *g, u64 *fp, f64 margin, const f32 *v, u32 n,
                      const f32 *xz, u32 corners) {
    f32 x0 = xz[0], x1 = xz[0], z0 = xz[1], z1 = xz[1];
    for (u32 c = 1; c < corners; ++c) {
        const f32 x = xz[c * 2], z = xz[c * 2 + 1];
        x0 = x < x0 ? x : x0;
        x1 = x > x1 ? x : x1;
        z0 = z < z0 ? z : z0;
        z1 = z > z1 ? z : z1;
    }
    fp_fold(g, fp, margin, x0, z0, x1, z1, fp_fnv(0xcbf29ce484222325ull, v, sizeof(f32) * n));
}

u64 de__nav_fp_seed(const de_nav_agent_settings *a, f32 tile_size,
                    const de_nav_area_settings *areas, u32 area_count) {
    /* The byte order of DENG-864's editor hash (six f32 settings, the mask, the tile size, then
     * id and cost per area), so files saved before DENG-877 keep their fingerprints. */
    const f32 s[6] = {a->radius,    a->height,    a->max_slope_deg,
                      a->step_height, a->cell_size, a->cell_height};
    u64 h = 0xcbf29ce484222325ull;
    h = fp_fnv(h, s, sizeof s);
    h = fp_fnv(h, &a->collision_mask, sizeof a->collision_mask);
    h = fp_fnv(h, &tile_size, sizeof tile_size);
    for (u32 i = 0; i < area_count; ++i) {
        h = fp_fnv(h, &areas[i].id, sizeof(u32));
        h = fp_fnv(h, &areas[i].cost, sizeof(f32));
    }
    return h;
}

void de__nav_fp_tiles(const de__nav_fp_grid *g, const de_nav_agent_settings *a, u64 seed,
                      const de__nav_soup *soup, u64 *fp) {
    const u32 tc = g->tiles_x * g->tiles_y;
    for (u32 t = 0; t < tc; ++t)
        fp[t] = seed;
    if (soup == NULL)
        return;
    const f64 margin = (f64)a->radius + 4.0 * (f64)a->cell_size;
    for (u32 k = 0; k < soup->tri_count; ++k) {
        f32 v[9], xz[6];
        for (u32 c = 0; c < 3; ++c) {
            const f32 *p = &soup->verts[(u32)soup->tris[k * 3 + c] * 3];
            v[c * 3] = p[0], v[c * 3 + 1] = p[1], v[c * 3 + 2] = p[2];
            xz[c * 2] = p[0], xz[c * 2 + 1] = p[2];
        }
        fp_floats(g, fp, margin, v, 9, xz, 3);
    }
    for (u32 k = 0; k < soup->obstacle_count; ++k) {
        const f32 *o = &soup->obstacles[k * DE__NAV_RC_OBSTACLE_FLOATS];
        fp_floats(g, fp, margin, o, DE__NAV_RC_OBSTACLE_FLOATS, o, 4);
    }
    for (u32 k = 0; k < soup->area_vol_count; ++k) {
        const f32 *o = &soup->area_vols[k * DE__NAV_RC_AREA_VOL_FLOATS];
        fp_floats(g, fp, margin, o, DE__NAV_RC_AREA_VOL_FLOATS, o, 4);
    }
    for (u32 k = 0; k < soup->offmesh_count; ++k) {
        const f32 *o = &soup->offmesh[k * DE__NAV_RC_OFFMESH_FLOATS];
        const f32 xz[2] = {o[0], o[2]}; /* built into the tile of its start */
        fp_floats(g, fp, margin, o, DE__NAV_RC_OFFMESH_FLOATS, xz, 1);
    }
}
