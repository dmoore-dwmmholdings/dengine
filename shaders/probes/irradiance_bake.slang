module irradiance_bake;
import bindless;
import irradiance_sample;
struct GiBakeParams {
    uint raw, distances, irradiance, moments;
    uint probe, columns, face, depth;
    float near_z, range; uint operation, probes;
};
float3 gi_face(uint f,float2 st) {
    if(f==0)return normalize(float3(1,-st.y,-st.x));
    if(f==1)return normalize(float3(-1,-st.y,st.x));
    if(f==2)return normalize(float3(st.x,1,st.y));
    if(f==3)return normalize(float3(st.x,-1,-st.y));
    if(f==4)return normalize(float3(st.x,-st.y,1));
    return normalize(float3(-st.x,-st.y,-1));
}
[shader("compute")][numthreads(8,8,1)]
void cs_gi_distance(uint3 id:SV_DispatchThreadID,uniform GiBakeParams pc) {
    if(any(id.xy>=32))return;
    float d=de_tex2d(pc.depth).Load(int3(id.xy,0)).r;
    float2 st=(float2(id.xy)+.5)/32*2-1;
    // Perspective reverse-Z: axial distance near/depth; radial factor accounts for oblique rays.
    float radial=d>0?pc.near_z/d*sqrt(1+dot(st,st)):pc.range;
    radial=isfinite(radial)?clamp(radial/pc.range,0,1):1;
    de_rwtex2d(pc.distances)[uint2(id.x,id.y+pc.face*32)]=float4(radial,0,0,0);
}
// Quantize explicitly: native storage conversions can choose different rounding. Exact
// representable values make rebakes portable and preserve the documented finite saturation.
float gi_quantize(float value,uint mantissa) {
    if(!isfinite(value))value=value>0?60000:0;
    value=clamp(value,0,60000);
    uint bits=asuint(value),exponent=(bits>>23)&255u;
    uint drop=23u-mantissa;
    if(exponent>=113u)return asfloat(bits&~((1u<<drop)-1u));
    uint shift=(113u-exponent)+drop;
    uint fraction=shift>=32u?0u:((bits&0x7fffffu)|0x800000u)>>shift;
    return float(fraction)*asfloat((127u-14u-mantissa)<<23);
}
[shader("compute")][numthreads(8,8,1)]
void cs_gi_integrate(uint3 id:SV_DispatchThreadID,uniform GiBakeParams pc) {
    if(any(id.xy>=18))return;
    float3 mnormal=gi_oct_decode((float2(gi_border(id.xy,16))-.5)/16);
    float3 inormal=gi_oct_decode((float2(gi_border(id.xy,8))-.5)/8);
    float3 radiance=0;float cosine_total=0;float2 moments=0;float moment_total=0;
    for(uint face=0;face<6;++face)for(uint y=0;y<32;++y)for(uint x=0;x<32;++x) {
        float2 st=(float2(x,y)+.5)/32*2-1;
        float3 direction=gi_face(face,st);
        float solid_angle=4.0/(32*32*pow(1+dot(st,st),1.5));
        float w=max(dot(mnormal,direction),0);w=pow(w,512)*solid_angle;
        float distance=de_rwtex2d(pc.distances)[uint2(x,y+face*32)].r;
        moments+=float2(distance,distance*distance)*w;moment_total+=w;
        if(all(id.xy<10)) {
            float c=max(dot(inormal,direction),0)*solid_angle;
            float3 sample=de_rwtex2d(pc.raw)[uint2(x,y+face*32)].rgb;
            radiance+=max(float3(0),all(isfinite(sample))?sample:float3(0))*c;
            cosine_total+=c;
        }
    }
    uint2 tile=uint2(pc.probe%pc.columns,pc.probe/pc.columns);
    // True irradiance (not E/pi). Normalize cubature to pi to preserve a constant environment.
    if(all(id.xy<10)) {
        float3 e=clamp(radiance*3.141592653589793/max(cosine_total,1e-8),0,60000);
        de_rwtex2d(pc.irradiance)[tile*10+id.xy]=float4(gi_quantize(e.x,6),gi_quantize(e.y,6),gi_quantize(e.z,5),1);
    }
    de_rwtex2d(pc.moments)[tile*18+id.xy]=float4(saturate(moments/max(moment_total,1e-8)),0,0);
}

[shader("compute")][numthreads(8,8,1)]
void cs_gi_clear(uint3 id:SV_DispatchThreadID,uniform GiBakeParams pc) {
    if(id.x>=pc.columns||id.y>=pc.probes)return;
    de_rwtex2d(pc.irradiance)[id.xy]=pc.operation==0?float4(0):float4(1,1,0,0);
}

[shader("compute")][numthreads(64,1,1)]
void cs_gi_quantize(uint3 id:SV_DispatchThreadID,uniform GiBakeParams pc) {
    if(id.x>=pc.probes)return;
    float3 value=de_robuffer<float4>(pc.raw)[id.x].rgb;
    de_rwtex2d(pc.irradiance)[uint2(id.x,0)]=float4(gi_quantize(value.x,6),gi_quantize(value.y,6),gi_quantize(value.z,5),1);
}
[shader("compute")][numthreads(64,1,1)]
void cs_gi_sample_reference(uint3 id:SV_DispatchThreadID,uniform GiBakeParams pc) {
    if(id.x>=pc.probes)return;
    float3 p=de_robuffer<float4>(pc.distances)[id.x*3].xyz;
    float3 n=de_robuffer<float4>(pc.distances)[id.x*3+1].xyz;
    float3 v=de_robuffer<float4>(pc.distances)[id.x*3+2].xyz;
    de_buffer<float4>(pc.irradiance)[id.x]=gi_irradiance(pc.raw,p,n,v);
}
