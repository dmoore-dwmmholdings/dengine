module irradiance_sample;
import bindless;
public struct GiVolume {
    public float4 local[3];
    public float4 extent, step, rotation;
    public uint4 counts, textures;
    public float4 params; // intensity, blend metres, normalized distance range, ready
};
public float2 gi_oct_encode(float3 n) {
    n/=max(abs(n.x)+abs(n.y)+abs(n.z),1e-20);
    float2 uv=n.xy;
    if(n.z<0)uv=(1-abs(uv.yx))*float2(uv.x>=0?1:-1,uv.y>=0?1:-1);
    return uv*.5+.5;
}
public float3 gi_oct_decode(float2 uv) {
    float2 f=uv*2-1;float3 n=float3(f,1-abs(f.x)-abs(f.y));
    if(n.z<0)n.xy=(1-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1);
    return normalize(n);
}
// Fold a one-texel border across octahedral seams; corner is the diagonally opposite interior.
public uint2 gi_border(uint2 p,uint n) {
    if((p.x==0||p.x==n+1)&&(p.y==0||p.y==n+1))
        return uint2(p.x==0?n:1,p.y==0?n:1);
    if(p.x==0)return uint2(1,n+1-p.y);
    if(p.x==n+1)return uint2(n,n+1-p.y);
    if(p.y==0)return uint2(n+1-p.x,1);
    if(p.y==n+1)return uint2(n+1-p.x,n);
    return p;
}
public float4 gi_atlas(uint texture,uint probe,uint columns,uint n,float3 direction) {
    float2 xy=gi_oct_encode(direction)*float(n)+.5;
    int2 p=int2(floor(xy));float2 f=frac(xy);
    int2 base=int2(probe%columns,probe/columns)*int(n+2);
    float4 a=de_rwtex2d(texture)[base+p],b=de_rwtex2d(texture)[base+p+int2(1,0)];
    float4 c=de_rwtex2d(texture)[base+p+int2(0,1)],d=de_rwtex2d(texture)[base+p+1];
    return lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);
}
// Interpolate each directional distribution's variance, not variance of the mixture of
// different directions. The latter invents visibility uncertainty at a perfectly thin wall
// simply because adjacent octahedral directions intersect it at different radial distances.
public float2 gi_distance_distribution(uint texture,uint probe,uint columns,float3 direction) {
    float2 xy=gi_oct_encode(direction)*16+.5;int2 p=int2(floor(xy));float2 f=frac(xy);
    int2 base=int2(probe%columns,probe/columns)*18;
    float2 a=de_rwtex2d(texture)[base+p].xy,b=de_rwtex2d(texture)[base+p+int2(1,0)].xy;
    float2 c=de_rwtex2d(texture)[base+p+int2(0,1)].xy,d=de_rwtex2d(texture)[base+p+1].xy;
    a.y=max(a.y-a.x*a.x,0);b.y=max(b.y-b.x*b.x,0);
    c.y=max(c.y-c.x*c.x,0);d.y=max(d.y-d.x*d.x,0);
    return lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);
}
public float3 gi_rotate(float4 q,float3 p) {return p+2*cross(q.xyz,cross(q.xyz,p)+q.w*p);}
public float3 gi_volume_sample(GiVolume g,float3 p,float3 n,float3 v,out float coverage) {
    float3 local=float3(dot(g.local[0],float4(p,1)),dot(g.local[1],float4(p,1)),dot(g.local[2],float4(p,1)));
    float boundary=min(min(g.extent.x-abs(local.x),g.extent.y-abs(local.y)),g.extent.z-abs(local.z));
    coverage=g.params.w>0 && boundary>0?(g.params.y>0?saturate(boundary/g.params.y):1):0;
    if(coverage==0)return float3(0);
    float3 cell=clamp((local+g.extent.xyz)/g.step.xyz-.5,float3(0),float3(g.counts.xyz-1));
    uint3 base=min(uint3(floor(cell)),g.counts.xyz-2);float3 fraction=cell-float3(base);
    float3 biased=p+n*g.step.w+v*g.extent.w;
    float3 sum=0;float total=0;
    for(uint corner=0;corner<8;++corner) {
        uint3 c=uint3(corner&1,(corner>>1)&1,(corner>>2)&1),index=base+c;
        uint probe=(index.z*g.counts.y+index.y)*g.counts.x+index.x;
        float4 state=de_robuffer<float4>(g.textures.z)[probe];
        if(state.w!=0)continue;
        float3 lp=-g.extent.xyz+(float3(index)+.5)*g.step.xyz;
        float3 delta=gi_rotate(g.rotation,lp-local)+state.xyz+ p-biased;
        float distance=length(delta);float3 to_probe=distance>1e-8?delta/distance:n;
        float3 trilinear=lerp(1-fraction,fraction,float3(c));
        float weight=trilinear.x*trilinear.y*trilinear.z;
        weight*=max(.05,pow(saturate((dot(to_probe,n)+1)*.5),2));
        float2 moments=gi_distance_distribution(g.textures.y,probe,g.counts.w,-to_probe);
        float normalized=distance/g.params.z;
        float variance=max(moments.y,1e-7);
        float d=max(normalized-moments.x,0);
        // Conservative smooth sharpening suppresses uncertain occluded neighbours before
        // the normalized DDGI blend; unlike a cutoff it stays continuous through cell edges.
        float visibility=normalized<=moments.x?1:pow(saturate(variance/(variance+d*d)),16);
        weight*=visibility;

        sum+=gi_atlas(g.textures.x,probe,g.counts.w,8,n).rgb*weight;
        total+=weight;
    }
    return sum/max(total,1e-12)*g.params.x;
}
// Buffer header is a 144-byte GiVolume with counts.x=count; ordered priority then entity.
public float4 gi_irradiance(uint records,float3 p,float3 n,float3 v) {
    if(records==0)return float4(0);
    uint count=min(de_robuffer<GiVolume>(records)[0].counts.x,8u);
    float3 sum=0;float remaining=1;
    for(uint i=0;i<count;++i) {
        float coverage;float3 e=gi_volume_sample(de_robuffer<GiVolume>(records)[i+1],p,n,v,coverage);
        sum+=e*(coverage*remaining);remaining*=1-coverage;
    }
    return float4(sum,1-remaining);
}
