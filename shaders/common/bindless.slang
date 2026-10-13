// shaders/common/bindless.slang: the engine's bindless resource convention (SPEC §12.1, DENG-289).
//
// Shaders reach every texture, sampler and buffer through a 32-bit index into one global table
// per kind (the indices de_gpu hands out: de_gpu_texture_bindless_index and friends). Index 0 is
// reserved in every table (DE_GPU_BINDLESS_INVALID): the backends put a null descriptor there.
//
//     import bindless;
//     float4 c = de_tex2d(pc.albedo).Sample(de_sampler(pc.smp), uv);
//     de_buffer<float4>(pc.out)[i] = c;
//
// The shader importer (tools/cook/importers/shader/) embeds this file and compiles each target in
// its own Slang session with one of DE_SHADER_TARGET_SPIRV, DE_SHADER_TARGET_DXIL or
// DE_SHADER_TARGET_METAL defined to 1. What each backend binds:
//
//   D3D12 (DXIL, SM 6.6)  ResourceDescriptorHeap (textures, storage textures, buffers) and
//                         SamplerDescriptorHeap. No root-signature tables for them.
//   Vulkan (SPIR-V)       descriptor set 0, runtime arrays (descriptor indexing):
//                           binding 0  sampled images      (de_tex2d)
//                           binding 1  storage images      (de_rwtex2d)
//                           binding 2  samplers            (de_sampler)
//                           binding 3  storage buffers     (de_buffer)
//   Metal (metallib)      one argument buffer at buffer index 0, a DeBindlessHeap: fixed-size
//                         arrays of resource IDs (MTLResourceID, 8 bytes each) for textures,
//                         storage textures and samplers, then buffer GPU addresses (u64), in the
//                         order and sizes below. The Metal backend reports those sizes as its
//                         max_bindless_* caps. Samplers need supportArgumentBuffers.
//
// Push constants come after the heap: see push_constants.slang (Metal buffer index 1).
//
// Buffer element layout: Vulkan uses std430 and Metal the C layout of the type, D3D12 the
// structured-buffer layout. Keep buffer element types to scalars, 2- and 4-component vectors and
// structs of them, where all three agree.
module bindless;

/// The reserved index of every table: a null descriptor (DE_GPU_BINDLESS_INVALID).
public static const uint DE_BINDLESS_INVALID = 0;

#if defined(DE_SHADER_TARGET_METAL)
// The Metal argument-buffer table sizes (entries). The Metal backend sizes its heap and its caps
// from these; changing one is a cooked-shader format change (bump the importer version).
#define DE_METAL_BINDLESS_TEXTURES 65536
#define DE_METAL_BINDLESS_STORAGE_TEXTURES 16384
#define DE_METAL_BINDLESS_SAMPLERS 2048
#define DE_METAL_BINDLESS_BUFFERS 65536

struct DeBindlessHeap
{
    Texture2D<float4> textures[DE_METAL_BINDLESS_TEXTURES];
    RWTexture2D<float4> storage_textures[DE_METAL_BINDLESS_STORAGE_TEXTURES];
    SamplerState samplers[DE_METAL_BINDLESS_SAMPLERS];
    uint64_t buffers[DE_METAL_BINDLESS_BUFFERS];
};
// Declared first among the global parameters, so it gets buffer index 0. The importer adds this
// module to every program, so the index never moves.
ParameterBlock<DeBindlessHeap> de__heap;
#elif defined(DE_SHADER_TARGET_SPIRV)
[[vk::binding(0, 0)]] __DynamicResource<__DynamicResourceKind.General> de__vk_textures[];
[[vk::binding(1, 0)]] __DynamicResource<__DynamicResourceKind.General> de__vk_storage_textures[];
[[vk::binding(2, 0)]] __DynamicResource<__DynamicResourceKind.Sampler> de__vk_samplers[];
[[vk::binding(3, 0)]] __DynamicResource<__DynamicResourceKind.General> de__vk_buffers[];
#endif

/// The sampled 2D texture at bindless index `idx`.
public Texture2D<float4> de_tex2d(uint idx)
{
#if defined(DE_SHADER_TARGET_METAL)
    return de__heap.textures[idx];
#elif defined(DE_SHADER_TARGET_SPIRV)
    return de__vk_textures[NonUniformResourceIndex(idx)].as<Texture2D<float4>>();
#else
    return Texture2D<float4>(ResourceDescriptorHeap[NonUniformResourceIndex(idx)]);
#endif
}

/// The storage (read-write) 2D texture at bindless index `idx`.
public RWTexture2D<float4> de_rwtex2d(uint idx)
{
#if defined(DE_SHADER_TARGET_METAL)
    return de__heap.storage_textures[idx];
#elif defined(DE_SHADER_TARGET_SPIRV)
    return de__vk_storage_textures[NonUniformResourceIndex(idx)].as<RWTexture2D<float4>>();
#else
    return RWTexture2D<float4>(ResourceDescriptorHeap[NonUniformResourceIndex(idx)]);
#endif
}

/// The sampler at bindless index `idx` (its own index space).
public SamplerState de_sampler(uint idx)
{
#if defined(DE_SHADER_TARGET_METAL)
    return de__heap.samplers[idx];
#elif defined(DE_SHADER_TARGET_SPIRV)
    return de__vk_samplers[NonUniformResourceIndex(idx)].as<SamplerState>();
#else
    return SamplerState(SamplerDescriptorHeap[NonUniformResourceIndex(idx)]);
#endif
}

/// A buffer at a bindless index, read and written as an array of T: `de_buffer<T>(idx)[i]`.
public struct DeBuffer<T>
{
    public uint index;

    public __subscript(uint i)->T
    {
        get
        {
#if defined(DE_SHADER_TARGET_METAL)
            return ((T *)de__heap.buffers[index])[i];
#elif defined(DE_SHADER_TARGET_SPIRV)
            return de__vk_buffers[NonUniformResourceIndex(index)].as<RWStructuredBuffer<T>>()[i];
#else
            return RWStructuredBuffer<T>(ResourceDescriptorHeap[NonUniformResourceIndex(index)])[i];
#endif
        }
        [nonmutating] set
        {
#if defined(DE_SHADER_TARGET_METAL)
            ((T *)de__heap.buffers[index])[i] = newValue;
#elif defined(DE_SHADER_TARGET_SPIRV)
            de__vk_buffers[NonUniformResourceIndex(index)].as<RWStructuredBuffer<T>>()[i] = newValue;
#else
            RWStructuredBuffer<T>(ResourceDescriptorHeap[NonUniformResourceIndex(index)])[i] = newValue;
#endif
        }
    }
}

/// The buffer at bindless index `idx`, as an array of T.
public DeBuffer<T> de_buffer<T>(uint idx)
{
    DeBuffer<T> b;
    b.index = idx;
    return b;
}
