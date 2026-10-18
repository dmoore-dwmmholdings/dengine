import push_constants;
import ddgi_trace;
[shader("compute")][numthreads(64,1,1)]void cs_relocate(uint3 id:SV_DispatchThreadID,uniform DdgiPush push) {
    DdgiParams p=ddgi_params(push);if(id.x>=p.budget)return;uint probe=ddgi_probe(p,id.x);GiVolume g=ddgi_volume(p);
    uint backfaces=0,nearby=0;float closest=1e30;float3 move=0;float clearance=min(g.step.x,min(g.step.y,g.step.z))*.05;
    for(uint i=0;i<p.rays_per_probe;++i) {
        uint index=id.x*p.rays_per_probe+i;float d=de_robuffer<float4>(p.ray_info)[index].w;
        nearby+=abs(d)<g.params.z*.999;
        if(d<0){++backfaces;if(-d<closest){closest=-d;move=de_robuffer<DeRtRay>(p.rays)[index].direction.xyz*(-d+clearance);}}
    }
    float4 state=de_buffer<float4>(p.state_current)[probe];
    bool inside=backfaces>p.rays_per_probe/4;
    if(inside)state.xyz+=move;
    state.w=inside||nearby==0?1:0;de_buffer<float4>(p.state_current)[probe]=state;
}
