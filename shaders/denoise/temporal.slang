import history;
import push_constants;
struct DnPush {uint params,entry;};
[shader("compute")][numthreads(8,8,1)]
void cs_temporal(uint3 id:SV_DispatchThreadID,uniform DnPush pc){
    DenoiseParams p=dn_load(pc.params,pc.entry);if(id.x>=p.width||id.y>=p.height)return;int2 xy=int2(id.xy);
    float4 current=de_tex2d(p.input).Load(int3(xy,0)),old=de_tex2d(p.previous).Load(int3(xy,0));float n=de_tex2d(p.previous_length).Load(int3(xy,0)).x;
    if(p.clamp_mode!=0&&n>0){float3 lo=float3(1e30),hi=float3(-1e30);
        for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x){int2 q=clamp(xy+int2(x,y),int2(0),int2(p.width-1,p.height-1));if(p.key!=0&&any(de_tex2d(p.key).Load(int3(q,0)).xy!=de_tex2d(p.key).Load(int3(xy,0)).xy))continue;float3 c=de_tex2d(p.input).Load(int3(q,0)).rgb;c=p.clamp_mode==2?dn_ycocg(c):float3(dn_luma(c));lo=min(lo,c);hi=max(hi,c);}
        if(p.clamp_mode==2)old.rgb=dn_rgb(clamp(dn_ycocg(old.rgb),lo,hi));else{float l=dn_luma(old.rgb);old.rgb*=clamp(l,lo.x,hi.x)/max(l,1e-20);}}
    float minimum=p.temporal_alpha!=0?max(p.alpha_min,de_tex2d(p.temporal_alpha).Load(int3(xy,0)).x):p.alpha_min;float alpha=max(1/(n+1),minimum);float4 value=lerp(old,current,alpha);float lum=dn_luma(current.rgb);float2 previous=de_tex2d(p.previous_moments).Load(int3(xy,0)).xy,moments=lerp(previous,float2(lum,lum*lum),alpha);
    de_rwtex2d(p.output)[xy]=value;de_rwtex2d(p.moments)[xy]=float4(moments,0,0);de_rwtex2d(p.length)[xy]=float4(min(n+1,65535.0),0,0,0);
    de_rwtex2d(p.out_depth)[xy]=de_tex2d(p.depth).Load(int3(xy,0));de_rwtex2d(p.out_normal)[xy]=de_tex2d(p.normal).Load(int3(xy,0));de_rwtex2d(p.out_material)[xy]=de_tex2d(p.material).Load(int3(xy,0));
    if(p.out_key!=0)de_rwtex2d(p.out_key)[xy]=de_tex2d(p.key).Load(int3(xy,0));if(p.out_position!=0)de_rwtex2d(p.out_position)[xy]=de_tex2d(p.position).Load(int3(xy,0));
}
