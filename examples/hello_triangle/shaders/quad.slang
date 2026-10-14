// hello_triangle, scene 2 (DENG-327): a textured quad. The vertex shader makes the two triangles
// from SV_VertexID; the pixel shader samples a texture through bindless texture and sampler
// indices from the push constants.
import bindless;
import push_constants;

struct QuadData
{
    uint tex;    // bindless index of the sampled texture (de_tex2d)
    uint smp;    // bindless index of the sampler (de_sampler)
    float scale; // half the quad's size in clip space (1 = the whole target)
};

struct QuadVertex
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// Two counter-clockwise triangles (y up). uv (0, 0) is the texture's first row, at the top.
static const float2 k_uv[6] = {
    float2(0, 0), float2(0, 1), float2(1, 1),
    float2(0, 0), float2(1, 1), float2(1, 0),
};

[shader("vertex")]
QuadVertex vs_main(uint vid : SV_VertexID, uniform QuadData pc)
{
    float2 uv = k_uv[vid];
    QuadVertex o;
    o.pos = float4((uv.x * 2.0 - 1.0) * pc.scale, (1.0 - uv.y * 2.0) * pc.scale, 0.5, 1.0);
    o.uv = uv;
    return o;
}

[shader("fragment")]
float4 ps_main(QuadVertex v, uniform QuadData pc) : SV_Target
{
    return de_tex2d(pc.tex).Sample(de_sampler(pc.smp), v.uv);
}
