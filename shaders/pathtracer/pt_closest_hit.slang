module pt_closest_hit;
__exported import "../rt/rt_hit_shading";
import "../rt/rt_common";
__exported import "../rt/restir_di_common";
__exported import pt_sampling;
public struct PtParams {
    public uint query,width,height,sample;
    public uint max_bounces,sources,source_count,env_cdf;
    public uint accumulation,output,brdf_lut,seed;
    public float4 inv_view_proj[4];
    public float4 camera;
    public DeIbl ibl;
    public uint environment_revision,flags,reserved0,reserved1;
    public float exposure; public uint light_count,unused0,unused1;
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
public PtStep pt_step(PtParams p,DeRtRay ray,DeRtHit hit,uint pixel,uint bounce,float last_pdf,float3 previous) {
    PtStep o=(PtStep)0;o.stop=1;
    uint strategy=p.flags&3u;
    float area_pdf;float4 area=pt_area_hit(p,ray,hit.t,area_pdf);
    if(area.w>=0) {o.emitted=area.rgb*(bounce==0||strategy==2?1:strategy==1?0:pt_power(last_pdf,area_pdf));return o;}
    if(hit.t<0) {float w=bounce==0||strategy==2?1:strategy==1?0:pt_power(last_pdf,pt_env_pdf(p,ray.direction.xyz)/3.0);o.emitted=pt_environment(p,ray.direction.xyz)*w;return o;}
    DeRtContext query=pt_query(p);
    DeRtSurface surface=de_rt_hit_surface(query,hit,ray.origin.xyz,p.brdf_lut);
    if(!surface.valid)return o;
    SurfaceData s=surface.material;float3 position=ray.origin.xyz+ray.direction.xyz*hit.t;
    o.emitted=s.emissive*(bounce==0||strategy==2?1:strategy==1?0:pt_power(last_pdf,pt_emission_pdf(p,hit,previous,position)));
    if((s.features&DE_MATERIAL_FEATURE_UNLIT)!=0)return o;
    ShadingFrame frame=de_surface_frame(s);float3 wo=frame.to_local(-ray.direction.xyz);
    uint dimension=2+bounce*12;float3 u=pt_random3(pixel^p.seed,p.sample,dimension);
    float3 wi=0,incident=0;float density=0,mis=1;bool casts=true;
    uint choice=min(uint(u.z*3),2u);
    if(choice==0) {
        float a=u.x;uint lo=0,hi=511;
        while(lo<hi){uint mid=(lo+hi)/2;if(de_robuffer<float2>(p.env_cdf)[mid].x<a)lo=mid+1;else hi=mid;}
        float prev=lo==0?0:de_robuffer<float2>(p.env_cdf)[lo-1].x;
        float2 cell=de_robuffer<float2>(p.env_cdf)[lo];
        float v=saturate((a-prev)/max(cell.y,1e-20));
        wi=pt_env_direction((float2(lo%32,lo/32)+float2(v,u.y))/float2(32,16));
        density=cell.y*512/(4*3.141592653589793)/3.0;incident=pt_environment(p,wi);
        mis=pt_power(density,StandardModel.pdf(s,wo,frame.to_local(wi)));
        o.shadow.origin=float4(de_rt_offset(position,s.normal,wi),.00001);o.shadow.direction=float4(wi,1e6);
    } else if(choice==1 && p.source_count>0) {
        uint entry=min(uint(u.x*p.source_count),p.source_count-1);DiSource a=de_robuffer<DiSource>(p.sources)[entry];
        uint index=u.y<a.probability?entry:a.alias;DiSource source=de_robuffer<DiSource>(p.sources)[index];
        float2 uv=float2(pt_random(pixel^p.seed,p.sample,dimension+3),pt_random(pixel^p.seed,p.sample,dimension+4));
        density=source.pdf/3.0;
        if(source.kind==0) {
            DeClusterLight light=source.light;o.shadow=de_rt_shadow_ray(light,position,s.normal,uv);wi=o.shadow.direction.xyz;
            uint kind=de_light_kind(light);casts=(asuint(light.axis.w)&DE_CLUSTER_LIGHT_SHADOWS)!=0;
            if(kind==DE_LIGHT_KIND_RECT||kind==DE_LIGHT_KIND_DISK) {
                float area=kind==DE_LIGHT_KIND_RECT?4*light.shape.y*light.shape.z:3.141592653589793*light.shape.y*light.shape.y;
                float cosine=max(0.0,dot(light.direction.xyz,-wi));
                density*=o.shadow.direction.w*o.shadow.direction.w/max(area*cosine,1e-20);
                incident=light.color.xyz*de_light_window(o.shadow.direction.w,light.position.w);
                mis=pt_power(density,StandardModel.pdf(s,wo,frame.to_local(wi)));
            } else incident=de_light_punctual(light,position).illuminance;
        } else {
            float a=sqrt(uv.x);float3 b=float3(1-a,a*(1-uv.y),a*uv.y);
            float3 target=source.p0.xyz*b.x+source.p1.xyz*b.y+source.p2.xyz*b.z;
            float3 delta=target-position;float distance=length(delta);wi=delta/max(distance,1e-15);
            float3 normal=normalize(cross(source.p1.xyz-source.p0.xyz,source.p2.xyz-source.p0.xyz));
            float cosine=source.radiance.w>0?abs(dot(normal,-wi)):max(0.0,dot(normal,-wi));
            density*=distance*distance/max(source.p0.w*cosine,1e-20);
            DiParams dp=(DiParams)0;dp.scene=query.scene;dp.scene_slot=query.view;dp.layers=query.layers;dp.sampler=query.sampler;
            incident=di_emission(source,uv,dp);mis=pt_power(density,StandardModel.pdf(s,wo,frame.to_local(wi)));
            o.shadow.origin=float4(de_rt_offset(position,s.normal,wi),.00001);o.shadow.direction=float4(wi,max(0.0,distance-.0002));
        }
    } else if(choice==2 && p.light_count>0) {
        DeSceneConstants sc=de_robuffer<DeSceneConstants>(query.scene)[query.view];
        // One stable uniform proposal over authored slots; non-directional slots have zero response here.
        uint index=min(uint(u.x*p.light_count),p.light_count-1);DeGpuLight l=de_robuffer<DeGpuLight>(sc.lights)[index];
        if(l.kind==DE_LIGHT_KIND_DIRECTIONAL && (l.flags&DE_GPU_LIGHT_LIVE)!=0) {
            wi=normalize(float3(l.transform[0].z,l.transform[1].z,l.transform[2].z));incident=l.color.rgb*l.color.w;
            density=1.0/(3*p.light_count);casts=(l.flags&DE_GPU_LIGHT_CAST_SHADOWS)!=0;
            o.shadow.origin=float4(de_rt_offset(position,s.normal,wi),.00001);o.shadow.direction=float4(wi,1e6);
        }
    }
    if(density>0 && strategy!=2)o.direct=StandardModel.evaluate(s,wo,frame.to_local(wi))*incident*((strategy==1?1:mis)/density);
    if(!casts)o.shadow.direction.w=0;
    BsdfSample next=StandardModel.sample(s,wo,pt_random3(pixel^p.seed,p.sample,dimension+5));
    if(!(next.pdf>0)||!all(isfinite(next.weight)))return o;
    o.pdf=next.pdf;o.weight=next.weight;wi=normalize(frame.to_world(next.wi));
    o.next.origin=float4(de_rt_offset(position,s.normal,wi),.00001);o.next.direction=float4(wi,1e6);o.stop=0;
    // DENG-73: medium stack/SSS insertion; DENG-74: first-hit AOV insertion, before continuation.
    return o;
}
