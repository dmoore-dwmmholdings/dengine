// Sprite shares the Standard BSDF so deferred, forward and ray shading agree.
module sprite;
import "standard/standard";
public struct SpriteModel : IMaterialSplitModel
{
    public typealias Params = DeGpuMaterial;
    public static Params load_params(DeGpuMaterial m) { return m; }
    public static SurfaceData resolve_gbuffer(Params m, VertexAttribs v, TextureLodInfo l)
    { return StandardModel.resolve_gbuffer(m,v,l); }
    public static float3 evaluate(SurfaceData s,float3 wo,float3 wi)
    { return StandardModel.evaluate(s,wo,wi); }
    public static BsdfSample sample(SurfaceData s,float3 wo,float3 u)
    { return StandardModel.sample(s,wo,u); }
    public static float pdf(SurfaceData s,float3 wo,float3 wi)
    { return StandardModel.pdf(s,wo,wi); }
    public static uint lobes(SurfaceData s) { return StandardModel.lobes(s); }
    public static float3 evaluate_split(SurfaceData s,float3 wo,float3 wi,out float3 d)
    { return StandardModel.evaluate_split(s,wo,wi,d); }
    public static void layers_from_material(inout SurfaceData s,DeGpuMaterial m)
    { StandardModel.layers_from_material(s,m); }
};
