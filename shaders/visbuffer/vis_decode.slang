// shaders/visbuffer/vis_decode.slang: reading a visible meshlet's geometry (SPEC §12.5 step 2;
// DENG-234). Shared by every pass that turns a visible meshlet back into triangles:
//
//     import "../visbuffer/vis_decode";
//
// the mesh-shader raster (vis_mesh.slang, DENG-234), the compute-expansion raster (DENG-235), the
// alpha-tested raster (DENG-236) and the resolve (DENG-238). It holds no pass-specific code; an
// addition must keep every function below unchanged (others import it read-only).
//
// The data is the GPU scene's (scene_types.slang, DENG-177) and the cooked mesh's (DMSH v2,
// src/render/formats/de_cooked_mesh.h, DENG-103), uploaded as they are by the mesh loader
// (DENG-105): DeGpuMesh.meshlets holds the 64-byte meshlet records, .meshlet_vertices the u32
// mesh vertex index per meshlet vertex, .meshlet_triangles 3 u8 local indices per triangle, and
// .vertices the vertex streams at .stream_offset[stream] in .stream_format[stream].
//
// Typical use (one visible meshlet, its local vertex v and triangle t):
//
//     DeSceneConstants sc = de_robuffer<DeSceneConstants>(scene)[view_slot];
//     DeVisMeshlet e = de_robuffer<DeVisMeshlet>(visible)[index];
//     DeGpuInstance inst = de_robuffer<DeGpuInstance>(sc.instances)[e.instance];
//     DeGpuMesh mesh = de_robuffer<DeGpuMesh>(sc.meshes)[inst.mesh];
//     DeVisMeshletInfo m = de_vis_meshlet_info(mesh, e.meshlet);
//     float3 p = de_vis_meshlet_position(mesh, m, v);         // object space, dequantized
//     float3 w = de_vis_transform_point(inst.transform, p);   // render-origin space
//     float4 clip = de_vis_clip(sc.view_proj, w);
//     uint3 tri = de_vis_meshlet_triangle(mesh, m, t);        // local vertex indices
//     uint id = de_vis_id_pack(index, t);                     // vis_types.slang
module vis_decode;

import push_constants;
import "../scene/scene_types";
import vis_types;

/// Bytes of one meshlet record (DE_COOKED_MESHLET_SIZE).
public static const uint DE_VIS_DECODE_RECORD_BYTES = 64;
/// Vertices and triangles of one meshlet at most (DE_COOKED_MESHLET_MAX_VERTICES / _TRIANGLES).
public static const uint DE_VIS_MESHLET_MAX_VERTICES = 64;
public static const uint DE_VIS_MESHLET_MAX_TRIANGLES = 124;
/// The vertex stream of positions (de_cooked_stream) and its two formats
/// (de_cooked_vertex_format).
public static const uint DE_VIS_STREAM_POSITION = 0;
public static const uint DE_VIS_VF_F32X3 = 2;
public static const uint DE_VIS_VF_UNORM16X4 = 5;

/// The parts of a meshlet record that drawing needs.
public struct DeVisMeshletInfo
{
    public uint vertex_offset;   // first entry of the meshlet in DeGpuMesh.meshlet_vertices
    public uint triangle_offset; // byte offset of its triangles in DeGpuMesh.meshlet_triangles
    public uint vertex_count;    // 0 .. DE_VIS_MESHLET_MAX_VERTICES
    public uint triangle_count;  // 0 .. DE_VIS_MESHLET_MAX_TRIANGLES
    public uint submesh;         // its submesh (material slot index of the instance)
};

/// True when `mesh` has every buffer drawing reads (meshlets, meshlet vertices and triangles,
/// vertices): a mesh without them (bindless 0) must not be read.
public bool de_vis_mesh_drawable(DeGpuMesh mesh)
{
    return mesh.meshlets != DE_BINDLESS_INVALID && mesh.meshlet_vertices != DE_BINDLESS_INVALID &&
           mesh.meshlet_triangles != DE_BINDLESS_INVALID && mesh.vertices != DE_BINDLESS_INVALID;
}

/// Record `meshlet` of `mesh`'s meshlet table.
public DeVisMeshletInfo de_vis_meshlet_info(DeGpuMesh mesh, uint meshlet)
{
    DeBytes r = de_bytes(mesh.meshlets);
    uint at = meshlet * DE_VIS_DECODE_RECORD_BYTES;
    uint3 w = r.load3(at);
    DeVisMeshletInfo m;
    m.vertex_offset = w.x;
    m.triangle_offset = w.y;
    m.vertex_count = min(w.z & 0xFFu, DE_VIS_MESHLET_MAX_VERTICES);
    m.triangle_count = min((w.z >> 8) & 0xFFu, DE_VIS_MESHLET_MAX_TRIANGLES);
    m.submesh = w.z >> 16;
    return m;
}

/// The mesh vertex index of local vertex `v` (< m.vertex_count) of meshlet `m`.
public uint de_vis_meshlet_vertex(DeGpuMesh mesh, DeVisMeshletInfo m, uint v)
{
    return de_bytes(mesh.meshlet_vertices).load((m.vertex_offset + v) * 4u);
}

/// Byte `offset` of raw buffer `b`.
public uint de_vis_load_u8(DeBytes b, uint offset)
{
    return (b.load(offset & ~3u) >> ((offset & 3u) * 8u)) & 0xFFu;
}

