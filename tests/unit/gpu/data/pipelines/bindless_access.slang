// Shared pipeline test shader (DENG-410): the four buffer accessors of bindless.slang on one
// dispatch. 24 bytes of push constants, 64x1x1 threadgroups; the values are in pipeline_shaders.h.
//   - de_robuffer<uint>: an UPLOAD buffer and a GPU_ONLY one the pass reads (stride 4);
//   - de_buffer<uint>:   the destination (GPU_ONLY, stride 4);
//   - de_bytes:          an UPLOAD buffer (stride 0), with load, load2, load3 and load4;
//   - de_rwbytes:        a GPU_ONLY buffer (stride 0) read and written in place, with every load
//                        and store width.
import push_constants;

struct Params
{
    uint ro_upload;  // UPLOAD, stride 4
    uint ro_gpu;     // GPU_ONLY, stride 4, DE_ACCESS_SHADER_READ
    uint raw_upload; // UPLOAD, stride 0, `count` words
    uint raw_rw;     // GPU_ONLY, stride 0, 2 * `count` words, DE_ACCESS_SHADER_WRITE
    uint dst;        // GPU_ONLY, stride 4, `count` words
    uint count;      // a multiple of 4
};

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID, uniform Params pc)
{
    uint i = id.x;
    if (i >= pc.count)
        return;
    // dst[i] = ro_upload[i] * 3 + ro_gpu[i]
    de_buffer<uint>(pc.dst)[i] =
        de_robuffer<uint>(pc.ro_upload)[i] * 3u + de_robuffer<uint>(pc.ro_gpu)[i];

    // First half of raw_rw, one word per thread: rw[i] += src[i].
    DeBytes src = de_bytes(pc.raw_upload);
    DeRWBytes rw = de_rwbytes(pc.raw_rw);
    rw.store(4u * i, rw.load(4u * i) + src.load(4u * i));

    // Second half, four words per thread for the first count / 4 threads, every width:
    // rw[count + w] += 2 * src[w].
    if (i >= pc.count / 4u)
        return;
    uint s = 16u * i;
    uint d = 4u * pc.count + s;
    switch (i % 3u)
    {
    case 0:
        rw.store4(d, rw.load4(d) + src.load4(s) * 2u);
        break;
    case 1:
        rw.store2(d, rw.load2(d) + src.load2(s) * 2u);
        rw.store2(d + 8u, rw.load2(d + 8u) + src.load2(s + 8u) * 2u);
        break;
    default:
        rw.store3(d, rw.load3(d) + src.load3(s) * 2u);
        rw.store(d + 12u, rw.load(d + 12u) + src.load(s + 12u) * 2u);
        break;
    }
}
