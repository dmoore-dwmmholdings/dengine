import bindless;
import push_constants;
import ao_filter;
import "../visbuffer/gbuffer";
import "../scene/scene_types";
import "../lighting/lights";
public struct PolicyParams {
    public uint width,height,full_width,full_height,scale,slot,kind,quality;
    public uint scene,scene_slot,raw,distance,keys,depth,normal,motion;
    public uint material,lights,directions,source,output,key,out_depth,out_normal;
    public uint out_motion,out_material,position,radius,out_position,hit_value,r0,r1;
    public float radius_limit,p0,p1,p2;
};
struct PolicyPush {uint params,entry;};
public float de_shadow_penumbra(float source,float blocker,float light_distance,float ppm,bool directional){
    if(!(blocker>0)||!(source>0))return 0;
    return directional?blocker*tan(min(source,1.55))*ppm:source*blocker/max(light_distance-blocker,.0001)*ppm;
}
float3 world_position(DeSceneConstants scene,uint2 pixel,PolicyParams p,float depth){float4 clip=float4((float2(pixel)+.5)/float2(p.full_width,p.full_height)*float2(2,-2)+float2(-1,1),depth,1);float4 w=float4(dot(scene.inv_view_proj[0],clip),dot(scene.inv_view_proj[1],clip),dot(scene.inv_view_proj[2],clip),dot(scene.inv_view_proj[3],clip));return w.xyz/(abs(w.w)>1e-20?w.w:1e-20);}
uint key_at(PolicyParams p,uint2 pixel){return de_bytes(p.keys).load((p.slot*p.width*p.height+pixel.y*p.width+pixel.x)*4);}
[shader("compute")][numthreads(8,8,1)]
void cs_prepare(uint3 id:SV_DispatchThreadID,uniform PolicyPush push){
    PolicyParams p=de_robuffer<PolicyParams>(push.params)[push.entry];if(id.x>=p.width||id.y>=p.height)return;
    uint2 q=p.kind==0?min(id.xy*p.scale+p.scale/2,uint2(p.full_width-1,p.full_height-1)):de_denoise_ao_representative(p.depth,id.xy,p.scale,uint2(p.full_width,p.full_height));
    DeSceneConstants scene=de_robuffer<DeSceneConstants>(p.scene)[p.scene_slot];float depth=de_tex2d(p.depth).Load(int3(q,0)).x;float3 position=world_position(scene,q,p,depth);
    float4 raw=de_tex2d(p.raw).Load(int3(id.x,id.y+(p.kind==0?p.slot*p.height:0),0));uint key=p.kind==0?key_at(p,id.xy):0;
    float radius=0,distance=p.kind==0?de_tex2d(p.distance).Load(int3(id.x,id.y+p.slot*p.height,0)).x:raw.y;
    if(p.kind==0&&key!=0xffffffffu){float low=1,high=0,blocker=0,count=0;
        for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){int2 a=int2(id.xy)+int2(x,y);if(any(a<0)||a.x>=int(p.width)||a.y>=int(p.height)||key_at(p,uint2(a))!=key)continue;
            float v=de_tex2d(p.raw).Load(int3(a.x,a.y+int(p.slot*p.height),0)).x,d=de_tex2d(p.distance).Load(int3(a.x,a.y+int(p.slot*p.height),0)).x;low=min(low,v);high=max(high,v);if(d>0){blocker+=d;count++;}}
        DeClusterLight light=(key&0x80000000u)!=0?de_robuffer<DeClusterLight>(p.directions)[key&0x7fffffffu]:de_robuffer<DeClusterLight>(p.lights)[key-1];
        float view_depth=abs(dot(scene.view[2],float4(position,1))),ppm=abs(scene.proj[1].y)*p.full_height*.5/max(abs(scene.proj[3].w)>0.5?1:view_depth,.0001)/p.scale;
        uint kind=de_light_kind(light);float source=kind==DE_LIGHT_KIND_RECT||kind==DE_LIGHT_KIND_DISK?max(light.shape.y,light.shape.z):light.direction.w;
        if(high-low>.001)radius=clamp(de_shadow_penumbra(source,blocker/max(count,1),length(light.position.xyz-position),ppm,kind==DE_LIGHT_KIND_DIRECTIONAL),0.0,8.0);
    } else if(p.kind!=0)radius=de_denoise_ao_radius(distance);
    de_rwtex2d(p.output)[id.xy]=p.kind==0?float4(raw.x,0,0,1):float4(raw.xy,0,1);
    de_rwtex2d(p.key)[id.xy]=float4(float(key&65535u),float(key>>16),0,0);de_rwtex2d(p.out_depth)[id.xy]=float4(depth,0,0,0);
    de_rwtex2d(p.out_normal)[id.xy]=de_tex2d(p.normal).Load(int3(q,0));de_rwtex2d(p.out_motion)[id.xy]=de_tex2d(p.motion).Load(int3(q,0));de_rwtex2d(p.out_material)[id.xy]=de_tex2d(p.material).Load(int3(q,0));
    de_rwtex2d(p.out_position)[id.xy]=float4(position,1);de_rwtex2d(p.radius)[id.xy]=float4(radius,0,0,0);
}
[shader("compute")][numthreads(8,8,1)]
void cs_prefilter(uint3 id:SV_DispatchThreadID,uniform PolicyPush push){
    PolicyParams p=de_robuffer<PolicyParams>(push.params)[push.entry];if(id.x>=p.width||id.y>=p.height)return;
    int2 xy=int2(id.xy);float4 center=de_tex2d(p.source).Load(int3(xy,0));float radius=de_tex2d(p.radius).Load(int3(xy,0)).x;
    if(radius<.5){de_rwtex2d(p.output)[id.xy]=center;return;}
    float3 position=de_tex2d(p.position).Load(int3(xy,0)).xyz,normal=gb_decode(float4(0),de_tex2d(p.normal).Load(int3(xy,0)),float4(0)).normal;float4 sum=0;float weight=0;
    int taps=p.quality<=1?1:2,stride=max(1,int(round(radius/2)));
    for(int y=-taps;y<=taps;++y)for(int x=-taps;x<=taps;++x){int2 q=xy+int2(x,y)*stride;if(any(q<0)||q.x>=int(p.width)||q.y>=int(p.height))continue;
        if(any(de_tex2d(p.key).Load(int3(q,0)).xy!=de_tex2d(p.key).Load(int3(xy,0)).xy)||any(de_tex2d(p.material).Load(int3(q,0)).xy!=de_tex2d(p.material).Load(int3(xy,0)).xy))continue;
        float3 delta=de_tex2d(p.position).Load(int3(q,0)).xyz-position;if(abs(dot(delta,normal))>.01)continue;
        float3 n=gb_decode(float4(0),de_tex2d(p.normal).Load(int3(q,0)),float4(0)).normal;if(dot(n,normal)<.95)continue;
        float w=exp(-.5*float(x*x+y*y));sum+=de_tex2d(p.source).Load(int3(q,0))*w;weight+=w;}
    de_rwtex2d(p.output)[id.xy]=weight>0?sum/weight:center;
}
