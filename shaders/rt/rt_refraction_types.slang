module rt_refraction_types;
// Captured by the actual forward rasterizer, including material derivatives and world thickness.
public struct DeRefractionCapture {
    public float4 position_thickness;
    public float4 view_ior;
    public float4 normal_roughness;
    public float4 geometry_curvature;
    public float4 sampled;
};
