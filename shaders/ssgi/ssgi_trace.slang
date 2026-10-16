module ssgi_trace;
import bindless;
import "../ssr/ssr_common";
import "../ssr/hiz_trace";
// SsrParams is the shared 128-byte screen input layout; max_roughness carries cosine ray count
// for this consumer. The closest-depth tracer and scene history remain renderer-owned.
[shader("compute")][numthreads(8,8,1)]
void cs_ssgi_trace(uint3 id:SV_DispatchThreadID,uniform SsrParams pc) {
    if(id.x>=pc.tw||id.y>=pc.th)return;
    de_rwtex2d(pc.hits)[id.xy]=float4(0);
    de_rwtex2d(pc.rays)[id.xy]=float4(0);
    if((pc.history_valid&1u)==0)return;
    uint2 pixel=min(uint2((float2(id.xy)+.5)*float2(pc.width,pc.height)/float2(pc.tw,pc.th)),uint2(pc.width-1,pc.height-1));
    float2 uv=(float2(pixel)+.5)/float2(pc.width,pc.height);
    float depth=de_tex2d(pc.depth).Load(int3(pixel,0)).r;
    SurfaceData surface=ssr_surface(pc,pixel);
    if(!(depth>0)||(surface.features&DE_MATERIAL_FEATURE_UNLIT)!=0)return;
    DeSceneConstants sc=de_robuffer<DeSceneConstants>(pc.scene)[pc.slot];
    float3 position=ssr_position(sc,uv,depth);
    ShadingFrame frame=de_surface_frame(surface);
    float3 sum=0;float coverage=0;
    uint rays=clamp(uint(pc.max_roughness),1u,8u);
    float2 scramble=float2(de_u32_to_unit(de_pcg_hash(pixel.x+pixel.y*pc.width)),de_u32_to_unit(de_pcg_hash(pixel.y+pixel.x*pc.height+31)));
    for(uint ray=0;ray<rays;++ray) {
        float2 random=frac(de_r2(pc.frame*rays+ray)+scramble);
        float3 direction=frame.to_world(de_sample_cosine_hemisphere(random));
        float3 start=position+surface.normal*max(.002,pc.thickness*.05);
        float distance=100;
        float vz=dot(sc.view[2],float4(start,1)),dz=dot(sc.view[2].xyz,direction);
        if(dz>0)distance=min(distance,max((-pc.near_z*1.01-vz)/dz,0));
        float4 a=float4(dot(sc.view_proj[0],float4(start,1)),dot(sc.view_proj[1],float4(start,1)),dot(sc.view_proj[2],float4(start,1)),dot(sc.view_proj[3],float4(start,1)));
        float4 b=float4(dot(sc.view_proj[0],float4(start+direction*distance,1)),dot(sc.view_proj[1],float4(start+direction*distance,1)),dot(sc.view_proj[2],float4(start+direction*distance,1)),dot(sc.view_proj[3],float4(start+direction*distance,1)));
        if(!(a.w>0)||!(b.w>0))continue;
        float3 origin=float3(a.x/a.w*.5+.5,.5-a.y/a.w*.5,a.z/a.w);
        float3 end=float3(b.x/b.w*.5+.5,.5-b.y/b.w*.5,b.z/b.w);
        DeHiZHit hit=de_hiz_trace(pc.hzb,pc.depth,uint2(pc.hzb_width,pc.hzb_height),pc.mips,uint2(pc.width,pc.height),origin,end-origin,pc.near_z,pc.thickness,sc.proj[3].w>.5?abs(sc.proj[2].z):0,pc.max_steps);
        if(!(hit.confidence>0))continue;
        uint2 hp=min(uint2(hit.uv*float2(pc.width,pc.height)),uint2(pc.width-1,pc.height-1));
        SurfaceData hs=ssr_surface(pc,hp);
        if(dot(hs.normal,-direction)<.05)continue;
        float3 hit_position=ssr_position(sc,hit.uv,hit.depth);
        float2 previous=ssr_previous_uv(hit.uv,de_tex2d(pc.motion).Load(int3(hp,0)).xy);
        if(!ssr_history_valid(pc,sc,previous,hit_position,hs.normal))continue;
        float exposure=max(sc.pre_exposure,1e-8);
        float3 color=ssr_history_load(pc.history_color,previous,uint2(pc.width,pc.height)).rgb*(exposure/max(pc.previous_exposure,1e-8))/exposure;
        if(!all(isfinite(color)))continue;
        sum+=max(color,float3(0));coverage+=1;
        de_rwtex2d(pc.hits)[id.xy]=float4(hit.uv,hit.depth,1);
    }
    // Cosine importance sampling cancels cos/pdf, giving E=pi*average(L). Keep conditional
    // incident radiance here; resolve applies the receiving material once. Miss coverage falls
    // back to that surface's static-volume/sky response at lighting composition.
    if(coverage>0)de_rwtex2d(pc.rays)[id.xy]=float4(sum/coverage,coverage/float(rays));
}
