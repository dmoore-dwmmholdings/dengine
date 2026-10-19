module pt_sampling;
import "../rt/light_sampling";
public float pt_power(float a,float b) { return a*a/max(a*a+b*b,1e-30); }
// Owen-style nested integer scramble of the base-2 digital net, independently per dimension.
public uint pt_reverse(uint x) { return reversebits(x); }
public float pt_random(uint pixel,uint sample,uint dimension) {
    uint x=sample; uint seed=de_rt_hash(pixel^de_rt_hash(dimension+0x9e3779b9u));
    x^=x*0x3d20adeau;x+=seed;x*=((seed>>16)|1u);x^=x*0x05526c56u;x^=x*0x53a22864u;
    return min(float(pt_reverse(x))*(1.0/4294967296.0),0.99999994);
}
public float3 pt_random3(uint pixel,uint sample,uint dimension) {
    return float3(pt_random(pixel,sample,dimension),pt_random(pixel,sample,dimension+1),pt_random(pixel,sample,dimension+2));
}
public float3 pt_env_direction(float2 uv) {
    float z=1-2*uv.y,phi=6.283185307179586*uv.x,r=sqrt(max(0.0,1-z*z));
    return float3(r*cos(phi),r*sin(phi),z);
}
