// Shared pipeline test shader (DENG-323): 128 bytes of push constants, the SPEC §12.1 limit,
// copied word by word into a buffer. The destination's bindless index is the last word (v[7].w),
// so thread i writes word i of the block and the readback equals the pushed bytes.
import push_constants;

struct Block
{
    uint4 v[8]; // 128 bytes; v[7].w is the destination buffer's bindless index
};

[shader("compute")]
[numthreads(32, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID, uniform Block pc)
{
    uint i = id.x;
    de_buffer<uint>(pc.v[7].w)[i] = pc.v[i / 4u][i % 4u];
}
