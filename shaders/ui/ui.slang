import bindless;
import push_constants;
import "../text/text_common";
struct UiQuad {float4 rect,uv,color,border,params,outline,shadow,shadow_offset;uint texture,sampler,kind,flags;};
struct UiPush {uint data,first;float width,height,white;};
struct UiVertex {float4 position:SV_Position;float2 uv:TEXCOORD0;float2 local:TEXCOORD1;nointerpolation uint index:TEXCOORD2;};
[shader("vertex")]
UiVertex vs_main(uint vertex:SV_VertexID,uint instance:SV_InstanceID,uniform UiPush p){
 float2 c=vertex==0?float2(0,0):vertex==1?float2(1,0):vertex==2?float2(0,1):vertex==3?float2(0,1):vertex==4?float2(1,0):float2(1,1);
 uint index=p.first+instance;UiQuad q=de_robuffer<UiQuad>(p.data)[index];float2 pos=q.rect.xy+c*q.rect.zw;
 UiVertex v;v.position=float4(pos.x/p.width*2-1,1-pos.y/p.height*2,0,1);v.uv=lerp(q.uv.xy,q.uv.zw,c);v.local=c*q.rect.zw;v.index=index;return v;
}
float distance_rect(float2 point,float2 half_size,float radius){float2 d=abs(point)-half_size+radius;return length(max(d,0))+min(max(d.x,d.y),0)-radius;}
[shader("fragment")]
float4 ps_main(UiVertex v,uniform UiPush p):SV_Target{
 UiQuad q=de_robuffer<UiQuad>(p.data)[v.index];float4 color;
 if(q.kind==0){
  float2 half_size=q.rect.zw*.5;float radius=clamp(q.params.x,0,min(half_size.x,half_size.y));float border=clamp(q.params.y,0,min(half_size.x,half_size.y));
  float outer=distance_rect(v.local-half_size,half_size,radius);float aa=max(fwidth(outer),1e-5);float coverage=saturate(.5-outer/aa);
  float inside=distance_rect(v.local-half_size,max(half_size-border,0),max(radius-border,0));float fill=border>0?saturate(.5-inside/aa):coverage;
  float4 a=float4(q.color.rgb*q.color.a,q.color.a)*fill;float4 b=float4(q.border.rgb*q.border.a,q.border.a)*max(coverage-fill,0);color=a+b;
 }else if(q.kind==1){float4 sample=de_tex2d(q.texture).Sample(de_sampler(q.sampler),v.uv);float alpha=sample.a*q.color.a;color=float4(sample.rgb*q.color.rgb*alpha,alpha);}
 else color=de_text_color(q.texture,q.sampler,v.uv,q.color,q.outline,q.shadow,q.params.z,q.params.w,q.shadow_offset.xy,q.flags&1);
 color.rgb*=p.white;return color;
}
