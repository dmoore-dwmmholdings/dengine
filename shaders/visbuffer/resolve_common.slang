// shaders/visbuffer/resolve_common.slang: from a vis-buffer pixel back to its surface point (SPEC
// §12.5 step 4; DENG-238). The material resolve (resolve_standard.slang) imports it; later passes
// that need a pixel's triangle (lighting layers, S14; RT, S16) can import it too. Geometry is read
// only through vis_decode.slang.
//
//   de_resolve_point   the pixel's vis ID -> visible-list entry -> instance, meshlet, triangle ->
//                      the three vertices; perspective-correct barycentrics from a ray-triangle
//                      intersection of the camera ray through the pixel centre (world space), and
//                      their screen-space derivatives from the rays through the next pixel in x and
//                      in y (the one-pixel differences a pixel quad would give, analytically)
//   de_resolve_*       attributes interpolated with them (position, UV0 / UV1 and their
//                      derivatives, normal, tangent, colour) and the motion vector (the point
//                      through the instance's previous transform and the previous view-projection)
//
// Precision: positions are relative to the render origin (DENG-2), so the intersection is well
// conditioned near the camera. A triangle the ray runs parallel to gives no point (valid false).
module resolve_common;

import bindless;
import "../scene/scene_types";
import "../visbuffer/vis_types";
import "../visbuffer/vis_decode";

/// One pixel's surface point.
public struct DeResolvePoint
{
    public bool valid;
    public uint entry;      // visible-list entry (the vis ID's meshlet index)
    public uint triangle;   // in the meshlet
    public DeGpuInstance inst;
    public DeGpuMesh mesh;
    public DeVisMeshletInfo meshlet;
    public uint3 vertex;    // mesh vertex indices of the triangle's corners
    public float3 bary;     // perspective-correct barycentrics at the pixel centre
    public float3 bary_dx;  // their change one pixel to the right
    public float3 bary_dy;  // and one pixel down
    public float3 object_pos[3]; // the corners in object space
};

/// The render-origin-space point of NDC (x, y, z) under the inverse view-projection rows.
float3 unproject(float4 inv_vp[4], float3 ndc)
{
    float4 h = float4(ndc, 1.0);
    float4 p = float4(dot(inv_vp[0], h), dot(inv_vp[1], h), dot(inv_vp[2], h), dot(inv_vp[3], h));
    return p.xyz / p.w;
}

/// Barycentrics (of v1, v2; v0 = 1 - both) where the line o + t d meets the triangle's plane
/// (Moller-Trumbore without the bounds tests); false when the line is parallel to the plane.
bool ray_bary(float3 o, float3 d, float3 v0, float3 v1, float3 v2, out float3 bary)
{
    float3 e1 = v1 - v0, e2 = v2 - v0;
    float3 p = cross(d, e2);
    float det = dot(e1, p);
    bary = float3(1.0, 0.0, 0.0);
    if (abs(det) < 1e-30)
        return false;
    float inv = 1.0 / det;
    float3 s = o - v0;
    float u = dot(s, p) * inv;
    float3 q = cross(s, e1);
    float v = dot(d, q) * inv;
    bary = float3(1.0 - u - v, u, v);
    return true;
}

/// The camera ray (origin, direction) through NDC point `ndc` (reverse-Z: z = 1 the near plane).
void camera_ray(float4 inv_vp[4], float2 ndc, out float3 o, out float3 d)
{
    float3 a = unproject(inv_vp, float3(ndc, 1.0));
    float3 b = unproject(inv_vp, float3(ndc, 0.5));
    o = a;
    d = b - a;
}

