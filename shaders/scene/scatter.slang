// shaders/scene/scatter.slang: the GPU scene's scatter upload (SPEC §12.4; DENG-177).
//
// Short runs of changed table entries are not copied one by one: the gpu_scene.upload pass copies
// their payloads and their destination slots from the upload ring into GPU_ONLY scratch buffers,
// and the gpu_scene.scatter pass runs one of these kernels per table: thread i writes payload
// entry src_first + i to slot indices[index_first + i] of the table. Long runs go straight from
// the ring with de_gpu_cmd_copy_buffer (src/render/scene/gpu_scene.c chooses by run length).
//
// One entry point per table type, because D3D12 views a structured buffer with its own stride
// (the buffer's de_gpu_buffer_desc.stride): the scratch buffer of a table has the table's stride,
// the slot list a stride of 4. The pass declares the scratch buffers DE_ACCESS_SHADER_READ, so
// they are read through de_robuffer (DENG-410), and the tables DE_ACCESS_SHADER_WRITE (de_buffer).
// The slots of one dispatch are distinct, so threads never race.
import push_constants;
import scene_types;

struct ScatterParams
{
    uint src;         // bindless index: the payload scratch (T elements)
    uint indices;     // bindless index: the destination slots (uint elements)
    uint dst;         // bindless index: the table (T elements)
    uint src_first;   // first payload element of this dispatch
    uint index_first; // first slot element of this dispatch
    uint count;       // entries
};

void scatter<T>(uint i, ScatterParams pc)
{
    if (i >= pc.count)
        return;
    uint slot = de_robuffer<uint>(pc.indices)[pc.index_first + i];
    de_buffer<T>(pc.dst)[slot] = de_robuffer<T>(pc.src)[pc.src_first + i];
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_scatter_instances(uint3 id : SV_DispatchThreadID, uniform ScatterParams pc)
{
    scatter<DeGpuInstance>(id.x, pc);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_scatter_lights(uint3 id : SV_DispatchThreadID, uniform ScatterParams pc)
{
    scatter<DeGpuLight>(id.x, pc);
}
