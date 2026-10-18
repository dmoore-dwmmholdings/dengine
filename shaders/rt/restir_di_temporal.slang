import push_constants;
import restir_di_common;
[shader("compute")][numthreads(8,8,1)]
void cs_temporal(uint3 id:SV_DispatchThreadID,uniform DiPush pc) {
    DiParams p=di_params(pc);if(id.x>=p.width || id.y>=p.height)return;
    uint pixel=id.y*p.width+id.x;DiSurface s=di_surface(p,id.xy);
    DiReservoir own=di_load(p.initial,pixel),r=di_empty();
    di_merge(r,own,own.target,p.candidates,0);r.age=0;
    if(p.history_valid!=0 && s.valid) {
        float2 motion=de_tex2d(p.motion).Load(int3(id.xy,0)).xy;
        int2 previous=int2(round(float2(id.xy)-motion*float2(p.width*.5,-p.height*.5)));
        if(all(previous>=0) && previous.x<int(p.width) && previous.y<int(p.height)) {
            uint at=uint(previous.y)*p.width+uint(previous.x);
            DiHistory history=de_robuffer<DiHistory>(p.history_previous)[at];
            if(di_compatible(s,history)) {
                DiReservoir old=di_load(p.previous,at);
                float target=di_evaluate(p,s,old.source,float2(old.u,old.v)).target;
                di_merge(r,old,target,p.candidates*20,de_rt_random(pixel,p.frame,300).x);
                r.age=min(old.age+1,65535u);
            }
        }
    }
    di_finalize(r,r.M);de_buffer<DiReservoir>(p.temporal)[pixel]=r;
}
