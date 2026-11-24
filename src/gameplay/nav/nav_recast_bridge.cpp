// nav_recast_bridge.cpp: the Recast / Detour bridge (DENG-76; queries and off-mesh links DENG-77).
// The only C++ of the nav module; its C interface is nav_recast_bridge.h. No exceptions and no
// RTTI are needed by Recast or Detour; nothing here throws. Built with the module's deterministic
// flags (no FP contraction).
//
// Allocation: process-wide hooks (installed once) take every block from the thread's current
// de_allocator (alloc_scope, set by each entry point below) and put a 16-byte header in front of
// it that records the allocator and the size, so the free hook needs no scope.
#include "gameplay/nav/nav_recast_bridge.h"

#include <dengine/de_math.h>

#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>
#include <RecastAlloc.h>

#include <atomic>
#include <cfloat>
#include <cstring>
#include <new>

namespace {

struct alignas(16) block_header {
    const de_allocator *alloc;
    size_t size; // bytes after the header
};
static_assert(sizeof(block_header) == 16, "block header must be 16 bytes");

thread_local const de_allocator *t_alloc = nullptr;
std::atomic<u64> g_unscoped{0};

void *hook_alloc(size_t size) {
    const de_allocator *a = t_alloc;
    if (a == nullptr) {
        g_unscoped.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    if (size > (size_t)-1 - sizeof(block_header))
        return nullptr;
    void *raw = DE_ALLOC(a, sizeof(block_header) + size, 16, DE_MEM_GENERAL);
    if (raw == nullptr)
        return nullptr;
    block_header *h = static_cast<block_header *>(raw);
    h->alloc = a;
    h->size = size;
    return h + 1;
}

void hook_free(void *p) {
    if (p == nullptr)
        return;
    block_header *h = static_cast<block_header *>(p) - 1;
    const de_allocator *a = h->alloc;
    DE_FREE(a, h, sizeof(block_header) + h->size);
}

void *rc_alloc_hook(size_t size, rcAllocHint) {
    return hook_alloc(size);
}
void *dt_alloc_hook(size_t size, dtAllocHint) {
    return hook_alloc(size);
}

void install_hooks() {
    // Thread-safe one-time initialization (C++11 function-local static).
    static const bool installed = [] {
        rcAllocSetCustom(rc_alloc_hook, hook_free);
        dtAllocSetCustom(dt_alloc_hook, hook_free);
        return true;
    }();
    (void)installed;
}

// Makes `a` the current allocator of the thread for the scope (scopes nest).
struct alloc_scope {
    const de_allocator *prev;
    explicit alloc_scope(const de_allocator *a) : prev(t_alloc) {
        install_hooks();
        t_alloc = a;
    }
    ~alloc_scope() { t_alloc = prev; }
    alloc_scope(const alloc_scope &) = delete;
    alloc_scope &operator=(const alloc_scope &) = delete;
};

struct tile_work {
    rcHeightfield *hf = nullptr;
    rcCompactHeightfield *chf = nullptr;
    rcContourSet *cset = nullptr;
    rcPolyMesh *pmesh = nullptr;
    rcPolyMeshDetail *dmesh = nullptr;
    unsigned char *areas = nullptr;
    void *offmesh = nullptr; // one block for the off-mesh connection arrays
    ~tile_work() {
        rcFree(offmesh);
        rcFreeHeightField(hf);
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
        rcFree(areas);
    }
};

const unsigned short k_walk_flag = 1;

// Makes Detour's math (de_detour_math.h) deterministic on this thread for the scope when `on`.
struct det_scope {
    bool prev;
    explicit det_scope(bool on) : prev(de__nav_detmath_on) {
        if (on)
            de__nav_detmath_on = true;
    }
    ~det_scope() { de__nav_detmath_on = prev; }
    det_scope(const det_scope &) = delete;
    det_scope &operator=(const det_scope &) = delete;
};

// FNV-1a 64.
struct fnv {
    u64 h = 1469598103934665603ull;
    void bytes(const void *p, size_t n) {
        const unsigned char *b = static_cast<const unsigned char *>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    template <typename T> void val(const T &v) { bytes(&v, sizeof v); }
};

} // namespace

struct de__nav_mesh {
    const de_allocator *alloc;
    dtNavMesh *nav;
};

extern "C" {

de_result de__nav_rc_build_tile(const de_allocator *a, const de__nav_rc_tile_config *c,
                                const f32 *verts, i32 nv, const i32 *tris, i32 nt,
                                u8 **out_data, i32 *out_size) {
    if (a == nullptr || c == nullptr || out_data == nullptr || out_size == nullptr || nv < 0 ||
        nt < 0 || (nt > 0 && (verts == nullptr || tris == nullptr)))
        return DE_ERR_INVALID_ARGUMENT;
    *out_data = nullptr;
    *out_size = 0;
    if (nt == 0)
        return DE_OK;
    alloc_scope scope(a);
    rcContext ctx(false);

    rcConfig cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.cs = c->cs;
    cfg.ch = c->ch;
    cfg.walkableSlopeAngle = c->walkable_slope_deg;
    cfg.walkableHeight = c->walkable_height;
    cfg.walkableClimb = c->walkable_climb;
    cfg.walkableRadius = c->walkable_radius;
    cfg.maxEdgeLen = (int)(12.0f / c->cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = 64;    // 8 x 8 cells
    cfg.mergeRegionArea = 400; // 20 x 20 cells
    cfg.maxVertsPerPoly = 6;
    cfg.tileSize = c->tile_cells;
    cfg.borderSize = c->border;
    cfg.width = c->tile_cells + 2 * c->border;
    cfg.height = c->tile_cells + 2 * c->border;
    cfg.detailSampleDist = 6.0f * c->cs;
    cfg.detailSampleMaxError = 1.0f * c->ch;
    for (int i = 0; i < 3; ++i) {
        cfg.bmin[i] = c->bmin[i];
        cfg.bmax[i] = c->bmax[i];
    }
    const float pad = (float)c->border * c->cs;
    cfg.bmin[0] -= pad;
    cfg.bmin[2] -= pad;
    cfg.bmax[0] += pad;
    cfg.bmax[2] += pad;

    tile_work w;
    w.hf = rcAllocHeightfield();
    if (w.hf == nullptr)
        return DE_ERR_OUT_OF_MEMORY;
    if (!rcCreateHeightfield(&ctx, *w.hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs,
                             cfg.ch))
        return DE_ERR_OUT_OF_MEMORY;
    w.areas = static_cast<unsigned char *>(rcAlloc((size_t)nt, RC_ALLOC_TEMP));
    if (w.areas == nullptr)
        return DE_ERR_OUT_OF_MEMORY;
    std::memset(w.areas, 0, (size_t)nt);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts, nv, tris, nt, w.areas);
    if (!rcRasterizeTriangles(&ctx, verts, nv, tris, w.areas, nt, *w.hf, cfg.walkableClimb))
        return DE_ERR_OUT_OF_MEMORY;
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *w.hf);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *w.hf);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *w.hf);

