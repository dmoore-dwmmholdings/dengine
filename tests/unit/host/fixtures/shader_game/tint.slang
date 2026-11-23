// DENG-789 fixture: the game's own shader, a vertex/pixel pair that uses the engine's embedded
// common modules (bindless, push constants), as a game post pass does.
import bindless;
import push_constants;

struct TintData
{
    float4 tint;
    uint positions;
};

[shader("vertex")]
float4 vs_main(uint vid : SV_VertexID, uniform TintData pc) : SV_Position
{
    return de_buffer<float4>(pc.positions)[vid];
}

[shader("fragment")]
float4 ps_main(uniform TintData pc) : SV_Target
{
    return pc.tint;
}
