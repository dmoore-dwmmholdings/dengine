module pt_closest_hit;
__exported import "../rt/rt_hit_shading";
import "../rt/rt_common";
__exported import "../rt/restir_di_common";
__exported import pt_sampling;
__exported import pt_volume;
import "../visbuffer/vis_decode";
import "../materials/standard/volume";
__exported import pt_sss;
public struct PtMediaParams {public uint fog,count,features,max_steps,stats,stats_offset,unused0,unused1;};
public struct PtParams {
    public uint query,width,height,sample;
    public uint max_bounces,sources,source_count,env_cdf;
    public uint accumulation,output,brdf_lut,seed;
    public float4 inv_view_proj[4];
    public float4 camera;
    public DeIbl ibl;
    public uint environment_revision,flags,reserved0,reserved1;
    public float exposure; public uint light_count,unused0,unused1;
    public PtMediaParams media;
    public uint aov_albedo,aov_normal,aov_depth,aov_position,aov_material,aov_pad0,aov_pad1,aov_pad2;
};
public PtParams pt_load(uint buffer,uint entry) {return de_robuffer<PtParams>(buffer)[entry];}
public struct PtStep {
    public float3 emitted;
    public float3 direct;
    public DeRtRay shadow;
    public DeRtRay next;
    public float3 weight;
    public float pdf;
    public uint stop;
    public uint consume,lobe;
};
public float3 pt_environment(PtParams p,float3 wi) { return de_ibl_radiance(p.ibl,wi,0); }
public float pt_env_pdf(PtParams p,float3 wi) {
    float phi=atan2(wi.y,wi.x);if(phi<0)phi+=6.283185307179586;
    uint x=min(uint(phi*(32.0/6.283185307179586)),31u),y=min(uint((1-wi.z)*8),15u);
    return de_robuffer<float2>(p.env_cdf)[y*32+x].y*(512.0/(4*3.141592653589793));
}
public DeRtRay pt_camera(PtParams p,uint pixel) {
    uint2 xy=uint2(pixel%p.width,pixel/p.width);
    float2 jitter=float2(pt_random(pixel^p.seed,p.sample,0),pt_random(pixel^p.seed,p.sample,1));
    float4 c=float4((float2(xy)+jitter)/float2(p.width,p.height)*float2(2,-2)+float2(-1,1),1,1);
    float4 v=float4(dot(p.inv_view_proj[0],c),dot(p.inv_view_proj[1],c),dot(p.inv_view_proj[2],c),dot(p.inv_view_proj[3],c));
    float3 position=v.xyz/v.w;
    DeRtRay r;r.origin=float4(p.camera.xyz,.00001);r.direction=float4(normalize(position-p.camera.xyz),1e6);return r;
}
public DeRtContext pt_query(PtParams p) { return de_robuffer<DeRtContext>(p.query)[0]; }
public float pt_emission_pdf(PtParams p,DeRtHit h,float3 previous,float3 position) {
    float result=0;
    for(uint i=0;i<p.source_count;++i) {
        DiSource s=de_robuffer<DiSource>(p.sources)[i];
        if(s.kind!=1 || s.index!=h.instance)continue;
        DeRtContext q=pt_query(p);DeSceneConstants sc=de_robuffer<DeSceneConstants>(q.scene)[q.view];
        DeGpuInstance inst=de_robuffer<DeGpuInstance>(sc.instances)[h.instance];
        DeGpuMesh mesh=de_robuffer<DeGpuMesh>(sc.meshes)[inst.mesh];
        if(mesh.lods[h.lod].index_first+h.primitive*3!=s.first)continue;
        float3 delta=position-previous;float d2=dot(delta,delta);
        float cosine=abs(dot(normalize(cross(s.p1.xyz-s.p0.xyz,s.p2.xyz-s.p0.xyz)),-normalize(delta)));
        result+=s.pdf*d2/max(s.p0.w*cosine,1e-20)/3.0;
    }
    return result;
}
// Analytic area emitters are visible to BSDF rays as well as NEE. Geometry still occludes them.
public float4 pt_area_hit(PtParams p,DeRtRay ray,float geometry_t,out float light_pdf) {
    float nearest=geometry_t>=0?geometry_t:ray.direction.w;float3 emitted=0;light_pdf=0;
    for(uint i=0;i<p.source_count;++i) {
        DiSource source=de_robuffer<DiSource>(p.sources)[i];if(source.kind!=0)continue;
        DeClusterLight light=source.light;uint kind=de_light_kind(light);
        if(kind!=DE_LIGHT_KIND_RECT&&kind!=DE_LIGHT_KIND_DISK)continue;
        float3 normal=normalize(light.direction.xyz);float denom=dot(normal,ray.direction.xyz);
        if(!(denom<-.000001))continue;
        float t=dot(light.position.xyz-ray.origin.xyz,normal)/denom;
        if(!(t>ray.origin.w&&t<nearest))continue;
        float3 x=normalize(light.axis.xyz),y=normalize(cross(normal,x));
        float3 delta=ray.origin.xyz+ray.direction.xyz*t-light.position.xyz;
        float2 uv=float2(dot(delta,x)/max(light.shape.y,1e-10),dot(delta,y)/max(light.shape.z,1e-10));
        bool inside=kind==DE_LIGHT_KIND_RECT?all(abs(uv)<=1):dot(uv,uv)<=1;
        if(!inside)continue;nearest=t;emitted=light.color.rgb*de_light_window(t,light.position.w);
        float area=kind==DE_LIGHT_KIND_RECT?4*light.shape.y*light.shape.z:3.141592653589793*light.shape.y*light.shape.y;
        light_pdf=source.pdf*t*t/(3*max(area*(-denom),1e-20));
    }
    return float4(emitted,light_pdf>0?nearest:-1);
}
public struct PtLightProposal {public float3 wi,incident;public float pdf;public uint delta;public DeRtRay shadow;};
public PtLightProposal pt_light_sample(PtParams p,float3 position,float3 normal,uint pixel,uint bounce) {
    PtLightProposal o=(PtLightProposal)0;o.delta=1;DeRtContext query=pt_query(p);
    uint dimension=2+bounce*12;float3 u=pt_random3(pixel^p.seed,p.sample,dimension);
    float3 wi=0,incident=0;float density=0;bool casts=true;
    uint choice=min(uint(u.z*3),2u);
    if(choice==0) {o.delta=0;
        float a=u.x;uint lo=0,hi=511;
        while(lo<hi){uint mid=(lo+hi)/2;if(de_robuffer<float2>(p.env_cdf)[mid].x<a)lo=mid+1;else hi=mid;}
        float prev=lo==0?0:de_robuffer<float2>(p.env_cdf)[lo-1].x;
        float2 cell=de_robuffer<float2>(p.env_cdf)[lo];
        float v=saturate((a-prev)/max(cell.y,1e-20));
        wi=pt_env_direction((float2(lo%32,lo/32)+float2(v,u.y))/float2(32,16));
        density=cell.y*512/(4*3.141592653589793)/3.0;incident=pt_environment(p,wi);

        o.shadow.origin=float4(de_rt_offset(position,normal,wi),.00001);o.shadow.direction=float4(wi,1e6);
    } else if(choice==1 && p.source_count>0) {
        uint entry=min(uint(u.x*p.source_count),p.source_count-1);DiSource a=de_robuffer<DiSource>(p.sources)[entry];
        uint index=u.y<a.probability?entry:a.alias;DiSource source=de_robuffer<DiSource>(p.sources)[index];
        float2 uv=float2(pt_random(pixel^p.seed,p.sample,dimension+3),pt_random(pixel^p.seed,p.sample,dimension+4));
        density=source.pdf/3.0;
        if(source.kind==0) {
            DeClusterLight light=source.light;o.shadow=de_rt_shadow_ray(light,position,normal,uv);wi=o.shadow.direction.xyz;
            uint kind=de_light_kind(light);casts=(asuint(light.axis.w)&DE_CLUSTER_LIGHT_SHADOWS)!=0;
            if(kind==DE_LIGHT_KIND_RECT||kind==DE_LIGHT_KIND_DISK) {o.delta=0;
                float area=kind==DE_LIGHT_KIND_RECT?4*light.shape.y*light.shape.z:3.141592653589793*light.shape.y*light.shape.y;
                float cosine=max(0.0,dot(light.direction.xyz,-wi));
                density*=o.shadow.direction.w*o.shadow.direction.w/max(area*cosine,1e-20);
                incident=light.color.xyz*de_light_window(o.shadow.direction.w,light.position.w);

            } else incident=de_light_punctual(light,position).illuminance;
        } else {o.delta=0;
            float a=sqrt(uv.x);float3 b=float3(1-a,a*(1-uv.y),a*uv.y);
            float3 target=source.p0.xyz*b.x+source.p1.xyz*b.y+source.p2.xyz*b.z;
            float3 delta=target-position;float distance=length(delta);wi=delta/max(distance,1e-15);
            float3 normal=normalize(cross(source.p1.xyz-source.p0.xyz,source.p2.xyz-source.p0.xyz));
            float cosine=source.radiance.w>0?abs(dot(normal,-wi)):max(0.0,dot(normal,-wi));
            density*=distance*distance/max(source.p0.w*cosine,1e-20);
            DiParams dp=(DiParams)0;dp.scene=query.scene;dp.scene_slot=query.view;dp.layers=query.layers;dp.sampler=query.sampler;
            incident=di_emission(source,uv,dp);
            o.shadow.origin=float4(de_rt_offset(position,normal,wi),.00001);o.shadow.direction=float4(wi,max(0.0,distance-.0002));
        }
    } else if(choice==2 && p.light_count>0) {
        DeSceneConstants sc=de_robuffer<DeSceneConstants>(query.scene)[query.view];
        // One stable uniform proposal over authored slots; non-directional slots have zero response here.
        uint index=min(uint(u.x*p.light_count),p.light_count-1);DeGpuLight l=de_robuffer<DeGpuLight>(sc.lights)[index];
        if(l.kind==DE_LIGHT_KIND_DIRECTIONAL && (l.flags&DE_GPU_LIGHT_LIVE)!=0) {
            wi=normalize(float3(l.transform[0].z,l.transform[1].z,l.transform[2].z));incident=l.color.rgb*l.color.w;
            density=1.0/(3*p.light_count);casts=(l.flags&DE_GPU_LIGHT_CAST_SHADOWS)!=0;
            o.shadow.origin=float4(de_rt_offset(position,normal,wi),.00001);o.shadow.direction=float4(wi,1e6);
        }
    }
    if(!casts)o.shadow.direction.w=0;
    o.wi=wi;o.incident=incident;o.pdf=density;return o;
}
public PtStep pt_step(PtParams p,DeRtRay ray,DeRtHit hit,uint pixel,uint bounce,float last_pdf,float3 previous) {
    PtStep o=(PtStep)0;o.stop=1;o.consume=1;
    uint strategy=p.flags&3u;
    float area_pdf;float4 area=pt_area_hit(p,ray,hit.t,area_pdf);
    if(area.w>=0) {o.emitted=area.rgb*(bounce==0||strategy==2?1:strategy==1?0:pt_power(last_pdf,area_pdf));return o;}
    if(hit.t<0) {float w=bounce==0||strategy==2?1:strategy==1?0:pt_power(last_pdf,pt_env_pdf(p,ray.direction.xyz)/3.0);o.emitted=pt_environment(p,ray.direction.xyz)*w;return o;}
    DeRtContext query=pt_query(p);
    DeRtSurface surface=de_rt_hit_surface(query,hit,ray.origin.xyz,p.brdf_lut);
    if(!surface.valid)return o;
    SurfaceData s=surface.material;
    if((p.media.features&2u)!=0 && (s.features&DE_MATERIAL_FEATURE_VOLUME)!=0)s.attenuation_color=float3(1);
    float3 position=ray.origin.xyz+ray.direction.xyz*hit.t;
    o.emitted=s.emissive*(bounce==0||strategy==2?1:strategy==1?0:pt_power(last_pdf,pt_emission_pdf(p,hit,previous,position)));
    if((s.features&DE_MATERIAL_FEATURE_UNLIT)!=0)return o;
    ShadingFrame frame=de_surface_frame(s);float3 wo=frame.to_local(-ray.direction.xyz);
    uint dimension=2+bounce*12;
    PtLightProposal light=pt_light_sample(p,position,s.normal,pixel,bounce);o.shadow=light.shadow;
    float3 wi=light.wi;float mis=light.delta!=0?1:pt_power(light.pdf,StandardModel.pdf(s,wo,frame.to_local(wi)));
    if(light.pdf>0 && strategy!=2) {
        float3 d;float3 value=StandardModel.evaluate_split(s,wo,frame.to_local(wi),d);
        if((p.media.features&4u)!=0) {
            DeResolvePoint point=de_rt_point(query,hit.instance,hit.primitive,float2(hit.u,hit.v));
            DeGpuMaterial raw=de_resolve_material_of(de_resolve_material(point),de_robuffer<DeSceneConstants>(query.scene)[query.view]);
            if(max(raw.subsurface.x,max(raw.subsurface.y,raw.subsurface.z))>1e-6)value-=d*saturate(raw.extra.z);
        }
        o.direct=value*light.incident*((strategy==1?1:mis)/light.pdf);
    }
    BsdfSample next=StandardModel.sample(s,wo,pt_random3(pixel^p.seed,p.sample,dimension+5));
    if(!(next.pdf>0)||!all(isfinite(next.weight)))return o;
    o.pdf=next.pdf;o.weight=next.weight;o.lobe=next.lobe;wi=normalize(frame.to_world(next.wi));
    o.next.origin=float4(de_rt_offset(position,s.normal,wi),.00001);o.next.direction=float4(wi,1e6);o.stop=0;
    // DENG-73: medium stack/SSS insertion; DENG-74: first-hit AOV insertion, before continuation.
    return o;
}

