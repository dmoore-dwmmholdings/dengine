import "../materials/standard/volume";
import "../materials/standard/lobe_transmission";
import "../materials/standard/standard_base";
import push_constants;
import rt_hit_shading;
import rt_refraction_types;
import light_sampling;
import "../forward/ss_refraction";
struct RefractionParams {
    uint scene,slot,view,entry;
    uint query,capture,rays,hits;
    uint shadow_rays,shadow_hits,result,path;
    uint width,height,frame,events;
    uint lights,dirs,grid,list,counters,ltc1,ltc2,sampler;
    uint light,instance,side,reserved;
};
DeLightingView lighting(RefractionParams p){return de_robuffer<DeLightingView>(p.view)[p.entry];}
DeClusterLists lists(RefractionParams p){DeClusterLists c;c.lights=p.lights;c.dir_lights=p.dirs;c.grid=p.grid;c.list=p.list;c.counters=p.counters;c.ltc1=p.ltc1;c.ltc2=p.ltc2;c.sampler=p.sampler;return c;}
uint cluster(RefractionParams p,float3 position){DeSceneConstants sc=de_robuffer<DeSceneConstants>(p.scene)[p.slot];float4 h=float4(dot(sc.view_proj[0],float4(position,1)),dot(sc.view_proj[1],float4(position,1)),0,dot(sc.view_proj[3],float4(position,1)));float2 uv=float2(h.x,-h.y)/max(h.w,1e-6)*.5+.5;return de_cluster_index(lighting(p),uint2(saturate(uv)*float2(p.width-1,p.height-1)),-dot(sc.view[2],float4(position,1)));}
DeRtSurface hit_surface(RefractionParams p,uint index){return de_rt_hit_surface(de_robuffer<DeRtContext>(p.query)[0],de_robuffer<DeRtHit>(p.hits)[index],de_robuffer<DeRtRay>(p.rays)[index].origin.xyz,lighting(p).brdf_lut);}
float3 sampled_direction(DeRefractionCapture c,uint pixel,uint frame,out bool tir) {
    bool inside=dot(c.view_ior.xyz,c.geometry_curvature.xyz)<0;
    float3 normal=c.normal_roughness.xyz;if(dot(normal,c.view_ior.xyz)<0)normal=-normal;
    ShadingFrame basis=de_frame_from_normal(normal);float3 wo=basis.to_local(c.view_ior.xyz),wi;
    TransmissionLobes l=(TransmissionLobes)0;l.alpha=max(.001,c.normal_roughness.w*c.normal_roughness.w);l.eta=inside?1/c.view_ior.w:c.view_ior.w;l.refracts=c.position_thickness.w>0&&abs(c.view_ior.w-1)>=1e-3;
    float2 u=de_rt_random(pixel,frame,0);tir=false;
    if(c.normal_roughness.w<.05) {
        if(!l.refracts)return -c.view_ior.xyz;
        if(de_refract(c.view_ior.xyz,normal,l.eta,wi))return wi;
        tir=true;return reflect(-c.view_ior.xyz,normal);
    }
    if(de_transmission_specular_sample(l,wo,u,wi))return basis.to_world(wi);
    tir=true;float3 h=de_ggx_vndf_sample(l.alpha,l.alpha,wo,u);return basis.to_world(de_reflect(wo,h));
}
[shader("compute")][numthreads(8,8,1)]void cs_clear(uint3 id:SV_DispatchThreadID,uniform RefractionParams p){if(id.x>=p.width||id.y>=p.height)return;uint index=id.y*p.width+id.x;de_buffer<DeRefractionCapture>(p.capture)[index]=(DeRefractionCapture)0;de_buffer<float4>(p.result)[index]=float4(0);de_buffer<float4>(p.path)[index]=float4(0);if(index==0&&(p.side&2u)!=0)for(uint i=0;i<4;++i)de_buffer<float4>(p.reserved)[i]=float4(0);}
[shader("compute")][numthreads(8,8,1)]void cs_generate(uint3 id:SV_DispatchThreadID,uniform RefractionParams p) {
    if(id.x>=p.width||id.y>=p.height)return;uint index=id.y*p.width+id.x;DeRefractionCapture c=de_robuffer<DeRefractionCapture>(p.capture)[index];DeRtRay ray=(DeRtRay)0;float4 path=0;
    if(c.view_ior.w>0) {
        bool tir;float3 direction=sampled_direction(c,index,p.frame,tir);c.sampled=float4(direction,1);de_buffer<DeRefractionCapture>(p.capture)[index]=c;
        float thickness=c.position_thickness.w;bool inside=dot(c.view_ior.xyz,c.geometry_curvature.xyz)<0;
        bool pending=p.events==2&&thickness>0&&!inside&&!tir&&abs(c.view_ior.w-1)>=1e-3;
        float distance=0;
        if(!pending&&!tir&&thickness>0&&!inside){bool curved=c.geometry_curvature.w>0&&c.geometry_curvature.w*thickness>.25;distance=curved?max(-dot(c.geometry_curvature.xyz,direction)*thickness,0):thickness/max(abs(dot(direction,c.geometry_curvature.xyz)),1e-4);}
        else if(inside&&!tir)distance=thickness;
        float3 origin=c.position_thickness.xyz+direction*(pending||inside?0:distance);
        ray.origin=float4(origin+direction*.0001,.00001);ray.direction=float4(direction,10000);path=float4(distance,tir?1:0,1,pending?1:0);
    }de_buffer<DeRtRay>(p.rays)[index]=ray;de_buffer<float4>(p.path)[index]=path;
}
[shader("compute")][numthreads(8,8,1)]void cs_exit(uint3 id:SV_DispatchThreadID,uniform RefractionParams p) {
    if(id.x>=p.width||id.y>=p.height)return;uint index=id.y*p.width+id.x;float4 path=de_robuffer<float4>(p.path)[index];if(path.w==0)return;
    DeRtHit hit=de_robuffer<DeRtHit>(p.hits)[index];
    if(hit.t<0||hit.instance!=p.instance) {
        // An authored thickness may describe an open raster surface. Retain its single-exit
        // approximation when no matching closed-volume interface exists in the scene.
        DeRefractionCapture c=de_robuffer<DeRefractionCapture>(p.capture)[index];DeRtRay ray=de_robuffer<DeRtRay>(p.rays)[index];float thickness=c.position_thickness.w;
        bool curved=c.geometry_curvature.w>0&&c.geometry_curvature.w*thickness>.25;
        float distance=curved?max(-dot(c.geometry_curvature.xyz,ray.direction.xyz)*thickness,0):thickness/max(abs(dot(ray.direction.xyz,c.geometry_curvature.xyz)),1e-4);
        ray.origin.xyz+=ray.direction.xyz*distance;path.x=distance;path.w=0;de_buffer<DeRtRay>(p.rays)[index]=ray;de_buffer<float4>(p.path)[index]=path;return;
    }
    DeRefractionCapture c=de_robuffer<DeRefractionCapture>(p.capture)[index];DeRtRay ray=de_robuffer<DeRtRay>(p.rays)[index];DeResolvePoint point=de_rt_point(de_robuffer<DeRtContext>(p.query)[0],hit.instance,hit.primitive,float2(hit.u,hit.v));float4 tangent;float3 geometric;float3 normal=de_resolve_normal(point,tangent,geometric);
    if(dot(normal,ray.direction.xyz)<0)normal=-normal;
    float3 h=-normal;
    if(c.normal_roughness.w>=.05){ShadingFrame f=de_frame_from_normal(h);float3 wo=f.to_local(-ray.direction.xyz);h=f.to_world(de_ggx_vndf_sample(max(.001,c.normal_roughness.w*c.normal_roughness.w),max(.001,c.normal_roughness.w*c.normal_roughness.w),wo,de_rt_random(index,p.frame,1)));}
    float3 direction;bool transmitted=de_refract(-ray.direction.xyz,h,1/c.view_ior.w,direction);if(!transmitted)direction=reflect(ray.direction.xyz,h);
    float3 position=ray.origin.xyz+ray.direction.xyz*hit.t;ray.origin=float4(position+direction*.0001,.00001);ray.direction=float4(direction,10000);path.x=hit.t+.0001;path.y=transmitted?0:1;path.z=2;path.w=0;de_buffer<DeRtRay>(p.rays)[index]=ray;de_buffer<float4>(p.path)[index]=path;
}
[shader("compute")][numthreads(8,8,1)]void cs_shade(uint3 id:SV_DispatchThreadID,uniform RefractionParams p) {
    if(id.x>=p.width||id.y>=p.height)return;uint index=id.y*p.width+id.x;DeRtRay ray=de_robuffer<DeRtRay>(p.rays)[index];DeRefractionCapture c=de_robuffer<DeRefractionCapture>(p.capture)[index];DeRtSurface s=hit_surface(p,index);float3 radiance=0;
    if(ray.direction.w>0)radiance=s.valid?de_rt_hit_radiance<StandardModel>(s,-ray.direction.xyz,lighting(p),lists(p),cluster(p,s.position),false):de_rt_environment(lighting(p),lists(p),c.position_thickness.xyz,ray.direction.xyz,cluster(p,c.position_thickness.xyz));
    de_buffer<float4>(p.result)[index]=float4(radiance*lighting(p).pre_exposure,c.view_ior.w>0?1:0);
    // Preserve the latest nonempty center draw for backend-neutral ray debugging. Empty
    // opposite-side draws must not erase the visible backface's TIR/event observation.
    if(id.x==p.width/2&&id.y==p.height/2&&c.view_ior.w>0){de_buffer<float4>(p.reserved)[0]=ray.direction;de_buffer<float4>(p.reserved)[1]=de_robuffer<float4>(p.path)[index];de_buffer<float4>(p.reserved)[2]=ray.origin;DeRtHit hit=de_robuffer<DeRtHit>(p.hits)[index];de_buffer<float4>(p.reserved)[3]=float4(hit.t,asfloat(hit.instance),asfloat(hit.primitive),0);}
}
[shader("compute")][numthreads(8,8,1)]void cs_shadow_ray(uint3 id:SV_DispatchThreadID,uniform RefractionParams p) {
    if(id.x>=p.width||id.y>=p.height)return;uint index=id.y*p.width+id.x;DeRtSurface s=hit_surface(p,index);DeRtRay ray=(DeRtRay)0;bool local=p.light>=4;uint light=local?p.light-4:p.light,count=local?min(de_bytes(p.counters).load(0),lighting(p).local_capacity):min(de_bytes(p.counters).load(4),4u);
    if(s.valid&&(s.material.features&DE_MATERIAL_FEATURE_UNLIT)==0&&light<count){DeClusterLight l=de_robuffer<DeClusterLight>(local?p.lights:p.dirs)[light];if((asuint(l.axis.w)&DE_CLUSTER_LIGHT_SHADOWS)!=0)ray=de_rt_shadow_ray(l,s.position,s.material.normal,de_rt_random(index,p.frame,p.light));}de_buffer<DeRtRay>(p.shadow_rays)[index]=ray;
}
[shader("compute")][numthreads(8,8,1)]void cs_shadow_add(uint3 id:SV_DispatchThreadID,uniform RefractionParams p) {
    if(id.x>=p.width||id.y>=p.height)return;uint index=id.y*p.width+id.x;bool local=p.light>=4;uint light=local?p.light-4:p.light,count=local?min(de_bytes(p.counters).load(0),lighting(p).local_capacity):min(de_bytes(p.counters).load(4),4u);if(light>=count)return;DeRtSurface s=hit_surface(p,index);if(!s.valid||(s.material.features&DE_MATERIAL_FEATURE_UNLIT)!=0)return;
    DeClusterLight l=de_robuffer<DeClusterLight>(local?p.lights:p.dirs)[light];if((asuint(l.axis.w)&DE_CLUSTER_LIGHT_SHADOWS)!=0&&de_robuffer<DeRtHit>(p.shadow_hits)[index].t>=0)return;
    float3 eye=-de_robuffer<DeRtRay>(p.rays)[index].direction.xyz;float3 value=local?de_rt_local_light<StandardModel>(s.material,s.position,eye,lighting(p),lists(p),l):de_rt_sun_direct<StandardModel>(s.material,s.position,eye,l);
    de_buffer<float4>(p.result)[index]+=float4(value*lighting(p).pre_exposure,0);
}
