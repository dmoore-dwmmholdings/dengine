// Shared Vulkan/DXR candidate and Metal IFT material-opacity implementation (DENG-44).
module rt_opacity;
import bindless;
import sprite_opacity;
__exported import rt_types;
import "../visbuffer/resolve_common";
import "../visbuffer/vis_decode";
import "../scene/scene_types";

public DeResolvePoint de_rt_point(DeRtContext q,uint instance,uint primitive,float2 bary) {
    DeResolvePoint r=(DeResolvePoint)0;
    DeSceneConstants sc=de_robuffer<DeSceneConstants>(q.scene)[q.view];
    if(instance>=sc.instance_count)return r;
    r.inst=de_robuffer<DeGpuInstance>(sc.instances)[instance];
    if((r.inst.flags&DE_GPU_INSTANCE_LIVE)==0 || r.inst.mesh>=sc.mesh_count)return r;
    r.mesh=de_robuffer<DeGpuMesh>(sc.meshes)[r.inst.mesh];
    if((r.mesh.flags&DE_GPU_MESH_READY)==0)return r;
    if((r.inst.flags&DE_GPU_INSTANCE_SPRITE)!=0) {
        DeRtMesh geometry=de_robuffer<DeRtMesh>(q.metadata)[r.inst.mesh];
        if(primitive>=geometry.index_first[3])return r;
        uint3 indices=de_bytes(geometry.index_first[2]).load3(primitive*12);
        DeBytes positions=de_bytes(geometry.index_first[1]);
        float3 a=asfloat(positions.load3(indices.x*12)),b=asfloat(positions.load3(indices.y*12)),c=asfloat(positions.load3(indices.z*12));
        float thickness=max(0.0,asfloat(r.inst.reserved0));
        a.z*=thickness;b.z*=thickness;c.z*=thickness;
        if(geometry.blas[1]==2u) {
            if((r.inst.reserved1&1u)!=0) {a.x=1-a.x;b.x=1-b.x;c.x=1-c.x;}
            if((r.inst.reserved1&2u)!=0) {a.y=1-a.y;b.y=1-b.y;c.y=1-c.y;}
        }
        r.object_pos[0]=a;r.object_pos[1]=b;r.object_pos[2]=c;r.vertex=uint3(0,1,2);
        r.bary=float3(1-bary.x-bary.y,bary.x,bary.y);r.bary_dx=0;r.bary_dy=0;
        r.triangle=primitive;r.entry=instance;r.valid=true;return r;
    }
    uint lod=min(de_bytes(q.lods).load(instance*4),min(r.mesh.lod_count,8u)-1);
    uint first=r.mesh.lods[lod].index_first+primitive*3;
    if(primitive>=r.mesh.lods[lod].index_count/3)return r;
    r.vertex=de_bytes(r.mesh.indices).load3(first*4);
    if(any(r.vertex>=r.mesh.vertex_count))return r;
    DeRtMesh rt=de_robuffer<DeRtMesh>(q.metadata)[r.inst.mesh];
    for(uint sub=0;sub<rt.submeshes;++sub) {
        uint2 range=de_bytes(rt.ranges).load2((lod*rt.submeshes+sub)*8);
        if(first>=range.x && first-range.x<range.y){r.meshlet.submesh=sub;break;}
    }
    r.object_pos[0]=de_vis_mesh_position(r.mesh,r.vertex.x);
    r.object_pos[1]=de_vis_mesh_position(r.mesh,r.vertex.y);
    r.object_pos[2]=de_vis_mesh_position(r.mesh,r.vertex.z);
    r.bary=float3(1-bary.x-bary.y,bary.x,bary.y);
    r.bary_dx=float3(0);r.bary_dy=float3(0);
    r.triangle=primitive;r.entry=instance;r.valid=true;return r;
}
public bool de_rt_opacity(DeRtContext q,uint instance,uint primitive,float2 bary) {
    DeResolvePoint r=de_rt_point(q,instance,primitive,bary);
    if(!r.valid)return false;
    if((r.inst.flags&DE_GPU_INSTANCE_VISIBLE_IN_RT)==0)return false;
    if((q.flags&DE_RT_EXCLUDE_SPRITES)!=0 && (r.inst.flags&DE_GPU_INSTANCE_SPRITE)!=0)return false;
    if(q.layers!=0 && (r.inst.layer_mask&q.layers)==0)return false;
    if((q.flags&DE_RT_CAST_SHADOWS)!=0 && (r.inst.flags&DE_GPU_INSTANCE_CAST_SHADOWS)==0)return false;
    DeSceneConstants sc=de_robuffer<DeSceneConstants>(q.scene)[q.view];
    uint material=de_resolve_material(r);
    if(material>=sc.material_count)return true;
    DeGpuMaterial m=de_robuffer<DeGpuMaterial>(sc.materials)[material];
    return de_rt_material_opacity(r,m,q.sampler);
}

public bool de_rt_material_opacity(DeResolvePoint r,DeGpuMaterial m,uint sampler) {
    if(m.model==DE_GPU_MATERIAL_MODEL_SPRITE) {float3 p=de_resolve_object_pos(r);return de_sprite_opacity(m,float2(p.x,1-p.y),sampler);}
    if(m.alpha_mode!=DE_GPU_MATERIAL_ALPHA_MASK)return true;
    float a=m.base_color.a;
    uint tex=de_material_texture(m,0);
    if(tex!=DE_BINDLESS_INVALID) {
        float2 dx,dy;
        float2 uv=de_resolve_uv(r,de_material_texcoord(m,0),dx,dy);
        a*=de_tex2d(tex).SampleLevel(de_sampler(sampler),uv,0).a;
    }
    return a>=m.pbr.w;
}
