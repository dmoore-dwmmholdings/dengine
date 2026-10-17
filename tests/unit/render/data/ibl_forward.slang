// tests/unit/render/data/ibl_forward.slang: a "forward" use of the shared indirect lighting
// (DENG-304, test_ibl_exposure.c ibl_deferred_forward_equal). Cooked at test time on one root
// with the engine's shaders/. Per pixel it rebuilds the surface from the same G-buffer texels the
// deferred pass reads and calls lighting_common.slang's evaluate_indirect, as the forward pass
// (DENG-305) will, times the view's pre-exposure. The test compares it with the deferred pass's
// lighting target of a scene without lights.
import bindless;
import push_constants;
import "lighting/lighting_common";
import "materials/standard/standard";
import "visbuffer/resolve_common";
import "visbuffer/vis_decode";
import "visbuffer/vis_types";
import "visbuffer/gbuffer";
import "scene/scene_types";

struct ForwardParams
{
    uint view;     // DeLightingView ring
    uint entry;
    uint scene;    // DeSceneConstants table
    uint view_slot;
    uint depth;    // sampled
    uint gb0;
    uint gb1;
    uint gb2;
    uint out_tex;  // storage RGBA32F
    uint width;
    uint height;
    uint reserved;
    uint ids;
    uint ids_pitch;
    uint visible;
    uint visible_count;
};

bool pixel_material(uint2 p, DeSceneConstants sc, ForwardParams pc, out uint id,
                    out DeGpuMaterial m)
{
    m = (DeGpuMaterial)0;
    id = DE_VIS_ID_EMPTY;
    if (pc.ids == DE_BINDLESS_INVALID || pc.visible == DE_BINDLESS_INVALID ||
        sc.materials == DE_BINDLESS_INVALID)
        return false;
    id = de_bytes(pc.ids).load(p.y * pc.ids_pitch + p.x * 4u);
    if (id == DE_VIS_ID_EMPTY)
        return false;
    uint entry = de_vis_id_meshlet(id);
    if (entry >= pc.visible_count)
        return false;
    DeVisMeshlet vm = de_robuffer<DeVisMeshlet>(pc.visible)[entry];
    DeGpuInstance inst = de_robuffer<DeGpuInstance>(sc.instances)[vm.instance];
    if (inst.mesh == DE_GPU_NONE || inst.mesh >= sc.mesh_count)
        return false;
    DeGpuMesh mesh = de_robuffer<DeGpuMesh>(sc.meshes)[inst.mesh];
    if (mesh.meshlets == DE_BINDLESS_INVALID || vm.meshlet >= mesh.meshlet_count)
        return false;
    uint slot = de_vis_instance_material(inst, de_vis_meshlet_info(mesh, vm.meshlet).submesh);
    if (slot == DE_GPU_NONE || slot >= sc.material_count)
        return false;
    m = de_robuffer<DeGpuMaterial>(sc.materials)[slot];
    return (m.flags & DE_GPU_MATERIAL_READY) != 0u;
}


[shader("compute")]
[numthreads(8, 8, 1)]
void cs_ibl_forward(uint3 id : SV_DispatchThreadID, uniform ForwardParams pc)
{
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    int3 pl = int3(int(id.x), int(id.y), 0);
    RWTexture2D<float4> out_tex = de_rwtex2d(pc.out_tex);
    float z = de_tex2d(pc.depth).Load(pl).r;
    if (!(z > 0.0))
    {
        out_tex[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    DeLightingView lv = de_robuffer<DeLightingView>(pc.view)[pc.entry];
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    DeGbSurface g = gb_decode(de_tex2d(pc.gb0).Load(pl), de_tex2d(pc.gb1).Load(pl),
                              de_tex2d(pc.gb2).Load(pl));
    float2 ndc = float2((float(id.x) + 0.5) / float(pc.width) * 2.0 - 1.0,
                        1.0 - (float(id.y) + 0.5) / float(pc.height) * 2.0);
    float4 c = float4(ndc, z, 1.0);
    float4 w = float4(dot(sc.inv_view_proj[0], c), dot(sc.inv_view_proj[1], c),
                      dot(sc.inv_view_proj[2], c), dot(sc.inv_view_proj[3], c));
    float3 pos = w.xyz / w.w;
    float3 v = normalize(sc.camera_position.xyz - pos);
    SurfaceData s = de_surface_from_gb(g);
    s.brdf_lut = lv.brdf_lut;
    uint visId;
    DeGpuMaterial material;
    if (pixel_material(id.xy, sc, pc, visId, material))
        StandardModel.layers_from_material(s, material);
    float3 indirect = evaluate_indirect(s, pos, v, id.xy, lv);
    out_tex[id.xy] = float4(indirect * lv.pre_exposure, 1.0);
}
