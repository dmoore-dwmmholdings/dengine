// shaders/common/bindless.slang: the engine's bindless resource convention (SPEC §12.1, DENG-289).
//
// Shaders reach every texture, sampler and buffer through a 32-bit index into one global table
// per kind (the indices de_gpu hands out: de_gpu_texture_bindless_index and friends). Index 0 is
// reserved in every table (DE_GPU_BINDLESS_INVALID): the backends put a null descriptor there.
//
//     import bindless;
//     float4 c = de_tex2d(pc.albedo).Sample(de_sampler(pc.smp), uv);
//     de_buffer<float4>(pc.out)[i] = c * de_robuffer<float4>(pc.tint)[i];
//     uint flags = de_bytes(pc.blob).load(16u * i + 4u);
//
// Buffers have one bindless index and four accessors (DENG-410). Pick the one that matches what
// the pass declares for the buffer (its de_gpu_barrier access):
//
//   de_buffer<T>(idx)    read-write, T elements   DE_ACCESS_SHADER_WRITE (reads may go with it)
//   de_robuffer<T>(idx)  read-only, T elements    DE_ACCESS_SHADER_READ; UPLOAD buffers too
//   de_rwbytes(idx)      read-write, byte address DE_ACCESS_SHADER_WRITE; also atomic_add
//   de_bytes(idx)       read-only, byte address  DE_ACCESS_SHADER_READ; UPLOAD buffers too
//
// Vulkan and Metal see the same buffer through all four. D3D12 has a view per access: the index
// holds the read-write view (UAV) of a GPU_ONLY buffer, or the read-only view (SRV) of an UPLOAD
// buffer, which a shader cannot write; DE__D3D12_BUFFER_SRV_OFFSET slots later is every buffer's
// read-only view. Both views follow the buffer's stride: a buffer with a stride is a structured
// buffer of that stride (de_buffer / de_robuffer, with sizeof(T) == stride), a buffer with stride
// 0 is a raw buffer (de_bytes / de_rwbytes). So, to run on every backend: read UPLOAD buffers
// only through de_robuffer / de_bytes, read a buffer through the read-only accessors when its
// access is DE_ACCESS_SHADER_READ, and give it the stride its accessors expect.
//
// The shader importer (tools/cook/importers/shader/) embeds this file and compiles each target in
// its own Slang session with one of DE_SHADER_TARGET_SPIRV, DE_SHADER_TARGET_DXIL or
// DE_SHADER_TARGET_METAL defined to 1. What each backend binds:
//
//   D3D12 (DXIL, SM 6.6)  ResourceDescriptorHeap (textures, storage textures, buffers, and the
//                         buffers' read-only views) and SamplerDescriptorHeap. No root-signature
//                         tables for them.
//   Vulkan (SPIR-V)       descriptor set 0, runtime arrays (descriptor indexing):
//                           binding 0  sampled images      (de_tex2d)
//                           binding 1  storage images      (de_rwtex2d)
//                           binding 2  samplers            (de_sampler)
//                           binding 3  storage buffers     (de_buffer, de_robuffer, de_bytes,
//                                                           de_rwbytes)
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
    RaytracingAccelerationStructure accels[65536];
};
// Declared first among the global parameters, so it gets buffer index 0. The importer adds this
// module to every program, so the index never moves.
ParameterBlock<DeBindlessHeap> de__heap;
#elif defined(DE_SHADER_TARGET_SPIRV)
// de_buffer, de_robuffer, de_bytes and de_rwbytes all read binding 3 (storage buffers), each as
// its own type.
[[vk::binding(0, 0)]] __DynamicResource<__DynamicResourceKind.General> de__vk_textures[];
[[vk::binding(1, 0)]] __DynamicResource<__DynamicResourceKind.General> de__vk_storage_textures[];
[[vk::binding(2, 0)]] __DynamicResource<__DynamicResourceKind.Sampler> de__vk_samplers[];
[[vk::binding(3, 0)]] __DynamicResource<__DynamicResourceKind.General> de__vk_buffers[];
[[vk::binding(4, 0)]] RaytracingAccelerationStructure de__vk_accels[];
#else
// D3D12: the read-only view (SRV) of the buffer at index i is at heap slot i + this offset
// (DENG-410). It must equal DE__D3D12_BUFFER_SRV_OFFSET in src/gpu/d3d12/d3d12_internal.h, the
// size of the backend's buffer range plus its acceleration-structure range; changing it is a
// cooked-shader format change (bump the importer version).
static const uint DE__D3D12_BUFFER_SRV_OFFSET = 200000;
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

