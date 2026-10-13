// Test vertex/pixel pair (DENG-289): vertex pulling from a bindless buffer, a bindless texture in
// the pixel shader, one shared 32-byte push-constant block.
import push_constants;
import math;

struct DrawData
{
    float4 tint;
    uint positions;
    uint tex;
    uint smp;
    uint pad;
};

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VsOut vs_main(uint vid : SV_VertexID, uniform DrawData pc)
{
    float4 p = de_buffer<float4>(pc.positions)[vid];
    VsOut o;
    o.pos = float4(p.xy, de_reverse_z_depth(p.z, 0.1), 1.0);
    o.uv = de_clip_to_uv(p.xy);
    return o;
}

[shader("fragment")]
float4 ps_main(VsOut i, uniform DrawData pc) : SV_Target
{
    return de_tex2d(pc.tex).Sample(de_sampler(pc.smp), i.uv) * pc.tint;
}
