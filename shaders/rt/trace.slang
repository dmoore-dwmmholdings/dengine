import push_constants;
import rt_common;
struct Params { uint context, rays, hits, count, flags; };
[shader("compute")]
[numthreads(64,1,1)]
void cs_trace(uint3 id:SV_DispatchThreadID,uniform Params p) {
    if((id.x+id.y*(65535u*64u))>=p.count)return;
    DeRtContext q=de_robuffer<DeRtContext>(p.context)[0];
    q.flags=p.flags;
    de_buffer<DeRtHit>(p.hits)[(id.x+id.y*(65535u*64u))]=de_rt_query(q,de_robuffer<DeRtRay>(p.rays)[(id.x+id.y*(65535u*64u))]);
}
