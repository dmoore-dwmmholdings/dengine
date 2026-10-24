// Shared pipeline test shader (DENG-323; DENG-324 and DENG-325 use it as is): a vertex/pixel pair
// that pulls clip-space positions from a bindless buffer and outputs the push-constant color.
// Push constants: 24 bytes. The vertex index is `first + SV_VertexID`: SV_VertexID does not
// portably include a draw's first_vertex or vertex_offset (de_gpu.h), so draws pass 0 for those
// and put the offset here, or select vertices through index values.
import push_constants;

struct DrawData
{
    float4 color;   // the pixel color
    uint positions; // bindless index of a buffer of float4 clip-space positions
    uint first;     // first position this draw reads
};

[shader("vertex")]
float4 vs_main(uint vid : SV_VertexID, uniform DrawData pc) : SV_Position
{
    return de_buffer<float4>(pc.positions)[pc.first + vid];
}

[shader("fragment")]
float4 ps_main(uniform DrawData pc) : SV_Target
{
    return pc.color;
}

// DENG-617: a present fragment stage that legitimately has no push block.
[shader("fragment")]
float4 ps_solid() : SV_Target
{
    return float4(0.25, 0.5, 0.75, 1.0);
}
