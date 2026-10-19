module text_common;
import bindless;
public float de_text_median(float3 v){return max(min(v.r,v.g),min(max(v.r,v.g),v.b));}
public float de_text_range(float2 uv,float2 atlas_size,float range){float2 screen=1.0/max(fwidth(uv),float2(1e-6));return max(.5*dot(float2(range)/atlas_size,screen),1.0);}
public float4 de_text_color(uint texture,uint sampler,float2 uv,float4 uv_bounds,float4 tint,float4 outline,float4 shadow,float range,float width,float2 shadow_offset,uint bitmap){
 uint w,h;de_tex2d(texture).GetDimensions(w,h);float2 size=float2(w,h);
 // Keep bilinear footprints inside this glyph's allocation, including its padding.
 float2 lo=uv_bounds.xy+.5/size,hi=max(lo,uv_bounds.zw-.5/size);
 float4 texel=de_tex2d(texture).Sample(de_sampler(sampler),clamp(uv,lo,hi));
 if(bitmap!=0)return float4(tint.rgb*texel.rgb*tint.a*texel.a,tint.a*texel.a);
 float pr=de_text_range(uv,size,range);float distance=de_text_median(texel.rgb)-.5;float fill=saturate(distance*pr+.5);float edge=saturate((distance+width/max(pr,1.0))*pr+.5);
 float2 shadow_uv=uv-shadow_offset*fwidth(uv);
 bool shadow_inside=all(shadow_uv>=uv_bounds.xy)&&all(shadow_uv<=uv_bounds.zw);
 float sdf=de_tex2d(texture).Sample(de_sampler(sampler),clamp(shadow_uv,lo,hi)).a-.5;
 float shadow_alpha=shadow_inside?saturate(sdf*pr+.5)*shadow.a:0;
 float4 bg=float4(shadow.rgb*shadow_alpha,shadow_alpha);float oa=edge*outline.a;float4 rim=float4(outline.rgb*oa,oa);bg=rim+bg*(1-rim.a);float fa=fill*tint.a;return float4(tint.rgb*fa,fa)+bg*(1-fa);
}
