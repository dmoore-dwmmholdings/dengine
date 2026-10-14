// DENG-409 fixture: a vertex/pixel pair (bindless buffer, push constants).
import bindless;
import push_constants;

struct TriangleData
{
    float4 color;
    uint positions;
};

[shader("vertex")]
float4 vs_main(uint vid : SV_VertexID, uniform TriangleData pc) : SV_Position
{
    return de_buffer<float4>(pc.positions)[vid];
}

[shader("fragment")]
float4 ps_main(uniform TriangleData pc) : SV_Target
{
    return pc.color;
}
