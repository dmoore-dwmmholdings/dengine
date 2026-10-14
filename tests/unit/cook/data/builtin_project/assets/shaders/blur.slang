// DENG-409 fixture: a compute shader with a local import (an extra source of the cook) and a
// permutation define.
import bindless;
import push_constants;
import weights;

struct BlurData
{
    uint src;
    uint dst;
    uint width;
};

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID, uniform BlurData pc)
{
    float sum = 0.0;
#ifdef WIDE
    const int radius = 4;
#else
    const int radius = 2;
#endif
    for (int i = -radius; i <= radius; ++i)
        sum += de_buffer<float>(pc.src)[clamp(int(id.x) + i, 0, int(pc.width) - 1)] *
               blur_weight(i, radius);
    de_buffer<float>(pc.dst)[id.x] = sum;
}
