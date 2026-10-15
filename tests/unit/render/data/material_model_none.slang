// tests/unit/render/data/material_model_none.slang: a shader that is not a material model (no
// cs_resolve_model), which de_material_model_register must refuse (DENG-298).
import bindless;

struct Push
{
    uint out_buf;
};

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID, uniform Push pc)
{
    de_buffer<float>(pc.out_buf)[id.x] = 1.0;
}
