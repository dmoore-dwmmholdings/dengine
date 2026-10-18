// DENG-49: typed batch rays and shared hit shading. No ordinary Metal inline query is used.
module ddgi_trace;
import push_constants;
__exported import rt_hit_shading;
import light_sampling;
__exported import "../probes/irradiance_sample";
public struct DdgiParams {
    public uint scene,scene_slot,query,view;
    public uint entry,previous_records,current_records,volume;
    public uint rays,hits,shadow_rays,shadow_hits;
    public uint radiance,ray_info,state_previous,state_current;
    public uint irradiance_previous,moments_previous,payload,selected_first;
    public uint probes,budget,rays_per_probe,frame;
    public uint irradiance_width,irradiance_height,moments_width,moments_height;
    public uint irradiance_pitch,moments_pitch,moments_offset,initialized;
    public uint lights,dirs,grid,list;
    public uint counters,ltc1,ltc2,sampler;
    public int shift_x,shift_y,shift_z;public float hysteresis;
    public uint history_previous,history_current,padding0,padding1;
};
public struct DdgiPush {public uint params,entry,light,reserved;};
public DdgiParams ddgi_params(DdgiPush p){return de_robuffer<DdgiParams>(p.params)[p.entry];}
public GiVolume ddgi_volume(DdgiParams p){return de_robuffer<GiVolume>(p.current_records)[p.volume+1];}
public uint ddgi_probe(DdgiParams p,uint selected){return (p.selected_first+selected)%p.probes;}
public uint3 ddgi_coord(uint index,uint3 counts){return uint3(index%counts.x,index/counts.x%counts.y,index/(counts.x*counts.y));}
public float3 ddgi_position(DdgiParams p,uint probe) {
    GiVolume g=ddgi_volume(p);float3 local=-g.extent.xyz+(float3(ddgi_coord(probe,g.counts.xyz))+.5)*g.step.xyz;
    float3 center=-gi_rotate(g.rotation,float3(g.local[0].w,g.local[1].w,g.local[2].w));
    return center+gi_rotate(g.rotation,local)+de_robuffer<float4>(p.state_current)[probe].xyz;
}
public float3 ddgi_direction(uint ray,uint rays,uint frame) {
    float z=1-2*(float(ray)+.5)/rays,phi=float(ray)*2.399963229728653;
    float3 d=float3(sqrt(max(0.0,1-z*z))*cos(phi),sqrt(max(0.0,1-z*z))*sin(phi),z);
    float3 axis=normalize(float3(de_u32_to_unit(de_pcg_hash(frame+1))-.5,de_u32_to_unit(de_pcg_hash(frame+23))-.5,de_u32_to_unit(de_pcg_hash(frame+77))-.5));
    float angle=6.28318530718*de_u32_to_unit(de_pcg_hash(frame+129));
    return d*cos(angle)+cross(axis,d)*sin(angle)+axis*dot(axis,d)*(1-cos(angle));
}
public DeLightingView ddgi_lighting(DdgiParams p){return de_robuffer<DeLightingView>(p.view)[p.entry];}
public DeClusterLists ddgi_lists(DdgiParams p){DeClusterLists c;c.lights=p.lights;c.dir_lights=p.dirs;c.grid=p.grid;c.list=p.list;c.counters=p.counters;c.ltc1=p.ltc1;c.ltc2=p.ltc2;c.sampler=p.sampler;return c;}
public uint ddgi_cluster(DdgiParams p,float3 position) {
    DeSceneConstants s=de_robuffer<DeSceneConstants>(p.scene)[p.scene_slot];DeLightingView l=ddgi_lighting(p);
    float4 h=float4(dot(s.view_proj[0],float4(position,1)),dot(s.view_proj[1],float4(position,1)),0,dot(s.view_proj[3],float4(position,1)));
    float2 uv=float2(h.x,-h.y)/max(h.w,1e-6)*.5+.5;
    return de_cluster_index(l,uint2(saturate(uv)*float2(l.tiles_x*l.tile_px-1,l.tiles_y*l.tile_px-1)),-dot(s.view[2],float4(position,1)));
}
public DeRtSurface ddgi_hit(DdgiParams p,uint index){return de_rt_hit_surface(de_robuffer<DeRtContext>(p.query)[0],de_robuffer<DeRtHit>(p.hits)[index],de_robuffer<DeRtRay>(p.rays)[index].origin.xyz,ddgi_lighting(p).brdf_lut);}
[shader("compute")][numthreads(64,1,1)]void cs_generate(uint3 id:SV_DispatchThreadID,uniform DdgiPush push) {
    DdgiParams p=ddgi_params(push);uint index=id.x+id.y*(65535u*64u);if(index>=p.budget*p.rays_per_probe)return;
    uint probe=ddgi_probe(p,index/p.rays_per_probe);DeRtRay ray;
    ray.origin=float4(ddgi_position(p,probe),.00001);ray.direction=float4(ddgi_direction(index%p.rays_per_probe,p.rays_per_probe,p.frame),ddgi_volume(p).params.z);
    de_buffer<DeRtRay>(p.rays)[index]=ray;
}
[shader("compute")][numthreads(64,1,1)]void cs_shade(uint3 id:SV_DispatchThreadID,uniform DdgiPush push) {
    DdgiParams p=ddgi_params(push);uint index=id.x+id.y*(65535u*64u);if(index>=p.budget*p.rays_per_probe)return;
    DeRtRay ray=de_robuffer<DeRtRay>(p.rays)[index];DeRtHit hit=de_robuffer<DeRtHit>(p.hits)[index];DeRtSurface s=ddgi_hit(p,index);float3 value=0,geometric=0;float distance=ddgi_volume(p).params.z;
    if(s.valid) {
        value=de_rt_hit_radiance<StandardModel>(s,-ray.direction.xyz,ddgi_lighting(p),ddgi_lists(p),ddgi_cluster(p,s.position),false,p.previous_records);
        DeResolvePoint point=de_rt_point(de_robuffer<DeRtContext>(p.query)[0],hit.instance,hit.primitive,float2(hit.u,hit.v));float4 tangent;
        de_resolve_normal(point,tangent,geometric);distance=hit.t*(dot(geometric,ray.direction.xyz)>0?-1:1);
    } else value=de_rt_environment(ddgi_lighting(p),ddgi_lists(p),ray.origin.xyz,ray.direction.xyz,ddgi_cluster(p,ray.origin.xyz));
    de_buffer<float4>(p.radiance)[index]=float4(max(value,float3(0)),0);de_buffer<float4>(p.ray_info)[index]=float4(geometric,distance);
}
[shader("compute")][numthreads(64,1,1)]void cs_shadow_ray(uint3 id:SV_DispatchThreadID,uniform DdgiPush push) {
    DdgiParams p=ddgi_params(push);uint index=id.x+id.y*(65535u*64u);if(index>=p.budget*p.rays_per_probe)return;
    bool local=push.light>=4;uint light=local?push.light-4:push.light,count=local?min(de_bytes(p.counters).load(0),ddgi_lighting(p).local_capacity):min(de_bytes(p.counters).load(4),4u);
    DeRtSurface s=ddgi_hit(p,index);DeRtRay ray=(DeRtRay)0;
    if(s.valid&&(s.material.features&DE_MATERIAL_FEATURE_UNLIT)==0&&light<count) {
        DeClusterLight l=de_robuffer<DeClusterLight>(local?p.lights:p.dirs)[light];
        if((asuint(l.axis.w)&DE_CLUSTER_LIGHT_SHADOWS)!=0)ray=de_rt_shadow_ray(l,s.position,s.material.normal,de_rt_random(index,p.frame,push.light));
    }de_buffer<DeRtRay>(p.shadow_rays)[index]=ray;
}
[shader("compute")][numthreads(64,1,1)]void cs_shadow_add(uint3 id:SV_DispatchThreadID,uniform DdgiPush push) {
    DdgiParams p=ddgi_params(push);uint index=id.x+id.y*(65535u*64u);if(index>=p.budget*p.rays_per_probe)return;
    bool local=push.light>=4;uint light=local?push.light-4:push.light,count=local?min(de_bytes(p.counters).load(0),ddgi_lighting(p).local_capacity):min(de_bytes(p.counters).load(4),4u);if(light>=count)return;
    DeRtSurface s=ddgi_hit(p,index);if(!s.valid||(s.material.features&DE_MATERIAL_FEATURE_UNLIT)!=0)return;
    DeClusterLight l=de_robuffer<DeClusterLight>(local?p.lights:p.dirs)[light];
    if((asuint(l.axis.w)&DE_CLUSTER_LIGHT_SHADOWS)!=0&&de_robuffer<DeRtHit>(p.shadow_hits)[index].t>=0)return;
    float3 eye=-de_robuffer<DeRtRay>(p.rays)[index].direction.xyz;
    float3 value=local?de_rt_local_light<StandardModel>(s.material,s.position,eye,ddgi_lighting(p),ddgi_lists(p),l):de_rt_sun_direct<StandardModel>(s.material,s.position,eye,l);
    de_buffer<float4>(p.radiance)[index]+=float4(value,0);
}
