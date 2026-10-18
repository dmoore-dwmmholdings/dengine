import push_constants;
import ddgi_trace;
import "../probes/irradiance_sample";
uint ddgi_component(float v,uint mantissa) {
    v=isfinite(v)?clamp(v,0,60000):(v>0?60000:0);if(v<asfloat(113u<<23))return 0;
    uint b=asuint(v);return ((((b>>23)&255u)-112u)<<mantissa)|((b&0x7fffffu)>>(23-mantissa));
}
uint ddgi_pack(float3 v){return ddgi_component(v.x,6)|(ddgi_component(v.y,6)<<11)|(ddgi_component(v.z,5)<<22);}
uint ddgi_half(float v) {
    uint b=asuint(isfinite(v)?saturate(v):1),exponent=(b>>23)&255u;
    if(exponent>=113)return ((exponent-112)<<10)|((b&0x7fffffu)>>13);
    uint shift=126-exponent;return shift>=32?0:((b&0x7fffffu)|0x800000u)>>min(shift,31u);
}
uint ddgi_moments(float2 v){return ddgi_half(v.x)|(ddgi_half(v.y)<<16);}
bool previous_probe(DdgiParams p,uint probe,out uint old) {
    uint3 counts=ddgi_volume(p).counts.xyz;int3 c=int3(ddgi_coord(probe,counts))+int3(p.shift_x,p.shift_y,p.shift_z);
    old=0;if(p.initialized==0||any(c<0)||any(c>=int3(counts)))return false;
    old=(uint(c.z)*counts.y+uint(c.y))*counts.x+uint(c.x);return true;
}
[shader("compute")][numthreads(8,8,1)]void cs_scroll(uint3 id:SV_DispatchThreadID,uniform DdgiPush push) {
    DdgiParams p=ddgi_params(push);GiVolume g=ddgi_volume(p);uint old=0;
    if(id.x<p.irradiance_width&&id.y<p.irradiance_height) {
        uint probe=(id.y/10)*g.counts.w+id.x/10;bool valid=probe<p.probes&&previous_probe(p,probe,old);
        uint2 source=uint2(old%g.counts.w,old/g.counts.w)*10+id.xy%10;
        float3 value=valid?de_rwtex2d(p.irradiance_previous)[source].rgb:float3(0);
        de_buffer<uint>(p.payload)[id.y*p.irradiance_pitch+id.x]=ddgi_pack(value);
    }
    if(id.x<p.moments_width&&id.y<p.moments_height) {
        uint probe=(id.y/18)*g.counts.w+id.x/18;bool valid=probe<p.probes&&previous_probe(p,probe,old);
        uint2 source=uint2(old%g.counts.w,old/g.counts.w)*18+id.xy%18;
        float2 value=valid?de_rwtex2d(p.moments_previous)[source].xy:float2(1);
        de_buffer<uint>(p.payload)[p.moments_offset+id.y*p.moments_pitch+id.x]=ddgi_moments(value);
    }
    uint linear=id.y*((p.moments_width+7)/8*8)+id.x;
    if(linear<p.probes*64) {
        uint probe=linear/64;bool valid=previous_probe(p,probe,old);
        float4 value=valid?asfloat(de_bytes(p.history_previous).load4((old*64+linear%64)*16)):float4(0);
        de_rwbytes(p.history_current).store4(linear*16,asuint(value));
    }
    if(linear<p.probes*256) {
        uint probe=linear/256;bool valid=previous_probe(p,probe,old);uint offset=p.probes*64*16;
        float2 value=valid?asfloat(de_bytes(p.history_previous).load2(offset+(old*256+linear%256)*8)):float2(1);
        de_rwbytes(p.history_current).store2(offset+linear*8,asuint(value));
    }
    uint probe=id.y*((p.moments_width+7)/8*8)+id.x;
    if(probe<p.probes)de_buffer<float4>(p.state_current)[probe]=previous_probe(p,probe,old)?de_robuffer<float4>(p.state_previous)[old]:float4(0,0,0,1);
}
[shader("compute")][numthreads(8,8,1)]void cs_update(uint3 id:SV_DispatchThreadID,uniform DdgiPush push) {
    DdgiParams p=ddgi_params(push);if(id.x>=16||id.y>=16||id.z>=p.budget)return;uint probe=ddgi_probe(p,id.z);GiVolume g=ddgi_volume(p);
    float3 inormal=gi_oct_decode((float2(id.xy)+.5)/8),mnormal=gi_oct_decode((float2(id.xy)+.5)/16);
    float3 sum=0;float cosine_total=0;float2 moments=0;float moment_total=0;
    for(uint i=0;i<p.rays_per_probe;++i) {
        uint index=id.z*p.rays_per_probe+i;float3 direction=de_robuffer<DeRtRay>(p.rays)[index].direction.xyz;
        float d=saturate(abs(de_robuffer<float4>(p.ray_info)[index].w)/g.params.z);
        float w=pow(max(dot(mnormal,direction),0),512);moments+=float2(d,d*d)*w;moment_total+=w;
        if(all(id.xy<8)) {float c=max(dot(inormal,direction),0);sum+=de_robuffer<float4>(p.radiance)[index].rgb*c;cosine_total+=c;}
    }
    uint old;bool valid=previous_probe(p,probe,old);float4 state=de_robuffer<float4>(p.state_current)[probe];
    if(valid){float4 before=de_robuffer<float4>(p.state_previous)[old];valid=before.w==0&&all(abs(before.xyz-state.xyz)<1e-5);}
    float h=valid?p.hysteresis:0;uint2 tile=uint2(probe%g.counts.w,probe/g.counts.w);
    if(all(id.xy<8)) {
        float3 fresh=sum*(3.141592653589793/max(cosine_total,1e-8));
        float3 previous=valid?asfloat(de_bytes(p.history_previous).load4((old*64+id.y*8+id.x)*16)).rgb:float3(0);
        // A significant new lighting observation must not retain stale energy for dozens of updates.
        // Keep the authored/default hysteresis for stable observations; accelerate large changes.
        float fresh_luma=dot(fresh,float3(.2126,.7152,.0722)),old_luma=dot(previous,float3(.2126,.7152,.0722));
        float response_h=abs(fresh_luma-old_luma)>max(.1,old_luma*.25)?min(h,.8):h;
        float3 value=state.w==0?lerp(fresh,previous,response_h):float3(0);uint2 pixel=tile*10+id.xy+1;
        de_rwbytes(p.history_current).store4((probe*64+id.y*8+id.x)*16,asuint(float4(value,0)));
        de_buffer<uint>(p.payload)[pixel.y*p.irradiance_pitch+pixel.x]=ddgi_pack(value);
    }
    float2 fresh=moment_total>1e-20?moments/moment_total:float2(1);
    float2 previous=valid?asfloat(de_bytes(p.history_previous).load2(p.probes*64*16+(old*256+id.y*16+id.x)*8)):float2(1);
    float2 result=lerp(fresh,previous,h);de_rwbytes(p.history_current).store2(p.probes*64*16+(probe*256+id.y*16+id.x)*8,asuint(result));
    uint2 pixel=tile*18+id.xy+1;de_buffer<uint>(p.payload)[p.moments_offset+pixel.y*p.moments_pitch+pixel.x]=ddgi_moments(result);
}
[shader("compute")][numthreads(8,8,1)]void cs_border(uint3 id:SV_DispatchThreadID,uniform DdgiPush push) {
    DdgiParams p=ddgi_params(push);if(id.x>=18||id.y>=18||id.z>=p.budget)return;uint probe=ddgi_probe(p,id.z);GiVolume g=ddgi_volume(p);uint2 tile=uint2(probe%g.counts.w,probe/g.counts.w);
    if(id.x<10&&id.y<10&&(id.x==0||id.y==0||id.x==9||id.y==9)) {
        uint2 src=tile*10+gi_border(id.xy,8),dst=tile*10+id.xy;
        de_buffer<uint>(p.payload)[dst.y*p.irradiance_pitch+dst.x]=de_robuffer<uint>(p.payload)[src.y*p.irradiance_pitch+src.x];
    }
    if(id.x==0||id.y==0||id.x==17||id.y==17) {
        uint2 src=tile*18+gi_border(id.xy,16),dst=tile*18+id.xy;
        de_buffer<uint>(p.payload)[p.moments_offset+dst.y*p.moments_pitch+dst.x]=de_robuffer<uint>(p.payload)[p.moments_offset+src.y*p.moments_pitch+src.x];
    }
}
