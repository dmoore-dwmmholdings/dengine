// DENG-156: HDR temporal reconstruction. Canonical motion remains unjittered current-minus-
// previous NDC; history lives on the unjittered output grid.
import bindless;
import push_constants;
import "../denoise/history";
struct TaaPush {
    uint color, depth, motion, exposure;
    uint reactive, previous_color, previous_depth, output;
    uint history_color, history_depth, iw, ih;
    uint ow, oh, flags, padding;
    float2 jitter; float exposure_ratio, gamma;
    float history_weight, sharpen, depth_relative, reserved;
    float4 depth_coeff;
};
float3 finite_color(float3 c) {
    return float3(isfinite(c.r)?max(c.r,0):0,isfinite(c.g)?max(c.g,0):0,
                  isfinite(c.b)?max(c.b,0):0);
}
int2 input_pixel(int2 q,TaaPush p) { return clamp(q,int2(0),int2(p.iw,p.ih)-1); }
float3 current_at(int2 q,TaaPush p) { return finite_color(de_tex2d(p.color).Load(int3(input_pixel(q,p),0)).rgb); }
float catmull(float x) {
    x=abs(x);return x<1?1.5*x*x*x-2.5*x*x+1:x<2?-.5*x*x*x+2.5*x*x-4*x+2:0;
}
float4 history_at(float2 uv,TaaPush p) {
    float2 xy=uv*float2(p.ow,p.oh)-.5;int2 base=int2(floor(xy));float4 sum=0;
    for(int y=-1;y<=2;++y)for(int x=-1;x<=2;++x){int2 q=base+int2(x,y);
        float w=catmull(xy.x-q.x)*catmull(xy.y-q.y);
        sum+=de_tex2d(p.previous_color).Load(int3(clamp(q,int2(0),int2(p.ow,p.oh)-1),0))*w;
    }return sum;
}
float linear_depth(float z,TaaPush p) {
    if((p.flags&2)!=0)return max(z,0);
    if(z<=0)return 0;
    float denominator=p.depth_coeff.z*z+p.depth_coeff.w;
    return abs(denominator)>1e-20?abs((p.depth_coeff.x*z+p.depth_coeff.y)/denominator):0;
}
[shader("compute")][numthreads(8,8,1)]
void cs_taa(uint3 id:SV_DispatchThreadID,uniform TaaPush p) {
    if(id.x>=p.ow||id.y>=p.oh)return;
    float2 uv=(float2(id.xy)+.5)/float2(p.ow,p.oh);
    float2 at=uv*float2(p.iw,p.ih)-.5-p.jitter;
    float2 footprint=max(float2(p.ow,p.oh)/float2(p.iw,p.ih),1);
    bool upscale=any(footprint>1);
    // The upscale kernel has the variance of one output pixel's box footprint (1/12).
    float kernel=upscale?6:2.29;
    int2 nearest=input_pixel(int2(floor(at+.5)),p);
    float3 sum=0,m1=0,m2=0,lo=float3(3.402823e38),hi=0;
    float3 rgb_lo=float3(3.402823e38),rgb_hi=0;float weights=0;
    float2 chroma_lo=float2(3.402823e38),chroma_hi=float2(-3.402823e38);
    bool has_color=false;
    for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){int2 q=nearest+int2(x,y);
        float2 delta=(float2(q)-at)*footprint;float w=exp(-kernel*dot(delta,delta));float3 c=current_at(q,p);
        sum+=c*w;weights+=w;float3 v=dn_ycocg(c);m1+=v;m2+=v*v;lo=min(lo,v);hi=max(hi,v);
        rgb_lo=min(rgb_lo,c);rgb_hi=max(rgb_hi,c);
        if(v.x>1e-6){float2 chroma=v.yz/v.x;chroma_lo=min(chroma_lo,chroma);
            chroma_hi=max(chroma_hi,chroma);has_color=true;}
    }
    float3 current=sum/max(weights,1e-8);m1/=9;m2/=9;
    // Input samples outside this output pixel's reconstruction footprint contribute less to
    // the temporal average. Normalizing every sparse phase to one would retain input blur.
    float2 centerWeights=1+2*exp(-kernel*footprint*footprint);
    float confidence=upscale?saturate(weights/(centerWeights.x*centerWeights.y)):1;
    float3 sigma=sqrt(max(m2-m1*m1,0));lo=max(lo,m1-p.gamma*sigma);hi=min(hi,m1+p.gamma*sigma);
    // Reverse-Z closest-depth dilation chooses one surface's motion around thin silhouettes.
    int2 surface=nearest;float z=de_tex2d(p.depth).Load(int3(nearest,0)).x;
    for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){int2 q=input_pixel(nearest+int2(x,y),p);
        float d=de_tex2d(p.depth).Load(int3(q,0)).x;
        if(((p.flags&2)==0&&d>z)||((p.flags&2)!=0&&d>0&&(z<=0||d<z))){surface=q;z=d;}
    }
    float2 motion=de_tex2d(p.motion).Load(int3(surface,0)).xy;
    float velocity=length(motion*float2(p.ow,p.oh)*.5);
    float2 previous_uv=denoise_previous_uv(uv,motion);
    float depth=linear_depth(z,p),storedDepth=depth,uncertainFrames=0;
    float4 old=0;bool valid=(p.flags&1)==0&&
        all(previous_uv>=0)&&all(previous_uv<1)&&all(isfinite(motion));
    if(valid){int2 q=clamp(int2(previous_uv*float2(p.ow,p.oh)),int2(0),int2(p.ow,p.oh)-1);
        float2 previousSurface=de_tex2d(p.previous_depth).Load(int3(q,0)).xy;
        float previous=previousSurface.x;
        old=history_at(previous_uv,p);old.rgb=finite_color(old.rgb*p.exposure_ratio);
        valid=(depth==0&&previous==0)||(depth>0&&previous>0&&
              abs(previous-depth)<=p.depth_relative*max(depth,1e-3));
        // An under-resolution thin silhouette can miss all input samples for a phase. Retain
        // uncertain static coverage for at most three frames; a persistent disocclusion still
        // discards its old surface on frame four. Positive-depth disagreements reject at once.
        bool empty_coverage=hi.x<=1e-6&&dn_luma(old.rgb)>1e-6;
        bool coverage_gap=(!valid&&((depth==0)!=(previous==0)))||empty_coverage;
        if(upscale&&velocity<.01&&coverage_gap){
            valid=previousSurface.y<3;
            if(valid){storedDepth=previous;uncertainFrames=previousSurface.y+1;}
        }
        // Coverage changes luminance, not chromaticity. Reject an old hue outside the current
        // nonblack neighborhood even when its RGB lies inside a background-to-surface box.
        // This handles both adding and removing material color channels without erasing static
        // subpixel coverage. The margin absorbs quantization and Catmull-Rom overshoot.
        if(valid&&upscale&&velocity<.01&&has_color){
            float3 previous_ycocg=dn_ycocg(old.rgb);
            float2 previous_chroma=previous_ycocg.yz/max(previous_ycocg.x,1e-6);
            if(any(previous_chroma<chroma_lo-.05)||any(previous_chroma>chroma_hi+.05)){
                valid=false;storedDepth=depth;uncertainFrames=0;
            }
        }
        if(valid){
            // Static TAAU phases cover different subpixel regions, so a phase-local variance
            // bound would erase retained line coverage. Its conservative RGB bounds still
            // reject colors no longer present; wholly missed coverage keeps the bounded timeout.
            if(!upscale||velocity>=.01)old.rgb=dn_rgb(clamp(dn_ycocg(old.rgb),lo,hi));
            if(!upscale||velocity>=.01||dn_luma(rgb_hi)>1e-6)
                old.rgb=clamp(old.rgb,rgb_lo,rgb_hi);}
    }
    float age=valid?clamp(old.a,0,64):0;
    if(!valid)old=0;
    float alpha=valid?max(confidence/max(age+confidence,1e-8),
        confidence*lerp(1-p.history_weight,.25,saturate(velocity/8))):1;
    if(p.reactive!=0)alpha=max(alpha,saturate(de_tex2d(p.reactive).Load(int3(id.xy,0)).x));
    float exposure=p.exposure!=0?max(de_tex2d(p.exposure).Load(int3(0,0,0)).x,1e-6):1;
    // Anti-flicker weighting applies to HDR highlights. Weighting ordinary coverage samples
    // by 1/(1+Y) biases a static subpixel silhouette dark relative to its supersampled energy.
    float wc=alpha/(1+max(dn_luma(current)*exposure-1,0));
    float wh=(1-alpha)/(1+max(dn_luma(old.rgb)*exposure-1,0));
    float3 color=finite_color((current*wc+old.rgb*wh)/max(wc+wh,1e-8));
    de_rwtex2d(p.history_color)[id.xy]=float4(color,min(age+confidence,64));
    de_rwtex2d(p.history_depth)[id.xy]=float4(storedDepth,uncertainFrames,0,0);
    float3 sharp=finite_color(color+p.sharpen*(color-dn_rgb(m1)));
    de_rwtex2d(p.output)[id.xy]=float4(min(sharp,65504),1);
}