    w.chf = rcAllocCompactHeightfield();
    if (w.chf == nullptr ||
        !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *w.hf, *w.chf))
        return DE_ERR_OUT_OF_MEMORY;
    rcFreeHeightField(w.hf);
    w.hf = nullptr;
    /* Obstacles: their footprint becomes unwalkable before erosion, so erosion inflates it by
     * the agent radius like any wall. */
    for (i32 i = 0; i < c->obstacle_count; ++i) {
        const f32 *o = &c->obstacles[i * DE__NAV_RC_OBSTACLE_FLOATS];
        f32 poly[12];
        for (int k = 0; k < 4; ++k) {
            poly[k * 3 + 0] = o[k * 2 + 0];
            poly[k * 3 + 1] = o[8];
            poly[k * 3 + 2] = o[k * 2 + 1];
        }
        rcMarkConvexPolyArea(&ctx, poly, 4, o[8], o[9], RC_NULL_AREA, *w.chf);
    }
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *w.chf))
        return DE_ERR_INTERNAL;
    /* Area volumes (DENG-582): after erosion, so they change costs and flags, not the shape. */
    for (i32 i = 0; i < c->area_vol_count; ++i) {
        const f32 *o = &c->area_vols[i * DE__NAV_RC_AREA_VOL_FLOATS];
        f32 poly[12];
        for (int k = 0; k < 4; ++k) {
            poly[k * 3 + 0] = o[k * 2 + 0];
            poly[k * 3 + 1] = o[8];
            poly[k * 3 + 2] = o[k * 2 + 1];
        }
        const int area = (int)o[10];
        if (area >= 1 && area <= 15)
            rcMarkConvexPolyArea(&ctx, poly, 4, o[8], o[9], (unsigned char)area, *w.chf);
    }
    if (!rcBuildDistanceField(&ctx, *w.chf) ||
        !rcBuildRegions(&ctx, *w.chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea))
        return DE_ERR_INTERNAL;
    w.cset = rcAllocContourSet();
    if (w.cset == nullptr)
        return DE_ERR_OUT_OF_MEMORY;
    if (!rcBuildContours(&ctx, *w.chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *w.cset))
        return DE_ERR_INTERNAL;
    if (w.cset->nconts == 0)
        return DE_OK;
    w.pmesh = rcAllocPolyMesh();
    if (w.pmesh == nullptr)
        return DE_ERR_OUT_OF_MEMORY;
    if (!rcBuildPolyMesh(&ctx, *w.cset, cfg.maxVertsPerPoly, *w.pmesh))
        return DE_ERR_INTERNAL;
    w.dmesh = rcAllocPolyMeshDetail();
    if (w.dmesh == nullptr)
        return DE_ERR_OUT_OF_MEMORY;
    if (!rcBuildPolyMeshDetail(&ctx, *w.pmesh, *w.chf, cfg.detailSampleDist,
                               cfg.detailSampleMaxError, *w.dmesh))
        return DE_ERR_INTERNAL;
    if (w.pmesh->npolys == 0)
        return DE_OK;
    if (w.pmesh->nverts >= 0xffff)
        return DE_ERR_INTERNAL; // Detour stores vertex indices as 16 bits
    for (int i = 0; i < w.pmesh->npolys; ++i) {
        if (w.pmesh->areas[i] == RC_WALKABLE_AREA) {
            w.pmesh->areas[i] = 0;
            w.pmesh->flags[i] = k_walk_flag;
        } else if (w.pmesh->areas[i] >= 1 && w.pmesh->areas[i] <= 15) {
            w.pmesh->flags[i] = (unsigned short)(1u << w.pmesh->areas[i]); // area volume
        } else {
            w.pmesh->flags[i] = 0;
        }
    }

    dtNavMeshCreateParams p;
    std::memset(&p, 0, sizeof p);
    p.verts = w.pmesh->verts;
    p.vertCount = w.pmesh->nverts;
    p.polys = w.pmesh->polys;
    p.polyAreas = w.pmesh->areas;
    p.polyFlags = w.pmesh->flags;
    p.polyCount = w.pmesh->npolys;
    p.nvp = w.pmesh->nvp;
    p.detailMeshes = w.dmesh->meshes;
    p.detailVerts = w.dmesh->verts;
    p.detailVertsCount = w.dmesh->nverts;
    p.detailTris = w.dmesh->tris;
    p.detailTriCount = w.dmesh->ntris;
    p.walkableHeight = c->agent_height;
    p.walkableRadius = c->agent_radius;
    p.walkableClimb = c->agent_max_climb;
    p.tileX = c->tx;
    p.tileY = c->tz;
    p.tileLayer = 0;
    for (int i = 0; i < 3; ++i) {
        p.bmin[i] = w.pmesh->bmin[i];
        p.bmax[i] = w.pmesh->bmax[i];
    }
    p.cs = cfg.cs;
    p.ch = cfg.ch;
    p.buildBvTree = true;
    if (c->offmesh_count > 0 && c->offmesh != nullptr) {
        const size_t n = (size_t)c->offmesh_count;
        // verts (6 floats), rad (float), flags (u16), user id (u32), dir and area (u8 each).
        const size_t bytes = n * (6 * sizeof(float) + sizeof(float) + sizeof(unsigned short) +
                                  sizeof(unsigned int) + 2);
        w.offmesh = rcAlloc(bytes + 16, RC_ALLOC_TEMP);
        if (w.offmesh == nullptr)
            return DE_ERR_OUT_OF_MEMORY;
        float *verts = static_cast<float *>(w.offmesh);
        float *rad = verts + n * 6;
        unsigned int *ids = reinterpret_cast<unsigned int *>(rad + n);
        unsigned short *flags = reinterpret_cast<unsigned short *>(ids + n);
        unsigned char *dirs = reinterpret_cast<unsigned char *>(flags + n);
        unsigned char *areas = dirs + n;
        for (size_t i = 0; i < n; ++i) {
            const f32 *o = &c->offmesh[i * DE__NAV_RC_OFFMESH_FLOATS];
            // Recast puts the walkable surface one or two voxels above the geometry, and Detour
            // drops a link whose start is more than the climb below it. Endpoints authored on the
            // geometry are lifted by one voxel, so ones on the geometry or on the mesh both fit.
            for (int k = 0; k < 6; ++k)
                verts[i * 6 + k] = o[k] + ((k == 1 || k == 4) ? cfg.ch : 0.0f);
            rad[i] = o[6];
            dirs[i] = o[7] != 0.0f ? DT_OFFMESH_CON_BIDIR : 0;
            areas[i] = (unsigned char)o[8];
            flags[i] = (unsigned short)(1u << areas[i]);
            ids[i] = (unsigned int)o[9];
        }
        p.offMeshConVerts = verts;
        p.offMeshConRad = rad;
        p.offMeshConDir = dirs;
        p.offMeshConAreas = areas;
        p.offMeshConFlags = flags;
        p.offMeshConUserID = ids;
        p.offMeshConCount = c->offmesh_count;
    }
    unsigned char *data = nullptr;
    int size = 0;
    if (!dtCreateNavMeshData(&p, &data, &size))
        return DE_ERR_INTERNAL;
    *out_data = data;
    *out_size = size;
    return DE_OK;
}

