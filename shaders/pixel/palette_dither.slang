import bindless;
import push_constants;
struct PaletteData {uint src;uint dst;uint lut;uint width;uint height;float strength;int phase_x;int phase_y;};
uint bayer(uint2 xy) {
 uint value=0;
 for(uint bit=0;bit<3;++bit){uint bx=(xy.x>>bit)&1,by=(xy.y>>bit)&1;value|=((bx^by)*2+by)<<(4-bit*2);}
 return value;
}
[shader("compute")][numthreads(8,8,1)]
void cs_main(uint3 id:SV_DispatchThreadID,uniform PaletteData p) {
 if(id.x>=p.width||id.y>=p.height)return;
 float4 color=de_tex2d(p.src).Load(int3(id.xy,0));
 float threshold=(float(bayer(uint2(int2(id.xy)+int2(p.phase_x,p.phase_y))))+.5)/64-.5;
 float3 rgb=saturate(color.rgb+threshold*p.strength);
 if(p.lut!=0) {
 uint3 q=uint3(round(rgb*31));float4 chosen=asfloat(de_bytes(p.lut).load4(((q.z*32+q.y)*32+q.x)*16));
 color.rgb=chosen.rgb;
 }else color.rgb=rgb;
 de_rwtex2d(p.dst)[id.xy]=color;
}
