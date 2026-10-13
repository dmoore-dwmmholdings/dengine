// 132 bytes of push constants: must fail to cook (DENG-289, limit 128).
import push_constants;

struct Big
{
    float4 a[8];
    uint extra;
};

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID, uniform Big pc)
{
    de_buffer<float4>(pc.extra)[id.x] = pc.a[id.x & 7];
}
