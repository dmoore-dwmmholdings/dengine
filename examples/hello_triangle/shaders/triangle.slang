// hello_triangle, scene 1 (DENG-327): one triangle. The vertex shader pulls clip-space positions
// from a bindless buffer (there are no vertex buffers, de_gpu.h); the pixel shader outputs the
// colour from the push constants.
import bindless;
import push_constants;

struct TriangleData
{
    float4 color;   // the pixel colour
    uint positions; // bindless index of a buffer of float4 clip-space positions
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
