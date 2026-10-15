// tests/unit/render/data/material_model_bad.slang: a type that claims IMaterialModel but lacks
// evaluate / sample / pdf / lobes, so the shader's cook fails with Slang's conformance error and
// de_material_model_register refuses it (DENG-298).
import "visbuffer/resolve_model";

struct BadModel : IMaterialModel
{
    typealias Params = DeGpuMaterial;

    static Params load_params(DeGpuMaterial material)
    {
        return material;
    }

    static SurfaceData resolve_gbuffer(Params m, VertexAttribs v, TextureLodInfo lod)
    {
        SurfaceData s = {};
        return s;
    }
};

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_resolve_model(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
                      uniform ResolveParams pc)
{
    de_resolve_tiles<BadModel>(group, thread.x, pc);
}
