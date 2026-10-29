// shaders/culling/cull_common.slang: what GPU culling shares (SPEC §12.5 step 1; DENG-229): the
// per-view cull constants, the frustum tests and the occlusion-test hook.
//
//     import cull_common;   // from shaders/culling/
//
// The cull constants (DeVisCullView) are written by the CPU each frame into an UPLOAD buffer,
// one entry per frame slot (src/render/passes/vis/cull_instances.c), and read through
// de_robuffer<DeVisCullView>. The C mirror is de__vis_cull_view in
// src/render/passes/vis/vis_stages.h, filled by de__vis_cull_view_init; the CPU reference tests
// (de__vis_cull_sphere, de__vis_cull_aabb) do the same arithmetic in the same order.
//
// Occlusion (DENG-233): two phases per view. Phase 1 draws what was visible after the last frame
// (frustum-tested with the current camera; no occlusion test). The HZB is built from that depth.
// Phase 2 re-tests every frustum-visible instance (and meshlet) against it, draws what phase 1
// did not, and writes this frame's visibility. cull_occlusion_test reads the view's HZB from the
// DeVisCullView given to de_cull_occlusion_begin, which a kernel calls once per thread first.
//
// Frustum: the planes of de_frustum_from_m4 (Gribb-Hartmann, normalized, normals inward) in
// render-origin space. The projection is reverse-Z with an infinite far plane (or a far plane
// 100 km away for orthographic views), so the far plane is never tested: the five planes are
// left, right, bottom, top, near.
module cull_common;
import bindless;

import occlusion;

/// DeVisCullView.flags
public static const uint DE_VIS_CULL_FRUSTUM = 1;   // test the frustum (off: r.cull_disable)
public static const uint DE_VIS_CULL_FROZEN = 2;    // the culling camera is frozen (r.cull_freeze)
public static const uint DE_VIS_CULL_OCCLUSION = 4; // two-phase occlusion on (r.occlusion)
/// Frustum planes tested (the far plane is skipped).
public static const uint DE_VIS_CULL_PLANES = 5;

/// One view's cull constants (224 bytes).
public struct DeVisCullView
{
    public float4 planes[5];    // left, right, bottom, top, near: normal xyz (unit), d in w
    public float4 camera;       // xyz: the culling camera, relative to the render origin; w: near
    public float4 lod;          // x: LOD scale (pixels per metre at distance 1 / r.lod_error_px);
                                // y: orthographic; z: r.lod_bias; w: forced level+1 (0 automatic)
    public float4 view_proj[4]; // the culling view-projection, rows
    public uint instance_count; // instance slots to test
    public uint layer_mask;     // instances whose layer_mask shares no bit with it are skipped
    public uint work_capacity;  // DeVisWorkItem entries the work list holds
    public uint flags;          // DE_VIS_CULL_*
    public uint hzb;            // sampled bindless index of the view's HZB (DENG-233), 0 = none
    public uint hzb_width;      // its mip 0 size and mip count
    public uint hzb_height;
    public uint hzb_mips;
    public uint visibility;     // bindless index of the view's visibility flags (raw, u32 per
                                // instance slot: 1 = visible after the last frame's phase 2)
    public uint visibility_count; // slots the last frame wrote (0: no history, e.g. a camera cut)
    public uint hidden_entities; // sorted uint2 full entity IDs: lo,hi
    public uint hidden_count;
};

/// Sorted full IDs preserve generations; no instance table is copied per view.
public bool de_hidden_entity(uint entities, uint count, uint lo_word, uint hi_word)
{
    if (count == 0u || entities == 0u)
        return false;
    uint lo = 0u, hi = count;
    while (lo < hi)
    {
        uint mid = lo + (hi - lo) / 2u;
        uint2 value = de_robuffer<uint2>(entities)[mid];
        if (value.y < hi_word || (value.y == hi_word && value.x < lo_word))
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (lo >= count)
        return false;
    return all(de_robuffer<uint2>(entities)[lo] == uint2(lo_word, hi_word));
}
public bool de_cull_hidden(DeVisCullView v, uint lo_word, uint hi_word) {
    return de_hidden_entity(v.hidden_entities, v.hidden_count, lo_word, hi_word);
}

/// The bounds an occlusion test sees: a world AABB and its bounding sphere (render-origin space).
public struct DeCullBounds
{
    public float3 aabb_min;
    public float3 aabb_max;
    public float3 center;
    public float radius;
};

/// False when the sphere is entirely behind one frustum plane.
public bool de_cull_sphere(DeVisCullView v, float3 c, float r)
{
    for (uint i = 0; i < DE_VIS_CULL_PLANES; ++i)
    {
        float4 p = v.planes[i];
        if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r)
            return false;
    }
    return true;
}

/// False when the box is entirely behind one frustum plane (its corner furthest along the normal
/// is behind it).
public bool de_cull_aabb(DeVisCullView v, float3 mn, float3 mx)
{
    for (uint i = 0; i < DE_VIS_CULL_PLANES; ++i)
    {
        float4 p = v.planes[i];
        float x = p.x >= 0.0 ? mx.x : mn.x;
        float y = p.y >= 0.0 ? mx.y : mn.y;
        float z = p.z >= 0.0 ? mx.z : mn.z;
        if (p.x * x + p.y * y + p.z * z + p.w < 0.0)
            return false;
    }
    return true;
}

/// The view whose HZB cull_occlusion_test reads (set by de_cull_occlusion_begin).
static DeVisCullView g_de_cull_view;
static bool g_de_cull_ready = false;

/// Gives cull_occlusion_test the view's cull constants: call it once per thread, before the
/// first test (with the DeVisCullView the kernel uses for its frustum test).
public void de_cull_occlusion_begin(DeVisCullView v)
{
    g_de_cull_view = v;
    g_de_cull_ready = true;
}

/// The occlusion test of `b` in culling phase `phase` (1 or 2): true when it may be visible.
/// Phase 1 never culls (it draws last frame's visible set); phase 2 tests the bounds against
/// this frame's HZB (de_occlusion_visible). Visible without de_cull_occlusion_begin, with
/// occlusion off, or without an HZB.
public bool cull_occlusion_test(DeCullBounds b, uint phase)
{
    if (!g_de_cull_ready || phase != 2u)
        return true;
    DeVisCullView v = g_de_cull_view;
    if ((v.flags & DE_VIS_CULL_OCCLUSION) == 0 || v.hzb == 0)
        return true;
    DeHzbInfo h;
    h.tex = v.hzb;
    h.width = v.hzb_width;
    h.height = v.hzb_height;
    h.mips = v.hzb_mips;
    return de_occlusion_visible(v.view_proj, b.aabb_min, b.aabb_max, h);
}