u8 *de__nav_rc_alloc(const de_allocator *a, i32 size) {
    if (a == nullptr || size <= 0)
        return nullptr;
    alloc_scope scope(a);
    return static_cast<u8 *>(dtAlloc((size_t)size, DT_ALLOC_PERM));
}

void de__nav_rc_free(void *p) {
    // The header names the allocator; dtFree and rcFree both end in hook_free.
    install_hooks();
    dtFree(p);
}

u64 de__nav_rc_unscoped_allocs(void) {
    return g_unscoped.load(std::memory_order_relaxed);
}

de_result de__nav_mesh_create(const de_allocator *a, f32 tile_size, de__nav_mesh **out) {
    if (a == nullptr || out == nullptr || !(tile_size > 0.0f))
        return DE_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    alloc_scope scope(a);
    de__nav_mesh *m =
        static_cast<de__nav_mesh *>(DE_ALLOC(a, sizeof(de__nav_mesh), alignof(de__nav_mesh),
                                             DE_MEM_GENERAL));
    if (m == nullptr)
        return DE_ERR_OUT_OF_MEMORY;
    m->alloc = a;
    m->nav = dtAllocNavMesh();
    if (m->nav == nullptr) {
        DE_FREE(a, m, sizeof *m);
        return DE_ERR_OUT_OF_MEMORY;
    }
    dtNavMeshParams params;
    std::memset(&params, 0, sizeof params);
    params.tileWidth = tile_size;
    params.tileHeight = tile_size;
    params.maxTiles = DE__NAV_MAX_TILES;
    params.maxPolys = 1 << 16;
    if (dtStatusFailed(m->nav->init(&params))) {
        dtFreeNavMesh(m->nav);
        DE_FREE(a, m, sizeof *m);
        return DE_ERR_OUT_OF_MEMORY;
    }
    *out = m;
    return DE_OK;
}

