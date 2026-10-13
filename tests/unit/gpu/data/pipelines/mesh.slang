// Shared pipeline test shader (DENG-323): amplification + mesh + pixel. The amplification shader
// launches one mesh group, which emits one triangle from the same position buffer and push
// constants as triangle.slang, so the mesh test expects the triangle test's pixels.
import push_constants;

struct DrawData
{
    float4 color;
    uint positions;
    uint first;
};

struct Payload
{
    uint first;
};

groupshared Payload g_payload;

[shader("amplification")]
[numthreads(1, 1, 1)]
void as_main(uniform DrawData pc)
{
    g_payload.first = pc.first;
    DispatchMesh(1, 1, 1, g_payload);
}

struct V
{
    float4 pos : SV_Position;
};

[shader("mesh")]
[numthreads(3, 1, 1)]
[outputtopology("triangle")]
void ms_main(uint tid : SV_GroupThreadID, in payload Payload pl, uniform DrawData pc,
             out vertices V verts[3], out indices uint3 tris[1])
{
    SetMeshOutputCounts(3, 1);
    V v;
    v.pos = de_buffer<float4>(pc.positions)[pl.first + tid];
    verts[tid] = v;
    if (tid == 0)
        tris[0] = uint3(0, 1, 2);
}

[shader("fragment")]
float4 ps_main(uniform DrawData pc) : SV_Target
{
    return pc.color;
}
