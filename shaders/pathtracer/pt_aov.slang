module pt_aov;
__exported import pt_closest_hit;
// One common first-hit implementation; the native wrapper calls its generated version.
public void pt_store_aov(PtParams p,DeRtRay ray,DeRtHit hit,uint pixel) {
    uint2 xy=uint2(pixel%p.width,pixel/p.width);
    float4 albedo=0,normal=0,position=0;float depth=0;uint material=0xffffffffu;
    if(hit.t>=0) {
        DeRtContext q=pt_query(p);DeRtSurface surface=de_rt_hit_surface(q,hit,ray.origin.xyz,p.brdf_lut);
        if(surface.valid){albedo=float4(surface.material.base_color,1);normal=float4(surface.material.normal,surface.material.roughness);depth=hit.t;position=float4(ray.origin.xyz+ray.direction.xyz*hit.t,1);
            DeSceneConstants sc=de_robuffer<DeSceneConstants>(q.scene)[q.view];DeResolvePoint point=de_rt_point(q,hit.instance,hit.primitive,float2(hit.u,hit.v));material=de_resolve_material(point);}
    }
    de_rwtex2d(p.aov_albedo)[xy]=albedo;de_rwtex2d(p.aov_normal)[xy]=normal;de_rwtex2d(p.aov_depth)[xy]=float4(depth,0,0,0);de_rwtex2d(p.aov_position)[xy]=position;de_rwtex2d(p.aov_material)[xy]=float4(material&65535u,material>>16,0,0);
}