void de__nav_mesh_destroy(de__nav_mesh *m) {
    if (m == nullptr)
        return;
    const de_allocator *a = m->alloc;
    {
        alloc_scope scope(a);
        dtFreeNavMesh(m->nav);
    }
    DE_FREE(a, m, sizeof *m);
}

de_result de__nav_mesh_set_tile(de__nav_mesh *m, i32 tx, i32 tz, u8 *data, i32 size) {
    if (m == nullptr) {
        de__nav_rc_free(data);
        return DE_ERR_INVALID_ARGUMENT;
    }
    alloc_scope scope(m->alloc);
    dtTileRef old = m->nav->getTileRefAt(tx, tz, 0);
    if (old != 0)
        m->nav->removeTile(old, nullptr, nullptr); // frees the old data (DT_TILE_FREE_DATA)
    if (data == nullptr)
        return DE_OK;
    dtTileRef ref = 0;
    dtStatus st = m->nav->addTile(data, size, DT_TILE_FREE_DATA, 0, &ref);
    if (dtStatusFailed(st)) {
        dtFree(data);
        if (st & DT_OUT_OF_MEMORY)
            return DE_ERR_OUT_OF_MEMORY;
        return DE_ERR_PARSE;
    }
    return DE_OK;
}

u64 de__nav_mesh_tile_ref(const de__nav_mesh *m, i32 tx, i32 tz) {
    if (m == nullptr)
        return 0;
    return (u64)m->nav->getTileRefAt(tx, tz, 0);
}

