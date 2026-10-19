// Native Vulkan/DXR pipeline entry points. Material estimators are shared with the Metal adapter.
import push_constants;
import pt_aov;
struct PtPush {uint params,entry;};
struct PtPayload {DeRtHit hit;uint flags,owner;};
PtParams pt_parameters(PtPush pc) {return de_robuffer<PtParams>(pc.params)[pc.entry];}
DeRtHit pt_intersect(PtParams p,DeRtRay r,uint flags,uint owner=0xffffffffu) {
    PtPayload payload=(PtPayload)0;payload.hit.t=-1;payload.hit.instance=0xffffffffu;payload.flags=flags;payload.owner=owner;
    if(!(r.direction.w>r.origin.w))return payload.hit;
    RayDesc ray;ray.Origin=r.origin.xyz;ray.Direction=r.direction.xyz;ray.TMin=r.origin.w;ray.TMax=r.direction.w;
    DeRtContext q=pt_query(p);TraceRay(de_accel(q.tlas),owner!=0xffffffffu?RAY_FLAG_FORCE_NON_OPAQUE:RAY_FLAG_NONE,255,0,1,0,ray,payload);return payload.hit;
}
DeRtHit pt_intersect_sss(PtParams p,DeRtRay ray,PtSssState state) {
    return pt_intersect(p,ray,0,state.active!=0?state.instance:0xffffffffu);
}
float3 pt_shadow_trace(PtParams p,PtMediumStack media,DeRtRay ray,uint casts) {
    PtShadow shadow=pt_shadow_start(p,media,ray);
    if((p.media.features&2u)==0) {
        if(casts!=0&&pt_intersect(p,ray,DE_RT_CAST_SHADOWS).t>=0)return float3(0);
        return shadow.value;
    }
    for(uint boundary=0;boundary<64&&shadow.finished==0;++boundary) {
        DeRtHit hit=pt_intersect(p,ray,0);pt_shadow_hit(p,shadow,ray,hit,casts);
    }
    return shadow.finished!=0?shadow.value:float3(0);
}
[shader("raygeneration")]
void pt_raygen(uniform PtPush pc) {
    PtParams p=pt_parameters(pc);uint2 id=DispatchRaysIndex().xy;uint pixel=id.y*p.width+id.x;
    DeRtRay ray=pt_camera(p,pixel);DeRtHit first=pt_intersect(p,ray,0);pt_store_aov(p,ray,first,pixel);float3 throughput=1,radiance=0,previous=ray.origin.xyz;float pdf=0;
    if(p.media.features==0) {
    for(uint bounce=0;bounce<p.max_bounces;++bounce) {
        DeRtHit hit=bounce==0?first:pt_intersect(p,ray,0);PtStep step=pt_step(p,ray,hit,pixel,bounce,pdf,previous);
        radiance+=throughput*step.emitted;
        bool visible=step.casts_shadows==0 || step.shadow.direction.w<=step.shadow.origin.w || !any(step.direct>0) || pt_intersect(p,step.shadow,DE_RT_CAST_SHADOWS).t<0;
        if(visible)radiance+=throughput*step.direct;
        if(step.stop!=0)break;
        throughput*=step.weight;if(!all(isfinite(throughput))||!any(throughput>0))break;
        if(bounce>=3){float survival=clamp(max(throughput.x,max(throughput.y,throughput.z)),.05,.95);
            if(pt_random(p.sampling_table,pixel^p.seed,p.sample,2+bounce*12+8)>=survival)break;throughput/=survival;}
        previous=ray.origin.xyz+ray.direction.xyz*hit.t;ray=step.next;pdf=step.pdf;
    }
    } else {
        PtPath path=pt_path_init(p,pixel);uint limit=p.max_bounces*(p.media.max_steps+5);
        for(uint event=0;event<limit&&path.finished==0;++event) {
            DeRtHit hit=event==0?first:pt_intersect_sss(p,path.ray,path.sss);PtStep step=pt_path_event(p,path,hit,pixel);
            float3 visibility=1;
            if(step.shadow.direction.w>step.shadow.origin.w&&any(step.direct>0)) {
                visibility=pt_shadow_trace(p,step.shadow_media,step.shadow,step.casts_shadows);
            }
            pt_path_finish(p,path,step,visibility,pixel);
        }
        pt_path_diagnostics(p,path);radiance=path.radiance;
    }
    radiance=select(isfinite(radiance),radiance,float3(0));
    float4 old=p.sample>0?de_rwtex2d(p.accumulation)[id]:float4(0);
    float4 sum=old+float4(radiance,1);de_rwtex2d(p.accumulation)[id]=sum;
    float3 previous_moments=p.sample>0?de_rwtex2d(p.aov_moments)[id].rgb:float3(0);
    de_rwtex2d(p.aov_moments)[id]=float4(pt_sample_moments(previous_moments,old.rgb/max(old.a,1),radiance,p.sample),0);
    de_rwtex2d(p.output)[id]=float4(sum.rgb/max(sum.a,1)*p.exposure,1);
}
[shader("miss")]
void pt_miss(inout PtPayload payload) {payload.hit.t=-1;payload.hit.instance=0xffffffffu;}
[shader("closesthit")]
void pt_closest(inout PtPayload payload,BuiltInTriangleIntersectionAttributes attributes,uniform PtPush pc) {
    PtParams p=pt_parameters(pc);DeRtContext q=pt_query(p);
    payload.hit.t=RayTCurrent();payload.hit.instance=InstanceID();payload.hit.primitive=PrimitiveIndex();
    payload.hit.u=attributes.barycentrics.x;payload.hit.v=attributes.barycentrics.y;
    payload.hit.lod=de_bytes(q.lods).load(InstanceID()*4);
}
[shader("anyhit")]
void pt_alpha(inout PtPayload payload,BuiltInTriangleIntersectionAttributes attributes,uniform PtPush pc) {
    PtParams p=pt_parameters(pc);DeRtContext q=pt_query(p);q.flags=payload.flags;
    if((payload.owner!=0xffffffffu&&InstanceID()!=payload.owner)||
       !de_rt_opacity(q,InstanceID(),PrimitiveIndex(),attributes.barycentrics))IgnoreHit();
}
