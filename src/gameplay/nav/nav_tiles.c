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
