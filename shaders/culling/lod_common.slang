module lod_common;
import cull_common;
import "../scene/scene_types";
import "../visbuffer/vis_types";
/// The largest axis scale of a 3x4 transform (rows): the longest column of its linear part.
public float max_axis_scale(float4 rows[3])
{
    float sx = rows[0].x * rows[0].x + rows[1].x * rows[1].x + rows[2].x * rows[2].x;
    float sy = rows[0].y * rows[0].y + rows[1].y * rows[1].y + rows[2].y * rows[2].y;
    float sz = rows[0].z * rows[0].z + rows[1].z * rows[1].z + rows[2].z * rows[2].z;
    return sqrt(max(sx, max(sy, sz)));
}

/// The LOD of `m` for an instance with bounding sphere (c, r) and axis scale `scale`.
public uint select_lod(DeGpuMesh m, float scale, float3 c, float r, DeVisCullView v)
{
    uint levels = min(m.lod_count, DE_GPU_MESH_LODS);
    if (levels == 0u)
        return 0u;
    if (v.lod.w > 0.0)
        return min(uint(v.lod.w) - 1u, levels - 1u);
    float dist = 1.0;
    if (v.lod.y == 0.0)
    {
        float3 d = c - v.camera.xyz;
        dist = max(sqrt(d.x * d.x + d.y * d.y + d.z * d.z) - r, v.camera.w);
    }
    int count = int(levels);
    int lod = 0;
    for (int l = count - 1; l > 0; --l)
    {
        if (m.lods[l].error * scale * v.lod.x <= dist)
        {
            lod = l;
            break;
        }
    }
    return uint(clamp(lod + int(v.lod.z), 0, count - 1));
}

