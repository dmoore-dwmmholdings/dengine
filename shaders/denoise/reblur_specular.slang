import reblur_common;
import push_constants;
struct ReblurPush {uint params,entry;};
[shader("compute")][numthreads(8,8,1)]
void cs_anisotropic(uint3 id:SV_DispatchThreadID,uniform ReblurPush push){
    ReblurParams p=de_robuffer<ReblurParams>(push.params)[push.entry];if(id.x>=p.width||id.y>=p.height)return;int2 xy=int2(id.xy);float4 center=de_tex2d(p.input).Load(int3(xy,0));DeRtSurface s=reblur_surface(p,id.xy);
    if(!s.valid||s.material.roughness<.05){de_rwtex2d(p.output)[id.xy]=center;return;}
    DeSceneConstants sc=de_robuffer<DeSceneConstants>(p.scene)[p.slot];float3 position=de_tex2d(p.position).Load(int3(xy,0)).xyz,incident=normalize(position-sc.camera_position.xyz),axis=reflect(incident,s.material.normal);
    float4 clip0=de_vis_clip(sc.view_proj,position),clip1=de_vis_clip(sc.view_proj,position+axis);float2 projected=(clip1.xy/max(abs(clip1.w),1e-20)-clip0.xy/max(abs(clip0.w),1e-20))*float2(1,-1);float2 tangent=length(projected)>1e-5?normalize(projected):float2(1,0),bitangent=float2(-tangent.y,tangent.x);
    float radius=de_tex2d(p.radius).Load(int3(xy,0)).x,hit=de_tex2d(p.distance).Load(int3(xy,0)).x;float4 sum=center;float weight=1;
    for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x){if(x==0&&y==0)continue;int2 q=xy+int2(round((tangent*x*2+bitangent*y)*radius));if(!reblur_same(p,xy,q))continue;
        DeGbSurface n=gb_decode(float4(0),de_tex2d(p.gb1).Load(int3(q,0)),float4(0));float next=de_tex2d(p.distance).Load(int3(q,0)).x;
        float w=exp(-.5*float(x*x+y*y))*pow(max(dot(s.material.normal,n.normal),0),64)*exp(-abs(next-hit)/max(.1,hit*.2))*exp(-abs(n.roughness-s.material.roughness)*32);sum+=de_tex2d(p.input).Load(int3(q,0))*w;weight+=w;}
    de_rwtex2d(p.output)[id.xy]=sum/weight;
}
