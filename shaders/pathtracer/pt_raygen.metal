// Native traversal/callable-table adapter only. All reconstruction, material, BSDF and light
// estimators below come from the canonical Slang source through regen_metal.py.
#include "pt_metal.generated"
#include "../common/rt_pipeline_metal.h"
struct PtPush {uint params,entry;};
inline PtParams_0 pt_load(constant DeRtHeap &heap,constant PtPush &pc,thread KernelContext_0 &ctx) {
    ctx={};ctx.de_heap_0=(constant DeBindlessHeap_default_0 *)&heap;
    return pt_load_0(pc.params,pc.entry,&ctx);
}
[[visible]] void pt_miss(thread DeRtPayload &p,thread const DeRtHit &,constant DeRtHeap &,constant uchar *) {p.words[0]=as_type<uint>(-1.f);p.words[1]=0xffffffffu;}
[[visible]] void pt_closest(thread DeRtPayload &p,thread const DeRtHit &h,constant DeRtHeap &,constant uchar *) {
    p.words[0]=as_type<uint>(h.distance);p.words[1]=h.custom_index;p.words[2]=h.primitive;
    p.words[3]=as_type<uint>(h.barycentrics.x);p.words[4]=as_type<uint>(h.barycentrics.y);
}
[[intersection(triangle,triangle_data,instancing)]]
bool pt_alpha(uint instance [[user_instance_id]],uint primitive [[primitive_id]],float2 bary [[barycentric_coord]],
              ray_data DeRtPayload &payload [[payload]],constant DeRtHeap &heap [[buffer(0)]]) {
    PtPush local={payload.words[13],payload.words[14]};
    KernelContext_0 ctx;ctx={};ctx.de_heap_0=(constant DeBindlessHeap_default_0 *)&heap;auto p=pt_load_0(local.params,local.entry,&ctx);auto q=pt_query_0(&p,&ctx);q.ptq_flags=payload.words[15];
    return de_rt_opacity_0(&q,instance,primitive,bary,&ctx);
}
inline DeRtHit_0 pt_intersect(constant DeRtHeap &heap,thread const PtParams_0 &p,DeRtRay_0 r,uint flags,
                             DeRtIntersectionTable intersections,DeRtVisibleTable functions,
                             device const uint4 *sbt,constant uchar *push,thread KernelContext_0 &ctx) {
    DeRtPayload payload={};payload.words[15]=flags;
    constant PtPush &pc=*(constant PtPush *)push;payload.words[13]=pc.params;payload.words[14]=pc.entry;
    ray native;native.origin=r.ptr_origin.xyz;native.direction=r.ptr_direction.xyz;
    native.min_distance=r.ptr_origin.w;native.max_distance=r.ptr_direction.w;
    auto query=pt_query_0(&p,&ctx);
    de_rt_trace(heap,query.ptq_tlas,native,255,0,payload,intersections,functions,sbt,push);
    DeRtHit_0 h={};h.pth_t=as_type<float>(payload.words[0]);h.pth_instance=payload.words[1];h.pth_primitive=payload.words[2];
    h.pth_u=as_type<float>(payload.words[3]);h.pth_v=as_type<float>(payload.words[4]);
    if(h.pth_t>=0)h.pth_lod=((device const uint *)heap.buffers[query.ptq_lods])[h.pth_instance];return h;
}
kernel void pt_raygen(constant DeRtHeap &heap [[buffer(0)]],constant PtPush &pc [[buffer(1)]],
                      DeRtIntersectionTable intersections [[buffer(2)]],DeRtVisibleTable functions [[buffer(3)]],
                      device const uint4 *sbt [[buffer(4)]],uint3 id [[thread_position_in_grid]]) {
    KernelContext_0 ctx;auto p=pt_load(heap,pc,ctx);if(id.x>=p.ptp_width||id.y>=p.ptp_height)return;
    uint pixel=id.y*p.ptp_width+id.x;auto r=pt_camera_0(&p,pixel);
    float3 throughput=1,radiance=0,previous=r.ptr_origin.xyz;float pdf=0;
    if(p.ptp_media.ptm_features==0) {
    for(uint bounce=0;bounce<p.ptp_max_bounces;++bounce) {
        auto h=pt_intersect(heap,p,r,0,intersections,functions,sbt,(constant uchar *)&pc,ctx);
        auto step=pt_step_0(&p,&r,&h,pixel,bounce,pdf,previous,&ctx);
        radiance+=throughput*step.pts_emitted;
        bool visible=true;
        if(step.pts_shadow.ptr_direction.w>step.pts_shadow.ptr_origin.w && any(step.pts_direct>0))
            visible=pt_intersect(heap,p,step.pts_shadow,1,intersections,functions,sbt,(constant uchar *)&pc,ctx).pth_t<0;
        if(visible)radiance+=throughput*step.pts_direct;
        if(step.pts_stop)break;
        throughput*=step.pts_weight;if(!all(isfinite(throughput))||!any(throughput>0))break;
        if(bounce>=3) {float survival=clamp(max(throughput.x,max(throughput.y,throughput.z)),.05f,.95f);
            if(pt_random_0(pixel^p.ptp_seed,p.ptp_sample,2+bounce*12+8)>=survival)break;throughput/=survival;}
        previous=r.ptr_origin.xyz+r.ptr_direction.xyz*h.pth_t;r=step.pts_next;pdf=step.pts_pdf;
    }
    } else {
        auto path=pt_path_init_0(&p,pixel);
        uint limit=p.ptp_max_bounces*(p.ptp_media.ptm_max_steps+5);
        for(uint event=0;event<limit && path.ptpath_finished==0;++event) {
            auto h=pt_intersect(heap,p,path.ptpath_ray,0,intersections,functions,sbt,(constant uchar *)&pc,ctx);
            auto step=pt_path_event_0(&p,&path,&h,pixel,&ctx);
            float3 visibility=1;
            if(step.pts_shadow.ptr_direction.w>step.pts_shadow.ptr_origin.w && any(step.pts_direct>0)) {
                bool clear=pt_intersect(heap,p,step.pts_shadow,1,intersections,functions,sbt,(constant uchar *)&pc,ctx).pth_t<0;
                visibility=clear?pt_path_shadow_0(&p,&path,&step.pts_shadow,&ctx):float3(0);
            }
            pt_path_finish_0(&p,&path,&step,visibility,pixel);
        }
        pt_path_diagnostics_0(&p,&path,&ctx);radiance=path.ptpath_radiance;
    }
    radiance=select(float3(0),radiance,isfinite(radiance));
    auto accum=heap.storage[p.ptp_accumulation];float4 old=p.ptp_sample?accum.read(id.xy):float4(0);
    float4 sum=old+float4(radiance,1);accum.write(sum,id.xy);
    heap.storage[p.ptp_output].write(float4(sum.rgb/max(sum.a,1.f)*p.ptp_exposure,1),id.xy);
}
kernel void pt_env_cdf(constant DeRtHeap &heap [[buffer(0)]],constant PtPush &pc [[buffer(1)]]) {
    KernelContext_0 ctx;auto p=pt_load(heap,pc,ctx);device float2 *cdf=(device float2 *)heap.buffers[p.ptp_env_cdf];
    float total=0;
    for(uint i=0;i<512;++i){float3 wi=pt_env_direction_0((float2(i%32,i/32)+.5f)/float2(32,16));
        float3 value=pt_environment_0(&p,wi,&ctx);float w=max(dot(value,float3(.2126,.7152,.0722)),0.f);cdf[i]=float2(0,w);total+=w;}
    float sum=0;for(uint i=0;i<512;++i){float pdf=total>0?cdf[i].y/total:1.f/512;sum+=pdf;cdf[i]=float2(i==511?1.f:sum,pdf);}
}
kernel void pt_media_stack_probe(constant DeRtHeap &heap [[buffer(0)]],constant uint &output [[buffer(1)]]) {
    uint2 result=pt_stack_probe_0();device uint *dst=(device uint *)heap.buffers[output];dst[0]=result.x;dst[1]=result.y;
}
