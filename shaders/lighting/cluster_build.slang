// shaders/lighting/cluster_build.slang: the view-space bounds of a view's froxel clusters (SPEC
// §12.5 step 5; DENG-303). Engine shader `lighting_clusters`; src/render/lighting/clusters.c runs
// it when the view's projection, size or cluster cvars change (the bounds persist in a per-view
// buffer between frames).
//
//   cs_cluster_build   64 threads, one per cluster: the AABB (view space) of the cluster's tile
//                      frustum between its slice's depths: the 4 tile-corner rays, each cut at
//                      both depths (the last slice's far end at DE_CLUSTER_FAR_DEPTH)
import bindless;
import push_constants;
import "../lighting/lighting_common";

/// The far depth the last (infinite) slice's bounds use.
public static const float DE_CLUSTER_FAR_DEPTH = 1e20;

/// Push constants (32 bytes; clusters.c cl_build_params).
struct ClusterBuildParams
{
    uint view;     // DeLightingView ring (de_robuffer)
    uint entry;    // this frame's entry in it
    uint bounds;   // float4 x 2 per cluster: min, max (view space; DE_ACCESS_SHADER_WRITE)
    uint width;    // the view's internal size
    uint height;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

float3 unproject(DeLightingView lv, float2 ndc, float z)
{
    float4 c = float4(ndc, z, 1.0);
    float4 v = float4(dot(lv.inv_proj[0], c), dot(lv.inv_proj[1], c), dot(lv.inv_proj[2], c),
                      dot(lv.inv_proj[3], c));
    return v.xyz / v.w;
}

/// The point of the ray through `a`, `b` (view space) at view depth `depth` (-z).
float3 at_depth(float3 a, float3 b, float depth)
{
    float da = -a.z, db = -b.z;
    float t = abs(db - da) > 1e-12 ? (depth - da) / (db - da) : 0.0;
    return a + (b - a) * t;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_cluster_build(uint3 id : SV_DispatchThreadID, uniform ClusterBuildParams pc)
{
    DeLightingView lv = de_robuffer<DeLightingView>(pc.view)[pc.entry];
    uint c = id.x;
    if (c >= lv.cluster_count)
        return;
    uint tx = c % lv.tiles_x;
    uint ty = (c / lv.tiles_x) % lv.tiles_y;
    uint k = c / (lv.tiles_x * lv.tiles_y);
    float d0 = k == 0u ? lv.near : de_cluster_slice_depth(lv, k);
    float d1 = k + 1u >= lv.slices ? DE_CLUSTER_FAR_DEPTH : de_cluster_slice_depth(lv, k + 1u);
    float2 size = float2(float(pc.width), float(pc.height));
    float3 lo = float3(1e30, 1e30, 1e30), hi = float3(-1e30, -1e30, -1e30);
    for (uint i = 0; i < 4u; ++i)
    {
        float2 px = float2(float((tx + (i & 1u)) * lv.tile_px), float((ty + (i >> 1)) * lv.tile_px));
        px = min(px, size);
        float2 ndc = float2(px.x / size.x * 2.0 - 1.0, 1.0 - px.y / size.y * 2.0);
        float3 a = unproject(lv, ndc, 1.0); // reverse-Z: 1 = the near plane
        float3 b = unproject(lv, ndc, 0.5);
        float3 p0 = at_depth(a, b, d0), p1 = at_depth(a, b, d1);
        lo = min(lo, min(p0, p1));
        hi = max(hi, max(p0, p1));
    }
    DeRWBytes out = de_rwbytes(pc.bounds);
    out.store4(c * 32u, asuint(float4(lo, 0.0)));
    out.store4(c * 32u + 16u, asuint(float4(hi, 0.0)));
}
