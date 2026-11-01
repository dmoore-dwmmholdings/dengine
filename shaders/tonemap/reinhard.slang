import "itonemapper";
public struct ReinhardTonemapper : ITonemapper {
    public float3 apply(float3 c, TonemapParams p, OutputInfo o) {
        float peak = de_tonemap_peak(o);
        float white = p.data0.x > 0.0 ? p.data0.x : 4.0;
        float luminance = dot(c, float3(0.2126, 0.7152, 0.0722));
        if (luminance <= 0.0) return float3(0.0);
        float mapped = luminance * (1.0 + luminance / (white * white)) / (1.0 + luminance);
        return clamp(c * (mapped / luminance), 0.0, peak);
    }
}
