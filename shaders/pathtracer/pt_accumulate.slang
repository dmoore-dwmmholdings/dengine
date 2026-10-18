import push_constants;
import pt_closest_hit;
struct PtExport {uint params,entry,pixel,output;};
[shader("compute")][numthreads(1,1,1)]
void cs_pt_export(uniform PtExport pc) {
    PtParams p=pt_load(pc.params,pc.entry);DeRtRay r=pt_camera(p,pc.pixel);
    DeRtHit h=(DeRtHit)0;h.t=1;h.instance=pc.pixel;
    PtStep s=pt_step(p,r,h,pc.pixel,pc.entry,1,r.origin.xyz);
    bool alpha=de_rt_opacity(pt_query(p),h.instance,h.primitive,float2(0));
    de_rwbytes(pc.output).store(0,asuint(s.emitted.x+s.direct.x+s.weight.x)+(alpha?1:0));
}
