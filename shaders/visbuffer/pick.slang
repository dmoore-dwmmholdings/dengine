// shaders/visbuffer/pick.slang: vis-buffer picking (SPEC §20.1; DENG-239). Engine shader
// "vis_pick" (shaders/engine_shaders.cmake); src/render/passes/vis/picking.c runs it once per view
// and frame that has pick requests, after the frame's last raster pass and before classify, so
// every vis ID it reads names an entry of this frame's visible-meshlet list.
//
//   cs_pick   one 16-thread group, one thread per request: reads the vis ID at the request's
//             internal pixel (from a raw copy of the vis buffer: the R32_UINT target has neither a
//             sampled nor a storage view), decodes it (vis_types.slang) to the visible-meshlet
//             entry, the instance and the triangle, and writes a DePickResult:
//               entity_lo, entity_hi  DeGpuInstance.entity_* (the render world's entity bits)
//               triangle              (meshlet << 7) | triangle in the meshlet; the meshlet is the
//                                     index into the mesh's meshlets (DeVisMeshlet.meshlet)
//               hit                   1 when the pixel holds geometry, else 0 (background, or a
//                                     pixel outside the target): entity 0, triangle
//                                     DE_VIS_PICK_TRIANGLE_NONE
import push_constants;
import "../scene/scene_types";
import "../visbuffer/vis_types";

/// Requests per dispatch (and per view and frame).
public static const uint DE_VIS_PICK_MAX = 16;
/// Bytes of one DePickResult.
public static const uint DE_VIS_PICK_RESULT_BYTES = 16;
/// DePickResult.triangle without a hit.
public static const uint DE_VIS_PICK_TRIANGLE_NONE = 0xFFFFFFFF;

/// Push constants (112 bytes; bindless indices).
struct PickParams
{
    uint ids;            // raw u32 per pixel, rows `pitch` bytes apart (DE_ACCESS_SHADER_READ)
    uint pitch;          // bytes per row of `ids`
    uint width;          // the vis buffer's size
    uint height;
    uint visible;        // DeVisMeshlet list (DE_ACCESS_SHADER_READ)
    uint visible_count;  // its entries (both buckets' regions)
    uint instances;      // DeGpuInstance table (DE_ACCESS_SHADER_READ)
    uint instance_count; // its slots
    uint results;        // raw DePickResult[count] (DE_ACCESS_SHADER_WRITE)
    uint count;          // requests (<= DE_VIS_PICK_MAX)
    uint reserved0;
    uint reserved1;
    uint4 coords[4];     // request k: coords[k / 4][k % 4] = x | (y << 16), internal pixels
};

[shader("compute")]
[numthreads(16, 1, 1)]
void cs_pick(uint3 thread : SV_GroupThreadID, uniform PickParams pc)
{
    uint k = thread.x;
    if (k >= pc.count)
        return;
    uint packed = pc.coords[k >> 2u][k & 3u];
    uint x = packed & 0xFFFFu;
    uint y = packed >> 16u;
    uint4 r = uint4(0u, 0u, DE_VIS_PICK_TRIANGLE_NONE, 0u);
    if (x < pc.width && y < pc.height)
    {
        uint id = de_bytes(pc.ids).load(y * pc.pitch + x * 4u);
        uint entry = de_vis_id_meshlet(id);
        if (id != DE_VIS_ID_EMPTY && entry < pc.visible_count)
        {
            DeVisMeshlet vm = de_robuffer<DeVisMeshlet>(pc.visible)[entry];
            if (vm.instance < pc.instance_count)
            {
                DeGpuInstance inst = de_robuffer<DeGpuInstance>(pc.instances)[vm.instance];
                uint triangle = (vm.meshlet << DE_VIS_ID_TRIANGLE_BITS) | de_vis_id_triangle(id);
                r = uint4(inst.entity_lo, inst.entity_hi, triangle, 1u);
            }
        }
    }
    de_rwbytes(pc.results).store4(k * DE_VIS_PICK_RESULT_BYTES, r);
}