/// The surface point of the pixel at `pixel` (target size `size`) whose vis ID is `id` (not
/// DE_VIS_ID_EMPTY), with visible-list `visible` (`visible_count` entries) and the scene of `sc`.
public DeResolvePoint de_resolve_point(uint id, uint2 pixel, float2 size, DeSceneConstants sc,
                                       uint visible, uint visible_count)
{
    DeResolvePoint r;
    r.valid = false;
    r.entry = de_vis_id_meshlet(id);
    r.triangle = de_vis_id_triangle(id);
    r.vertex = uint3(0, 0, 0);
    r.bary = float3(1.0, 0.0, 0.0);
    r.bary_dx = r.bary;
    r.bary_dy = r.bary;
    if (r.entry >= visible_count)
        return r;
    DeVisMeshlet vm = de_robuffer<DeVisMeshlet>(visible)[r.entry];
    r.inst = de_robuffer<DeGpuInstance>(sc.instances)[vm.instance];
    r.mesh = de_robuffer<DeGpuMesh>(sc.meshes)[r.inst.mesh];
    if (!de_vis_mesh_drawable(r.mesh) || vm.meshlet >= r.mesh.meshlet_count)
        return r;
    r.meshlet = de_vis_meshlet_info(r.mesh, vm.meshlet);
    if (r.triangle >= r.meshlet.triangle_count)
        return r;
    uint3 lv = de_vis_meshlet_triangle(r.mesh, r.meshlet, r.triangle);
    r.vertex = uint3(de_vis_meshlet_vertex(r.mesh, r.meshlet, lv.x),
                     de_vis_meshlet_vertex(r.mesh, r.meshlet, lv.y),
                     de_vis_meshlet_vertex(r.mesh, r.meshlet, lv.z));
    float3 w[3];
    // Unrolled on purpose (DENG-430): a loop storing r.object_pos[k] with a dynamic k (an array
    // inside this ~800-byte struct, in thread memory) gave values that changed from frame to frame
    // under Metal shader validation (MTL_SHADER_VALIDATION=1), so textured surfaces flickered.
    // Every index was in range; the instrumented code was not stable.
    r.object_pos[0] = de_vis_mesh_position(r.mesh, r.vertex.x);
    r.object_pos[1] = de_vis_mesh_position(r.mesh, r.vertex.y);
    r.object_pos[2] = de_vis_mesh_position(r.mesh, r.vertex.z);
    w[0] = de_vis_transform_point(r.inst.transform, r.object_pos[0]);
    w[1] = de_vis_transform_point(r.inst.transform, r.object_pos[1]);
    w[2] = de_vis_transform_point(r.inst.transform, r.object_pos[2]);
    // NDC of the pixel centre and of its right and lower neighbours (y up in NDC).
    float2 step = 2.0 / size;
    float2 ndc = float2((float(pixel.x) + 0.5) * step.x - 1.0, 1.0 - (float(pixel.y) + 0.5) * step.y);
    float3 o, d;
    camera_ray(sc.inv_view_proj, ndc, o, d);
    if (!ray_bary(o, d, w[0], w[1], w[2], r.bary))
        return r;
    camera_ray(sc.inv_view_proj, ndc + float2(step.x, 0.0), o, d);
    float3 bx;
    ray_bary(o, d, w[0], w[1], w[2], bx);
    camera_ray(sc.inv_view_proj, ndc - float2(0.0, step.y), o, d);
    float3 by;
    ray_bary(o, d, w[0], w[1], w[2], by);
    r.bary_dx = bx - r.bary;
    r.bary_dy = by - r.bary;
    r.valid = true;
    return r;
}

/// Interpolations with the point's barycentrics (and their one-pixel differences).
public float3 de_resolve_lerp3(DeResolvePoint r, float3 a, float3 b, float3 c)
{
    return a * r.bary.x + b * r.bary.y + c * r.bary.z;
}
public float2 de_resolve_lerp2(float3 bary, float2 a, float2 b, float2 c)
{
    return a * bary.x + b * bary.y + c * bary.z;
}

/// The point in object space and in render-origin (world) space.
public float3 de_resolve_object_pos(DeResolvePoint r)
{
    return de_resolve_lerp3(r, r.object_pos[0], r.object_pos[1], r.object_pos[2]);
}
public float3 de_resolve_world_pos(DeResolvePoint r)
{
    return de_vis_transform_point(r.inst.transform, de_resolve_object_pos(r));
}

/// UV set `set` (0 or 1) at the point, and its one-pixel differences in x and y.
public float2 de_resolve_uv(DeResolvePoint r, uint set, out float2 dx, out float2 dy)
{
    if((r.inst.flags&DE_GPU_INSTANCE_SPRITE)!=0) {
        float3 a=r.object_pos[0],b=r.object_pos[1],c=r.object_pos[2];
        float2 u=float2(a.x,1-a.y),v=float2(b.x,1-b.y),w=float2(c.x,1-c.y);
        dx=de_resolve_lerp2(r.bary_dx,u,v,w);dy=de_resolve_lerp2(r.bary_dy,u,v,w);
        return de_resolve_lerp2(r.bary,u,v,w);
    }
    float2 t[3];
    for (uint k = 0; k < 3u; ++k)
        t[k] = set == 0u ? de_vis_mesh_uv0(r.mesh, r.vertex[k]) : de_vis_mesh_uv1(r.mesh, r.vertex[k]);
    dx = de_resolve_lerp2(r.bary_dx, t[0], t[1], t[2]);
    dy = de_resolve_lerp2(r.bary_dy, t[0], t[1], t[2]);
    return de_resolve_lerp2(r.bary, t[0], t[1], t[2]);
}

/// Object normal `n` through a 3x4 transform's linear part M: the cofactor matrix of M (rows
/// cross(r1, r2), cross(r2, r0), cross(r0, r1)) times n, the inverse transpose up to the
/// determinant's scale, so non-uniform scale keeps it perpendicular.
float3 normal_to_world(float4 rows[3], float3 n)
{
    float3 r0 = rows[0].xyz, r1 = rows[1].xyz, r2 = rows[2].xyz;
    float3 c0 = cross(r1, r2), c1 = cross(r2, r0), c2 = cross(r0, r1);
    return normalize(float3(dot(c0, n), dot(c1, n), dot(c2, n)));
}

