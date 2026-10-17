// Stable hybrid RT query/hit interface (DENG-44); lighting effects append consumers.
module rt_common;
import bindless;
__exported import rt_types;
__exported import rt_opacity;
__exported import "../visbuffer/resolve_common";
// Render-world extraction and GPU scene transforms already subtract the grid-snapped origin.
public float3 de_rt_relative_origin(float3 position,float3 origin_delta) { return position-origin_delta; }
public float3 de_rt_offset(float3 p,float3 geometric_normal,float3 direction) {
    float scale=max(1.0,max(abs(p.x),max(abs(p.y),abs(p.z))));
    return p+geometric_normal*(dot(geometric_normal,direction)>=0?1:-1)*(0.00001*scale);
}
public DeRtHit de_rt_query(DeRtContext q,DeRtRay r) {
    DeRtHit hit=(DeRtHit)0;hit.t=-1;hit.instance=0xffffffffu;
    RayDesc ray;ray.Origin=r.origin.xyz;ray.Direction=r.direction.xyz;
    ray.TMin=r.origin.w;ray.TMax=r.direction.w;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(de_accel(q.tlas),(q.flags&DE_RT_ACCEPT_FIRST_HIT)!=0?RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH:RAY_FLAG_NONE,255,ray);
    while(query.Proceed()) {
        if(query.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE &&
           de_rt_opacity(q,query.CandidateInstanceID(),query.CandidatePrimitiveIndex(),
                         query.CandidateTriangleBarycentrics()))query.CommitNonOpaqueTriangleHit();
    }
    if(query.CommittedStatus()==COMMITTED_TRIANGLE_HIT) {
        hit.t=query.CommittedRayT();hit.instance=query.CommittedInstanceID();
        hit.primitive=query.CommittedPrimitiveIndex();
        float2 uv=query.CommittedTriangleBarycentrics();hit.u=uv.x;hit.v=uv.y;
        hit.lod=de_bytes(q.lods).load(hit.instance*4);
    }
    return hit;
}
