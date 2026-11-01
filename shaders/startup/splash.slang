// DENG-703: shared cinematic emblem, real font outlines and bounded ray marching.
import bindless;
import push_constants;
struct BrandData { float width,height,time,alpha,text_alpha,pad; uint image,sampler; };
struct SplashVertex { float4 position:SV_Position;float2 uv:TEXCOORD0; };
float4 brandAtlas(float2 uv,BrandData p){return de_tex2d(p.image).SampleLevel(de_sampler(p.sampler),uv,0);}
#include "brand_body.slang"
[shader("vertex")]
SplashVertex vs_main(uint id:SV_VertexID,uniform BrandData p){
 float2 uv=float2(float((id<<1)&2u),float(id&2u));
 SplashVertex v;v.position=float4(uv.x*2-1,1-uv.y*2,0,1);v.uv=uv;return v;
}
[shader("fragment")]
float4 ps_main(SplashVertex v,uniform BrandData p):SV_Target{return brandPixel(v.uv,p);}
