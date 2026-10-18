module sprite_vis;
import "../scene/scene_types";
public float2 de_sprite_uv(DeGpuMaterial m, float2 uv)
{
    return m.model == DE_GPU_MATERIAL_MODEL_SPRITE ? m.subsurface.xy + uv * m.subsurface.zw : uv;
}
