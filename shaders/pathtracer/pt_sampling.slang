module pt_sampling;
import bindless;
import "../rt/light_sampling";
public float pt_power(float a,float b) { return a*a/max(a*a+b*b,1e-30); }
// Owen-style scramble of distinct Joe-Kuo Sobol dimensions.
// The old per-dimension scrambled van der Corput copies had correlated leading bits.
public uint pt_reverse(uint x) { return reversebits(x); }
public float pt_random(uint table,uint pixel,uint sample,uint dimension) {
    uint seed=de_rt_hash(pixel^de_rt_hash(dimension+0x9e3779b9u));
    // Deep media paths can exhaust the finite Sobol dimensions. Independent
    // counter samples avoid silently reusing an earlier projection.
    if(dimension>=1024u)return float(de_rt_hash(sample^seed)>>8)*(1.0/16777216.0);
    uint value=0,base=dimension*32u;
    for(uint index=sample,column=0;index!=0;index>>=1,++column)
        if((index&1u)!=0)value^=de_robuffer<uint>(table)[base+column];
    uint x=reversebits(value);
    x^=x*0x3d20adeau;x+=seed;x*=((seed>>16)|1u);x^=x*0x05526c56u;x^=x*0x53a22864u;
    return min(float(pt_reverse(x))*(1.0/4294967296.0),0.99999994);
}
public float3 pt_random3(uint table,uint pixel,uint sample,uint dimension) {
    return float3(pt_random(table,pixel,sample,dimension),pt_random(table,pixel,sample,dimension+1),pt_random(table,pixel,sample,dimension+2));
}
public float3 pt_env_direction(float2 uv) {
    float z=1-2*uv.y,phi=6.283185307179586*uv.x,r=sqrt(max(0.0,1-z*z));
    return float3(r*cos(phi),r*sin(phi),z);
}
