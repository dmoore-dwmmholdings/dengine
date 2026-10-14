// Golden scene rhi_textured_quad (DENG-329): a rotated quad that samples a texture through
// bindless texture and sampler indices. The vertex shader builds the two triangles from
// SV_VertexID; uv runs 0..2, so a REPEAT sampler tiles the texture twice each way.
import bindless;
import push_constants;

struct QuadData
{
    float2 rot;  // cos, sin of the rotation
    float scale; // half the quad's size in clip space
    uint tex;    // bindless index of the sampled texture
    uint smp;    // bindless index of the sampler
};

struct QuadVertex
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// Two counter-clockwise triangles (y up). uv (0, 0) is the top-left corner.
static const float2 k_uv[6] = {
    float2(0, 0), float2(0, 1), float2(1, 1),
    float2(0, 0), float2(1, 1), float2(1, 0),
};

[shader("vertex")]
QuadVertex vs_main(uint vid : SV_VertexID, uniform QuadData pc)
{
    float2 uv = k_uv[vid];
    float2 p = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) * pc.scale;
    QuadVertex o;
    o.pos = float4(p.x * pc.rot.x - p.y * pc.rot.y, p.x * pc.rot.y + p.y * pc.rot.x, 0.5, 1.0);
    o.uv = uv * 2.0;
    return o;
}

[shader("fragment")]
float4 ps_main(QuadVertex v, uniform QuadData pc) : SV_Target
{
    return de_tex2d(pc.tex).Sample(de_sampler(pc.smp), v.uv);
}
