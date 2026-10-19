import push_constants;
import pt_aov;
struct PtExport {uint params,entry,pixel,output;};
[shader("compute")][numthreads(1,1,1)]
void cs_pt_export(uniform PtExport pc) {
    PtParams p=pt_load(pc.params,pc.entry);DeRtRay r=pt_camera(p,pc.pixel);
    DeRtHit h=(DeRtHit)0;h.t=1;h.instance=pc.pixel;
    pt_store_aov(p,r,h,pc.pixel);
    PtStep s=pt_step(p,r,h,pc.pixel,pc.entry,1,r.origin.xyz);
    PtPath path=pt_path_init(p,pc.pixel);
    PtStep event=pt_path_event(p,path,h,pc.pixel);
    PtShadow shadow=pt_shadow_start(p,path.media,event.shadow);
    pt_shadow_hit(p,shadow,event.shadow,h,0);
    pt_path_finish(p,path,event,shadow.value,pc.pixel);
    pt_path_diagnostics(p,path);
    float2 moments=pt_sample_moments(float2(0),path.radiance.x,p.sample);
    uint2 safety=pt_stack_probe();
    bool alpha=de_rt_opacity(pt_query(p),h.instance,h.primitive,float2(0));
    de_rwbytes(pc.output).store(0,asuint(s.emitted.x+s.direct.x+s.weight.x+path.radiance.x)+(alpha?1:0)+safety.x+safety.y+asuint(moments.x));
}
