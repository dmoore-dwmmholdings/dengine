// C++ half of the de_tp_recastnavigation smoke test (DENG-76): includes the Recast and Detour
// headers with the engine's strict flags and DT_POLYREF64, builds one tile of a flat 10 x 10 m
// quad and loads it into a Detour navmesh. Gives the C test one function.
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <Recast.h>

#include <cstring>

extern "C" int de_smoke_recast_build_quad(int *out_poly_count);

namespace {

struct Owned {
    rcHeightfield *hf = nullptr;
    rcCompactHeightfield *chf = nullptr;
    rcContourSet *cset = nullptr;
    rcPolyMesh *pmesh = nullptr;
    rcPolyMeshDetail *dmesh = nullptr;
    dtNavMesh *nav = nullptr;
    ~Owned() {
        rcFreeHeightField(hf);
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
        dtFreeNavMesh(nav);
    }
};

} // namespace

// 1 on success; *out_poly_count receives the polygon count of the tile.
extern "C" int de_smoke_recast_build_quad(int *out_poly_count) {
    static const float verts[] = {0, 0, 0, 0, 0, 10, 10, 0, 10, 10, 0, 0};
    static const int tris[] = {0, 1, 2, 0, 2, 3};
    unsigned char areas[2] = {0, 0};
    rcContext ctx(false);
    rcConfig cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.cs = 0.25f;
    cfg.ch = 0.2f;
    cfg.walkableSlopeAngle = 45.0f;
    cfg.walkableHeight = 10;
    cfg.walkableClimb = 2;
    cfg.walkableRadius = 2;
    cfg.maxEdgeLen = 48;
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = 8;
    cfg.mergeRegionArea = 20;
    cfg.maxVertsPerPoly = 6;
    cfg.detailSampleDist = 1.5f;
    cfg.detailSampleMaxError = 0.2f;
    cfg.bmin[0] = 0;
    cfg.bmin[1] = -1;
    cfg.bmin[2] = 0;
    cfg.bmax[0] = 10;
    cfg.bmax[1] = 1;
    cfg.bmax[2] = 10;
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

    Owned o;
    o.hf = rcAllocHeightfield();
    if (!o.hf || !rcCreateHeightfield(&ctx, *o.hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs,
                                      cfg.ch))
        return 0;
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts, 4, tris, 2, areas);
    if (!rcRasterizeTriangles(&ctx, verts, 4, tris, areas, 2, *o.hf, cfg.walkableClimb))
        return 0;
    o.chf = rcAllocCompactHeightfield();
    if (!o.chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *o.hf, *o.chf))
        return 0;
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *o.chf) || !rcBuildDistanceField(&ctx, *o.chf) ||
        !rcBuildRegions(&ctx, *o.chf, 0, cfg.minRegionArea, cfg.mergeRegionArea))
        return 0;
    o.cset = rcAllocContourSet();
    if (!o.cset ||
        !rcBuildContours(&ctx, *o.chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *o.cset))
        return 0;
    o.pmesh = rcAllocPolyMesh();
    if (!o.pmesh || !rcBuildPolyMesh(&ctx, *o.cset, cfg.maxVertsPerPoly, *o.pmesh))
        return 0;
    o.dmesh = rcAllocPolyMeshDetail();
    if (!o.dmesh || !rcBuildPolyMeshDetail(&ctx, *o.pmesh, *o.chf, cfg.detailSampleDist,
                                           cfg.detailSampleMaxError, *o.dmesh))
        return 0;
    for (int i = 0; i < o.pmesh->npolys; ++i)
        o.pmesh->flags[i] = 1;

    dtNavMeshCreateParams p;
    std::memset(&p, 0, sizeof p);
    p.verts = o.pmesh->verts;
    p.vertCount = o.pmesh->nverts;
    p.polys = o.pmesh->polys;
    p.polyAreas = o.pmesh->areas;
    p.polyFlags = o.pmesh->flags;
    p.polyCount = o.pmesh->npolys;
    p.nvp = o.pmesh->nvp;
    p.detailMeshes = o.dmesh->meshes;
    p.detailVerts = o.dmesh->verts;
    p.detailVertsCount = o.dmesh->nverts;
    p.detailTris = o.dmesh->tris;
    p.detailTriCount = o.dmesh->ntris;
    p.walkableHeight = 2.0f;
    p.walkableRadius = 0.5f;
    p.walkableClimb = 0.4f;
    rcVcopy(p.bmin, o.pmesh->bmin);
    rcVcopy(p.bmax, o.pmesh->bmax);
    p.cs = cfg.cs;
    p.ch = cfg.ch;
    p.buildBvTree = true;
    unsigned char *data = nullptr;
    int size = 0;
    if (!dtCreateNavMeshData(&p, &data, &size))
        return 0;
    o.nav = dtAllocNavMesh();
    if (!o.nav || dtStatusFailed(o.nav->init(data, size, DT_TILE_FREE_DATA))) {
        dtFree(data);
        return 0;
    }
    const dtNavMesh *cnav = o.nav;
    const dtMeshTile *tile = cnav->getTile(0);
    *out_poly_count = tile && tile->header ? tile->header->polyCount : 0;
    return 1;
}
