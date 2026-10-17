import push_constants;
import "../scene/scene_types";
import "../visbuffer/vis_decode";
import "../culling/lod_common";
import "../culling/cull_common";
struct RtMesh {
    uint blas[8]; uint index_first[8];
    uint ranges, submeshes, resident_mask, reserved;
};
struct Params { uint source, output, meshes, metadata, count, cull, lods, unused; };
[shader("compute")]
[numthreads(64,1,1)]
void cs_decode(uint3 id:SV_DispatchThreadID, uniform Params p) {
    DeGpuMesh mesh=de_robuffer<DeGpuMesh>(p.source)[0];
    if(id.x>=mesh.vertex_count) return;
    de_rwbytes(p.output).store3(id.x*12,asuint(de_vis_mesh_position(mesh,id.x)));
}
[shader("compute")]
[numthreads(64,1,1)]
void cs_instances(uint3 id:SV_DispatchThreadID, uniform Params p) {
    uint i=id.x;
    if(i==0)de_rwbytes(p.unused).store(0,p.count);
    if(i>=p.count) return;
    DeGpuInstance inst=de_robuffer<DeGpuInstance>(p.source)[i];
    DeVisCullView v=de_robuffer<DeVisCullView>(p.cull)[0];
    uint need=DE_GPU_INSTANCE_LIVE|DE_GPU_INSTANCE_VISIBLE_IN_RT;
    bool valid=(inst.flags&need)==need && inst.mesh!=DE_GPU_NONE &&
        (v.layer_mask==0 || (inst.layer_mask&v.layer_mask)!=0);
    uint handle=0,lod=0;
    if(valid) {
        DeGpuMesh mesh=de_robuffer<DeGpuMesh>(p.meshes)[inst.mesh];
        RtMesh rt=de_robuffer<RtMesh>(p.metadata)[inst.mesh];
        valid=(mesh.flags&DE_GPU_MESH_READY)!=0 && mesh.lod_count!=0;
        if(valid) {
            lod=select_lod(mesh,max_axis_scale(inst.transform),inst.sphere.xyz,inst.sphere.w,v);
            // Prefer the chosen LOD, then coarser resident LODs; never read unavailable geometry.
            for(uint l=lod;l<min(mesh.lod_count,8u);++l)
                if((rt.resident_mask&(1u<<l))!=0) { lod=l; handle=rt.blas[l]; break; }
        }
    }
    de_rwbytes(p.lods).store(i*4,lod);
    DeRWBytes dst=de_rwbytes(p.output); uint at=i*60;
    dst.store4(at,asuint(inst.transform[0]));
    dst.store4(at+16,asuint(inst.transform[1]));
    dst.store4(at+32,asuint(inst.transform[2]));
    // Hardware mask has eight bits; authored 32-bit layers remain in the GPU scene and are
    // tested again by shared candidate filtering (including cast_shadows for shadow effects).
    uint layers=(inst.layer_mask|(inst.layer_mask>>8)|(inst.layer_mask>>16)|(inst.layer_mask>>24))&255u;
    dst.store(at+48,(i&0xffffffu)|((valid && handle!=0 ? layers:0u)<<24));
    dst.store(at+52,0); dst.store(at+56,handle);
}
