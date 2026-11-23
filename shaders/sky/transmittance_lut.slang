// shaders/sky/transmittance_lut.slang: the transmittance LUT (SPEC §12.13; DENG-206). Engine
// shader `sky_transmittance`. One thread per texel of the 256 x 64 LUT; it writes a float4
// buffer, which src/world/sky/sky_luts.c copies into the RGBA32F texture. The CPU reference
// (atmosphere.c, de__sky_transmittance_cpu) integrates the same optical depth in double.
import bindless;
import push_constants;
import "sky_common";

struct TransmittanceParams
{
    uint target;     // float4 buffer, row-major W x H
    float mie_scale; // turbidity - 1
    uint steps;      // midpoint steps along the ray
    uint reserved;
};

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_transmittance(uint3 id : SV_DispatchThreadID, uniform TransmittanceParams pc)
{
    if (id.x >= SKY_TRANSMITTANCE_W || id.y >= SKY_TRANSMITTANCE_H)
        return;
    float2 uv = (float2(id.xy) + 0.5) / float2(SKY_TRANSMITTANCE_W, SKY_TRANSMITTANCE_H);
    float r, mu;
    sky_transmittance_rmu(uv, r, mu);
    float len = sky_dist_to_top(r, mu);
    float dt = len / float(pc.steps);
    float3 depth = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < pc.steps; ++i)
    {
        float t = (float(i) + 0.5) * dt;
        float h = sqrt(r * r + t * t + 2.0 * r * mu * t) - SKY_BOTTOM;
        depth += sky_medium(h, pc.mie_scale).extinction;
    }
    float3 tr = exp(-depth * dt);
    de_buffer<float4>(pc.target)[id.y * SKY_TRANSMITTANCE_W + id.x] = float4(tr, 1.0);
}
