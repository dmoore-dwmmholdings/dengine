// Shared pipeline test shader (DENG-323): dst[i] = src[i] * 3 + seed for i < count, both buffers
// reached through bindless indices. 16 bytes of push constants, 64x1x1 threadgroups.
import push_constants;

struct Params
{
    uint src;   // bindless index of the input buffer (uint elements)
    uint dst;   // bindless index of the output buffer (uint elements)
    uint count; // elements to write
    uint seed;
};

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID, uniform Params pc)
{
    if (id.x >= pc.count)
        return;
    de_buffer<uint>(pc.dst)[id.x] = de_buffer<uint>(pc.src)[id.x] * 3u + pc.seed;
}