/// A buffer at a bindless index, read as an array of T: `de_robuffer<T>(idx)[i]` (DENG-410). For
/// buffers the pass reads with DE_ACCESS_SHADER_READ, and for UPLOAD buffers.
public struct DeROBuffer<T>
{
    public uint index;

    public __subscript(uint i)->T
    {
        get
        {
#if defined(DE_SHADER_TARGET_METAL)
            return ((T *)de__heap.buffers[index])[i];
#elif defined(DE_SHADER_TARGET_SPIRV)
            return de__vk_buffers[NonUniformResourceIndex(index)].as<StructuredBuffer<T>>()[i];
#else
            return StructuredBuffer<T>(
                ResourceDescriptorHeap[NonUniformResourceIndex(index + DE__D3D12_BUFFER_SRV_OFFSET)])[i];
#endif
        }
    }
}

/// The buffer at bindless index `idx`, read-only, as an array of T.
public DeROBuffer<T> de_robuffer<T>(uint idx)
{
    DeROBuffer<T> b;
    b.index = idx;
    return b;
}

/// A buffer at a bindless index, read as bytes (DENG-410): `de_bytes(idx).load(offset)` reads the
/// 32-bit word at byte `offset`; load2 / load3 / load4 read 2, 3 or 4 words from there. Offsets
/// are multiples of 4. Use asfloat / asint for other types. On D3D12 the buffer needs stride 0.
public struct DeBytes
{
    public uint index;

#if defined(DE_SHADER_TARGET_METAL)
    uint *words() { return (uint *)de__heap.buffers[index]; }
#elif defined(DE_SHADER_TARGET_SPIRV)
    ByteAddressBuffer view() { return de__vk_buffers[NonUniformResourceIndex(index)].as<ByteAddressBuffer>(); }
#else
    ByteAddressBuffer view()
    {
        return ByteAddressBuffer(
            ResourceDescriptorHeap[NonUniformResourceIndex(index + DE__D3D12_BUFFER_SRV_OFFSET)]);
    }
#endif

    public uint load(uint offset)
    {
#if defined(DE_SHADER_TARGET_METAL)
        return words()[offset >> 2];
#else
        return view().Load(offset);
#endif
    }
    public uint2 load2(uint offset)
    {
#if defined(DE_SHADER_TARGET_METAL)
        uint *w = words();
        uint o = offset >> 2;
        return uint2(w[o], w[o + 1]);
#else
        return view().Load2(offset);
#endif
    }
    public uint3 load3(uint offset)
    {
#if defined(DE_SHADER_TARGET_METAL)
        uint *w = words();
        uint o = offset >> 2;
        return uint3(w[o], w[o + 1], w[o + 2]);
#else
        return view().Load3(offset);
#endif
    }
    public uint4 load4(uint offset)
    {
#if defined(DE_SHADER_TARGET_METAL)
        uint *w = words();
        uint o = offset >> 2;
        return uint4(w[o], w[o + 1], w[o + 2], w[o + 3]);
#else
        return view().Load4(offset);
#endif
    }
}

/// The buffer at bindless index `idx`, read-only, as bytes.
public DeBytes de_bytes(uint idx)
{
    DeBytes b;
    b.index = idx;
    return b;
}

/// A buffer at a bindless index, read and written as bytes (DENG-410): the loads of DeBytes plus
/// store / store2 / store3 / store4 of 1 to 4 words at byte `offset` (a multiple of 4). On D3D12
/// the buffer is GPU_ONLY with stride 0.
public struct DeRWBytes
{
    public uint index;

#if defined(DE_SHADER_TARGET_METAL)
    uint *words() { return (uint *)de__heap.buffers[index]; }
#elif defined(DE_SHADER_TARGET_SPIRV)
    RWByteAddressBuffer view() { return de__vk_buffers[NonUniformResourceIndex(index)].as<RWByteAddressBuffer>(); }
#else
    RWByteAddressBuffer view() { return RWByteAddressBuffer(ResourceDescriptorHeap[NonUniformResourceIndex(index)]); }
#endif

