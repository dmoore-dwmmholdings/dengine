// GGX Hammersley/PDF mapping matches DENG-100 env_prefilter.c.
module probe_prefilter;
import bindless;
import "../lighting/ibl";
struct ProbeFilterParams { uint src; uint dst; uint size; uint face; uint mip; uint mips; uint sampler; uint samples; uint4 raw_mips[3]; };
float3 face_dir(uint f, float2 st) {
    if(f==0) return normalize(float3(1,-st.y,-st.x));
    if(f==1) return normalize(float3(-1,-st.y,st.x));
    if(f==2) return normalize(float3(st.x,1,st.y));
    if(f==3) return normalize(float3(st.x,-1,-st.y));
    if(f==4) return normalize(float3(st.x,-st.y,1));
    return normalize(float3(-st.x,-st.y,-1));
}
// Storage-only source views stay GENERAL while building and filtering the chain. Manual
// face-clamped bilinear/trilinear filtering matches DENG-100 without sampling a full-chain
// SHADER_READ view while another mip is being written.
float3 source_bilinear(ProbeFilterParams pc,uint face,float2 st,uint mip) {
    uint n=max(pc.size>>mip,1u);
    float2 pixel=(st*.5+.5)*float(n)-.5;
    int2 lo=int2(floor(pixel)),hi=lo+1;
    float2 fraction=frac(pixel);
    lo=clamp(lo,int2(0),int2(int(n)-1));hi=clamp(hi,int2(0),int2(int(n)-1));
    int offset=int(face*n);
    uint source=pc.raw_mips[mip/4][mip%4];
    float3 a=de_rwtex2d(source)[int2(lo.x,lo.y+offset)].rgb;
    float3 b=de_rwtex2d(source)[int2(hi.x,lo.y+offset)].rgb;
    float3 c=de_rwtex2d(source)[int2(lo.x,hi.y+offset)].rgb;
    float3 d=de_rwtex2d(source)[int2(hi.x,hi.y+offset)].rgb;
    return lerp(lerp(a,b,fraction.x),lerp(c,d,fraction.x),fraction.y);
}
float3 radiance(ProbeFilterParams pc,float3 d,float lod) {
    float2 st;uint face=de_ibl_cube_face(d,st);
    uint lo=uint(floor(lod)),hi=min(lo+1,pc.mips-1);
    return lerp(source_bilinear(pc,face,st,lo),source_bilinear(pc,face,st,hi),frac(lod));
}
[shader("compute")] [numthreads(8,8,1)]
void cs_probe_copy(uint3 id:SV_DispatchThreadID,uniform ProbeFilterParams pc) {
    if(any(id.xy>=pc.size)) return;
    de_rwtex2d(pc.dst)[int2(id.x,id.y+pc.face*pc.size)]=de_tex2d(pc.src).Load(int3(id.xy,0));
}
[shader("compute")] [numthreads(8,8,1)]
void cs_probe_downsample(uint3 id:SV_DispatchThreadID,uniform ProbeFilterParams pc) {
    if(id.x>=pc.size || id.y>=pc.size*6) return;
    uint2 q=id.xy*2;
    float4 v=(de_rwtex2d(pc.src)[q]+de_rwtex2d(pc.src)[q+uint2(1,0)]+
              de_rwtex2d(pc.src)[q+uint2(0,1)]+de_rwtex2d(pc.src)[q+1])*.25;
    de_rwtex2d(pc.dst)[id.xy]=v;
}
[shader("compute")] [numthreads(8,8,1)]
void cs_probe_prefilter(uint3 id:SV_DispatchThreadID,uniform ProbeFilterParams pc) {
    uint n=max(pc.size>>pc.mip,1u);
    if(id.x>=n || id.y>=6*n) return;
    uint face=id.y/n; float2 st=(float2(id.x,id.y%n)+.5)/float(n)*2-1;
    float3 normal=face_dir(face,st);
    if(pc.mip==0) { de_rwtex2d(pc.dst)[id.xy]=float4(radiance(pc,normal,0),1); return; }
    float rough=float(pc.mip)/float(pc.mips-1), a=rough*rough, a2=a*a;
    // Duff et al. 2017 frame, identical to DENG-100's CPU reference sample orientation.
    float sign=normal.z>=0?1.0:-1.0;
    float aa=-1.0/(sign+normal.z), bb=normal.x*normal.y*aa;
    float3 tangent=float3(1+sign*normal.x*normal.x*aa,sign*bb,-sign*normal.x);
    float3 bitangent=float3(bb,sign+normal.y*normal.y*aa,-normal.y);
    float3 total=0; float weight=0;
    for(uint k=0;k<pc.samples;++k) {
        float u=float(k)/float(pc.samples), v=float(reversebits(k))*2.3283064365386963e-10;
        float phi=6.283185307179586*u;
        float cosTheta=sqrt((1-v)/(1+(a2-1)*v)), sinTheta=sqrt(max(0.0,1-cosTheta*cosTheta));
        float3 h=normalize(tangent*(cos(phi)*sinTheta)+bitangent*(sin(phi)*sinTheta)+normal*cosTheta);
        float3 l=normalize(2*dot(normal,h)*h-normal); float nl=dot(normal,l);
        if(nl>0) {
            float nh=max(dot(normal,h),0), vh=max(dot(normal,h),1e-8);
            float denom=nh*nh*(a2-1)+1;
            float pdf=(a2/(3.141592653589793*denom*denom))*nh/(4*vh);
            float omegaS=1/(float(pc.samples)*max(pdf,1e-8));
            float omegaP=12.566370614359172/(6*float(pc.size*pc.size));
            float lod=clamp(.5*log2(omegaS/omegaP)+1,0,float(pc.mips-1));
            total+=radiance(pc,l,lod)*nl; weight+=nl;
        }
    }
    de_rwtex2d(pc.dst)[id.xy]=float4(total/max(weight,1e-8),1);
}
