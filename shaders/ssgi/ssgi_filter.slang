module ssgi_filter;
import bindless;
import "../ssr/ssr_common";
[shader("compute")][numthreads(8,8,1)]
void cs_ssgi_filter(uint3 id:SV_DispatchThreadID,uniform SsrParams pc) {
    if(id.x>=pc.width||id.y>=pc.height)return;
    uint2 pixel=id.xy;float2 uv=(float2(pixel)+.5)/float2(pc.width,pc.height);
    float z=de_tex2d(pc.depth).Load(int3(pixel,0)).r;
    SurfaceData surface=ssr_surface(pc,pixel);
    DeSceneConstants sc=de_robuffer<DeSceneConstants>(pc.scene)[pc.slot];
    float4 result=0,moments=0;
    if(z>0&&(surface.features&DE_MATERIAL_FEATURE_UNLIT)==0) {
        float3 position=ssr_position(sc,uv,z),v=ssr_view(sc,position);
        int2 base=int2(uv*float2(pc.tw,pc.th));
        float3 sum=0;float weight=0,coverage=0,total=0,lum=0,lum2=0;
        for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x) {
            int2 rp=base+int2(x,y);
            if(any(rp<0)||any(rp>=int2(pc.tw,pc.th)))continue;
            uint2 source=min(uint2((float2(rp)+.5)*float2(pc.width,pc.height)/float2(pc.tw,pc.th)),uint2(pc.width-1,pc.height-1));
            float source_z=de_tex2d(pc.depth).Load(int3(source,0)).r;
            SurfaceData source_surface=ssr_surface(pc,source);
            float nd=dot(surface.normal,source_surface.normal);
            if(nd<.85||abs(source_z-z)>max(z*.05,1e-4))continue;
            float w=pow(saturate(nd),16)*exp(-float(x*x+y*y)*.5);
            total+=w;float4 sample=de_tex2d(pc.rays).Load(int3(rp,0));
            if(!(sample.a>0)||!all(isfinite(sample)))continue;
            float weighted=w*sample.a;
            sum+=sample.rgb*weighted;weight+=weighted;coverage+=weighted;
            float l=dot(sample.rgb,float3(.2126,.7152,.0722));lum+=l*weighted;lum2+=l*l*weighted;
        }
        if(total>1e-8) {
            DeIbl unit=(DeIbl)0;unit.ambient=float4(1,1,1,0);float3 response,unused;
            de_ibl_evaluate(surface,v,unit,pc.lut,1,response,unused);
            // Accumulate the Monte Carlo numerator and valid-ray probability, including
            // zero-hit frames. Dividing their histories is the ratio estimator; dropping
            // miss frames would reset history and bias confidence towards current hits.
            float3 numerator=sum/total*response;
            float probability=saturate(coverage/total);
            float l=dot(numerator,float3(.2126,.7152,.0722));
            moments=float4(l,l*l,1,0);
            float2 previous=ssr_previous_uv(uv,de_tex2d(pc.motion).Load(int3(pixel,0)).xy);
            if((pc.history_valid&2u)!=0&&ssr_history_valid(pc,sc,previous,position,surface.normal)) {
                float4 old=ssr_history_load(pc.previous_specular,previous,uint2(pc.width,pc.height));
                float4 stats=ssr_history_load(pc.previous_moments,previous,uint2(pc.width,pc.height));
                if(all(isfinite(old))&&all(isfinite(stats))&&stats.z>0) {
                    float variance=max(stats.y-stats.x*stats.x,0);
                    float current_variance=weight>1e-8?max(lum2/weight-pow(lum/weight,2),0):0;
                    float response_max=max(max(response.x,response.y),response.z);
                    float sigma=max(sqrt(max(variance,current_variance*response_max*response_max)),max(l*.1,.005));
                    float3 previous_numerator=old.rgb*old.a;
                    // Clip only when there is current support; a miss is a legitimate zero
                    // estimator sample, not evidence that the previous radiance is invalid.
                    if(weight>1e-8&&old.a>0) {
                        float3 conditional=sum/weight*response;
                        float conditional_sigma=sigma/max(probability,.05);
                        float current_luminance=dot(conditional,float3(.2126,.7152,.0722));
                        float old_luminance=dot(old.rgb,float3(.2126,.7152,.0722));
                        float clipped_luminance=clamp(old_luminance,max(current_luminance-3*conditional_sigma,0),current_luminance+3*conditional_sigma);
                        previous_numerator=old_luminance>1e-8?old.rgb*(clipped_luminance/old_luminance)*old.a:float3(0);
                    }
                    float count=min(stats.z,31),a=1/(count+1);
                    numerator=lerp(previous_numerator,numerator,a);probability=lerp(old.a,probability,a);
                    moments=float4(lerp(stats.xy,float2(l,l*l),a),count+1,0);
                }
            }
            result=float4(probability>1e-8?numerator/probability:float3(0),probability);
        }
    }
    if(!all(isfinite(result))||!all(isfinite(moments))){result=0;moments=0;}
    result.rgb=clamp(result.rgb,0,65000);
    de_rwtex2d(pc.output)[pixel]=result;
    de_rwtex2d(pc.write_specular)[pixel]=result;
    de_rwtex2d(pc.write_moments)[pixel]=moments;
}