/// The local vertex indices of triangle `t` (< m.triangle_count) of meshlet `m`.
public uint3 de_vis_meshlet_triangle(DeGpuMesh mesh, DeVisMeshletInfo m, uint t)
{
    DeBytes b = de_bytes(mesh.meshlet_triangles);
    uint at = m.triangle_offset + t * 3u;
    return uint3(de_vis_load_u8(b, at), de_vis_load_u8(b, at + 1u), de_vis_load_u8(b, at + 2u));
}

/// Object-space position of mesh vertex `vertex`: UNORM16X4 (offset + q * scale, where scale is
/// one step of q: the cooker's (max - min) / 65535, de_cooked_mesh_position) or F32X3, as the
/// mesh's position stream is stored.
public float3 de_vis_mesh_position(DeGpuMesh mesh, uint vertex)
{
    DeBytes s = de_bytes(mesh.vertices);
    uint base = mesh.stream_offset[0].x;
    if (mesh.stream_format[0].x == DE_VIS_VF_F32X3)
        return asfloat(s.load3(base + vertex * 12u));
    uint2 q = s.load2(base + vertex * 8u);
    float3 u = float3(float(q.x & 0xFFFFu), float(q.x >> 16), float(q.y & 0xFFFFu));
    return mesh.position_offset.xyz + u * mesh.position_scale.xyz;
}

/// Object-space position of local vertex `v` of meshlet `m`.
public float3 de_vis_meshlet_position(DeGpuMesh mesh, DeVisMeshletInfo m, uint v)
{
    return de_vis_mesh_position(mesh, de_vis_meshlet_vertex(mesh, m, v));
}

/// A point through a 3x4 transform (rows).
public float3 de_vis_transform_point(float4 rows[3], float3 p)
{
    float4 h = float4(p, 1.0);
    return float3(dot(rows[0], h), dot(rows[1], h), dot(rows[2], h));
}

/// The determinant of a 3x4 transform's linear part: negative for a mirroring transform.
public float de_vis_transform_det(float4 rows[3])
{
    return dot(rows[0].xyz, cross(rows[1].xyz, rows[2].xyz));
}

/// Clip-space position of render-origin-space point `w` under a view-projection (rows).
public float4 de_vis_clip(float4 vp[4], float3 w)
{
    float4 h = float4(w, 1.0);
    return float4(dot(vp[0], h), dot(vp[1], h), dot(vp[2], h), dot(vp[3], h));
}

/// The material slot of submesh `submesh` of an instance (DE_GPU_NONE: the default material).
public uint de_vis_instance_material(DeGpuInstance inst, uint submesh)
{
    if (submesh >= DE_GPU_INSTANCE_MATERIALS)
        return DE_GPU_NONE;
    uint4 q = inst.materials[submesh >> 2];
    uint k = submesh & 3u;
    return k == 0u ? q.x : k == 1u ? q.y : k == 2u ? q.z : q.w;
}

/// True when the material in slot `material` of table `materials` (`count` entries) draws both
/// faces: a READY material with DE_GPU_MATERIAL_DOUBLE_SIDED. The default material
/// (DE_GPU_NONE) and one not loaded yet are single-sided.
public bool de_vis_material_double_sided(uint materials, uint count, uint material)
{
    if (material == DE_GPU_NONE || material >= count || materials == 0)
        return false;
    uint f = de_robuffer<DeGpuMaterial>(materials)[material].flags;
    return (f & (DE_GPU_MATERIAL_READY | DE_GPU_MATERIAL_DOUBLE_SIDED)) ==
           (DE_GPU_MATERIAL_READY | DE_GPU_MATERIAL_DOUBLE_SIDED);
}

/// Twice the signed area of a triangle in normalized device coordinates (x right, y up):
/// positive when it winds counter-clockwise, the front face of the engine's pipelines.
public float de_vis_ndc_area2(float2 a, float2 b, float2 c)
{
    return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
}

// ---- DENG-236 (additions; the functions above are unchanged) ----

/// The vertex stream of UV set 0 (de_cooked_stream) and its two formats (de_cooked_vertex_format).
public static const uint DE_VIS_STREAM_UV0 = 3;
public static const uint DE_VIS_VF_F32X2 = 1;
public static const uint DE_VIS_VF_UNORM16X2 = 8;

/// UV set 0 of mesh vertex `vertex`: UNORM16X2 (DeGpuMesh.uv_transform[0]: offset xy + q * scale
/// zw, q the raw u16, as de_cooked_mesh_uv) or F32X2; (0, 0) for a mesh without UV0.
public float2 de_vis_mesh_uv0(DeGpuMesh mesh, uint vertex)
{
    uint format = mesh.stream_format[0].w;
    uint base = mesh.stream_offset[0].w;
    DeBytes s = de_bytes(mesh.vertices);
    if (format == DE_VIS_VF_F32X2)
        return asfloat(s.load2(base + vertex * 8u));
    if (format != DE_VIS_VF_UNORM16X2)
        return float2(0.0, 0.0);
    uint q = s.load(base + vertex * 4u);
    float4 t = mesh.uv_transform[0];
    return t.xy + float2(float(q & 0xFFFFu), float(q >> 16)) * t.zw;
}

/// UV set 0 of local vertex `v` of meshlet `m`.
public float2 de_vis_meshlet_uv0(DeGpuMesh mesh, DeVisMeshletInfo m, uint v)
{
    return de_vis_mesh_uv0(mesh, de_vis_meshlet_vertex(mesh, m, v));
}
