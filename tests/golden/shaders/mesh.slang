// Golden scene rhi_mesh_triangle (DENG-329): amplification + mesh + pixel. The amplification
// shader launches one mesh group, which emits a hexagon fan (7 vertices, 6 triangles) with a
// color per vertex, from constants only.
import push_constants;

struct DrawData
{
    float4 bias;  // added to the interpolated color
    float scale;  // the hexagon's radius in clip space
};

struct Payload
{
    uint unused;
};

groupshared Payload g_payload;

[shader("amplification")]
[numthreads(1, 1, 1)]
void as_main(uniform DrawData pc)
{
    g_payload.unused = 0u;
    DispatchMesh(1, 1, 1, g_payload);
}

struct V
{
    float4 pos : SV_Position;
    float4 color : COLOR0;
};

// Vertex 0 is the center; 1..6 go counter-clockwise around it (cos, sin of 0, 60, ... 300 deg).
static const float2 k_dir[7] = {
    float2(0.0, 0.0), float2(1.0, 0.0), float2(0.5, 0.8660254),
    float2(-0.5, 0.8660254), float2(-1.0, 0.0), float2(-0.5, -0.8660254),
    float2(0.5, -0.8660254),
};
static const float4 k_color[7] = {
    float4(1.0, 1.0, 1.0, 1.0), float4(1.0, 0.0, 0.0, 1.0), float4(1.0, 1.0, 0.0, 1.0),
    float4(0.0, 1.0, 0.0, 1.0), float4(0.0, 1.0, 1.0, 1.0), float4(0.0, 0.0, 1.0, 1.0),
    float4(1.0, 0.0, 1.0, 1.0),
};

[shader("mesh")]
[numthreads(8, 1, 1)]
[outputtopology("triangle")]
void ms_main(uint tid : SV_GroupThreadID, in payload Payload pl, uniform DrawData pc,
             out vertices V verts[7], out indices uint3 tris[6])
{
    SetMeshOutputCounts(7, 6);
    if (tid < 7u)
    {
        V v;
        v.pos = float4(k_dir[tid] * pc.scale, 0.5, 1.0);
        v.color = k_color[tid];
        verts[tid] = v;
    }
    if (tid < 6u)
        tris[tid] = uint3(0u, tid + 1u, tid == 5u ? 1u : tid + 2u);
}

[shader("fragment")]
float4 ps_main(V v, uniform DrawData pc) : SV_Target
{
    return saturate(v.color + pc.bias);
}
