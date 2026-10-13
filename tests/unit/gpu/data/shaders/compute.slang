// Test compute shader (DENG-289): bindless texture, sampler and buffer, 16 bytes of push
// constants, an 8x4x1 threadgroup, a permutation define and an import of a local module.
import push_constants;
import color;
import tint;

struct DrawData
{
    uint tex;
    uint smp;
    uint out_buf;
    float scale;
};

[shader("compute")]
[numthreads(8, 4, 1)]
void cs_main(uint3 id : SV_DispatchThreadID, uniform DrawData pc)
{
    float4 c = de_tex2d(pc.tex).SampleLevel(de_sampler(pc.smp), float2(id.xy) / 64.0, 0);
#ifdef USE_TINT
    c = de_tint(c);
#endif
    de_buffer<float4>(pc.out_buf)[id.y * 64 + id.x] = de_linear_to_srgb(c * pc.scale);
}
