// Publication adapter only; temporal accumulation/clamp remains the shared DENG-68 kernel.
import shadow_prefilter;
import bindless;
import push_constants;
struct PublishPush {uint params,entry;};
[shader("compute")][numthreads(8,8,1)]
void cs_publish(uint3 id:SV_DispatchThreadID,uniform PublishPush push){
    PolicyParams p=de_robuffer<PolicyParams>(push.params)[push.entry];if(id.x>=p.width||id.y>=p.height)return;
    float4 value=de_tex2d(p.source).Load(int3(id.xy,0));de_rwtex2d(p.output)[uint2(id.x,id.y+(p.kind==0?p.slot*p.height:0))]=value;
}
