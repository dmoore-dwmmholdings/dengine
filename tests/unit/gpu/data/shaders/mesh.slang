// Test mesh + pixel shader (DENG-289, R2: Slang's Metal mesh output): 64 vertices and 124
// triangles at most. Metal needs each mesh output written as a whole struct.
import push_constants;

struct DrawData
{
    float scale;
};

struct V
{
    float4 pos : SV_Position;
    float3 color : COLOR0;
};

[shader("mesh")]
[numthreads(32, 1, 1)]
[outputtopology("triangle")]
void ms_main(uint tid : SV_GroupThreadID, uniform DrawData pc, out vertices V verts[64],
             out indices uint3 tris[124])
{
    SetMeshOutputCounts(3, 1);
    if (tid < 3)
    {
        V v;
        v.pos = float4(float(tid) * pc.scale, 0.0, 0.5, 1.0);
        v.color = float3(1.0, 0.0, 0.0);
        verts[tid] = v;
    }
    if (tid == 0)
        tris[0] = uint3(0, 1, 2);
}

[shader("fragment")]
float4 mesh_ps(V v) : SV_Target
{
    return float4(v.color, 1.0);
}
