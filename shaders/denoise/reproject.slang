import history;
import "../scene/scene_types";
import "../visbuffer/vis_types";
import push_constants;
import "../visbuffer/vis_decode";
struct DnPush {uint params,entry;};
[shader("compute")][numthreads(8,8,1)]
void cs_reproject(uint3 id:SV_DispatchThreadID,uniform DnPush pc){
    DenoiseParams p=dn_load(pc.params,pc.entry);if(id.x>=p.width||id.y>=p.height)return;int2 xy=int2(id.xy);
    float2 motion=de_tex2d(p.motion).Load(int3(xy,0)).xy;
    float2 at=denoise_previous_uv((float2(xy)+.5)/float2(p.width,p.height),motion)*float2(p.width,p.height)-.5;
    int2 base=int2(floor(at));float2 f=frac(at);float weight=0,age=0;float4 color=0;float2 moments=0;
    DeGbSurface surface=dn_surface(p,p.normal,xy);float z=de_tex2d(p.depth).Load(int3(xy,0)).x;float3 position=dn_position(p,xy,z,false);
    float2 material=de_tex2d(p.material).Load(int3(xy,0)).xy;
    if(p.reset==0)for(int y=0;y<2;++y)for(int x=0;x<2;++x){int2 q=base+int2(x,y);if(any(q<0)||q.x>=int(p.width)||q.y>=int(p.height))continue;
        float tap=(x!=0?f.x:1-f.x)*(y!=0?f.y:1-f.y);if(tap<=0)continue;
        DeGbSurface old=dn_surface(p,p.previous_normal,q);float previous_depth=de_tex2d(p.previous_depth).Load(int3(q,0)).x;
        float3 delta=dn_position(p,q,previous_depth,true)-position;
        float scale=p.linear_depth!=0?max(abs(z),1e-3):max(length(position-p.camera.xyz),1e-3);
        bool valid=abs(dot(delta,surface.normal))<=p.depth_relative*scale && dot(old.normal,surface.normal)>=p.normal_cos && abs(old.roughness-surface.roughness)<=p.roughness_threshold && all(de_tex2d(p.previous_material).Load(int3(q,0)).xy==material);
        if(p.key!=0)valid=valid&&all(de_tex2d(p.key).Load(int3(xy,0)).xy==de_tex2d(p.previous_key).Load(int3(q,0)).xy);
        if(!valid)continue;weight+=tap;color+=de_tex2d(p.previous).Load(int3(q,0))*tap;moments+=de_tex2d(p.previous_moments).Load(int3(q,0)).xy*tap;age+=de_tex2d(p.previous_length).Load(int3(q,0)).x*tap;
    }
    de_rwtex2d(p.output)[xy]=weight>0?color/weight:float4(0);de_rwtex2d(p.moments)[xy]=float4(weight>0?moments/weight:float2(0),0,0);
    de_rwtex2d(p.length)[xy]=float4(weight>0?age/weight:0,0,0,0);de_rwtex2d(p.validity)[xy]=float4(weight,0,0,0);
}
[shader("compute")][numthreads(8,8,1)]
void cs_material(uint3 id:SV_DispatchThreadID,uniform DnPush pc){
    DenoiseParams p=dn_load(pc.params,pc.entry);if(id.x>=p.width||id.y>=p.height)return;
    uint packed=de_bytes(p.input).load(id.y*p.reserved+id.x*4),material=0xffffffffu;
    if(packed!=0xffffffffu){DeSceneConstants sc=de_robuffer<DeSceneConstants>(p.scene)[p.slot];DeVisMeshlet v=de_robuffer<DeVisMeshlet>(p.visible)[packed>>7];
        if(v.instance<sc.instance_count){DeGpuInstance instance=de_robuffer<DeGpuInstance>(sc.instances)[v.instance];DeGpuMesh mesh=de_robuffer<DeGpuMesh>(sc.meshes)[instance.mesh];material=de_vis_instance_material(instance,de_vis_meshlet_info(mesh,v.meshlet).submesh);}}
    de_rwtex2d(p.output)[id.xy]=float4(float(material&65535u),float(material>>16),0,0);
}
