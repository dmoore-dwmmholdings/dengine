// tests/unit/render/data/material_model_flat.slang: a game material model for the registry and
// resolve tests (DENG-298). FlatModel implements IMaterialModel: every point gets the material's
// base colour times (0.25, 0.5, 1), roughness 0.75, a fixed emission, and a Lambert BSDF. The
// tests mount the engine's shaders/ directory on the same root, so the engine modules import as
// "visbuffer/resolve_model".
import "visbuffer/resolve_model";
import "brdf/sampling";

struct FlatModel : IMaterialModel
{
    typealias Params = DeGpuMaterial;

    static Params load_params(DeGpuMaterial material)
    {
        return material;
    }

    static SurfaceData resolve_gbuffer(Params m, VertexAttribs v, TextureLodInfo lod)
    {
        SurfaceData s = de_surface_default();
        s.base_color = m.base_color.rgb * float3(0.25, 0.5, 1.0);
        s.ao = 1.0;
        s.normal = v.normal;
        s.roughness = 0.75;
        s.tangent = float4(0.0, 0.0, 0.0, 1.0);
        s.metallic = 0.0;
        s.specular = 0.5;
        s.emissive = float3(0.125, 0.25, 0.5);
        s.model = 0u;
        s.features = 0u;
        return s;
    }

    static float3 evaluate(SurfaceData s, float3 wo, float3 wi)
    {
        return wo.z > 0.0 && wi.z > 0.0 ? s.base_color * (DE_INV_PI * wi.z) : float3(0.0, 0.0, 0.0);
    }

    static float pdf(SurfaceData s, float3 wo, float3 wi)
    {
        return wo.z > 0.0 ? de_cosine_hemisphere_pdf(wi.z) : 0.0;
    }

    static BsdfSample sample(SurfaceData s, float3 wo, float3 u)
    {
        BsdfSample r;
        r.wi = de_sample_cosine_hemisphere(u.xy);
        r.pdf = pdf(s, wo, r.wi);
        r.weight = r.pdf > 0.0 ? evaluate(s, wo, r.wi) / r.pdf : float3(0.0, 0.0, 0.0);
        r.lobe = DE_BSDF_LOBE_DIFFUSE;
        return r;
    }

    static uint lobes(SurfaceData s)
    {
        return DE_BSDF_LOBE_DIFFUSE;
    }
};

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_resolve_model(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
                      uniform ResolveParams pc)
{
    de_resolve_tiles<FlatModel>(group, thread.x, pc);
}