u32 de__nav_mesh_tile_count(const de__nav_mesh *m) {
    if (m == nullptr)
        return 0;
    const dtNavMesh *nav = m->nav;
    u32 n = 0;
    for (int i = 0; i < nav->getMaxTiles(); ++i) {
        const dtMeshTile *t = nav->getTile(i);
        if (t != nullptr && t->header != nullptr)
            ++n;
    }
    return n;
}

u32 de__nav_mesh_poly_count(const de__nav_mesh *m) {
    if (m == nullptr)
        return 0;
    const dtNavMesh *nav = m->nav;
    u32 n = 0;
    for (int i = 0; i < nav->getMaxTiles(); ++i) {
        const dtMeshTile *t = nav->getTile(i);
        if (t != nullptr && t->header != nullptr)
            n += (u32)t->header->polyCount;
    }
    return n;
}

bool de__nav_mesh_point_on_poly(const de__nav_mesh *m, const f32 p[3], f32 max_dy) {
    if (m == nullptr || p == nullptr)
        return false;
    int tx = 0, tz = 0;
    m->nav->calcTileLoc(p, &tx, &tz);
    const dtMeshTile *t = m->nav->getTileAt(tx, tz, 0);
    if (t == nullptr || t->header == nullptr)
        return false;
    for (int i = 0; i < t->header->polyCount; ++i) {
        const dtPoly &poly = t->polys[i];
        if (poly.getType() != DT_POLYTYPE_GROUND)
            continue;
        // Even-odd test in xz, and the height range of the polygon's vertices.
        bool inside = false;
        float ymin = 0.0f, ymax = 0.0f;
        const int nv = poly.vertCount;
        for (int a = 0, b = nv - 1; a < nv; b = a++) {
            const float *va = &t->verts[poly.verts[a] * 3];
            const float *vb = &t->verts[poly.verts[b] * 3];
            if (a == 0 || va[1] < ymin)
                ymin = va[1];
            if (a == 0 || va[1] > ymax)
                ymax = va[1];
            if ((va[2] > p[2]) != (vb[2] > p[2])) {
                const float x = va[0] + (p[2] - va[2]) * (vb[0] - va[0]) / (vb[2] - va[2]);
                if (p[0] < x)
                    inside = !inside;
            }
        }
        if (inside && p[1] >= ymin - max_dy && p[1] <= ymax + max_dy)
            return true;
    }
    return false;
}

bool de__nav_mesh_tile_data(const de__nav_mesh *m, i32 tx, i32 tz, const u8 **data, i32 *size) {
    *data = nullptr;
    *size = 0;
    if (m == nullptr)
        return false;
    const dtMeshTile *t = m->nav->getTileAt(tx, tz, 0);
    if (t == nullptr || t->header == nullptr || t->data == nullptr || t->dataSize <= 0)
        return false;
    *data = t->data;
    *size = t->dataSize;
    return true;
}

