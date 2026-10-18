import push_constants;
import restir_di_common;
[shader("compute")][numthreads(8,8,1)]
void cs_final_ray(uint3 id:SV_DispatchThreadID,uniform DiPush pc) {
    DiParams p=di_params(pc);if(id.x>=p.width || id.y>=p.height)return;
    uint pixel=id.y*p.width+id.x;DiReservoir r=di_load(p.spatial,pixel);
    de_buffer<DeRtRay>(p.rays)[pixel]=di_evaluate(p,di_surface(p,id.xy),r.source,float2(r.u,r.v)).ray;
}
[shader("compute")][numthreads(8,8,1)]
void cs_shade(uint3 id:SV_DispatchThreadID,uniform DiPush pc) {
    DiParams p=di_params(pc);if(id.x>=p.width || id.y>=p.height)return;
    uint pixel=id.y*p.width+id.x;DiReservoir r=di_load(p.spatial,pixel);
    DiSample sample=di_evaluate(p,di_surface(p,id.xy),r.source,float2(r.u,r.v));
    float W=di_visible(p,pixel)?r.W:0;
    de_rwtex2d(p.diffuse)[id.xy]=float4(sample.diffuse*W,1);
    de_rwtex2d(p.specular)[id.xy]=float4(sample.specular*W,1);
    de_rwtex2d(p.distance)[id.xy]=float4(max(de_robuffer<DeRtHit>(p.hits)[pixel].t,0));
}
// Independent acceptance/debug reference: exhaustive source sum, each source traced separately.
[shader("compute")][numthreads(8,8,1)]
void cs_reference(uint3 id:SV_DispatchThreadID,uniform DiPush pc) {
    DiParams p=di_params(pc);if(id.x>=p.width || id.y>=p.height)return;
    uint pixel=id.y*p.width+id.x;
    if(pc.phase==0) {
        de_rwtex2d(p.diffuse)[id.xy]=float4(0);de_rwtex2d(p.specular)[id.xy]=float4(0);de_rwtex2d(p.distance)[id.xy]=float4(0);return;
    }
    DiSample sample=di_evaluate(p,di_surface(p,id.xy),pc.source,float2(.5,.5));
    if(pc.phase==1)de_buffer<DeRtRay>(p.rays)[pixel]=sample.ray;
    else {
        float visibility=di_visible(p,pixel)?1:0;
        de_rwtex2d(p.diffuse)[id.xy]+=float4(sample.diffuse*visibility,0);
        de_rwtex2d(p.specular)[id.xy]+=float4(sample.specular*visibility,0);
    }
}
