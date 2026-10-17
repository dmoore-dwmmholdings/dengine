// Weighted reservoir operations. Sample space is (source, two uniform source coordinates).
module reservoir;
public static const uint DI_NONE=0xffffffffu;
public struct DiReservoir {
    public uint source;
    public float u;
    public float v;
    public uint M;
    public float sum;
    public float W;
    public float target;
    public uint age;
};
public DiReservoir di_empty() { DiReservoir r=(DiReservoir)0;r.source=DI_NONE;return r; }
public void di_update(inout DiReservoir r,uint source,float2 uv,float target,float weight,uint M,float random) {
    r.M+=M;
    if(!(weight>0) || !isfinite(weight))return;
    r.sum+=weight;
    if(random*r.sum<weight) {r.source=source;r.u=uv.x;r.v=uv.y;r.target=target;}
}
public void di_merge(inout DiReservoir r,DiReservoir q,float target,uint cap,float random) {
    uint count=min(q.M,cap);
    if(q.source==DI_NONE){r.M+=count;return;}
    di_update(r,q.source,float2(q.u,q.v),target,target*q.W*count,count,random);
}
public void di_finalize(inout DiReservoir r,uint normalization) {
    r.W=r.target>0 && normalization>0?r.sum/(r.target*normalization):0;
    if(!isfinite(r.W))r.W=0;
}
