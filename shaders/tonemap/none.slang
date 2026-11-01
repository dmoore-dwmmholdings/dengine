import "itonemapper";
public struct NoneTonemapper : ITonemapper {
    public float3 apply(float3 c, TonemapParams p, OutputInfo o) {
        return o.hdr != 0 ? max(c, 0.0) : saturate(c);
    }
}
