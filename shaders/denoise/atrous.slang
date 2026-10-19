import history;
import push_constants;
struct DnPush {uint params,entry;};
[shader("compute")][numthreads(8,8,1)]
void cs_atrous(uint3 id:SV_DispatchThreadID,uniform DnPush pc){
    DenoiseParams p=dn_load(pc.params,pc.entry);if(id.x>=p.width||id.y>=p.height)return;int2 xy=int2(id.xy);float4 center=de_tex2d(p.input).Load(int3(xy,0));
    float z=de_tex2d(p.depth).Load(int3(xy,0)).x;float3 position=dn_position(p,xy,z,false),normal=dn_surface(p,p.normal,xy).normal;float variance=de_tex2d(p.variance).Load(int3(xy,0)).x;
    if(p.hit!=0&&p.zero_radius_passthrough!=0&&de_tex2d(p.hit).Load(int3(xy,0)).x<.5){de_rwtex2d(p.output)[xy]=center;if(p.out_variance!=0)de_rwtex2d(p.out_variance)[xy]=float4(variance,0,0,0);return;}
    // AO's guide is a support radius in pixels, not a wavelet stride multiplier.
    bool bounded_ao=p.channels==2&&p.hit!=0&&p.hit_scale>0;
    // Previous passes can reach at most 2*(step-1) pixels. Reserve that
    // footprint so the composed filter cannot exceed this pixel's AO support.
    float support=bounded_ao?max(de_tex2d(p.hit).Load(int3(xy,0)).x*p.hit_scale-2.0*float(p.step-1),0.0):0;
    uint stride=p.step;if(!bounded_ao&&p.hit!=0&&p.hit_scale>0)stride*=uint(clamp(de_tex2d(p.hit).Load(int3(xy,0)).x*p.hit_scale,1.0,4.0));
    float4 sum=0;float weight=0,variance_sum=0;const float kernel[5]={1,4,6,4,1};
    for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x){int2 q=xy+int2(x,y)*int(stride);if(any(q<0)||q.x>=int(p.width)||q.y>=int(p.height))continue;
        if(bounded_ao&&max(abs(x),abs(y))*stride>support)continue;
        if(p.key!=0&&any(de_tex2d(p.key).Load(int3(q,0)).xy!=de_tex2d(p.key).Load(int3(xy,0)).xy))continue;
        if(any(de_tex2d(p.material).Load(int3(q,0)).xy!=de_tex2d(p.material).Load(int3(xy,0)).xy))continue;
        float depth=de_tex2d(p.depth).Load(int3(q,0)).x;float3 delta=dn_position(p,q,depth,false)-position;float scale=p.linear_depth!=0?max(abs(z),1e-3):max(length(position-p.camera.xyz),1e-3);
        float plane=abs(dot(delta,normal));if(plane>p.depth_relative*scale)continue;
        float4 c=de_tex2d(p.input).Load(int3(q,0));float w=kernel[x+2]*kernel[y+2]*pow(max(dot(normal,dn_surface(p,p.normal,q).normal),0.0),p.normal_power);
        w*=exp(-abs(dn_signal_luma(p,c.rgb)-dn_signal_luma(p,center.rgb))/(p.luminance_scale*sqrt(max(variance,1e-8))+1e-6));sum+=c*w;weight+=w;
        if(p.out_variance!=0)variance_sum+=w*w*max(de_tex2d(p.variance).Load(int3(q,0)).x,0.0);}
    de_rwtex2d(p.output)[xy]=weight>0?sum/weight:center;
    // SVGF Eq. 3: carry variance of the weighted estimate into the next level.
    if(p.out_variance!=0)de_rwtex2d(p.out_variance)[xy]=float4(weight>0?max(variance_sum/(weight*weight),1e-8):variance,0,0,0);
}
