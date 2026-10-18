// DENG-48: shared one-bounce hit reconstruction and registered builtin shading for49/50.
module rt_hit_shading;
__exported import bindless;
__exported import "../visbuffer/resolve_common";
__exported import rt_opacity;
__exported import "../visbuffer/resolve_model";
__exported import "../materials/standard/standard";
__exported import "../materials/unlit/unlit";
__exported import "../lighting/lighting_common";
import "../shadows/atlas_sample";
import "../lighting/ltc";
import "../probes/reflection_probe_sample";
__exported import "../brdf/ggx";
__exported import "../brdf/sampling";
public struct DeRtSurface {public SurfaceData material;public float3 position;public bool valid;};
public DeRtSurface de_rt_surface(DeResolvePoint point,DeSceneConstants sc,float3 eye,uint sampler,uint lut) {
    DeRtSurface result=(DeRtSurface)0;if(!point.valid)return result;
    DeGpuMaterial m=de_resolve_material_of(de_resolve_material(point),sc);
    TextureLodInfo lod;VertexAttribs v=de_resolve_attribs(point,eye,lod);lod.sampler=sampler;
    result.material=m.model==1?UnlitModel.resolve_gbuffer(m,v,lod):StandardModel.resolve_gbuffer(m,v,lod);
    result.material.brdf_lut=lut;result.position=v.position;result.valid=true;return result;
}
public DeRtSurface de_rt_hit_surface(DeRtContext q,DeRtHit hit,float3 eye,uint lut) {
    if(hit.t<0)return (DeRtSurface)0;
    return de_rt_surface(de_rt_point(q,hit.instance,hit.primitive,float2(hit.u,hit.v)),de_robuffer<DeSceneConstants>(q.scene)[q.view],eye,q.sampler,lut);
}
// Per-light secondary-surface evaluation shares the registered BSDF and analytic LTC path.
public float3 de_rt_local_light<M:IMaterialSplitModel>(SurfaceData s,float3 position,float3 eye,DeLightingView lv,DeClusterLists cl,DeClusterLight light) {
    if(de_light_kind(light)==DE_LIGHT_KIND_RECT||de_light_kind(light)==DE_LIGHT_KIND_DISK) {
        float3 d,sp;de_ltc_area_light(light,position,s.normal,eye,s.roughness,lerp(float3(.04)*s.specular,s.base_color,s.metallic),lerp(s.specular,1.0,s.metallic),cl.ltc1,cl.ltc2,cl.sampler,d,sp);
        return d*s.base_color*(1-s.metallic)+sp;
    }
    DeDirect result=(DeDirect)0;ShadingFrame frame=de_surface_frame(s);DeLightHit h=de_light_punctual(light,position);
    add_light<M>(result,s,frame,frame.to_local(eye),h.wi,h.illuminance,false);return result.diffuse+result.specular;
}
public float3 de_rt_local_direct<M:IMaterialSplitModel>(SurfaceData s,float3 position,float3 eye,DeLightingView lv,DeClusterLists cl) {
    float3 result=0;uint count=min(de_bytes(cl.counters).load(DE_LIGHTING_COUNTER_LOCAL),lv.local_capacity);
    for(uint i=0;i<count;++i) {
        DeClusterLight light=de_robuffer<DeClusterLight>(cl.lights)[i];
        float visibility=(lv.inputs.reserved0&DE_INPUT_ATLAS_METADATA)!=0?atlas_shadow(lv.inputs.shadow_local,(asuint(light.axis.w)>>8)&255u,position,s.normal):1;
        result+=de_rt_local_light<M>(s,position,eye,lv,cl,light)*visibility;
    }return result;
}
public float3 de_rt_sun_direct<M:IMaterialSplitModel>(SurfaceData s,float3 position,float3 eye,DeClusterLight light) {
    DeDirect result=(DeDirect)0;ShadingFrame frame=de_surface_frame(s);DeLightHit h=de_light_punctual(light,position);
    add_light<M>(result,s,frame,frame.to_local(eye),h.wi,h.illuminance,false);return result.diffuse+result.specular;
}
public float3 de_rt_environment(DeLightingView lv,DeClusterLists cl,float3 position,float3 direction,uint cluster) {
    float4 local=de_probe_radiance(lv.inputs.probe_records,lv.inputs.probe_list,lv.inputs.probe_entry,lv.inputs.probe_sampler,cluster,position,direction,0);
    return lerp(de_ibl_radiance(lv.ibl,direction,0),local.rgb,saturate(local.a));
}
public float3 de_rt_hit_radiance<M:IMaterialSplitModel>(DeRtSurface hit,float3 eye,DeLightingView lv,DeClusterLists cl,uint cluster,bool local_lights=true) {
    SurfaceData s=hit.material;if((s.features&DE_MATERIAL_FEATURE_UNLIT)!=0)return s.emissive;
    float3 reflection=reflect(-eye,s.normal);
    float4 base=de_probe_radiance(lv.inputs.probe_records,lv.inputs.probe_list,lv.inputs.probe_entry,lv.inputs.probe_sampler,cluster,hit.position,de_ibl_dominant_dir(s.normal,reflection,s.roughness),s.roughness);
    float4 coat=de_probe_radiance(lv.inputs.probe_records,lv.inputs.probe_list,lv.inputs.probe_entry,lv.inputs.probe_sampler,cluster,hit.position,de_ibl_coat_direction(s,eye),s.clearcoat_roughness);
    float4 sheen=de_probe_radiance(lv.inputs.probe_records,lv.inputs.probe_list,lv.inputs.probe_entry,lv.inputs.probe_sampler,cluster,hit.position,de_ibl_sheen_direction(s,eye),s.sheen_roughness);
    float3 diffuse,specular;de_ibl_evaluate(s,eye,lv.ibl,lv.brdf_lut,s.ao,diffuse,specular,base,coat,sheen);
    return s.emissive+(local_lights?de_rt_local_direct<M>(s,hit.position,eye,lv,cl):float3(0))+diffuse+specular;
}

// Material evaluators already return BSDF * abs(cos(theta_i)); only divide by proposal PDF.
public float3 de_rt_specular_weight<M:IMaterialSplitModel>(SurfaceData s,float3 wo,float3 wi,float pdf) {
    if(!(pdf>0))return float3(0);float3 diffuse;return (M.evaluate_split(s,wo,wi,diffuse)-diffuse)/pdf;
}
