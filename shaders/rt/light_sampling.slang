// Stable analytic source sampling shared with DENG-46. Radius: radians for sun, metres locally.
module light_sampling;
import "../lighting/lights";
import rt_types;
public uint de_rt_hash(uint x) { x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16); }
public float2 de_rt_random(uint pixel,uint frame,uint sample) {
    uint seed=de_rt_hash(pixel^de_rt_hash(frame+0x9e3779b9u)^de_rt_hash(sample+17));
    return float2(de_rt_hash(seed),de_rt_hash(seed^0x68bc21ebu))*(1.0/4294967296.0);
}
public float3 de_rt_tangent(float3 n) { return normalize(cross(abs(n.z)<0.999?float3(0,0,1):float3(0,1,0),n)); }
public DeRtRay de_rt_shadow_ray(DeClusterLight l,float3 p,float3 normal,float2 u) {
    uint kind=de_light_kind(l);float phi=6.28318530718*u.y;
    float3 center=kind==DE_LIGHT_KIND_DIRECTIONAL?-l.direction.xyz:normalize(l.position.xyz-p);
    float3 x=de_rt_tangent(center),y=cross(center,x),wi;float distance=1e6;
    if(kind==DE_LIGHT_KIND_DIRECTIONAL) {
        float z=1-u.x*(1-cos(clamp(l.direction.w,0.0,1.55)));
        wi=normalize(center*z+(x*cos(phi)+y*sin(phi))*sqrt(max(0.0,1-z*z)));
    } else {
        float3 target=l.position.xyz;
        if(kind==DE_LIGHT_KIND_RECT || kind==DE_LIGHT_KIND_DISK) {
            x=normalize(l.axis.xyz);y=normalize(cross(l.direction.xyz,x));
            float2 xy=kind==DE_LIGHT_KIND_RECT?2*u-1:sqrt(u.x)*float2(cos(phi),sin(phi));
            target+=x*(xy.x*l.shape.y)+y*(xy.y*l.shape.z);
        } else {
            float z=1-2*u.x;float r=sqrt(max(0.0,1-z*z));
            target+=l.direction.w*float3(r*cos(phi),r*sin(phi),z);
        }
        float3 delta=target-p;distance=length(delta);wi=delta/max(distance,1e-8);
    }
    DeRtRay ray;float bias=max(0.0001,0.00001*max(abs(p.x),max(abs(p.y),abs(p.z))));
    ray.origin=float4(p+normal*(dot(normal,wi)>=0?bias:-bias),0.00001);
    ray.direction=float4(wi,max(0.0,distance-bias));return ray;
}