/// The interpolated world-space shading normal and tangent (xyz; w the bitangent sign, with the
/// transform's mirroring), and the geometric normal of the triangle (world space, unit).
public float3 de_resolve_normal(DeResolvePoint r, out float4 tangent, out float3 geometric)
{
    float3 n = de_resolve_lerp3(r, de_vis_mesh_normal(r.mesh, r.vertex.x),
                                de_vis_mesh_normal(r.mesh, r.vertex.y),
                                de_vis_mesh_normal(r.mesh, r.vertex.z));
    float4 t0 = de_vis_mesh_tangent(r.mesh, r.vertex.x);
    float4 t1 = de_vis_mesh_tangent(r.mesh, r.vertex.y);
    float4 t2 = de_vis_mesh_tangent(r.mesh, r.vertex.z);
    float3 t = de_resolve_lerp3(r, t0.xyz, t1.xyz, t2.xyz);
    float det = de_vis_transform_det(r.inst.transform);
    float3 tw = float3(dot(r.inst.transform[0].xyz, t), dot(r.inst.transform[1].xyz, t),
                       dot(r.inst.transform[2].xyz, t));
    tangent = float4(normalize(tw), t0.w * (det < 0.0 ? -1.0 : 1.0));
    float3 w0 = de_vis_transform_point(r.inst.transform, r.object_pos[0]);
    float3 w1 = de_vis_transform_point(r.inst.transform, r.object_pos[1]);
    float3 w2 = de_vis_transform_point(r.inst.transform, r.object_pos[2]);
    geometric = normalize(cross(w1 - w0, w2 - w0)) * (det < 0.0 ? -1.0 : 1.0);
    if (!de_vis_mesh_has_stream(r.mesh, DE_VIS_STREAM_NORMAL))
        return geometric;
    return normal_to_world(r.inst.transform, n);
}

/// Vertex colour 0 at the point (white without colours).
public float4 de_resolve_color(DeResolvePoint r)
{
    float4 c0 = de_vis_mesh_color0(r.mesh, r.vertex.x);
    float4 c1 = de_vis_mesh_color0(r.mesh, r.vertex.y);
    float4 c2 = de_vis_mesh_color0(r.mesh, r.vertex.z);
    return c0 * r.bary.x + c1 * r.bary.y + c2 * r.bary.z;
}

/// The motion vector: NDC xy of the point now minus its NDC xy in the previous frame (the
/// instance's previous transform under the previous view-projection; DeSceneConstants keeps it
/// rebased onto this frame's origin). A new instance has its previous transform equal to its
/// current one (the render world), so only camera motion moves it.
public float2 de_resolve_motion(DeResolvePoint r, DeSceneConstants sc)
{
    float3 p = de_resolve_object_pos(r);
    float4 now = de_vis_clip(sc.view_proj, de_vis_transform_point(r.inst.transform, p));
    float3 previous=p;
    if((r.inst.flags&DE_GPU_INSTANCE_SKINNED)!=0 && r.inst.reserved1!=0) {
        DeBytes b=de_bytes(r.inst.reserved1);
        previous=de_resolve_lerp3(r,asfloat(b.load3(r.vertex.x*12)),
            asfloat(b.load3(r.vertex.y*12)),asfloat(b.load3(r.vertex.z*12)));
    }
    float4 then = de_vis_clip(sc.prev_view_proj, de_vis_transform_point(r.inst.prev_transform, previous));
    if (!(now.w > 0.0) || !(then.w > 0.0))
        return float2(0.0, 0.0);
    return now.xy / now.w - then.xy / then.w;
}

/// The material slot of the point's submesh (DE_GPU_NONE: the mesh's default).
public uint de_resolve_material(DeResolvePoint r)
{
    return de_vis_instance_material(r.inst, r.meshlet.submesh);
}

/// The material model of the point (the classification rule, classify.slang's entry_model: the
/// model of a READY material of the table, else Standard).
public uint de_resolve_model(DeResolvePoint r, DeSceneConstants sc)
{
    uint slot = de_resolve_material(r);
    if (sc.materials == DE_BINDLESS_INVALID || slot == DE_GPU_NONE || slot >= sc.material_count)
        return DE_GPU_MATERIAL_MODEL_STANDARD;
    DeGpuMaterial m = de_robuffer<DeGpuMaterial>(sc.materials)[slot];
    if ((m.flags & DE_GPU_MATERIAL_READY) == 0u)
        return DE_GPU_MATERIAL_MODEL_STANDARD;
    return m.model & (DE_GPU_MATERIAL_MODELS - 1u);
}
