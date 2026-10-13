// Shared pipeline test shader (DENG-323): writes `draws` indexed indirect draw records (5 u32 each:
// index_count 3, instance_count 1, first_index 3*i, vertex_offset 0, first_instance 0) and the draw
// count `count`, so the indirect-count test checks that only `count` draws run.
import push_constants;

struct Params
{
    uint args;      // bindless index of the argument buffer (uint elements)
    uint count_buf; // bindless index of the count buffer (one uint)
    uint draws;     // records to write
    uint count;     // the draw count to write
};

[shader("compute")]
[numthreads(4, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID, uniform Params pc)
{
    if (id.x < pc.draws)
    {
        uint base = id.x * 5u;
        de_buffer<uint>(pc.args)[base + 0u] = 3u;
        de_buffer<uint>(pc.args)[base + 1u] = 1u;
        de_buffer<uint>(pc.args)[base + 2u] = id.x * 3u;
        de_buffer<uint>(pc.args)[base + 3u] = 0u;
        de_buffer<uint>(pc.args)[base + 4u] = 0u;
    }
    if (id.x == 0u)
        de_buffer<uint>(pc.count_buf)[0] = pc.count;
}
