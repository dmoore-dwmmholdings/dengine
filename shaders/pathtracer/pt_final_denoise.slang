import bindless;
import "../denoise/reblur_weights";
import push_constants;
struct PtFinalParams {uint width,height,accumulation,albedo,normal,depth,position,material;uint input,output,variance,moments;float exposure;uint pad0,pad1,pad2;};
struct PtFinalPush {uint params,entry;};
float3 pt_final_demod(PtFinalParams p,int2 xy){float4 sum=de_tex2d(p.accumulation).Load(int3(xy,0));return sum.rgb/max(sum.a,1)/reblur_floor_factor(de_tex2d(p.albedo).Load(int3(xy,0)).rgb);}
[shader("compute")][numthreads(8,8,1)]
void cs_pt_final_prepare(uint3 id:SV_DispatchThreadID,uniform PtFinalPush pc){
    PtFinalParams p=de_robuffer<PtFinalParams>(pc.params)[pc.entry];if(id.x>=p.width||id.y>=p.height)return;int2 xy=int2(id.xy);
    float3 center=pt_final_demod(p,xy);
    float samples=de_tex2d(p.accumulation).Load(int3(xy,0)).a;
    float3 factor=reblur_floor_factor(de_tex2d(p.albedo).Load(int3(xy,0)).rgb);
    float3 variance=max(de_tex2d(p.moments).Load(int3(xy,0)).rgb/(max(samples*(samples-1.0),1.0)*factor*factor),float3(1e-8));
    if(samples<=1.0) {
        // A single sample has no measured variance; bootstrap from compatible neighbors.
        float3 mean=0,second=0;float count=0;
        float4 normal=de_tex2d(p.normal).Load(int3(xy,0));float3 position=de_tex2d(p.position).Load(int3(xy,0)).xyz;
        for(uint i=0;i<8;++i){int2 q=xy+int2(round(reblur_poisson(i)*3));if(any(q<0)||q.x>=int(p.width)||q.y>=int(p.height))continue;
            if(any(de_tex2d(p.material).Load(int3(q,0)).xy!=de_tex2d(p.material).Load(int3(xy,0)).xy))continue;
            float4 n=de_tex2d(p.normal).Load(int3(q,0));if(!reblur_geometry(normal.xyz,normal.w,n.xyz,n.w,de_tex2d(p.position).Load(int3(q,0)).xyz-position))continue;
            float3 value=pt_final_demod(p,q);mean+=value;second+=value*value;count++;}
        mean/=max(count,1);variance=max(second/max(count,1)-mean*mean,float3(1e-8));
    }
    // Keep the estimate paired with its measured uncertainty. A spatial firefly clamp
    // changes the sample mean without updating its moments and clips valid lighting detail.
    de_rwtex2d(p.output)[xy]=float4(center,1);de_rwtex2d(p.variance)[xy]=float4(variance,0);
}
[shader("compute")][numthreads(8,8,1)]
void cs_pt_final_publish(uint3 id:SV_DispatchThreadID,uniform PtFinalPush pc){PtFinalParams p=de_robuffer<PtFinalParams>(pc.params)[pc.entry];if(id.x>=p.width||id.y>=p.height)return;float3 value=de_tex2d(p.input).Load(int3(id.xy,0)).rgb*reblur_floor_factor(de_tex2d(p.albedo).Load(int3(id.xy,0)).rgb);de_rwtex2d(p.output)[id.xy]=float4(value*p.exposure,1);}
