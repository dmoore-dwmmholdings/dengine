import push_constants;
import bindless;
import rt_types;
import "../scene/scene_types";
import "../visbuffer/gbuffer";
import "../materials/material_interface";
struct RtaoParams {
    uint scene,slot,depth,normal;
    uint rays,hits,raw,source;
    uint output,packed,width,height;
    uint tw,th,scale,spp;
    uint frame,pitch,distance_pitch,distance_offset;
    float radius;uint r0,r1,r2;
};
uint hash(uint x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);}
uint2 representative(RtaoParams p,uint2 q) {
    uint2 best=min(q*p.scale,uint2(p.width-1,p.height-1));float z=de_tex2d(p.depth).Load(int3(best,0)).r;
    for(uint y=0;y<p.scale;++y)for(uint x=0;x<p.scale;++x){
        uint2 at=min(q*p.scale+uint2(x,y),uint2(p.width-1,p.height-1));
        float next=de_tex2d(p.depth).Load(int3(at,0)).r;if(next>z){z=next;best=at;}
    }return best;
}
float3 normal_at(RtaoParams p,uint2 q){return gb_decode(float4(0),de_tex2d(p.normal).Load(int3(q,0)),float4(0)).normal;}
float3 position_at(RtaoParams p,uint2 q) {
    DeSceneConstants sc=de_robuffer<DeSceneConstants>(p.scene)[p.slot];
    float4 clip=float4((float2(q)+.5)/float2(p.width,p.height)*float2(2,-2)+float2(-1,1),de_tex2d(p.depth).Load(int3(q,0)).r,1);
    float4 world=float4(dot(sc.inv_view_proj[0],clip),dot(sc.inv_view_proj[1],clip),dot(sc.inv_view_proj[2],clip),dot(sc.inv_view_proj[3],clip));return world.xyz/world.w;
}
float2 sample_hemisphere(uint pixel,uint frame,uint sample,uint spp) {
    // Progressive stratification over64 frames; pixel rotation prevents spatial correlation.
    uint phase=(frame&63u)*spp+sample;
    float2 shift=float2(hash(pixel),hash(pixel^0x68bc21ebu))*(1.0/4294967296.0);
    return frac(float2((phase+.5)/(64.0*spp),reversebits(phase)*(1.0/4294967296.0))+shift);
}
[shader("compute")][numthreads(8,8,1)]
void cs_generate(uint3 id:SV_DispatchThreadID,uniform RtaoParams p) {
    if(id.x>=p.tw || id.y>=p.th)return;uint pixel=id.y*p.tw+id.x;uint2 q=representative(p,id.xy);
    float z=de_tex2d(p.depth).Load(int3(q,0)).r;float3 n=normal_at(p,q),pos=position_at(p,q);
    float3 tangent=normalize(cross(abs(n.z)<.999?float3(0,0,1):float3(0,1,0),n)),bitangent=cross(n,tangent);
    for(uint i=0;i<p.spp;++i){
        float2 u=sample_hemisphere(pixel,p.frame,i,p.spp);float r=sqrt(u.x),phi=6.28318530718*u.y;
        float3 wi=normalize(tangent*(r*cos(phi))+bitangent*(r*sin(phi))+n*sqrt(max(0.0,1-u.x)));
        DeRtRay ray;float bias=max(.0001,.00001*max(abs(pos.x),max(abs(pos.y),abs(pos.z))));
        ray.origin=float4(pos+n*bias,.00001);ray.direction=float4(wi,z>0?p.radius:0);
        de_buffer<DeRtRay>(p.rays)[pixel*p.spp+i]=ray;
    }
}
[shader("compute")][numthreads(8,8,1)]
void cs_resolve(uint3 id:SV_DispatchThreadID,uniform RtaoParams p) {
    if(id.x>=p.tw || id.y>=p.th)return;uint pixel=id.y*p.tw+id.x;float visibility=0,distance=0;
    for(uint i=0;i<p.spp;++i){DeRtHit hit=de_robuffer<DeRtHit>(p.hits)[pixel*p.spp+i];visibility+=hit.t<0?1:0;distance+=hit.t<0?p.radius:hit.t;}
    de_rwtex2d(p.raw)[id.xy]=float4(visibility/p.spp,distance/p.spp,0,0);
}
[shader("compute")][numthreads(8,8,1)]
void cs_filter(uint3 id:SV_DispatchThreadID,uniform RtaoParams p) {
    if(id.x>=p.tw || id.y>=p.th)return;uint2 q=representative(p,id.xy);float z=de_tex2d(p.depth).Load(int3(q,0)).r;
    if(!(z>0)){de_rwtex2d(p.output)[id.xy]=float4(1,p.radius,0,0);return;}
    float3 pos=position_at(p,q),n=normal_at(p,q);float2 value=0;float weight=0;
    for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x){
        int2 at=int2(id.xy)+int2(x,y);if(any(at<0)||at.x>=int(p.tw)||at.y>=int(p.th))continue;
        uint2 neighbor=representative(p,uint2(at));if(!(de_tex2d(p.depth).Load(int3(neighbor,0)).r>0))continue;
        float3 delta=position_at(p,neighbor)-pos;
        if(dot(n,normal_at(p,neighbor))<.95 || abs(dot(delta,n))>max(.005,.01*p.radius))continue;
        value+=de_tex2d(p.source).Load(int3(at,0)).xy;weight+=1;
    }
    de_rwtex2d(p.output)[id.xy]=float4(weight>0?value/weight:float2(1,p.radius),0,0);
}
float2 upsample(RtaoParams p,uint2 q) {
    if(!(de_tex2d(p.depth).Load(int3(q,0)).r>0))return float2(1,p.radius);
    float3 pos=position_at(p,q),n=normal_at(p,q);float2 at=(float2(q)+.5)/p.scale-.5;
    int2 base=int2(floor(at));float2 fracpart=frac(at),value=0;float weight=0;
    for(uint y=0;y<2;++y)for(uint x=0;x<2;++x){
        int2 t=clamp(base+int2(x,y),int2(0),int2(p.tw-1,p.th-1));uint2 r=representative(p,uint2(t));
        float w=(x!=0?fracpart.x:1-fracpart.x)*(y!=0?fracpart.y:1-fracpart.y);
        if(dot(n,normal_at(p,r))<.95 || abs(dot(position_at(p,r)-pos,n))>max(.005,.01*p.radius))continue;
        value+=de_tex2d(p.source).Load(int3(t,0)).xy*w;weight+=w;
    }
    return weight>0?value/weight:de_tex2d(p.source).Load(int3(q/p.scale,0)).xy;
}
[shader("compute")][numthreads(8,8,1)]
void cs_pack(uint3 id:SV_DispatchThreadID,uniform RtaoParams p) {
    uint x=id.x*4,y=id.y;if(x>=p.width || y>=p.height)return;uint ao=0;uint h[4];
    for(uint i=0;i<4;++i){float2 value=x+i<p.width?upsample(p,uint2(x+i,y)):float2(1,p.radius);
        ao|=uint(round(saturate(value.x)*255))<<(i*8);h[i]=f32tof16(value.y);}
    de_rwbytes(p.packed).store(y*p.pitch+x,ao);
    de_rwbytes(p.packed).store(p.distance_offset+y*p.distance_pitch+x*2,h[0]|(h[1]<<16));
    de_rwbytes(p.packed).store(p.distance_offset+y*p.distance_pitch+x*2+4,h[2]|(h[3]<<16));
}
