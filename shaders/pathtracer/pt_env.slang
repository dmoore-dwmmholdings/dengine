import push_constants;
import pt_closest_hit;
struct PtPush {uint params,entry;};
[shader("compute")][numthreads(1,1,1)]
void pt_env_cdf(uniform PtPush pc) {
    PtParams p=de_robuffer<PtParams>(pc.params)[pc.entry];float total=0;
    for(uint i=0;i<512;++i){float3 wi=pt_env_direction((float2(i%32,i/32)+.5)/float2(32,16));
        float w=max(dot(pt_environment(p,wi),float3(.2126,.7152,.0722)),0.0);de_buffer<float2>(p.env_cdf)[i]=float2(0,w);total+=w;}
    float sum=0;for(uint i=0;i<512;++i){float pdf=total>0?de_robuffer<float2>(p.env_cdf)[i].y/total:1.0/512;sum+=pdf;de_buffer<float2>(p.env_cdf)[i]=float2(i==511?1:sum,pdf);}
}