    public uint load(uint offset)
    {
#if defined(DE_SHADER_TARGET_METAL)
        return words()[offset >> 2];
#else
        return view().Load(offset);
#endif
    }
    public uint2 load2(uint offset)
    {
#if defined(DE_SHADER_TARGET_METAL)
        uint *w = words();
        uint o = offset >> 2;
        return uint2(w[o], w[o + 1]);
#else
        return view().Load2(offset);
#endif
    }
    public uint3 load3(uint offset)
    {
#if defined(DE_SHADER_TARGET_METAL)
        uint *w = words();
        uint o = offset >> 2;
        return uint3(w[o], w[o + 1], w[o + 2]);
#else
        return view().Load3(offset);
#endif
    }
    public uint4 load4(uint offset)
    {
#if defined(DE_SHADER_TARGET_METAL)
        uint *w = words();
        uint o = offset >> 2;
        return uint4(w[o], w[o + 1], w[o + 2], w[o + 3]);
#else
        return view().Load4(offset);
#endif
    }
    public void store(uint offset, uint v)
    {
#if defined(DE_SHADER_TARGET_METAL)
        words()[offset >> 2] = v;
#else
        view().Store(offset, v);
#endif
    }
    public void store2(uint offset, uint2 v)
    {
#if defined(DE_SHADER_TARGET_METAL)
        uint *w = words();
        uint o = offset >> 2;
        w[o] = v.x;
        w[o + 1] = v.y;
#else
        view().Store2(offset, v);
#endif
    }
    public void store3(uint offset, uint3 v)
    {
#if defined(DE_SHADER_TARGET_METAL)
        uint *w = words();
        uint o = offset >> 2;
        w[o] = v.x;
        w[o + 1] = v.y;
        w[o + 2] = v.z;
#else
        view().Store3(offset, v);
#endif
    }
    public void store4(uint offset, uint4 v)
    {
#if defined(DE_SHADER_TARGET_METAL)
        uint *w = words();
        uint o = offset >> 2;
        w[o] = v.x;
        w[o + 1] = v.y;
        w[o + 2] = v.z;
        w[o + 3] = v.w;
#else
        view().Store4(offset, v);
#endif
    }
    /// Atomically adds `v` to the 32-bit word at byte `offset` (a multiple of 4) and returns the
    /// value it held before (DENG-229: GPU culling appends to work lists with it). Relaxed order:
    /// the pass that reads the result runs after a barrier.
    public uint atomic_add(uint offset, uint v)
    {
#if defined(DE_SHADER_TARGET_METAL)
        Atomic<uint> *a = (Atomic<uint> *)de__heap.buffers[index];
        return a[offset >> 2].add(v);
#else
        uint old;
        view().InterlockedAdd(offset, v, old);
        return old;
#endif
    }
    /// Atomically keeps the smaller of the 32-bit word at byte `offset` and `v`; returns the value
    /// it held before (DENG-238: the texture-streaming feedback keeps the finest mip asked for).
    /// Relaxed order, as atomic_add.
    public uint atomic_min(uint offset, uint v)
    {
#if defined(DE_SHADER_TARGET_METAL)
        Atomic<uint> *a = (Atomic<uint> *)de__heap.buffers[index];
        return a[offset >> 2].min(v);
#else
        uint old;
        view().InterlockedMin(offset, v, old);
        return old;
#endif
    }
}

/// The buffer at bindless index `idx`, read-write, as bytes.
public DeRWBytes de_rwbytes(uint idx)
{
    DeRWBytes b;
    b.index = idx;
    return b;
}

/// A built TLAS at de_gpu_accel_bindless(). Ray queries need caps.ray_query.
public RaytracingAccelerationStructure de_accel(uint idx)
{
#if defined(DE_SHADER_TARGET_SPIRV)
    return de__vk_accels[NonUniformResourceIndex(idx)];
#elif defined(DE_SHADER_TARGET_METAL)
    return de__heap.accels[idx];
#else
    return RaytracingAccelerationStructure(ResourceDescriptorHeap[NonUniformResourceIndex(idx)]);
#endif
}
