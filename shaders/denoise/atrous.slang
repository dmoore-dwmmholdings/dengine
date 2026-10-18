import history;
import push_constants;
struct DnPush {uint params,entry;};
[shader("compute")][numthreads(8,8,1)]
void cs_atrous(uint3 id:SV_DispatchThreadID,uniform DnPush pc){
    DenoiseParams p=dn_load(pc.params,pc.entry);if(id.x>=p.width||id.y>=p.height)return;int2 xy=int2(id.xy);float4 center=de_tex2d(p.input).Load(int3(xy,0));
    float z=de_tex2d(p.depth).Load(int3(xy,0)).x;float3 position=dn_position(p,xy,z,false),normal=dn_surface(p.normal,xy).normal;float variance=de_tex2d(p.variance).Load(int3(xy,0)).x;
    uint stride=p.step;if(p.hit!=0&&p.hit_scale>0)stride*=uint(clamp(de_tex2d(p.hit).Load(int3(xy,0)).x*p.hit_scale,1.0,4.0));
    float4 sum=0;float weight=0;const float kernel[5]={1,4,6,4,1};
    for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x){int2 q=xy+int2(x,y)*int(stride);if(any(q<0)||q.x>=int(p.width)||q.y>=int(p.height))continue;
        float depth=de_tex2d(p.depth).Load(int3(q,0)).x;float3 delta=dn_position(p,q,depth,false)-position;float scale=p.linear_depth!=0?max(abs(z),1e-3):max(length(position-p.camera.xyz),1e-3);
        float plane=abs(dot(delta,normal));if(plane>p.depth_relative*scale)continue;
        float4 c=de_tex2d(p.input).Load(int3(q,0));float w=kernel[x+2]*kernel[y+2]*pow(max(dot(normal,dn_surface(p.normal,q).normal),0.0),p.normal_power);
        w*=exp(-abs(dn_luma(c.rgb)-dn_luma(center.rgb))/(p.luminance_scale*sqrt(max(variance,1e-8))+1e-6));sum+=c*w;weight+=w;}
    de_rwtex2d(p.output)[xy]=weight>0?sum/weight:center;
}
