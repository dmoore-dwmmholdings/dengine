// shaders/materials/unlit/unlit.slang: the Unlit material model, id 1 (KHR_materials_unlit; SPEC
// §12.6; DENG-301). The engine shader vis_resolve instantiates de_resolve_tiles<UnlitModel>
// (cs_resolve_unlit); deferred lighting never dispatches it (DENG-303 skips the model and
// DE_GB_FLAG_UNLIT), so what the resolve writes to the lighting target is the final colour: the
// emission, times the view's pre-exposure (DENG-304).
//
//   resolve_gbuffer  emission = base colour x factor x vertex colour x texture, exactly; the
//                    UNLIT feature (G-buffer DE_GB_FLAG_UNLIT, GB1 left cleared). KHR unlit has no
//                    emissive term, so the material's emissive is not added.
//   evaluate etc.    no BSDF: 0, and sample returns no direction
module unlit;

import bindless;
__exported import "../material_interface";
import "../../scene/scene_types";
import "../../visbuffer/resolve_model";

public struct UnlitModel : IMaterialModel
{
    public typealias Params = DeGpuMaterial;

    public static Params load_params(DeGpuMaterial material)
    {
        return material;
    }

    public static SurfaceData resolve_gbuffer(Params m, VertexAttribs v, TextureLodInfo lod)
    {
        float4 base = m.base_color * v.color *
                      de_resolve_sample(m, DE_RESOLVE_TEX_BASE_COLOR, v, lod,
                                        float4(1.0, 1.0, 1.0, 1.0));
        SurfaceData s = de_surface_default();
        s.base_color = base.rgb;
        s.ao = 1.0;
        s.normal = v.normal;
        s.clearcoat_normal = v.normal;
        s.roughness = 0.0;
        s.metallic = 0.0;
        s.specular = 0.0;
        s.emissive = base.rgb;
        s.model = m.model & (DE_GPU_MATERIAL_MODELS - 1u);
        s.features = DE_MATERIAL_FEATURE_UNLIT;
        return s;
    }

    public static float3 evaluate(SurfaceData s, float3 wo, float3 wi)
    {
        return float3(0.0, 0.0, 0.0);
    }

    public static float pdf(SurfaceData s, float3 wo, float3 wi)
    {
        return 0.0;
    }

    public static BsdfSample sample(SurfaceData s, float3 wo, float3 u)
    {
        BsdfSample r;
        r.wi = float3(0.0, 0.0, 1.0);
        r.weight = float3(0.0, 0.0, 0.0);
        r.pdf = 0.0;
        r.lobe = 0u;
        return r;
    }

    public static uint lobes(SurfaceData s)
    {
        return 0u;
    }
};
