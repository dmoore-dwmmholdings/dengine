// Golden scene rhi_compute_gradient (DENG-329): one thread per pixel writes an RGBA8 texel, packed
// in a uint (r in the low byte), into a bindless buffer that the scene then copies into the
// target. Integer math only, so every backend writes the same bytes.
import push_constants;

struct Params
{
    uint dst;    // bindless index of the output buffer (uint elements, row-major)
    uint width;
    uint height;
    uint cell;   // size of the blue checker cells, pixels
};

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID, uniform Params pc)
{
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    uint r = id.x * 255u / (pc.width - 1u);
    uint g = id.y * 255u / (pc.height - 1u);
    uint b = (((id.x / pc.cell) + (id.y / pc.cell)) & 1u) != 0u ? 200u : 40u;
    de_buffer<uint>(pc.dst)[id.y * pc.width + id.x] = r | (g << 8u) | (b << 16u) | (255u << 24u);
}
