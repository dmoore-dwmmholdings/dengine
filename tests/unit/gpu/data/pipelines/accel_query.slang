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
