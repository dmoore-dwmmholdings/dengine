// DENG-163. All values are linear Rec.709; output is relative to paper white.
public struct TonemapParams { public float4 data0; public float4 data1; };
public struct OutputInfo { public uint hdr; public float paper_white_nits; public float peak_nits; };
public interface ITonemapper {
    float3 apply(float3 scene_linear, TonemapParams p, OutputInfo o);
}
public float de_tonemap_peak(OutputInfo o) {
    return o.hdr != 0 ? max(1.0, o.peak_nits / max(1.0, o.paper_white_nits)) : 1.0;
}
