module sprite_vis;
import "../scene/scene_types";
/// True for a DeSprite's block, lit (the Sprite model) or unlit (the Unlit model with
/// DE_GPU_MATERIAL_SPRITE; DENG-763). Its subsurface row holds the atlas uv offset and scale.
public bool de_sprite_is(DeGpuMaterial m)
{
    return m.model == DE_GPU_MATERIAL_MODEL_SPRITE || (m.flags & DE_GPU_MATERIAL_SPRITE) != 0u;
}
public float2 de_sprite_uv(DeGpuMaterial m, float2 uv)
{
    return de_sprite_is(m) ? m.subsurface.xy + uv * m.subsurface.zw : uv;
}
