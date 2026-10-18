import reblur_common;
import push_constants;
struct ReblurPush {uint params,entry;};
[shader("compute")][numthreads(8,8,1)]
void cs_prepare(uint3 id:SV_DispatchThreadID,uniform ReblurPush push){
    ReblurParams p=de_robuffer<ReblurParams>(push.params)[push.entry];if(id.x>=p.width||id.y>=p.height)return;
    DeSceneConstants sc=de_robuffer<DeSceneConstants>(p.scene)[p.slot];DeRtSurface s=reblur_surface(p,id.xy);float3 pos=reblur_position(sc,id.xy,p),factor=reblur_factor(s.material,p.kind);
    float4 raw=de_tex2d(p.raw).Load(int3(id.xy,0));uint2 hit_pixel=id.xy;
    if(p.kind==2&&p.scale>1&&s.material.roughness>=.05&&p.rays!=0){float nearest=-1;for(uint y=0;y<p.scale;++y)for(uint x=0;x<p.scale;++x){uint2 q=min((id.xy/p.scale)*p.scale+uint2(x,y),uint2(p.width-1,p.height-1));DeRtRay ray=de_robuffer<DeRtRay>(p.rays)[q.y*p.width+q.x];float z=de_tex2d(p.depth).Load(int3(q,0)).x;if(ray.direction.w>0&&z>nearest){nearest=z;hit_pixel=q;}}}
    float hit=de_tex2d(p.distance).Load(int3(hit_pixel,0)).x;float2 uv=(float2(id.xy)+.5)/float2(p.width,p.height),motion=de_tex2d(p.motion).Load(int3(id.xy,0)).xy;
    if(p.kind==2&&hit>0&&s.valid){float2 previous=reblur_virtual_uv(sc,pos,s.material.normal,hit,uv+float2(-.5,.5)*motion,s.material.roughness);motion=(uv-previous)*float2(2,-2);}
    de_rwtex2d(p.output)[id.xy]=float4(raw.rgb/factor,raw.a);de_rwtex2d(p.factor)[id.xy]=float4(factor,raw.a);de_rwtex2d(p.position)[id.xy]=float4(pos,1);de_rwtex2d(p.out_motion)[id.xy]=float4(motion,0,0);
    de_rwtex2d(p.out_hit)[id.xy]=float4(hit,0,0,0);
    float view_depth=abs(dot(sc.view[2],float4(pos,1))),normalized=hit/max(hit+view_depth,.001);float radius=p.kind==0?1+3*normalized:(s.material.roughness<.05?0:clamp(s.material.roughness*s.material.roughness*normalized*16,1.0,4.0));de_rwtex2d(p.radius)[id.xy]=float4(radius,0,0,0);de_rwtex2d(p.alpha)[id.xy]=float4(p.kind==0?.05:reblur_roughness_alpha(s.material.roughness),0,0,0);
}
// Deterministic Poisson disk; radius follows the normalized hit-distance guide.
[shader("compute")][numthreads(8,8,1)]
void cs_preblur(uint3 id:SV_DispatchThreadID,uniform ReblurPush push){
    ReblurParams p=de_robuffer<ReblurParams>(push.params)[push.entry];if(id.x>=p.width||id.y>=p.height)return;int2 xy=int2(id.xy);float4 center=de_tex2d(p.input).Load(int3(xy,0));float radius=de_tex2d(p.radius).Load(int3(xy,0)).x;
    if(radius<.5){de_rwtex2d(p.output)[id.xy]=center;return;}
    const float2 disk[8]={float2(-.326,-.406),float2(-.840,-.074),float2(-.696,.457),float2(-.203,.621),float2(.962,-.195),float2(.473,-.480),float2(.519,.767),float2(.185,-.893)};
    float4 sum=center;float weight=1;float mean=0,second=0,count=0;
    for(uint i=0;i<8;++i){int2 q=xy+int2(round(disk[i]*radius*2));if(!reblur_same(p,xy,q))continue;float4 c=de_tex2d(p.input).Load(int3(q,0));sum+=c;weight++;float l=reblur_luma(c.rgb);mean+=l;second+=l*l;count++;}
    mean/=max(count,1);float sigma=sqrt(max(second/max(count,1)-mean*mean,0));float4 value=sum/weight;float l=reblur_luma(value.rgb),limit=mean+3*sigma;
    if(count>2&&l>limit)value.rgb*=limit/max(l,1e-20);de_rwtex2d(p.output)[id.xy]=value;
}
// Bilateral mip pyramid reconstruction, only for histories shorter than four samples.
[shader("compute")][numthreads(8,8,1)]
void cs_mip(uint3 id:SV_DispatchThreadID,uniform ReblurPush push){
    ReblurParams p=de_robuffer<ReblurParams>(push.params)[push.entry];uint w=(p.width+(1u<<p.mip)-1)>>p.mip,h=(p.height+(1u<<p.mip)-1)>>p.mip;if(id.x>=w||id.y>=h)return;
    int2 anchor=min(int2(id.xy<<p.mip),int2(p.width-1,p.height-1));float4 sum=0;float count=0;
    for(uint y=0;y<2;++y)for(uint x=0;x<2;++x){int2 q=int2(id.xy*2+uint2(x,y));int2 full=min(q*int(1u<<(p.mip-1)),int2(p.width-1,p.height-1));if(!reblur_same(p,anchor,full))continue;uint iw=(p.width+(1u<<(p.mip-1))-1)>>(p.mip-1),ih=(p.height+(1u<<(p.mip-1))-1)>>(p.mip-1);if(q.x>=int(iw)||q.y>=int(ih))continue;sum+=de_tex2d(p.input).Load(int3(q,0));count++;}
    de_rwtex2d(p.output)[id.xy]=count>0?sum/count:float4(0);
}
[shader("compute")][numthreads(8,8,1)]
void cs_history_fix(uint3 id:SV_DispatchThreadID,uniform ReblurPush push){
    ReblurParams p=de_robuffer<ReblurParams>(push.params)[push.entry];if(id.x>=p.width||id.y>=p.height)return;int2 xy=int2(id.xy);float4 c=de_tex2d(p.raw).Load(int3(xy,0));float age=de_tex2d(p.length).Load(int3(xy,0)).x;
    int2 q=xy>>int(p.mip),anchor=min(q*int(1u<<p.mip),int2(p.width-1,p.height-1));float4 low=de_tex2d(p.input).Load(int3(q,0));if(p.kind!=0&&gb_decode(float4(0),de_tex2d(p.gb1).Load(int3(xy,0)),float4(0)).roughness<.05){de_rwtex2d(p.output)[id.xy]=c;return;}
    if(age<4&&reblur_same(p,xy,anchor))c=lerp(c,low,(4-age)/4*.5);
    de_rwtex2d(p.output)[id.xy]=c;
}
[shader("compute")][numthreads(8,8,1)]
void cs_remodulate(uint3 id:SV_DispatchThreadID,uniform ReblurPush push){ReblurParams p=de_robuffer<ReblurParams>(push.params)[push.entry];if(id.x>=p.width||id.y>=p.height)return;float4 factor=de_tex2d(p.factor).Load(int3(id.xy,0)),value=de_tex2d(p.input).Load(int3(id.xy,0));de_rwtex2d(p.output)[id.xy]=float4(max(value.rgb,0.0)*factor.rgb,factor.a);}