bool de__nav_mesh_tile_stats(const de__nav_mesh *m, i32 tx, i32 tz, i32 *polys, i32 *links) {
    *polys = 0;
    *links = 0;
    if (m == nullptr)
        return false;
    const dtMeshTile *t = m->nav->getTileAt(tx, tz, 0);
    if (t == nullptr || t->header == nullptr)
        return false;
    *links = t->header->offMeshConCount;
    *polys = t->header->polyCount - t->header->offMeshConCount;
    return true;
}

i32 de__nav_mesh_tile_tris(const de__nav_mesh *m, i32 tx, i32 tz, f32 *xyz, u8 *areas, i32 cap) {
    if (m == nullptr)
        return 0;
    const dtMeshTile *t = m->nav->getTileAt(tx, tz, 0);
    if (t == nullptr || t->header == nullptr)
        return 0;
    i32 n = 0;
    for (int i = 0; i < t->header->polyCount; ++i) {
        const dtPoly &p = t->polys[i];
        if (p.getType() != DT_POLYTYPE_GROUND || i >= t->header->detailMeshCount)
            continue;
        const dtPolyDetail &pd = t->detailMeshes[i];
        for (int j = 0; j < pd.triCount; ++j, ++n) {
            if (n >= cap)
                continue;
            const unsigned char *dt = &t->detailTris[(pd.triBase + (unsigned int)j) * 4];
            for (int k = 0; k < 3; ++k) {
                const float *v = dt[k] < p.vertCount
                                     ? &t->verts[p.verts[dt[k]] * 3]
                                     : &t->detailVerts[(pd.vertBase + dt[k] - p.vertCount) * 3];
                xyz[n * 9 + k * 3 + 0] = v[0];
                xyz[n * 9 + k * 3 + 1] = v[1];
                xyz[n * 9 + k * 3 + 2] = v[2];
            }
            areas[n] = p.getArea();
        }
    }
    return n;
}

u64 de__nav_mesh_hash(const de__nav_mesh *m) {
    if (m == nullptr)
        return 0;
    const dtNavMesh *nav = m->nav;
    fnv f;
    // Tiles in grid order (z, then x), whatever order Detour stores them in.
    bool have_prev = false;
    int px = 0, pz = 0;
    for (;;) {
        const dtMeshTile *best = nullptr;
        for (int i = 0; i < nav->getMaxTiles(); ++i) {
            const dtMeshTile *t = nav->getTile(i);
            if (t == nullptr || t->header == nullptr)
                continue;
            const int x = t->header->x, z = t->header->y;
            if (have_prev && (z < pz || (z == pz && x <= px)))
                continue;
            if (best == nullptr || z < best->header->y ||
                (z == best->header->y && x < best->header->x))
                best = t;
        }
        if (best == nullptr)
            break;
        const dtMeshHeader *h = best->header;
        have_prev = true;
        px = h->x;
        pz = h->y;
        f.val(h->x);
        f.val(h->y);
        f.val(h->polyCount);
        f.val(h->vertCount);
        f.val(h->detailMeshCount);
        f.val(h->detailVertCount);
        f.val(h->detailTriCount);
        f.val(h->offMeshConCount);
        f.bytes(best->verts, sizeof(float) * 3 * (size_t)h->vertCount);
        for (int i = 0; i < h->polyCount; ++i) {
            const dtPoly &p = best->polys[i];
            f.bytes(p.verts, sizeof p.verts);
            f.bytes(p.neis, sizeof p.neis);
            f.val(p.flags);
            f.val(p.vertCount);
            f.val(p.areaAndtype);
        }
        f.bytes(best->detailMeshes, sizeof(dtPolyDetail) * (size_t)h->detailMeshCount);
        f.bytes(best->detailVerts, sizeof(float) * 3 * (size_t)h->detailVertCount);
        f.bytes(best->detailTris, 4 * (size_t)h->detailTriCount);
        for (int i = 0; i < h->offMeshConCount; ++i) {
            const dtOffMeshConnection &o = best->offMeshCons[i];
            f.bytes(o.pos, sizeof o.pos);
            f.val(o.rad);
            f.val(o.poly);
            f.val(o.flags);
            f.val(o.side);
            f.val(o.userId);
        }
    }
    return f.h;
}

} // extern "C"

/* ---- Queries (DENG-77) ----------------------------------------------------------------------- */