// DENG-73 shared state machine. Native wrappers provide traversal only; all media math stays here.
public struct PtPath {
    public DeRtRay ray;
    public PtMediumStack media;
    public PtSssState sss;
    public float3 throughput,radiance,previous,position;
    public float pdf;
    public uint bounce,event,finished;
};
public PtPath pt_path_init(PtParams p,uint pixel) {
    PtPath path=(PtPath)0;path.ray=pt_camera(p,pixel);path.throughput=float3(1);path.previous=path.ray.origin.xyz;return path;
}
public float3 pt_geometric_normal(DeRtContext q,DeRtHit hit) {
    DeResolvePoint point=de_rt_point(q,hit.instance,hit.primitive,float2(hit.u,hit.v));
    float3 a=de_vis_transform_point(point.inst.transform,point.object_pos[0]);
    float3 b=de_vis_transform_point(point.inst.transform,point.object_pos[1]);
    float3 c=de_vis_transform_point(point.inst.transform,point.object_pos[2]);return normalize(cross(b-a,c-a));
}
public PtStep pt_phase_step(PtParams p,PtPath path,float3 position,float g,uint pixel) {
    PtStep out=(PtStep)0;out.consume=1;out.weight=float3(1);
    PtLightProposal light=pt_light_sample(p,position,-path.ray.direction.xyz,pixel,path.bounce+path.event*17);
    float phase=pt_hg(dot(path.ray.direction.xyz,light.wi),g);uint strategy=p.flags&3;
    float mis=light.delta!=0?1:pt_power(light.pdf,phase);
    if(light.pdf>0&&strategy!=2)out.direct=light.incident*phase*((strategy==1?1:mis)/light.pdf);
    out.shadow=light.shadow;float2 u=float2(pt_random(pixel^p.seed,p.sample,4096+path.event*5),pt_random(pixel^p.seed,p.sample,4097+path.event*5));
    float3 direction=pt_hg_sample(path.ray.direction.xyz,g,u);out.pdf=pt_hg(dot(path.ray.direction.xyz,direction),g);
    out.next.origin=float4(position+direction*.00001,.00001);out.next.direction=float4(direction,1e6);return out;
}
public PtStep pt_path_event(PtParams p,inout PtPath path,DeRtHit hit,uint pixel) {
    path.event++;float limit=hit.t>=0?hit.t:path.ray.direction.w;
    float area_pdf;float4 area=pt_area_hit(p,path.ray,hit.t,area_pdf);if(area.w>=0)limit=min(limit,area.w);
    PtStep step=(PtStep)0;step.stop=1;
    if(path.sss.active!=0) {
        float2 u=float2(pt_random(pixel^p.seed,p.sample,8192+path.event*7),pt_random(pixel^p.seed,p.sample,8193+path.event*7));
        PtSssFlight flight=pt_sss_flight(path.sss,limit,u);path.throughput*=flight.weight;
        path.position=path.ray.origin.xyz+path.ray.direction.xyz*flight.distance;
        if(flight.scattered!=0){
            if(++path.sss.steps>=p.media.max_steps)return step;
            step=pt_phase_step(p,path,path.position,0,pixel);step.consume=0;return step;
        }
        if(hit.t<0)return step;
        // The exit dielectric is the existing shared BSDF; TIR continues the internal walk.
        DeRtContext q=pt_query(p);DeRtSurface surface=de_rt_hit_surface(q,hit,path.ray.origin.xyz,p.brdf_lut);
        SurfaceData exit=surface.material;float3 normal=pt_geometric_normal(q,hit);exit.normal=normal;exit.clearcoat_normal=normal;
        exit.features|=DE_MATERIAL_FEATURE_TRANSMISSION|DE_MATERIAL_FEATURE_VOLUME;exit.transmission=1;exit.thickness=1;exit.attenuation_color=float3(1);
        ShadingFrame frame=de_surface_frame(exit);BsdfSample next=StandardModel.sample(exit,frame.to_local(-path.ray.direction.xyz),pt_random3(pixel^p.seed,p.sample,16384+path.event*3));
        if(!(next.pdf>0))return step;float3 wi=normalize(frame.to_world(next.wi));
        step.stop=0;step.weight=next.weight;step.pdf=next.pdf;step.consume=dot(wi,normal)>0?1u:0u;
        if(step.consume!=0)path.sss.active=0;
        step.next.origin=float4(de_rt_offset(path.position,normal,wi),.00001);step.next.direction=float4(wi,1e6);return step;
    }
    PtFogEvent fog=pt_fog_event(p.media.fog,p.media.count,path.ray.origin.xyz,path.ray.direction.xyz,limit,pixel^p.seed,p.sample,32768+path.event*37);
    path.throughput*=pt_medium_transmittance(path.media,fog.distance);
    path.position=path.ray.origin.xyz+path.ray.direction.xyz*fog.distance;
    if(fog.scattered!=0){path.throughput*=fog.albedo;return pt_phase_step(p,path,path.position,fog.g,pixel);}
    step=pt_step(p,path.ray,hit,pixel,path.bounce,path.pdf,path.previous);
    if(hit.t<0||step.stop!=0)return step;
    DeRtContext q=pt_query(p);DeRtSurface surface=de_rt_hit_surface(q,hit,path.ray.origin.xyz,p.brdf_lut);SurfaceData s=surface.material;
    float3 normal=pt_geometric_normal(q,hit);
    if((s.features&DE_MATERIAL_FEATURE_VOLUME)!=0&&s.thickness>0) {
        if(path.media.depth==0&&dot(path.ray.direction.xyz,normal)>0)
            path.throughput*=volume_transmittance(s.attenuation_color,s.attenuation_distance,hit.t); // camera began inside
        if((step.lobe&DE_BSDF_LOBE_TRANSMISSION)!=0&&dot(path.ray.direction.xyz,normal)*dot(step.next.direction.xyz,normal)>0) {
            if(dot(path.ray.direction.xyz,normal)<0){PtMedium m;m.instance=hit.instance;m.color=s.attenuation_color;m.distance=s.attenuation_distance;pt_medium_enter(path.media,m);}
            else pt_medium_exit(path.media,hit.instance);
        }
    }
    if((s.features&DE_MATERIAL_FEATURE_SUBSURFACE)!=0&&(step.lobe&DE_BSDF_LOBE_DIFFUSE)!=0) {
        DeResolvePoint point=de_rt_point(q,hit.instance,hit.primitive,float2(hit.u,hit.v));DeSceneConstants sc=de_robuffer<DeSceneConstants>(q.scene)[q.view];
        DeGpuMaterial raw=de_resolve_material_of(de_resolve_material(point),sc);float amount=saturate(raw.extra.z);float3 radius=max(raw.subsurface.rgb,float3(0));
        if(max(radius.x,max(radius.y,radius.z))>1e-6&&pt_random(pixel^p.seed,p.sample,65536+path.event)<amount) {
            PtSssCoefficients c=pt_sss_coefficients(radius,s.base_color);path.sss.active=1;path.sss.instance=hit.instance;path.sss.steps=0;path.sss.sigma_s=c.sigma_s;path.sss.sigma_a=c.sigma_a;
            ShadingFrame inward=de_frame_from_normal(-normal);float3 wi=inward.to_world(de_sample_cosine_hemisphere(float2(pt_random(pixel^p.seed,p.sample,65537+path.event*2),pt_random(pixel^p.seed,p.sample,65538+path.event*2))));
            step.next.origin=float4(path.position-normal*.00001,.00001);step.next.direction=float4(wi,1e6);step.weight/=max(s.base_color,float3(1e-6));
        }
    }
    return step;
}
public float3 pt_path_shadow(PtParams p,PtPath path,DeRtRay ray) {
    return pt_medium_transmittance(path.media,ray.direction.w)*pt_fog_transmittance(p.media.fog,p.media.count,ray.origin.xyz,ray.direction.xyz,ray.direction.w);
}
public void pt_path_finish(PtParams p,inout PtPath path,PtStep step,float3 visibility,uint pixel) {
    path.radiance+=path.throughput*(step.emitted+step.direct*visibility);
    if(step.stop!=0){path.finished=1;return;}
    path.throughput*=step.weight;
    if(!all(isfinite(path.throughput))||!any(path.throughput>0)){path.finished=1;return;}
    if(step.consume!=0&&path.bounce>=3){float survival=clamp(max(path.throughput.x,max(path.throughput.y,path.throughput.z)),.05,.95);
        if(pt_random(pixel^p.seed,p.sample,2+path.bounce*12+8)>=survival){path.finished=1;return;}path.throughput/=survival;}
    path.previous=path.position;path.ray=step.next;path.pdf=step.pdf;path.bounce+=step.consume;
    if(path.bounce>=p.max_bounces)path.finished=1;
}
public void pt_path_diagnostics(PtParams p,PtPath path) {
    if(path.media.overflow!=0)de_rwbytes(p.media.stats).atomic_add(p.media.stats_offset,path.media.overflow);
}
