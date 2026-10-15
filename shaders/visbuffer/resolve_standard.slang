// shaders/visbuffer/resolve_standard.slang: the material resolve of the visibility buffer (SPEC
// §12.5 step 4; DENG-238, DENG-298). Engine shader `vis_resolve`; src/render/passes/vis/resolve.c
// runs it once per view and frame, after classification (DENG-237):
//
//   cs_resolve_clear     8 x 8 threads per group over the view: the G-buffer targets (and the
//                        test-only debug targets) to 0
//   cs_feedback_clear    64 threads per group: the feedback buffer to DE_FEEDBACK_NONE, the
//                        processed-tile counters to 0
//   cs_resolve_standard  de_resolve_tiles<ResolveStandardModel> (resolve_model.slang): one
//                        64-thread group per tile of a model's tile list, for every model with no
//                        kernel of its own (Standard, and the built-in ids not implemented yet)
//   cs_resolve_unlit     de_resolve_tiles<ResolveUnlitModel>, for the Unlit model
//
// A game's material model (de_material_model_register) brings its own cs_resolve_model, the same
// generic kernel specialised for its type. The kernel, the ResolveParams push constants and the
// S13 models are in resolve_model.slang; the material interface in
// materials/material_interface.slang.
import bindless;
import push_constants;
import "../scene/scene_types";
import "../visbuffer/resolve_model";
import "../visbuffer/feedback";

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_resolve_clear(uint3 id : SV_DispatchThreadID, uniform ResolveParams pc)
{
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    float4 z = float4(0.0, 0.0, 0.0, 0.0);
    de_rwtex2d(pc.gb0)[id.xy] = z;
    de_rwtex2d(pc.gb1)[id.xy] = z;
    de_rwtex2d(pc.gb2)[id.xy] = z;
    de_rwtex2d(pc.motion)[id.xy] = z;
    de_rwtex2d(pc.lighting)[id.xy] = z;
    if ((pc.flags & DE_RESOLVE_DEBUG) != 0u)
    {
        de_rwtex2d(pc.debug_pos)[id.xy] = z;
        de_rwtex2d(pc.debug_uv)[id.xy] = z;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_feedback_clear(uint3 id : SV_DispatchThreadID, uniform ResolveParams pc)
{
    if (id.x < pc.feedback_capacity)
        de_rwbytes(pc.feedback).store(id.x * 4u, DE_FEEDBACK_NONE);
    if (id.x < DE_GPU_MATERIAL_MODELS)
        de_rwbytes(pc.counters).store(id.x * 4u, 0u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_resolve_standard(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
                         uniform ResolveParams pc)
{
    de_resolve_tiles<ResolveStandardModel>(group, thread.x, pc);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_resolve_unlit(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
                      uniform ResolveParams pc)
{
    de_resolve_tiles<ResolveUnlitModel>(group, thread.x, pc);
}
