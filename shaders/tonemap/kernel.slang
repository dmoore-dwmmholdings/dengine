import bindless;
import push_constants;
import "itonemapper";
// Stable custom kernel ABI: 80-byte push block, cs_tonemap, numthreads(8,8,1).
public struct TonemapPassParams {
    uint src, dst, width, height;
    float paper_white_nits, peak_nits;
    uint hdr, reserved;
    float4 data0, data1;
    float exposure_multiplier;
    uint padding0, padding1, padding2;
};
public void de_tonemap_pixel<T : ITonemapper>(uint3 id, TonemapPassParams pc) {
    if (id.x >= pc.width || id.y >= pc.height) return;
    float3 c = de_tex2d(pc.src).Load(int3(id.xy,0)).rgb;
    c = float3(isfinite(c.r) ? max(c.r,0.0) : 0.0,
               isfinite(c.g) ? max(c.g,0.0) : 0.0,
               isfinite(c.b) ? max(c.b,0.0) : 0.0);
    TonemapParams p; p.data0 = pc.data0; p.data1 = pc.data1;
    OutputInfo o; o.hdr = pc.hdr; o.paper_white_nits = pc.paper_white_nits;
    o.peak_nits = pc.peak_nits;
    T mapper;
    c = mapper.apply(c * pc.exposure_multiplier, p, o);
    c = float3(isfinite(c.r) ? max(c.r,0.0) : 0.0,
               isfinite(c.g) ? max(c.g,0.0) : 0.0,
               isfinite(c.b) ? max(c.b,0.0) : 0.0);
    de_rwtex2d(pc.dst)[id.xy] = float4(min(c, 65504.0),1.0);
}
