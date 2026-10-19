module ui_common;
import bindless;
import "../text/text_common";
public struct UiQuad {public float4 rect,uv,color,border,params,outline,shadow,shadow_offset;public uint texture,sampler,kind,flags;};
public float de_ui_distance_rect(float2 point,float2 half_size,float radius){float2 d=abs(point)-half_size+radius;return length(max(d,0))+min(max(d.x,d.y),0)-radius;}
public float4 de_ui_sample(UiQuad q,float2 uv,float2 local){
 if(q.kind!=0 && q.texture==0xffffffffu) return float4(0);
 float4 color;
 if(q.kind==0){
  float2 half_size=q.rect.zw*.5;float radius=clamp(q.params.x,0,min(half_size.x,half_size.y));float border=clamp(q.params.y,0,min(half_size.x,half_size.y));
  float outer=de_ui_distance_rect(local-half_size,half_size,radius);float aa=max(fwidth(outer),1e-5);float coverage=saturate(.5-outer/aa);
  float inside=de_ui_distance_rect(local-half_size,max(half_size-border,0),max(radius-border,0));float fill=border>0?saturate(.5-inside/aa):coverage;
  float4 a=float4(q.color.rgb*q.color.a,q.color.a)*fill;float4 b=float4(q.border.rgb*q.border.a,q.border.a)*max(coverage-fill,0);color=a+b;
 }else if(q.kind==1){float4 sample=de_tex2d(q.texture).Sample(de_sampler(q.sampler),uv);float alpha=sample.a*q.color.a;color=float4(sample.rgb*q.color.rgb*alpha,alpha);}
 else color=de_text_color(q.texture,q.sampler,uv,q.uv,q.color,q.outline,q.shadow,q.params.z,q.params.w,q.shadow_offset.xy,q.flags&1);
 return color;
}
