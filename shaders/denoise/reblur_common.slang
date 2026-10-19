module reblur_common;
__exported import reblur_weights;
__exported import "../visbuffer/gbuffer";
__exported import "../visbuffer/vis_decode";
__exported import "../scene/scene_types";
__exported import bindless;
__exported import "../rt/rt_hit_shading";
__exported import "../materials/standard/standard_base";
public struct ReblurParams {
    public uint width,height,scene,slot,view,entry,ids,ids_pitch;
    public uint visible,visible_count,sampler,lut,raw,distance,depth,gb0;
    public uint gb1,gb2,motion,material,input,output,factor,position;
    public uint out_motion,radius,alpha,length,kind,mip,quality,reserved;
    public uint rays,scale,out_hit,pad1;
};
public float reblur_roughness_alpha(float roughness){return max(.05,.5*pow(1-saturate(roughness),3));}
public float3 reblur_position(DeSceneConstants sc,uint2 xy,ReblurParams p){float4 clip=float4((float2(xy)+.5)/float2(p.width,p.height)*float2(2,-2)+float2(-1,1),de_tex2d(p.depth).Load(int3(xy,0)).x,1);float4 v=float4(dot(sc.inv_view_proj[0],clip),dot(sc.inv_view_proj[1],clip),dot(sc.inv_view_proj[2],clip),dot(sc.inv_view_proj[3],clip));return v.xyz/(abs(v.w)>1e-20?v.w:1e-20);}
public float2 reblur_virtual_uv(DeSceneConstants sc,float3 position,float3 normal,float hit,float2 surface_previous,float roughness){
    float3 incident=normalize(position-sc.camera_position.xyz),reflected=reflect(incident,normal),actual_hit=position+reflected*hit;
    float3 virtual_hit=actual_hit-2*normal*dot(actual_hit-position,normal);
    float4 previous=de_vis_clip(sc.prev_view_proj,virtual_hit);float2 virtual_uv=previous.xy/max(abs(previous.w),1e-20)*float2(.5,-.5)+.5;
    return lerp(virtual_uv,surface_previous,saturate(roughness*roughness*4));
}
public DeRtSurface reblur_surface(ReblurParams p,uint2 xy){DeSceneConstants sc=de_robuffer<DeSceneConstants>(p.scene)[p.slot];uint id=de_bytes(p.ids).load(xy.y*p.ids_pitch+xy.x*4);DeResolvePoint point=de_resolve_point(id,xy,float2(p.width,p.height),sc,p.visible,p.visible_count);return de_rt_surface(point,sc,sc.camera_position.xyz,p.sampler,p.lut);}
public float3 reblur_factor(SurfaceData surface,uint kind){return reblur_floor_factor(kind==0?surface.base_color:de_standard_fresnel(surface).f0);}
public float reblur_luma(float3 c){return reblur_luminance(c);}
public bool reblur_same(ReblurParams p,int2 a,int2 b){
    if(any(b<0)||b.x>=int(p.width)||b.y>=int(p.height))return false;
    if(any(de_tex2d(p.material).Load(int3(a,0)).xy!=de_tex2d(p.material).Load(int3(b,0)).xy))return false;
    DeGbSurface sa=gb_decode(float4(0),de_tex2d(p.gb1).Load(int3(a,0)),float4(0)),sb=gb_decode(float4(0),de_tex2d(p.gb1).Load(int3(b,0)),float4(0));

    float3 pa=de_tex2d(p.position).Load(int3(a,0)).xyz,pb=de_tex2d(p.position).Load(int3(b,0)).xyz;
    return reblur_geometry(sa.normal,sa.roughness,sb.normal,sb.roughness,pb-pa);
}
