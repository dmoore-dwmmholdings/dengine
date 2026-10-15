// tests/unit/render/data/standard_layers_flags.slang: test_standard_layers.c's compile-out check
// (DENG-300). Three kernels evaluate a surface: through the Standard model with no layer compiled
// in (StandardModelT<0>), through the base lobe alone, and through the model with every layer.
// The first two must compile to code of about the same size, and the third to much more.
import bindless;
import push_constants;
import "materials/material_interface";
import "materials/standard/standard_base";
import "materials/standard/standard";

struct FlagParams
{
    uint out_;
    uint lut;
    uint features;
    uint reserved;
};

SurfaceData surface(FlagParams pc, uint i)
{
    SurfaceData s = de_surface_default();
    s.roughness = float(i % 7u) / 7.0;
    s.brdf_lut = pc.lut;
    s.features = pc.features;
    s.clearcoat = 0.5;
    s.sheen_color = float3(0.5, 0.5, 0.5);
    s.anisotropy = 0.5;
    s.iridescence = 0.5;
    s.iridescence_thickness = 400.0;
    return s;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_none(uint3 id : SV_DispatchThreadID, uniform FlagParams pc)
{
    SurfaceData s = surface(pc, id.x);
    float3 wo = float3(0.6, 0.0, 0.8), wi = float3(-0.3, 0.4, 0.866);
    de_buffer<float4>(pc.out_)[id.x] = float4(StandardModelT<0>.evaluate(s, wo, wi), 1.0);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_base(uint3 id : SV_DispatchThreadID, uniform FlagParams pc)
{
    SurfaceData s = surface(pc, id.x);
    float3 wo = float3(0.6, 0.0, 0.8), wi = float3(-0.3, 0.4, 0.866);
    StandardBase b = de_standard_base(s, wo, true);
    de_buffer<float4>(pc.out_)[id.x] = float4(de_standard_base_evaluate(b, s, wo, wi), 1.0);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_all(uint3 id : SV_DispatchThreadID, uniform FlagParams pc)
{
    SurfaceData s = surface(pc, id.x);
    float3 wo = float3(0.6, 0.0, 0.8), wi = float3(-0.3, 0.4, 0.866);
    de_buffer<float4>(pc.out_)[id.x] = float4(StandardModel.evaluate(s, wo, wi), 1.0);
}
