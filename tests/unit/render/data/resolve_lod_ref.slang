// tests/unit/render/data/resolve_lod_ref.slang: the forward-rendered reference of
// test_resolve.c's texture-LOD test (DENG-238). It draws triangles whose clip positions and UVs
// the test computed and samples a texture with the hardware's implicit derivatives; the resolve
// samples the same texture with its analytic ones (SampleGrad). Cooked at test time.
import bindless;
import push_constants;

struct RefParams
{
    uint verts;   // float4 pairs per vertex: clip position, then uv (xy)
    uint tex;     // sampled texture
    uint sampler;
    uint reserved;
};

struct RefVertex
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
RefVertex vs_main(uint vid : SV_VertexID, uniform RefParams pc)
{
    RefVertex o;
    o.pos = de_robuffer<float4>(pc.verts)[vid * 2u];
    o.uv = de_robuffer<float4>(pc.verts)[vid * 2u + 1u].xy;
    return o;
}

[shader("fragment")]
float4 ps_main(RefVertex v, uniform RefParams pc) : SV_Target
{
    return de_tex2d(pc.tex).Sample(de_sampler(pc.sampler), v.uv);
}
