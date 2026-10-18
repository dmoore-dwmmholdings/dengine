import push_constants;
import rt_hit_shading;
import light_sampling;
import "../ssr/ssr_common";
struct ReflectionParams {
    uint scene,slot,depth,gb0;uint gb1,gb2,ids,visible;
    uint visible_count,ids_pitch,query,view;uint entry,lights,dirs,grid;
    uint list,counters,ltc1,ltc2;uint sampler,rays,hits,sun_rays;
    uint sun_hits,output,distance,width;uint height,frame,scale,samples;
    float max_roughness;uint resolved,source,rt_locals;
};
struct ReflectionPush {uint params,entry,sun,sample;};
ReflectionParams params(ReflectionPush push){return de_robuffer<ReflectionParams>(push.params)[push.entry];}
DeSceneConstants scene(ReflectionParams p){return de_robuffer<DeSceneConstants>(p.scene)[p.slot];}
DeLightingView lighting(ReflectionParams p){return de_robuffer<DeLightingView>(p.view)[p.entry];}
DeClusterLists lists(ReflectionParams p){DeClusterLists c;c.lights=p.lights;c.dir_lights=p.dirs;c.grid=p.grid;c.list=p.list;c.counters=p.counters;c.ltc1=p.ltc1;c.ltc2=p.ltc2;c.sampler=p.sampler;return c;}
float3 position(ReflectionParams p,uint2 q){return ssr_position(scene(p),(float2(q)+.5)/float2(p.width,p.height),de_tex2d(p.depth).Load(int3(q,0)).r);}
DeRtSurface receiver(ReflectionParams p,uint2 q) {
    DeSceneConstants sc=scene(p);uint id=de_bytes(p.ids).load(q.y*p.ids_pitch+q.x*4);
    DeResolvePoint point=de_resolve_point(id,q,float2(p.width,p.height),sc,p.visible,p.visible_count);
    float3 pos=position(p,q),v=ssr_view(sc,pos);
    return de_rt_surface(point,sc,pos+v,de_robuffer<DeRtContext>(p.query)[0].sampler,lighting(p).brdf_lut);
}
uint2 representative(ReflectionParams p,uint2 q,DeRtSurface receiver_surface) {
    if(p.scale==1||receiver_surface.material.roughness<.05)return q;
    uint2 best=q;float best_depth=-1;
    for(uint y=0;y<p.scale;++y)for(uint x=0;x<p.scale;++x) {
        uint2 at=min((q/p.scale)*p.scale+uint2(x,y),uint2(p.width-1,p.height-1));DeRtSurface candidate=receiver(p,at);
        if(!candidate.valid||candidate.material.roughness<.05||candidate.material.roughness>=p.max_roughness)continue;
        if(dot(candidate.material.normal,receiver_surface.material.normal)<.99||abs(dot(candidate.position-receiver_surface.position,receiver_surface.material.normal))>.01||any(abs(candidate.material.base_color-receiver_surface.material.base_color)>.01)||abs(candidate.material.roughness-receiver_surface.material.roughness)>.02)continue;
        float z=de_tex2d(p.depth).Load(int3(at,0)).r;if(z>best_depth){best=at;best_depth=z;}
    }return best;
}
float3 weight(ReflectionParams p,DeRtSurface s,float3 wi) {
    ShadingFrame f=de_surface_frame(s.material);float3 wo=f.to_local(ssr_view(scene(p),s.position)),w=f.to_local(wi);
    if(s.material.roughness<.05) {DeIbl unit=(DeIbl)0;unit.ambient=float4(1);float3 d,sp;de_ibl_evaluate(s.material,ssr_view(scene(p),s.position),unit,lighting(p).brdf_lut,1,d,sp);return sp;}
    float a=max(.001,s.material.roughness*s.material.roughness),pdf=de_ggx_reflect_pdf(a,a,wo,w);
    return de_rt_specular_weight<StandardModel>(s.material,wo,w,pdf);
}
float receiver_occlusion(ReflectionParams p,DeRtSurface s) {return de_ibl_specular_occlusion(max(dot(s.material.normal,ssr_view(scene(p),s.position)),1e-4),s.material.ao,s.material.roughness);}
[shader("compute")][numthreads(8,8,1)]void cs_generate(uint3 id:SV_DispatchThreadID,uniform ReflectionPush push) {
    ReflectionParams p=params(push);if(id.x>=p.width||id.y>=p.height)return;uint pixel=id.y*p.width+id.x;
    DeRtRay ray=(DeRtRay)0;DeRtSurface s=receiver(p,id.xy);
    if(s.valid&&s.material.roughness<p.max_roughness&&(s.material.features&DE_MATERIAL_FEATURE_UNLIT)==0&&all(representative(p,id.xy,s)==id.xy)) {
        ShadingFrame f=de_surface_frame(s.material);float3 wo=f.to_local(ssr_view(scene(p),s.position));
        float2 shift=float2(de_u32_to_unit(de_pcg_hash(pixel)),de_u32_to_unit(de_pcg_hash(pixel+17)));
        // Reference uses a fixed Hammersley integration, production uses progressive independent VNDF samples.
        float2 u=frac((p.samples>1?de_hammersley(push.sample,p.samples):float2((p.frame&63u)+.5,reversebits(p.frame&63u)) * float2(1.0/64.0,1.0/4294967296.0))+shift);
        float a=max(.001,s.material.roughness*s.material.roughness);float3 h=s.material.roughness<.05?float3(0,0,1):de_ggx_vndf_sample(a,a,wo,u);
        float3 wi=de_reflect(wo,h),direction=f.to_world(wi);
        if(wo.z>0&&wi.z>0){float bias=max(.0001,.00001*max(abs(s.position.x),max(abs(s.position.y),abs(s.position.z))));ray.origin=float4(s.position+s.material.normal*bias,.00001);ray.direction=float4(direction,10000);}
    }de_buffer<DeRtRay>(p.rays)[pixel]=ray;
}
DeRtSurface hit_surface(ReflectionParams p,uint pixel){DeRtRay ray=de_robuffer<DeRtRay>(p.rays)[pixel];return de_rt_hit_surface(de_robuffer<DeRtContext>(p.query)[0],de_robuffer<DeRtHit>(p.hits)[pixel],ray.origin.xyz,lighting(p).brdf_lut);}
uint cluster(ReflectionParams p,float3 position){DeSceneConstants sc=scene(p);float4 h=float4(dot(sc.view_proj[0],float4(position,1)),dot(sc.view_proj[1],float4(position,1)),0,dot(sc.view_proj[3],float4(position,1)));float2 uv=float2(h.x,-h.y)/max(h.w,1e-6)*.5+.5;return de_cluster_index(lighting(p),uint2(saturate(uv)*float2(p.width-1,p.height-1)),-dot(sc.view[2],float4(position,1)));}
[shader("compute")][numthreads(8,8,1)]void cs_shade(uint3 id:SV_DispatchThreadID,uniform ReflectionPush push) {
    ReflectionParams p=params(push);if(id.x>=p.width||id.y>=p.height)return;uint pixel=id.y*p.width+id.x;DeRtRay ray=de_robuffer<DeRtRay>(p.rays)[pixel];
    DeRtSurface hit=hit_surface(p,pixel);DeRtSurface s=receiver(p,id.xy);float3 radiance=0;
    if(ray.direction.w>0)radiance=hit.valid?de_rt_hit_radiance<StandardModel>(hit,-ray.direction.xyz,lighting(p),lists(p),cluster(p,hit.position),p.rt_locals==0):de_rt_environment(lighting(p),lists(p),s.position,ray.direction.xyz,cluster(p,s.position));
    float3 value=ray.direction.w>0?radiance*weight(p,s,ray.direction.xyz)*receiver_occlusion(p,s):float3(0);
    if(push.sample==0)de_rwtex2d(p.output)[id.xy]=float4(value/p.samples,0);else de_rwtex2d(p.output)[id.xy]+=float4(value/p.samples,0);
    de_rwtex2d(p.distance)[id.xy]=float4(max(de_robuffer<DeRtHit>(p.hits)[pixel].t,0));
}
[shader("compute")][numthreads(8,8,1)]void cs_sun_ray(uint3 id:SV_DispatchThreadID,uniform ReflectionPush push) {
    ReflectionParams p=params(push);if(id.x>=p.width||id.y>=p.height)return;uint pixel=id.y*p.width+id.x;DeRtSurface hit=hit_surface(p,pixel);DeRtRay ray=(DeRtRay)0;
    bool local=push.sun>=4;uint index=local?push.sun-4:push.sun;
    uint count=local?min(de_bytes(p.counters).load(0),lighting(p).local_capacity):min(de_bytes(p.counters).load(4),4u);
    if(hit.valid&&(hit.material.features&DE_MATERIAL_FEATURE_UNLIT)==0&&index<count) {
        DeClusterLight light=de_robuffer<DeClusterLight>(local?p.lights:p.dirs)[index];
        // A noncasting light still shades; tmax=0 below is distinguished in cs_sun_add.
        if((asuint(light.axis.w)&DE_CLUSTER_LIGHT_SHADOWS)!=0) {
            float3 contribution=local?de_rt_local_light<StandardModel>(hit.material,hit.position,-de_robuffer<DeRtRay>(p.rays)[pixel].direction.xyz,lighting(p),lists(p),light):float3(1);
            if(any(contribution>0))ray=de_rt_shadow_ray(light,hit.position,hit.material.normal,de_rt_random(pixel,p.frame+push.sample,push.sun));
        }
    }de_buffer<DeRtRay>(p.sun_rays)[pixel]=ray;
}
[shader("compute")][numthreads(8,8,1)]void cs_sun_add(uint3 id:SV_DispatchThreadID,uniform ReflectionPush push) {
    ReflectionParams p=params(push);if(id.x>=p.width||id.y>=p.height)return;uint pixel=id.y*p.width+id.x;
    bool local=push.sun>=4;uint index=local?push.sun-4:push.sun;
    uint count=local?min(de_bytes(p.counters).load(0),lighting(p).local_capacity):min(de_bytes(p.counters).load(4),4u);
    if(index>=count)return;DeRtSurface hit=hit_surface(p,pixel),s=receiver(p,id.xy);if(!hit.valid||(hit.material.features&DE_MATERIAL_FEATURE_UNLIT)!=0)return;
    DeClusterLight light=de_robuffer<DeClusterLight>(local?p.lights:p.dirs)[index];
    if((asuint(light.axis.w)&DE_CLUSTER_LIGHT_SHADOWS)!=0&&de_robuffer<DeRtHit>(p.sun_hits)[pixel].t>=0)return;
    DeRtRay ray=de_robuffer<DeRtRay>(p.rays)[pixel];
    float3 value=local?de_rt_local_light<StandardModel>(hit.material,hit.position,-ray.direction.xyz,lighting(p),lists(p),light):de_rt_sun_direct<StandardModel>(hit.material,hit.position,-ray.direction.xyz,light);
    de_rwtex2d(p.output)[id.xy]+=float4(value*weight(p,s,ray.direction.xyz)*receiver_occlusion(p,s)/p.samples,0);
}
// Resolve into a distinct texture; rough representative reads never race with publication writes.
[shader("compute")][numthreads(8,8,1)]void cs_resolve(uint3 id:SV_DispatchThreadID,uniform ReflectionPush push) {
    ReflectionParams p=params(push);if(id.x>=p.width||id.y>=p.height)return;DeRtSurface s=receiver(p,id.xy);
    float confidence=s.valid&&(s.material.features&DE_MATERIAL_FEATURE_UNLIT)==0?saturate((p.max_roughness-s.material.roughness)/.1):0;
    de_rwtex2d(p.resolved)[id.xy]=float4(de_tex2d(p.source).Load(int3(representative(p,id.xy,s),0)).rgb,confidence);
}

struct WeightPush {uint output,count,pad0,pad1;};
[shader("compute")][numthreads(64,1,1)]void cs_weight_contract(uint3 id:SV_DispatchThreadID,uniform WeightPush p) {
    if(id.x>=p.count)return;SurfaceData s=de_surface_default();s.base_color=float3(.7,.4,.2);s.metallic=1;s.roughness=.4;
    float3 wo=normalize(float3(.7,0,.6));BsdfSample oracle=StandardModel.sample(s,wo,float3(de_hammersley(id.x,p.count),.1));
    float a=s.roughness*s.roughness,pdf=de_ggx_reflect_pdf(a,a,wo,oracle.wi);float3 actual=de_rt_specular_weight<StandardModel>(s,wo,oracle.wi,pdf),expected=pdf>0?oracle.weight*(oracle.pdf/pdf):float3(0);
    de_buffer<float4>(p.output)[id.x]=float4(abs(actual-expected)/max(abs(expected),float3(1e-5)),oracle.wi.z);
}
