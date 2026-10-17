import push_constants;
import restir_di_common;
public bool valid_neighbor(DiParams p,DiSurface s,uint2 q) {
    DiHistory h=de_robuffer<DiHistory>(p.history_current)[q.y*p.width+q.x];
    // No cross-surface reuse: preserves disocclusion and geometric edge ownership.
    return s.valid && h.normal.w>0 && dot(s.material.normal,h.normal.xyz)>.95 && abs(s.depth-h.position.w)<max(.02,.02*s.depth);
}
[shader("compute")][numthreads(8,8,1)]
void cs_spatial(uint3 id:SV_DispatchThreadID,uniform DiPush pc) {
    DiParams p=di_params(pc);if(id.x>=p.width || id.y>=p.height)return;
    uint pixel=id.y*p.width+id.x;DiSurface s=di_surface(p,id.xy);DiReservoir r=di_empty();
    for(uint j=0;j<6;++j) {
        uint2 q=di_neighbor(id.xy,j,p);if(!valid_neighbor(p,s,q))continue;
        DiReservoir other=di_load(p.temporal,q.y*p.width+q.x);
        float target=di_evaluate(p,s,other.source,float2(other.u,other.v)).target;
        di_merge(r,other,target,p.candidates*21,de_rt_random(pixel,p.frame,400+j).x);
    }
    di_finalize(r,r.M);de_buffer<DiReservoir>(p.spatial)[pixel]=r;
}
[shader("compute")][numthreads(8,8,1)]
void cs_spatial_visibility(uint3 id:SV_DispatchThreadID,uniform DiPush pc) {
    DiParams p=di_params(pc);if(id.x>=p.width || id.y>=p.height)return;
    uint pixel=id.y*p.width+id.x;DiSurface s=di_surface(p,id.xy);DiReservoir r=di_load(p.spatial,pixel);
    uint normalization=0;
    for(uint j=0;j<6;++j) {
        uint2 q=di_neighbor(id.xy,j,p);DiSurface other=di_surface(p,q);
        bool valid=valid_neighbor(p,s,q);DiSample sample=di_evaluate(p,other,r.source,float2(r.u,r.v));
        if(pc.phase==0) {
            if(!valid || !(sample.target>0)){sample.ray.origin=float4(0);sample.ray.direction=float4(0,0,1,0);}
            de_buffer<DeRtRay>(p.rays)[pixel*6+j]=sample.ray;
        } else if(valid && sample.target>0 && di_visible(p,pixel*6+j))normalization+=di_load(p.temporal,q.y*p.width+q.x).M;
    }
    if(pc.phase!=0) {di_finalize(r,normalization);de_buffer<DiReservoir>(p.spatial)[pixel]=r;}
}
