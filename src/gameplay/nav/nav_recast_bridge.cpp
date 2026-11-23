// nav_recast_bridge.cpp: the Recast / Detour bridge (DENG-76). The only C++ of the nav module;
// its C interface is nav_recast_bridge.h. No exceptions and no RTTI are needed by Recast or
// Detour; nothing here throws. Built with the module's deterministic flags (no FP contraction).
//
// Allocation: process-wide hooks (installed once) take every block from the thread's current
// de_allocator (alloc_scope, set by each entry point below) and put a 16-byte header in front of
// it that records the allocator and the size, so the free hook needs no scope.
#include "gameplay/nav/nav_recast_bridge.h"

#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <Recast.h>
#include <RecastAlloc.h>

#include <atomic>
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
    ~tile_work() {
        rcFreeHeightField(hf);
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
        rcFree(areas);
    }
};

const unsigned short k_walk_flag = 1;

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
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *w.chf) ||
        !rcBuildDistanceField(&ctx, *w.chf) ||
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

} // extern "C"
