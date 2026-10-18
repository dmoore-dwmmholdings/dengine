import bindless;
import push_constants;
struct PixelData {
 uint src; uint smp; uint iw; uint ih;
 float4 rect;
 float2 residual; float border; float integer_only;
 float4 clear;
};
struct PixelVertex {float4 position:SV_Position;};
[shader("vertex")]
PixelVertex vs_main(uint id:SV_VertexID,uniform PixelData p) {
 float2 uv=float2(float((id<<1)&2),float(id&2));
 PixelVertex v;v.position=float4(uv.x*2-1,1-uv.y*2,0,1);return v;
}
[shader("fragment")]
float4 ps_main(PixelVertex v,uniform PixelData p):SV_Target {
 float2 local=v.position.xy-p.rect.xy;
 if(any(local<0)||any(local>=p.rect.zw))return p.clear;
 float2 scale=p.rect.zw/float2(p.iw,p.ih);
 float2 texel=local/scale+p.residual+p.border;
 if(p.integer_only!=0)texel=floor(texel)+.5;
 else {
  // Sharp bilinear: hold each texel center, interpolate only over one output pixel at edges.
  float2 center=floor(texel)+.5;
  texel=center+clamp((frac(texel)-.5)*scale,-.5,.5);
 }
 return de_tex2d(p.src).SampleLevel(de_sampler(p.smp),texel/(float2(p.iw,p.ih)+2*p.border),0);
}
