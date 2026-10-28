// Native traversal/callable-table adapter only. All reconstruction, material, BSDF and light
// estimators below come from the canonical Slang source through regen_metal.py.
#include "pt_metal.generated"
#include "../common/rt_pipeline_metal.h"
struct PtPush {uint params,entry;};
inline PtParams_0 pt_load(constant DeRtHeap &heap,constant PtPush &pc,thread KernelContext_0 &ctx) {
    ctx={};ctx.de_heap_0=(constant DeBindlessHeap_default_0 *)&heap;
    return pt_load_0(pc.params,pc.entry,&ctx);
}
// Keep the emission-only material branch out of the inlined scattering CFG.
// Instrumented Metal pipelines otherwise spend most of their test budget compiling it.
[[clang::noinline]] float3 pt_endpoint_native(thread const PtParams_0 *p,
                                            thread const DeRtRay_0 *ray,thread const DeRtHit_0 *hit,
                                            float pdf,float3 previous,thread KernelContext_0 *ctx) {
    return pt_endpoint_0(p,ray,hit,pdf,previous,ctx);
}
// Keep per-view light selection and material evaluation out of the traversal loop's CFG too.
[[clang::noinline]] PtStep_0 pt_step_native(thread const PtParams_0 *p,
                                          thread const DeRtRay_0 *ray,thread const DeRtHit_0 *hit,
                                          uint pixel,uint bounce,float pdf,float3 previous,
                                          thread KernelContext_0 *ctx) {
    return pt_step_0(p,ray,hit,pixel,bounce,pdf,previous,ctx);
}
[[visible]] void pt_miss(thread DeRtPayload &p,thread const DeRtHit &,constant DeRtHeap &,constant uchar *) {p.words[0]=as_type<uint>(-1.f);p.words[1]=0xffffffffu;}
[[visible]] void pt_closest(thread DeRtPayload &p,thread const DeRtHit &h,constant DeRtHeap &,constant uchar *) {
    p.words[0]=as_type<uint>(h.distance);p.words[1]=h.custom_index;p.words[2]=h.primitive;
    p.words[3]=as_type<uint>(h.barycentrics.x);p.words[4]=as_type<uint>(h.barycentrics.y);
}
[[visible]]
bool pt_alpha(thread DeRtPayload &payload,thread const DeRtHit &hit,
              constant DeRtHeap &heap,constant uchar *) {
    uint instance=hit.custom_index,primitive=hit.primitive;float2 bary=hit.barycentrics;
    if(payload.words[12]!=0xffffffffu&&instance!=payload.words[12])return false;
    PtPush local={payload.words[13],payload.words[14]};
    KernelContext_0 ctx;ctx={};ctx.de_heap_0=(constant DeBindlessHeap_default_0 *)&heap;auto p=pt_load_0(local.params,local.entry,&ctx);auto q=pt_query_0(&p,&ctx);q.ptq_flags=payload.words[15];
    return de_rt_opacity_0(&q,instance,primitive,bary,&ctx);
}
inline DeRtHit_0 pt_intersect(constant DeRtHeap &heap,thread const PtParams_0 &p,DeRtRay_0 r,uint flags,
                             DeRtIntersectionTable intersections,DeRtVisibleTable functions,
                             device const uint4 *sbt,constant uchar *push,thread KernelContext_0 &ctx,uint owner=0xffffffffu) {
    DeRtPayload payload={};payload.words[15]=flags;payload.words[12]=owner;
    constant PtPush &pc=*(constant PtPush *)push;payload.words[13]=pc.params;payload.words[14]=pc.entry;
    ray native;native.origin=r.ptr_origin.xyz;native.direction=r.ptr_direction.xyz;
    native.min_distance=r.ptr_origin.w;native.max_distance=r.ptr_direction.w;
    auto query=pt_query_0(&p,&ctx);
    de_rt_trace(heap,query.ptq_tlas,native,255,0,payload,intersections,functions,sbt,push,owner!=0xffffffffu);
    DeRtHit_0 h={};h.pth_t=as_type<float>(payload.words[0]);h.pth_instance=payload.words[1];h.pth_primitive=payload.words[2];
    h.pth_u=as_type<float>(payload.words[3]);h.pth_v=as_type<float>(payload.words[4]);
    if(h.pth_t>=0)h.pth_lod=((device const uint *)heap.buffers[query.ptq_lods])[h.pth_instance];return h;
}
inline DeRtHit_0 pt_intersect_sss(constant DeRtHeap &heap,thread const PtParams_0 &p,DeRtRay_0 ray,
                                 PtSssState_0 state,DeRtIntersectionTable intersections,DeRtVisibleTable functions,
                                 device const uint4 *sbt,constant uchar *push,thread KernelContext_0 &ctx) {
    return pt_intersect(heap,p,ray,0,intersections,functions,sbt,push,ctx,
                        state.ptsss_active!=0?state.ptsss_instance:0xffffffffu);
}
inline float3 pt_shadow_trace(constant DeRtHeap &heap,thread const PtParams_0 &p,PtMediumStack_0 media,DeRtRay_0 ray,uint casts,
                             DeRtIntersectionTable intersections,DeRtVisibleTable functions,
                             device const uint4 *sbt,constant uchar *push,thread KernelContext_0 &ctx) {
    auto shadow=pt_shadow_start_0(&p,&media,&ray,&ctx);
    if((p.ptp_media.ptm_features&2u)==0) {
        if(casts!=0&&pt_intersect(heap,p,ray,1,intersections,functions,sbt,push,ctx).pth_t>=0)return float3(0);
        return shadow.ptshadow_value;
    }
    for(uint boundary=0;boundary<64&&shadow.ptshadow_finished==0;++boundary) {
        auto h=pt_intersect(heap,p,ray,0,intersections,functions,sbt,push,ctx);
        pt_shadow_hit_0(&p,&shadow,&ray,&h,casts,&ctx);
    }
    return shadow.ptshadow_finished!=0?shadow.ptshadow_value:float3(0);
}
template<bool WithMedia>
inline void pt_raygen_impl(constant DeRtHeap &heap,constant PtPush &pc,
                      DeRtIntersectionTable intersections,DeRtVisibleTable functions,
                      device const uint4 *sbt,uint3 id) {
    KernelContext_0 ctx;auto p=pt_load(heap,pc,ctx);if(id.x>=p.ptp_width||id.y>=p.ptp_height)return;
    uint pixel=id.y*p.ptp_width+id.x;auto r=pt_camera_0(&p,pixel,&ctx);
    auto first=pt_intersect(heap,p,r,0,intersections,functions,sbt,(constant uchar *)&pc,ctx);
    pt_store_aov_0(&p,&r,&first,pixel,&ctx);
    float3 throughput=1,radiance=0,previous=r.ptr_origin.xyz;float pdf=0;
    if(!WithMedia) {
    for(uint bounce=0;bounce<=p.ptp_max_bounces;++bounce) {
        auto h=bounce==0?first:pt_intersect(heap,p,r,0,intersections,functions,sbt,(constant uchar *)&pc,ctx);
        if(bounce==p.ptp_max_bounces){radiance+=throughput*pt_endpoint_native(&p,&r,&h,pdf,previous,&ctx);break;}
        auto step=pt_step_native(&p,&r,&h,pixel,bounce,pdf,previous,&ctx);
        radiance+=throughput*step.pts_emitted;
        bool visible=true;
        if(step.pts_casts_shadows!=0 && step.pts_shadow.ptr_direction.w>step.pts_shadow.ptr_origin.w && any(step.pts_direct>0))
            visible=pt_intersect(heap,p,step.pts_shadow,1,intersections,functions,sbt,(constant uchar *)&pc,ctx).pth_t<0;
        if(visible)radiance+=throughput*step.pts_direct;
        if(step.pts_stop)break;
        throughput*=step.pts_weight;if(!all(isfinite(throughput))||!any(throughput>0))break;
        if(bounce>=3) {float survival=clamp(max(throughput.x,max(throughput.y,throughput.z)),.05f,.95f);
            if(pt_random_0(p.ptp_sampling_table,pixel^p.ptp_seed,p.ptp_sample,2+bounce*12+8,&ctx)>=survival)break;throughput/=survival;}
        previous=r.ptr_origin.xyz+r.ptr_direction.xyz*h.pth_t;r=step.pts_next;pdf=step.pts_pdf;
    }
    } else {
        auto path=pt_path_init_0(&p,pixel,&ctx);
        uint limit=p.ptp_max_bounces*(p.ptp_media.ptm_max_steps+5);
        for(uint event=0;event<limit && path.ptpath_finished==0;++event) {
            auto h=event==0?first:pt_intersect_sss(heap,p,path.ptpath_ray,path.ptpath_sss,intersections,functions,sbt,(constant uchar *)&pc,ctx);
            auto step=pt_path_event_0(&p,&path,&h,pixel,&ctx);
            float3 visibility=1;
            if(step.pts_shadow.ptr_direction.w>step.pts_shadow.ptr_origin.w && any(step.pts_direct>0)) {
                visibility=pt_shadow_trace(heap,p,step.pts_shadow_media,step.pts_shadow,step.pts_casts_shadows,intersections,functions,sbt,(constant uchar *)&pc,ctx);
            }
            pt_path_finish_0(&p,&path,&step,visibility,pixel,&ctx);
        }
        if(path.ptpath_finished==2) {
            auto h=pt_intersect_sss(heap,p,path.ptpath_ray,path.ptpath_sss,intersections,functions,sbt,(constant uchar *)&pc,ctx);
            path.ptpath_radiance+=pt_path_endpoint_0(&p,&path,&h,&ctx);
        }
        pt_path_diagnostics_0(&p,&path,&ctx);radiance=path.ptpath_radiance;
    }
    radiance=select(float3(0),radiance,isfinite(radiance));
    auto accum=heap.storage[p.ptp_accumulation];float4 old=p.ptp_sample?accum.read(id.xy):float4(0);
    float4 sum=old+float4(radiance,1);accum.write(sum,id.xy);
    auto moments=heap.storage[p.ptp_aov_moments];
    float3 previous_moments=p.ptp_sample?moments.read(id.xy).rgb:float3(0);
    moments.write(float4(pt_sample_moments_0(previous_moments,old.rgb/max(old.a,1.f),radiance,p.ptp_sample),0),id.xy);
    heap.storage[p.ptp_output].write(float4(sum.rgb/max(sum.a,1.f)*p.ptp_exposure,1),id.xy);
}
kernel void pt_raygen(constant DeRtHeap &heap [[buffer(0)]],constant PtPush &pc [[buffer(1)]],
                      DeRtIntersectionTable intersections [[buffer(2)]],DeRtVisibleTable functions [[buffer(3)]],
                      device const uint4 *sbt [[buffer(4)]],uint3 id [[thread_position_in_grid]]) {
    pt_raygen_impl<false>(heap,pc,intersections,functions,sbt,id);
}
kernel void pt_raygen_media(constant DeRtHeap &heap [[buffer(0)]],constant PtPush &pc [[buffer(1)]],
                      DeRtIntersectionTable intersections [[buffer(2)]],DeRtVisibleTable functions [[buffer(3)]],
                      device const uint4 *sbt [[buffer(4)]],uint3 id [[thread_position_in_grid]]) {
    pt_raygen_impl<true>(heap,pc,intersections,functions,sbt,id);
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

// Native traversal acceptance uses the same SSS intersection seam as raygen.
kernel void pt_sss_owner_probe(constant DeRtHeap &heap [[buffer(0)]],constant PtPush &pc [[buffer(1)]],
                              DeRtIntersectionTable intersections [[buffer(2)]],DeRtVisibleTable functions [[buffer(3)]],
                              device const uint4 *sbt [[buffer(4)]],uint3 id [[thread_position_in_grid]]) {
    KernelContext_0 ctx;auto p=pt_load(heap,pc,ctx);DeRtRay_0 r={};
    r.ptr_origin=float4(3,0,-.05f,.00001f);r.ptr_direction=float4(0,0,-1,1);
    PtSssState_0 state={};state.ptsss_active=id.x==0?0:1;state.ptsss_instance=id.x==2?0:(id.x==3?2:(id.x==4?3:1));
    auto h=pt_intersect_sss(heap,p,r,state,intersections,functions,sbt,(constant uchar *)&pc,ctx);
    ((device uint4 *)heap.buffers[p.ptp_output])[id.x]=uint4(as_type<uint>(h.pth_t),h.pth_instance,h.pth_primitive,h.pth_lod);
}

// Focused sampler acceptance without compiling the full ray-generation pipeline.
kernel void pt_sampling_probe(constant DeRtHeap &heap [[buffer(0)]],constant uint4 &pc [[buffer(1)]],
                              uint3 id [[thread_position_in_grid]]) {
    KernelContext_0 ctx={};ctx.de_heap_0=(constant DeBindlessHeap_default_0 *)&heap;
    float3 u=pt_random3_0(pc.x,0,id.x,pc.z,&ctx);
    ((device float4 *)heap.buffers[pc.y])[pc.w*256+id.x]=float4(u,0);
}

// Native acceptance exercises the production shadow-transmittance seam.
kernel void pt_shadow_boundary_probe(constant DeRtHeap &heap [[buffer(0)]],constant PtPush &pc [[buffer(1)]],
                              DeRtIntersectionTable intersections [[buffer(2)]],DeRtVisibleTable functions [[buffer(3)]],
                              device const uint4 *sbt [[buffer(4)]],uint3 id [[thread_position_in_grid]]) {
    KernelContext_0 ctx;auto p=pt_load(heap,pc,ctx);DeRtRay_0 ray={};PtPath_0 path={};
    ray.ptr_origin=float4(id.x==5?20:3,0,(id.x==1||id.x==2||id.x==4)?-.05f:.1f,.00001f);
    ray.ptr_direction=float4(0,0,id.x==4?1:-1,id.x==3?.2f:1.f);
    if(id.x==1||id.x==4){PtMedium_0 m={1,float3(.8f,.6f,.4f),1};pt_medium_enter_0(&path.ptpath_media,&m);}
    float3 visibility=pt_shadow_trace(heap,p,path.ptpath_media,ray,id.x==6?1:0,intersections,functions,sbt,(constant uchar *)&pc,ctx);
    ((device float4 *)heap.buffers[p.ptp_output])[id.x]=float4(visibility,1);
}

// Proposal acceptance: an emitter normal must never replace the receiver normal.
kernel void pt_emissive_origin_probe(constant DeRtHeap &heap [[buffer(0)]],constant uint4 &pc [[buffer(1)]],
                                    uint3 id [[thread_position_in_grid]]) {
    KernelContext_0 ctx={};ctx.de_heap_0=(constant DeBindlessHeap_default_0 *)&heap;
    PtParams_0 p={};p.ptp_sampling_table=pc.x;p.ptp_query=pc.z;p.ptp_sources=pc.w;
    p.ptp_source_count=1;p.ptp_sample=id.x%256;
    float3 position=id.x<256?float3(as_type<float>(as_type<uint>(100.f)-2u),0,1):float3(100,0,2.01);
    float3 normal=id.x<256?float3(1,0,0):float3(0,0,-1);
    auto light=pt_light_sample_0(&p,position,normal,0,0,&ctx);
    ((device float4 *)heap.buffers[pc.y])[2*id.x]=float4(light.ptlight_shadow.ptr_origin.xyz,light.ptlight_shadow.ptr_direction.w);
    ((device float4 *)heap.buffers[pc.y])[2*id.x+1]=float4(light.ptlight_shadow.ptr_direction.xyz,light.ptlight_pdf);
}

// Production subsurface entry coordinates must not reuse an earlier event's draws.
kernel void pt_sss_sampling_probe(constant DeRtHeap &heap [[buffer(0)]],constant uint &output [[buffer(1)]],
                                 uint3 id [[thread_position_in_grid]]) {
    KernelContext_0 ctx={};ctx.de_heap_0=(constant DeBindlessHeap_default_0 *)&heap;
    uint event=id.x<128?id.x+1:64u*(1024u+5u)-(id.x-128u);
    uint3 dimensions=pt_sss_entry_dimensions_0(event);
    uint3 later=pt_sss_entry_dimensions_0(2*event+1);
    float direction=pt_random_0(0,17,id.x,dimensions.y,&ctx);
    float branch=pt_random_0(0,17,id.x,later.x,&ctx);
    ((device uint4 *)heap.buffers[output])[id.x]=uint4(dimensions,as_type<uint>(direction)==as_type<uint>(branch));
}
