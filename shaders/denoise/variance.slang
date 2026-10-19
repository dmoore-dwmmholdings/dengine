import history;
import push_constants;
struct DnPush {uint params,entry;};
[shader("compute")][numthreads(8,8,1)]
void cs_variance(uint3 id:SV_DispatchThreadID,uniform DnPush pc){
    DenoiseParams p=dn_load(pc.params,pc.entry);if(id.x>=p.width||id.y>=p.height)return;int2 xy=int2(id.xy);float2 m=de_tex2d(p.moments).Load(int3(xy,0)).xy;
    float n=de_tex2d(p.length).Load(int3(xy,0)).x;
    if(n<4){float2 sum=0;float count=0;float z=de_tex2d(p.depth).Load(int3(xy,0)).x;float3 normal=dn_surface(p,p.normal,xy).normal;
        for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x){int2 q=xy+int2(x,y);if(any(q<0)||q.x>=int(p.width)||q.y>=int(p.height))continue;float other=de_tex2d(p.depth).Load(int3(q,0)).x;
            // Match spatial filtering: unrelated surfaces/lights cannot guide variance.
            if(any(de_tex2d(p.material).Load(int3(q,0)).xy!=de_tex2d(p.material).Load(int3(xy,0)).xy))continue;
            if(p.key!=0&&any(de_tex2d(p.key).Load(int3(q,0)).xy!=de_tex2d(p.key).Load(int3(xy,0)).xy))continue;
            if(abs(other-z)>p.depth_relative*max(abs(z),1e-3)||dot(normal,dn_surface(p,p.normal,q).normal)<p.normal_cos)continue;
            float l=dn_luma(de_tex2d(p.input).Load(int3(q,0)).rgb);sum+=float2(l,l*l);count++;}
        m=sum/max(count,1);}
    de_rwtex2d(p.variance)[xy]=float4(max(m.y-m.x*m.x,0),0,0,0);
}
