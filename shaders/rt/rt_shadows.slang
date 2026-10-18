import push_constants;
import light_sampling;
import rt_types;
import "../lighting/lighting_common";
import "../visbuffer/gbuffer";
import "../scene/scene_types";
struct Params {
    uint scene,scene_slot,view,entry;
    uint depth,normal,lights,dir_lights;
    uint grid,list,counters,rays;
    uint hits,keys,visibility,distance;
    uint source,output,width,height;
    uint full_width,full_height,slots,slot;
    uint scale,spp,frame,axis;
};
float3 position(DeSceneConstants sc,uint2 p,Params pc,float depth) {
    float4 clip=float4((float2(p)+0.5)/float2(pc.full_width,pc.full_height)*float2(2,-2)+float2(-1,1),depth,1);
    float4 w=float4(dot(sc.inv_view_proj[0],clip),dot(sc.inv_view_proj[1],clip),dot(sc.inv_view_proj[2],clip),dot(sc.inv_view_proj[3],clip));return w.xyz/w.w;
}
[shader("compute")][numthreads(8,8,1)]
void cs_generate(uint3 id:SV_DispatchThreadID,uniform Params pc) {
    if(id.x>=pc.width||id.y>=pc.height)return;
    uint pixel=id.y*pc.width+id.x;uint2 p=min(id.xy*pc.scale+pc.scale/2,uint2(pc.full_width-1,pc.full_height-1));
    DeSceneConstants sc=de_robuffer<DeSceneConstants>(pc.scene)[pc.scene_slot];
    DeLightingView lv=de_robuffer<DeLightingView>(pc.view)[pc.entry];
    float depth=de_tex2d(pc.depth).Load(int3(p,0)).r;
    float3 world=position(sc,p,pc,depth);
    DeGbSurface surface=gb_decode(float4(0),de_tex2d(pc.normal).Load(int3(p,0)),float4(0));
    uint key=0xffffffffu,selected=0;DeClusterLight light=(DeClusterLight)0;
    uint dirs=min(de_bytes(pc.counters).load(DE_LIGHTING_COUNTER_DIRECTIONAL),4u);
    for(uint j=0;j<dirs;++j) {
        DeClusterLight l=de_robuffer<DeClusterLight>(pc.dir_lights)[j];
        if((asuint(l.axis.w)&DE_CLUSTER_LIGHT_SHADOWS)!=0) {
            if(selected++==pc.slot){light=l;key=0x80000000u|j;}
        }
    }
    float view_depth=-dot(sc.view[2],float4(world,1));
    uint cluster=de_cluster_index(lv,p,max(view_depth,lv.near));
    uint2 cell=de_robuffer<uint2>(pc.grid)[cluster];
    for(uint j=0;j<cell.y && selected<=pc.slot && pc.axis==0;++j) {
        uint index=de_robuffer<uint>(pc.list)[cell.x+j];DeClusterLight l=de_robuffer<DeClusterLight>(pc.lights)[index];
        if((asuint(l.axis.w)&DE_CLUSTER_LIGHT_SHADOWS)!=0 && (asuint(l.axis.w)&DE_CLUSTER_LIGHT_PROXY)==0) {
            if(selected++==pc.slot){light=l;key=index+1;}
        }
    }
    if(!(depth>0))key=0xffffffffu;
    de_rwbytes(pc.keys).store((pc.slot*pc.width*pc.height+pixel)*4,key);
    for(uint sample=0;sample<pc.spp;++sample) {
        DeRtRay ray=de_rt_shadow_ray(light,world,surface.normal,de_rt_random(pixel,pc.frame,sample+pc.slot*2));
        if(key==0xffffffffu){ray.origin=float4(0);ray.direction=float4(0,0,1,0);}
        de_buffer<DeRtRay>(pc.rays)[pixel*pc.spp+sample]=ray;
    }
}
[shader("compute")][numthreads(8,8,1)]
void cs_resolve(uint3 id:SV_DispatchThreadID,uniform Params pc) {
    if(id.x>=pc.width||id.y>=pc.height)return;
    uint pixel=id.y*pc.width+id.x;float vis=0,dist=0;
    for(uint sample=0;sample<pc.spp;++sample) {
        float t=de_robuffer<DeRtHit>(pc.hits)[pixel*pc.spp+sample].t;
        vis+=t<0?1:0;dist+=max(t,0.0);
    }
    uint2 at=id.xy+uint2(0,pc.slot*pc.height);
    de_rwtex2d(pc.visibility)[at]=float4(vis/pc.spp);
    de_rwtex2d(pc.distance)[at]=float4(dist/pc.spp);
}
// Temporary separable 3x3 spatial filter. DENG-69 replaces it with the shadow denoiser.
[shader("compute")][numthreads(8,8,1)]
void cs_blur(uint3 id:SV_DispatchThreadID,uniform Params pc) {
    if(id.x>=pc.width || id.y>=pc.height*pc.slots)return;
    uint slot=id.y/pc.height;uint2 p=uint2(id.x,id.y%pc.height);
    uint key=de_bytes(pc.keys).load((slot*pc.width*pc.height+p.y*pc.width+p.x)*4);
    float sum=0,weight=0;
    for(int d=-1;d<=1;++d) {
        int2 q=int2(p)+(pc.axis==0?int2(d,0):int2(0,d));
        if(any(q<0)||q.x>=int(pc.width)||q.y>=int(pc.height))continue;
        if(de_bytes(pc.keys).load((slot*pc.width*pc.height+uint(q.y)*pc.width+uint(q.x))*4)!=key)continue;
        float w=d==0?2:1;sum+=de_tex2d(pc.source).Load(int3(q.x,q.y+int(slot*pc.height),0)).r*w;weight+=w;
    }
    de_rwtex2d(pc.output)[id.xy]=float4(sum/max(weight,1));
}
