// shaders/forward/forward_routing.slang: which materials the forward transparent pass draws (SPEC
// §12.5 step 6; DENG-305). One predicate with two readers: meshlet culling (culling/
// meshlet_cull.slang meshlet_bucket) keeps such meshlets out of the visibility buffer, and the
// forward pass's vertex shader (forward_transparent.slang) draws only such submeshes. The C mirror
// is de__forward_material (src/render/passes/forward/forward.h); both must give the same answer.
//
// A material is forward when it is READY and its CPU-derived feature word (DeGpuMaterial.features,
// DENG-302's de__material_features) has DE_MATERIAL_FEATURE_FORWARD: alpha blend, or transmission
// > 0.
module forward_routing;

import "../scene/scene_types";
import "../materials/material_interface"; // DE_MATERIAL_FEATURE_FORWARD

/// True when material record `m` draws in the forward transparent pass, not the vis buffer.
public bool de_material_forward(DeGpuMaterial m)
{
    return (m.flags & DE_GPU_MATERIAL_READY) != 0u &&
           (m.features & DE_MATERIAL_FEATURE_FORWARD) != 0u;
}
