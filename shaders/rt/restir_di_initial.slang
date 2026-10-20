import push_constants;
import restir_di_common;
import "../lighting/lights";
import "../scene/scene_types";
import "../visbuffer/vis_decode";
import "../materials/material_interface";
[shader("compute")][numthreads(64,1,1)]
void cs_sources(uint3 id:SV_DispatchThreadID,uniform DiPush pc) {
    DiParams p=di_params(pc);if(id.x>=p.source_count)return;
    DiDescriptor desc=de_robuffer<DiDescriptor>(p.descriptors)[id.x];
    DeSceneConstants sc=di_scene(p);DiSource s=(DiSource)0;
    s.kind=desc.kind;s.index=desc.index;s.first=desc.first;s.material=DI_NONE;
    if(desc.kind==0) {
        DeGpuLight g=de_robuffer<DeGpuLight>(sc.lights)[desc.index];
        DeClusterLight l=(DeClusterLight)0;uint kind=g.kind;
        float3 z=float3(g.transform[0].z,g.transform[1].z,g.transform[2].z);
        float3 x=float3(g.transform[0].x,g.transform[1].x,g.transform[2].x);
        l.position=float4(g.transform[0].w,g.transform[1].w,g.transform[2].w,g.params.x);
        l.direction=float4(normalize(-z),max(g.params.w,0));
        l.axis=float4(normalize(x),asfloat((g.flags&DE_GPU_LIGHT_CAST_SHADOWS)!=0?DE_CLUSTER_LIGHT_SHADOWS:0u));
        float intensity=g.color.w;
        if(kind==DE_LIGHT_KIND_POINT)intensity=de_point_candela(intensity);
        if(kind==DE_LIGHT_KIND_SPOT)intensity=de_spot_candela(intensity,g.params.z);
        l.color=float4(g.color.xyz*intensity,cos(g.params.z));
        l.shape=float4(1/max(cos(g.params.y)-cos(g.params.z),1e-4),g.area.x*.5,g.area.y*.5,asfloat(kind));
        if(kind==DE_LIGHT_KIND_DISK)l.shape.z=l.shape.y;
        if(!(l.position.w>0))l.position.w=de_light_derived_range(max(l.color.x,max(l.color.y,l.color.z)),.05);
        s.light=l;
        float area=kind==DE_LIGHT_KIND_RECT?g.area.x*g.area.y:kind==DE_LIGHT_KIND_DISK?3.14159265359*g.area.x*g.area.x*.25:1;
        s.power=max(0.0,di_luminance(g.color.xyz)*g.color.w*area);
        if((g.flags&DE_GPU_LIGHT_LIVE)==0 || kind==DE_LIGHT_KIND_DIRECTIONAL)s.power=0;
    } else {
        DeGpuInstance inst=de_robuffer<DeGpuInstance>(sc.instances)[desc.index];
        DeGpuMesh mesh=de_robuffer<DeGpuMesh>(sc.meshes)[inst.mesh];
        uint3 vertices=de_bytes(mesh.indices).load3(desc.first*4);
        s.material=de_vis_instance_material(inst,desc.submesh);
        DeGpuMaterial m=de_robuffer<DeGpuMaterial>(sc.materials)[s.material];
        float3 a=de_vis_transform_point(inst.transform,de_vis_mesh_position(mesh,vertices.x));
        float3 b=de_vis_transform_point(inst.transform,de_vis_mesh_position(mesh,vertices.y));
        float3 c=de_vis_transform_point(inst.transform,de_vis_mesh_position(mesh,vertices.z));
        float area=.5*length(cross(b-a,c-a));
        s.p0=float4(a,area);s.p1=float4(b,0);s.p2=float4(c,0);
        s.radiance=float4((m.model&(DE_GPU_MATERIAL_MODELS-1u))==DE_GPU_MATERIAL_MODEL_UNLIT?m.base_color.rgb:m.emissive.xyz*m.emissive.w,(m.flags&DE_GPU_MATERIAL_DOUBLE_SIDED)!=0?1:0);
        // Texture modulation is sampled at the actual point. Factor power keeps its full support.
        s.power=area*3.14159265359*di_luminance(s.radiance.xyz)*(s.radiance.w>0?2:1);
    }
    if(!isfinite(s.power))s.power=0;
    de_buffer<DiSource>(p.sources)[id.x]=s;
}
[shader("compute")][numthreads(1,1,1)]
void cs_alias(uint3 id:SV_DispatchThreadID,uniform DiPush pc) {
    DiParams p=di_params(pc);float total=0;
    for(uint i=0;i<p.source_count;++i)total+=de_robuffer<DiSource>(p.sources)[i].power;
    uint small=0,large=0;
    for(uint i=0;i<p.source_count;++i) {
        DiSource s=de_robuffer<DiSource>(p.sources)[i];s.pdf=total>0?s.power/total:1.0/max(p.source_count,1u);
        s.probability=s.pdf*p.source_count;s.alias=i;de_buffer<DiSource>(p.sources)[i]=s;
        if(s.probability<1)de_rwbytes(p.alias_work).store(small++*4,i);
        else de_rwbytes(p.alias_work).store((p.source_count+large++)*4,i);
    }
    while(small>0 && large>0) {
        uint a=de_bytes(p.alias_work).load(--small*4),b=de_bytes(p.alias_work).load((p.source_count+--large)*4);
        DiSource sa=de_robuffer<DiSource>(p.sources)[a],sb=de_robuffer<DiSource>(p.sources)[b];
        sa.alias=b;de_buffer<DiSource>(p.sources)[a]=sa;
        sb.probability=sb.probability+sa.probability-1;de_buffer<DiSource>(p.sources)[b]=sb;
        if(sb.probability<1)de_rwbytes(p.alias_work).store(small++*4,b);
        else de_rwbytes(p.alias_work).store((p.source_count+large++)*4,b);
    }
    while(large>0) {uint i=de_bytes(p.alias_work).load((p.source_count+--large)*4);DiSource s=de_robuffer<DiSource>(p.sources)[i];s.probability=1;de_buffer<DiSource>(p.sources)[i]=s;}
    while(small>0) {uint i=de_bytes(p.alias_work).load(--small*4);DiSource s=de_robuffer<DiSource>(p.sources)[i];s.probability=1;de_buffer<DiSource>(p.sources)[i]=s;}
}
[shader("compute")][numthreads(8,8,1)]
void cs_initial(uint3 id:SV_DispatchThreadID,uniform DiPush pc) {
    DiParams p=di_params(pc);if(id.x>=p.width || id.y>=p.height)return;
    uint pixel=id.y*p.width+id.x;DiSurface surface=di_surface(p,id.xy);DiReservoir r=di_empty();
    if(surface.valid)for(uint i=0;i<p.candidates;++i) {
        float2 a=de_rt_random(pixel,p.frame,i*3);
        uint entry=min(uint(a.x*p.source_count),p.source_count-1);
        DiSource s=de_robuffer<DiSource>(p.sources)[entry];uint source=a.y<s.probability?entry:s.alias;
        float2 uv=de_rt_random(pixel,p.frame,i*3+1);
        DiSample sample=di_evaluate(p,surface,source,uv);
        float pdf=de_robuffer<DiSource>(p.sources)[source].pdf;
        di_update(r,source,uv,sample.target,pdf>0?sample.target/pdf:0,1,de_rt_random(pixel,p.frame,i*3+2).x);
    }
    di_finalize(r,r.M);de_buffer<DiReservoir>(p.initial)[pixel]=r;
    de_buffer<DiHistory>(p.history_current)[pixel]=di_history(surface);
}
[shader("compute")][numthreads(8,8,1)]
void cs_initial_visibility(uint3 id:SV_DispatchThreadID,uniform DiPush pc) {
    DiParams p=di_params(pc);if(id.x>=p.width || id.y>=p.height)return;
    uint pixel=id.y*p.width+id.x;DiReservoir r=di_load(p.initial,pixel);
    if(pc.phase==0)de_buffer<DeRtRay>(p.rays)[pixel]=di_evaluate(p,di_surface(p,id.xy),r.source,float2(r.u,r.v)).ray;
    else {if(!di_visible(p,pixel))r.W=0;de_buffer<DiReservoir>(p.initial)[pixel]=r;}
}
