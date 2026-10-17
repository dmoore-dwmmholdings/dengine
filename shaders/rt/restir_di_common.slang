module restir_di_common;
import bindless;
__exported import reservoir;
__exported import rt_types;
import rt_opacity;
__exported import light_sampling;
import "../scene/scene_types";
import "../visbuffer/vis_decode";
import "../visbuffer/resolve_common";
import "../visbuffer/gbuffer";
import "../materials/standard/standard";
import "../lighting/lighting_common";

public struct DiDescriptor { public uint kind,index,first,submesh; };
public struct DiSource {
    public uint kind,index,first,material;
    public float4 p0,p1,p2,radiance;
    public DeClusterLight light;
    public float power,pdf,probability;
    public uint alias;
};
public struct DiHistory { public float4 position,normal; };
// 64 scalar words; a distinct immutable upload entry belongs to each recorded pass.
public struct DiParams {
    public uint scene,scene_slot,view,entry;
    public uint depth,gb0,gb1,gb2;
    public uint motion,ids,visible,ids_pitch;
    public uint visible_count,sources,source_count,descriptors;
    public uint alias_work,initial,previous,temporal;
    public uint spatial,history_previous,history_current,rays;
    public uint hits,diffuse,specular,distance;
    public uint width,height,frame,candidates;
    public uint unbiased,initial_visibility,history_valid,metadata;
    public uint sampler,layers,unused1,unused2;
    public uint unused3,unused4,unused5,unused6;
    public uint unused7,unused8,unused9,unused10;
    public uint unused11,unused12,unused13,unused14;
    public uint unused15,unused16,unused17,unused18;
    public uint unused19,unused20,unused21,unused22;
    public uint unused23,unused24,unused25,unused26;
};
public struct DiPush { public uint params,entry,phase,source; };
public DiParams di_params(DiPush p) {return de_robuffer<DiParams>(p.params)[p.entry];}
public float di_luminance(float3 v) {return max(0.0,dot(v,float3(.2126,.7152,.0722)));}
public DeSceneConstants di_scene(DiParams p) {return de_robuffer<DeSceneConstants>(p.scene)[p.scene_slot];}
public float3 di_position(DeSceneConstants sc,uint2 pixel,DiParams p,float depth) {
    float4 c=float4((float2(pixel)+.5)/float2(p.width,p.height)*float2(2,-2)+float2(-1,1),depth,1);
    float4 w=float4(dot(sc.inv_view_proj[0],c),dot(sc.inv_view_proj[1],c),dot(sc.inv_view_proj[2],c),dot(sc.inv_view_proj[3],c));
    return w.xyz/w.w;
}
public struct DiSurface { public SurfaceData material;public float3 position,eye;public float depth;public bool valid; };
public DiSurface di_surface(DiParams p,uint2 pixel) {
    DiSurface s=(DiSurface)0;
    float z=de_tex2d(p.depth).Load(int3(pixel,0)).r;
    if(!(z>0))return s;
    DeGbSurface g=gb_decode(de_tex2d(p.gb0).Load(int3(pixel,0)),de_tex2d(p.gb1).Load(int3(pixel,0)),de_tex2d(p.gb2).Load(int3(pixel,0)));
    if((g.flags&DE_GB_FLAG_UNLIT)!=0)return s;
    DeSceneConstants sc=di_scene(p);DeLightingView lv=de_robuffer<DeLightingView>(p.view)[p.entry];
    s.position=di_position(sc,pixel,p,z);s.depth=-dot(sc.view[2],float4(s.position,1));
    s.eye=(lv.flags&DE_LIGHTING_ORTHO)!=0?normalize(sc.view[2].xyz):normalize(sc.camera_position.xyz-s.position);
    s.material=de_surface_from_gb(g);s.material.brdf_lut=lv.brdf_lut;
    uint id=de_bytes(p.ids).load(pixel.y*p.ids_pitch+pixel.x*4);
    DeResolvePoint point=de_resolve_point(id,pixel,float2(p.width,p.height),sc,p.visible,p.visible_count);
    if(point.valid) {
        uint material=de_resolve_material(point);
        if(material<sc.material_count) {
            DeGpuMaterial m=de_robuffer<DeGpuMaterial>(sc.materials)[material];
            StandardModel.layers_from_material(s.material,m);
            if((s.material.features&DE_MATERIAL_FEATURE_ANISOTROPY)!=0 && de_vis_mesh_has_stream(point.mesh,DE_VIS_STREAM_TANGENT)) {
                float3 geometric;float4 tangent;de_resolve_normal(point,tangent,geometric);
                if((m.flags&DE_GPU_MATERIAL_DOUBLE_SIDED)!=0 && dot(geometric,s.eye)<0)tangent.w=-tangent.w;
                s.material.tangent=tangent;
            }
        }
    }
    s.valid=true;return s;
}
public DiHistory di_history(DiSurface s) {
    DiHistory h;h.position=float4(s.position,s.valid?s.depth:0);h.normal=float4(s.material.normal,s.valid?1:0);return h;
}
public bool di_compatible(DiSurface s,DiHistory h) {
    return s.valid && h.normal.w>0 && dot(s.material.normal,h.normal.xyz)>.95 &&
        distance(s.position,h.position.xyz)<max(.02,.02*s.depth) && abs(s.depth-h.position.w)<max(.02,.02*s.depth);
}
public uint2 di_neighbor(uint2 pixel,uint neighbor,DiParams p) {
    if(neighbor==0)return pixel;
    float2 u=de_rt_random(pixel.y*p.width+pixel.x,p.frame,800+neighbor);
    float phi=6.28318530718*u.x;int2 offset=int2(round(float2(cos(phi),sin(phi))*(1+7*u.y)));
    return uint2(clamp(int2(pixel)+offset,int2(0),int2(p.width-1,p.height-1)));
}
public float3 di_emission(DiSource source,float2 uv,DiParams p) {
    float3 le=source.radiance.xyz;
    if(source.material==DI_NONE)return le;
    DeSceneConstants sc=di_scene(p);
    DeGpuMaterial m=de_robuffer<DeGpuMaterial>(sc.materials)[source.material];
    DeGpuInstance inst=de_robuffer<DeGpuInstance>(sc.instances)[source.index];
    if(p.layers!=0 && (inst.layer_mask&p.layers)==0)return float3(0);
    DeGpuMesh mesh=de_robuffer<DeGpuMesh>(sc.meshes)[inst.mesh];
    uint3 v=de_bytes(mesh.indices).load3(source.first*4);
    float a=sqrt(uv.x);float3 bary=float3(1-a,a*(1-uv.y),a*uv.y);
    DeResolvePoint point=(DeResolvePoint)0;point.inst=inst;point.mesh=mesh;point.vertex=v;point.bary=bary;
    if(!de_rt_material_opacity(point,m,p.sampler))return float3(0);
    uint tex=de_material_texture(m,4); // cooked EMISSIVE slot
    if(tex!=0) {
        uint set=de_material_texcoord(m,4);
        float2 t0=set==0?de_vis_mesh_uv0(mesh,v.x):de_vis_mesh_uv1(mesh,v.x);
        float2 t1=set==0?de_vis_mesh_uv0(mesh,v.y):de_vis_mesh_uv1(mesh,v.y);
        float2 t2=set==0?de_vis_mesh_uv0(mesh,v.z):de_vis_mesh_uv1(mesh,v.z);
        le*=de_tex2d(tex).SampleLevel(de_sampler(p.sampler),bary.x*t0+bary.y*t1+bary.z*t2,0).rgb;
    }
    return le;
}
public struct DiSample { public float3 diffuse,specular;public DeRtRay ray;public float target; };
public DiSample di_evaluate(DiParams p,DiSurface surface,uint source_index,float2 uv) {
    DiSample out=(DiSample)0;out.ray.direction=float4(0,0,1,0);
    if(!surface.valid || source_index>=p.source_count)return out;
    DiSource source=de_robuffer<DiSource>(p.sources)[source_index];
    float3 wi,incident;float limit;
    SurfaceData material=surface.material;
    if(source.kind==0) {
        DeClusterLight light=source.light;uint kind=de_light_kind(light);
        out.ray=de_rt_shadow_ray(light,surface.position,material.normal,uv);wi=out.ray.direction.xyz;limit=out.ray.direction.w;
        if(kind==DE_LIGHT_KIND_RECT || kind==DE_LIGHT_KIND_DISK) {
            float area=kind==DE_LIGHT_KIND_RECT?4*light.shape.y*light.shape.z:3.14159265359*light.shape.y*light.shape.y;
            float cosine=max(0.0,dot(light.direction.xyz,-wi));
            incident=light.color.xyz*(area*cosine/max(limit*limit,1e-6))*de_light_window(limit,light.position.w);
        } else {
            DeLightHit h=de_light_punctual(light,surface.position);incident=h.illuminance;
            material.roughness=de_light_widened_roughness(material.roughness,light.direction.w,h.distance);
        }
        if((asuint(light.axis.w)&DE_CLUSTER_LIGHT_SHADOWS)==0)out.ray.direction.w=0;
    } else {
        float a=sqrt(uv.x);float3 bary=float3(1-a,a*(1-uv.y),a*uv.y);
        float3 q=source.p0.xyz*bary.x+source.p1.xyz*bary.y+source.p2.xyz*bary.z;
        float3 delta=q-surface.position;limit=length(delta);wi=delta/max(limit,1e-8);
        float3 n=normalize(cross(source.p1.xyz-source.p0.xyz,source.p2.xyz-source.p0.xyz));
        float cosine=source.radiance.w>0?abs(dot(n,-wi)):max(0.0,dot(n,-wi));
        incident=di_emission(source,uv,p)*(source.p0.w*cosine/max(limit*limit,1e-6));
        float bias=max(.0001,.00001*max(abs(surface.position.x),max(abs(surface.position.y),abs(surface.position.z))));
        out.ray.origin=float4(surface.position+material.normal*(dot(material.normal,wi)>=0?bias:-bias),.00001);
        out.ray.direction=float4(wi,max(0.0,limit-2*bias));
    }
    ShadingFrame frame=de_surface_frame(material);float3 wo=frame.to_local(surface.eye),wil=frame.to_local(wi);
    if((material.features&DE_MATERIAL_FEATURE_DOUBLE_SIDED)!=0 && wo.z<0)wo.z=-wo.z;
    if(wil.z>0 || (material.features&DE_MATERIAL_FEATURE_TRANSMISSION)!=0) {
        float3 diffuse;float3 total=StandardModel.evaluate_split(material,wo,wil,diffuse)*incident;
        out.diffuse=max(diffuse*incident,float3(0));out.specular=max(total-out.diffuse,float3(0));
    }
    out.target=di_luminance(out.diffuse+out.specular);return out;
}
public DiReservoir di_load(uint buffer,uint pixel) {return de_robuffer<DiReservoir>(buffer)[pixel];}
public bool di_visible(DiParams p,uint ray) {return de_robuffer<DeRtHit>(p.hits)[ray].t<0;}