struct de__nav_query {
    de__nav_mesh *mesh;
    dtNavMeshQuery *q;
    bool det;
    dtPolyRef polys[DE__NAV_QUERY_MAX_POLYS];
    float points[DE__NAV_QUERY_MAX_POINTS * 3];
    unsigned char flags[DE__NAV_QUERY_MAX_POINTS];
    dtPolyRef refs[DE__NAV_QUERY_MAX_POINTS];
    int npolys, npoints; // of the last successful path (DENG-582)
    float cost[16];      // its area costs
};

namespace {

// Nodes of the A* pool: enough for corridors of DE__NAV_QUERY_MAX_POLYS polygons.
const int k_query_nodes = 4096;

void make_filter(const de__nav_query_args *a, dtQueryFilter *f) {
    f->setIncludeFlags(a->include);
    f->setExcludeFlags(0);
    if (a->area_cost != nullptr)
        for (int i = 0; i < 16; ++i)
            f->setAreaCost(i, a->area_cost[i] > 0.0f ? a->area_cost[i] : 1.0f);
}

} // namespace

extern "C" {

de_result de__nav_query_create(de__nav_mesh *m, de__nav_query **out) {
    if (m == nullptr || out == nullptr)
        return DE_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    const de_allocator *a = m->alloc;
    alloc_scope scope(a);
    de__nav_query *q = static_cast<de__nav_query *>(
        DE_ALLOC(a, sizeof(de__nav_query), alignof(de__nav_query), DE_MEM_GENERAL));
    if (q == nullptr)
        return DE_ERR_OUT_OF_MEMORY;
    std::memset(q, 0, sizeof *q);
    q->mesh = m;
    q->q = dtAllocNavMeshQuery();
    if (q->q == nullptr || dtStatusFailed(q->q->init(m->nav, k_query_nodes))) {
        dtFreeNavMeshQuery(q->q);
        DE_FREE(a, q, sizeof *q);
        return DE_ERR_OUT_OF_MEMORY;
    }
    *out = q;
    return DE_OK;
}

void de__nav_query_destroy(de__nav_query *q) {
    if (q == nullptr)
        return;
    const de_allocator *a = q->mesh->alloc;
    {
        alloc_scope scope(a);
        dtFreeNavMeshQuery(q->q);
    }
    DE_FREE(a, q, sizeof *q);
}

void de__nav_query_set_deterministic(de__nav_query *q, bool on) {
    if (q != nullptr)
        q->det = on;
}

de_result de__nav_query_path(de__nav_query *q, const de__nav_query_args *args, const f32 start[3],
                             const f32 end[3], const f32 **points, i32 *count, bool *partial,
                             bool *bad_end) {
    *points = q->points;
    *count = 0;
    *partial = false;
    *bad_end = false;
    q->npolys = q->npoints = 0;
    for (int i = 0; i < 16; ++i)
        q->cost[i] = args->area_cost != nullptr && args->area_cost[i] > 0.0f ? args->area_cost[i]
                                                                               : 1.0f;
    det_scope det(q->det);
    dtQueryFilter f;
    make_filter(args, &f);
    dtPolyRef sref = 0, eref = 0;
    float sp[3], ep[3];
    if (dtStatusFailed(q->q->findNearestPoly(start, args->ext, &f, &sref, sp)))
        return DE_ERR_INTERNAL;
    if (sref == 0)
        return DE_ERR_NOT_FOUND;
    if (dtStatusFailed(q->q->findNearestPoly(end, args->ext, &f, &eref, ep)))
        return DE_ERR_INTERNAL;
    if (eref == 0) {
        *bad_end = true;
        return DE_ERR_NOT_FOUND;
    }
    int np = 0;
    const dtStatus st =
        q->q->findPath(sref, eref, sp, ep, &f, q->polys, &np, DE__NAV_QUERY_MAX_POLYS);
    if (dtStatusFailed(st) || np <= 0)
        return DE_ERR_INTERNAL;
    bool part = dtStatusDetail(st, DT_PARTIAL_RESULT) || q->polys[np - 1] != eref;
    float target[3] = {ep[0], ep[1], ep[2]};
    if (part && dtStatusFailed(q->q->closestPointOnPoly(q->polys[np - 1], ep, target, nullptr)))
        return DE_ERR_INTERNAL;
    int ns = 0;
    const dtStatus ss = q->q->findStraightPath(sp, target, q->polys, np, q->points, q->flags,
                                               q->refs, &ns, DE__NAV_QUERY_MAX_POINTS, 0);
    if (dtStatusFailed(ss) || ns <= 0)
        return DE_ERR_INTERNAL;
    if (dtStatusDetail(ss, DT_BUFFER_TOO_SMALL))
        part = true;
    q->npolys = np;
    q->npoints = ns;
    *count = ns;
    *partial = part;
    return DE_OK;
}

void de__nav_query_last_detail(de__nav_query *q, f32 *centres, i32 centre_cap, i32 *ncentres,
                               u32 *ids, i32 id_cap, i32 *nids, f64 *cost) {
    *ncentres = 0;
    *nids = 0;
    *cost = 0.0;
    const dtNavMesh *nav = q->mesh->nav;
    for (int i = 0; i < q->npolys; ++i) {
        const dtMeshTile *t = nullptr;
        const dtPoly *p = nullptr;
        if (dtStatusFailed(nav->getTileAndPolyByRef(q->polys[i], &t, &p)))
            continue;
        if (p->getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
            const unsigned int idx = (unsigned int)(p - t->polys);
            const int k = (int)idx - t->header->offMeshBase;
            if (k >= 0 && k < t->header->offMeshConCount && *nids < id_cap)
                ids[(*nids)++] = t->offMeshCons[k].userId;
        }
        if (*ncentres < centre_cap) {
            float c[3] = {0.0f, 0.0f, 0.0f};
            for (int v = 0; v < p->vertCount; ++v)
                for (int k = 0; k < 3; ++k)
                    c[k] += t->verts[p->verts[v] * 3 + k];
            for (int k = 0; k < 3; ++k)
                centres[*ncentres * 3 + k] = p->vertCount > 0 ? c[k] / (float)p->vertCount : 0.0f;
            ++*ncentres;
        }
    }
    double total = 0.0;
    for (int i = 0; i + 1 < q->npoints; ++i) {
        const float *a = &q->points[i * 3], *b = &q->points[(i + 1) * 3];
        const double dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
        unsigned char area = 0;
        if (q->refs[i] != 0)
            (void)nav->getPolyArea(q->refs[i], &area);
        total += de_dsqrt64(dx * dx + dy * dy + dz * dz) * (double)q->cost[area & 15];
    }
    *cost = total;
}

i32 de__nav_query_raycast(de__nav_query *q, const de__nav_query_args *args, const f32 start[3],
                          const f32 end[3], f32 hit[3]) {
    det_scope det(q->det);
    dtQueryFilter f;
    make_filter(args, &f);
    dtPolyRef sref = 0;
    float sp[3];
    if (dtStatusFailed(q->q->findNearestPoly(start, args->ext, &f, &sref, sp)) || sref == 0)
        return -1;
    float t = 0.0f, normal[3];
    int np = 0;
    if (dtStatusFailed(q->q->raycast(sref, sp, end, &f, &t, normal, q->polys, &np,
                                     DE__NAV_QUERY_MAX_POLYS)))
        return -1;
    if (t == FLT_MAX)
        return 0;
    float p[3];
    for (int k = 0; k < 3; ++k)
        p[k] = sp[k] + (end[k] - sp[k]) * t;
    const dtPolyRef last = np > 0 ? q->polys[np - 1] : sref;
    if (dtStatusFailed(q->q->closestPointOnPoly(last, p, hit, nullptr))) {
        hit[0] = p[0];
        hit[1] = p[1];
        hit[2] = p[2];
    }
    return 1;
}

bool de__nav_query_nearest(de__nav_query *q, const de__nav_query_args *args, const f32 p[3],
                           f32 out[3]) {
    det_scope det(q->det);
    dtQueryFilter f;
    make_filter(args, &f);
    dtPolyRef ref = 0;
    if (dtStatusFailed(q->q->findNearestPoly(p, args->ext, &f, &ref, out)))
        return false;
    return ref != 0;
}

} // extern "C"
