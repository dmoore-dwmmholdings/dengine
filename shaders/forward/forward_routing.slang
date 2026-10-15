// shaders/forward/forward_routing.slang: which materials the forward transparent pass draws (SPEC
// §12.5 step 6; DENG-305). One predicate with two readers: meshlet culling (culling/
// meshlet_cull.slang meshlet_bucket) keeps such meshlets out of the visibility buffer, and the
// forward pass's vertex shader (forward_transparent.slang) draws only such submeshes. The C mirror
// is de__forward_material (src/render/passes/forward/forward.h); both must give the same answer.
//
// A material is forward when it is READY and blends (alpha_mode blend) or transmits (KHR
// transmission > 0, DeGpuMaterial.clearcoat.w): DE_MATERIAL_FEATURE_FORWARD's definition. When the
// material record carries a CPU-derived feature word (DENG-302), the body becomes a test of that
// bit, here and in the C mirror, and nothing else changes.
module forward_routing;

import "../scene/scene_types";

/// DeGpuMaterial.alpha_mode of a blended material (de_cooked_alpha_mode blend).
public static const uint DE_FWD_ALPHA_MODE_BLEND = 2;

/// True when material record `m` draws in the forward transparent pass, not the vis buffer.
public bool de_material_forward(DeGpuMaterial m)
{
    return (m.flags & DE_GPU_MATERIAL_READY) != 0u &&
           (m.alpha_mode == DE_FWD_ALPHA_MODE_BLEND || m.clearcoat.w > 0.0);
}
