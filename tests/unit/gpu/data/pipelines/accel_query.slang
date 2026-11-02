import push_constants;
struct Params { uint tlas, output; };
[shader("compute")]
[numthreads(1, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID, uniform Params p)
{
    RayDesc ray;
    ray.Origin = float3(0, 0, 0);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0.001;
    ray.TMax = 100.0;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(de_accel(p.tlas), RAY_FLAG_NONE, 0xff, ray);
    while (q.Proceed()) { }
    float t = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0;
    de_rwbytes(p.output).store(0, asuint(t));
}

// DENG-735: write the public 60-byte instance record entirely on the GPU.
struct InstanceParams { uint blas, output; float translation; uint mask; };
[shader("compute")]
[numthreads(1, 1, 1)]
void cs_generate_instance(uint3 id : SV_DispatchThreadID, uniform InstanceParams p)
{
    let dst = de_rwbytes(p.output);
    for (uint i = 0; i < 12; ++i) {
        float value = i == 0 || i == 5 || i == 10 ? 1.0 : i == 11 ? p.translation : 0.0;
        dst.store(i * 4, asuint(value));
    }
    dst.store(48, p.mask << 24);
    dst.store(52, 0);
    dst.store(56, p.blas);
}

// Geometry input exercises a native BLAS read, independently of TLAS record translation.
[shader("compute")]
[numthreads(1, 1, 1)]
void cs_generate_vertices(uint3 id : SV_DispatchThreadID, uniform Params p)
{
    let dst = de_rwbytes(p.output);
    const float positions[9] = {-1, -1, 2, 1, -1, 2, 0, 1, 2};
    for (uint i = 0; i < 9; ++i)
        dst.store(i * 4, asuint(positions[i]));
}
