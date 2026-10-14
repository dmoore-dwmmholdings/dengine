// Golden scenes rhi_triangle, rhi_indirect_count and rhi_selftest_perturbed (DENG-329): vertices
// pulled from a bindless buffer of float4 pairs (clip-space position, color), interpolated color
// plus a bias from the push constants (the self test perturbs the color with it).
import push_constants;

struct DrawData
{
    float4 bias;  // added to the interpolated color (0 normally)
    uint verts;   // bindless index of the vertex buffer: float4 position, float4 color per vertex
    uint first;   // first vertex this draw reads (SV_VertexID does not include first_vertex)
};

struct VOut
{
    float4 pos : SV_Position;
    float4 color : COLOR0;
};

[shader("vertex")]
VOut vs_main(uint vid : SV_VertexID, uniform DrawData pc)
{
    uint v = pc.first + vid;
    VOut o;
    o.pos = de_buffer<float4>(pc.verts)[2u * v];
    o.color = de_buffer<float4>(pc.verts)[2u * v + 1u];
    return o;
}

[shader("fragment")]
float4 ps_main(VOut v, uniform DrawData pc) : SV_Target
{
    return saturate(v.color + pc.bias);
}
