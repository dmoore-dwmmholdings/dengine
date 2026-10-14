// shaders/common/composite.slang: the view composite (DENG-176, SPEC §12.4, §12.9).
//
// Draws one sampled texture (a view's output) into the current viewport, scaled with bilinear
// filtering (the sampler's). The renderer sets the viewport to the view's rectangle in the
// destination and draws three vertices: one triangle that covers the viewport, clipped to it.
// uv (0, 0) is the top-left texel, like the destination's viewport (y down).
//
// The renderer cooks this file itself (src/render/passes/composite.c); it is not an importable
// module of the shader importer.
import bindless;
import push_constants;

struct CompositeData
{
    uint src; // bindless index of the sampled source texture
    uint smp; // bindless index of the sampler (linear, clamp)
};

struct CompositeVertex
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
CompositeVertex vs_main(uint vid : SV_VertexID, uniform CompositeData pc)
{
    // Vertices (0, 0), (2, 0), (0, 2) in uv: the triangle covers the unit square.
    float2 uv = float2(float((vid << 1) & 2u), float(vid & 2u));
    CompositeVertex o;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    o.uv = uv;
    return o;
}

[shader("fragment")]
float4 ps_main(CompositeVertex v, uniform CompositeData pc) : SV_Target
{
    return de_tex2d(pc.src).Sample(de_sampler(pc.smp), v.uv);
}
