// Regeneration entry preserves the complete shared opacity function in emitted MSL.
import push_constants;
import rt_opacity;
struct ExportParams { uint context, instance, primitive, output; float2 bary; };
[shader("compute")]
[numthreads(1,1,1)]
void cs_opacity_export(uniform ExportParams p) {
    DeRtContext q=de_robuffer<DeRtContext>(p.context)[0];
    de_rwbytes(p.output).store(0,de_rt_opacity(q,p.instance,p.primitive,p.bary)?1u:0u);
}
