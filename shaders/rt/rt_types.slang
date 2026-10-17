module rt_types;
public struct DeRtMesh {
    public uint blas[8]; public uint index_first[8];
    public uint ranges, submeshes, resident_mask, reserved;
};
// 32-byte backend-neutral query context, all indices into the ordinary engine heap.
public struct DeRtContext {
    public uint scene, view, metadata, lods, sampler, layers, flags, tlas;
};
public static const uint DE_RT_CAST_SHADOWS = 1;
public struct DeRtRay { public float4 origin; public float4 direction; }; // w = tmin/tmax
public struct DeRtHit { public float t; public uint instance, primitive; public float u;
                       public float v; public uint lod, reserved0, reserved1; };
