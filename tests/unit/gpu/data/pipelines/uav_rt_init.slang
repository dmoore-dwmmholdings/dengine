// DENG-618: nonuniform compute image, then a scissored opaque UI-like draw with LOAD.
import push_constants;

struct InitParams
{
    uint output;
    uint width;
    uint height;
    uint phase;
};

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID, uniform InitParams pc)
{
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    // The second use preserves the right half: initialization must not discard it.
    if (pc.phase != 0u && id.x >= pc.width / 2u)
        return;
    uint r = (id.x * 3u + pc.phase * 41u) & 255u;
    uint g = (id.y * 5u + 17u) & 255u;
    de_rwtex2d(pc.output)[id.xy] = float4(float(r) / 255.0, float(g) / 255.0, 91.0 / 255.0, 1.0);
}

[shader("vertex")]
float4 vs_main(uint vid : SV_VertexID) : SV_Position
{
    float2 p = float2((vid << 1u) & 2u, vid & 2u);
    return float4(p * 2.0 - 1.0, 0.5, 1.0);
}

[shader("fragment")]
float4 ps_main() : SV_Target
{
    return float4(17.0 / 255.0, 211.0 / 255.0, 59.0 / 255.0, 1.0);
}
