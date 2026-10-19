// DENG-73: homogeneous transport and depth-four closed-volume ownership. No material BSDF copy.
module pt_volume;
import "../materials/standard/volume";
import pt_sampling;
import bindless;
import "../rt/light_sampling";
public struct PtMedium {public uint instance;public float3 color;public float distance;};
public struct PtMediumStack {public PtMedium entries[4];public uint depth;public uint overflow;};
public void pt_medium_enter(inout PtMediumStack stack,PtMedium medium) {
    if(stack.depth>=4){stack.overflow++;return;} // Drop the new innermost medium safely.
    stack.entries[stack.depth++]=medium;
}
public void pt_medium_exit(inout PtMediumStack stack,uint instance) {
    for(uint i=0;i<stack.depth;++i)if(stack.entries[i].instance==instance) {
        for(uint j=i+1;j<stack.depth;++j)stack.entries[j-1]=stack.entries[j];stack.depth--;return;
    }
}
public bool pt_medium_contains(PtMediumStack stack,uint instance) {
    for(uint i=0;i<stack.depth;++i)if(stack.entries[i].instance==instance)return true;
    return false;
}
public void pt_medium_boundary(inout PtMediumStack stack,PtMedium medium,bool entering) {
    if(!entering)pt_medium_exit(stack,medium.instance);
    else if(!pt_medium_contains(stack,medium.instance))pt_medium_enter(stack,medium);
}
public float3 pt_medium_transmittance(PtMediumStack stack,float distance) {
    if(stack.depth==0)return float3(1);PtMedium m=stack.entries[stack.depth-1];
    return volume_transmittance(m.color,m.distance,distance);
}
// HG is normalized over solid angle. Positive g scatters in the incident direction.
public float pt_hg(float cosine,float g) {
    g=clamp(g,-.99,.99);float d=max(1+g*g-2*g*cosine,1e-12);
    return (1-g*g)/(12.566370614359172*d*sqrt(d));
}
public float3 pt_hg_sample(float3 direction,float g,float2 u) {
    g=clamp(g,-.99,.99);float cosine;
    if(abs(g)<.001)cosine=1-2*u.x;
    else {float s=(1-g*g)/(1-g+2*g*u.x);cosine=clamp((1+g*g-s*s)/(2*g),-1.0,1.0);}
    float phi=6.283185307179586*u.y;float3 x=de_rt_tangent(direction),y=cross(direction,x);
    return normalize(direction*cosine+(x*cos(phi)+y*sin(phi))*sqrt(max(0.0,1-cosine*cosine)));
}
// An inverse affine maps rays into the authored fog-box coordinates. A negative extent is global.
public struct PtFog {public float4 inverse[3];public float4 extent_density;public float4 albedo_g;};
public float2 pt_fog_interval(PtFog fog,float3 origin,float3 direction,float limit) {
    if(fog.extent_density.x<0)return float2(0,limit);
    float3 o=float3(dot(fog.inverse[0],float4(origin,1)),dot(fog.inverse[1],float4(origin,1)),dot(fog.inverse[2],float4(origin,1)));
    float3 d=float3(dot(fog.inverse[0].xyz,direction),dot(fog.inverse[1].xyz,direction),dot(fog.inverse[2].xyz,direction));
    float near=0,far=limit;
    for(uint a=0;a<3;++a){if(abs(d[a])<1e-12){if(abs(o[a])>fog.extent_density[a])return float2(0);}
        else{float p=(-fog.extent_density[a]-o[a])/d[a],q=(fog.extent_density[a]-o[a])/d[a];near=max(near,min(p,q));far=min(far,max(p,q));}}
    return float2(near,max(near,far));
}
public float pt_fog_transmittance(uint buffer,uint count,float3 origin,float3 direction,float distance) {
    float optical=0;for(uint i=0;i<count;++i){PtFog fog=de_robuffer<PtFog>(buffer)[i];float2 t=pt_fog_interval(fog,origin,direction,distance);optical+=(t.y-t.x)*max(fog.extent_density.w,0.0);}
    return exp(-optical);
}
public struct PtFogEvent {public float distance;public float3 albedo;public float g;public uint scattered;};
public PtFogEvent pt_fog_event(uint sampling_table,uint buffer,uint count,float3 origin,float3 direction,float limit,uint pixel,uint sample,uint dimension) {
    PtFogEvent e=(PtFogEvent)0;e.distance=limit;
    // Independent exponential races exactly sample the sum of overlapping homogeneous regions.
    for(uint i=0;i<count;++i){PtFog fog=de_robuffer<PtFog>(buffer)[i];float density=max(fog.extent_density.w,0.0);if(!(density>0))continue;
        float2 interval=pt_fog_interval(fog,origin,direction,limit);float u=pt_random(sampling_table,pixel,sample,dimension+i);
        float d=interval.x-log(max(1-u,1e-20))/density;
        if(d<interval.y&&d<e.distance){e.distance=d;e.albedo=saturate(fog.albedo_g.rgb);e.g=fog.albedo_g.w;e.scattered=1;}}
    return e;
}
// Focused safety probe uses the exact production stack implementation on the GPU.
public uint2 pt_stack_probe() {
    PtMediumStack stack=(PtMediumStack)0;
    for(uint i=0;i<5;++i){PtMedium m;m.instance=i;m.color=float3(.5);m.distance=1;pt_medium_enter(stack,m);}
    uint depth=stack.depth;pt_medium_exit(stack,3);pt_medium_exit(stack,2);pt_medium_exit(stack,1);pt_medium_exit(stack,0);
    return uint2(depth,stack.overflow+(stack.depth<<16));
}
