module sprite_opacity;
import bindless;
import "../scene/scene_types";
import "../sprite/sprite_vis";
public bool de_sprite_opacity(DeGpuMaterial material,float2 plane_uv,uint sampler)
{
    float alpha=material.base_color.a;
    uint texture=material.textures[0].x&0x7fffffffu;
    if(texture!=0u) {
        uint width,height;de_tex2d(texture).GetDimensions(width,height);
        float2 half_texel=.5/float2(width,height);
        float2 a=material.subsurface.xy,b=a+material.subsurface.zw;
        float2 lo=min(a,b)+half_texel,hi=max(a,b)-half_texel;
        float2 uv=clamp(de_sprite_uv(material,plane_uv),min(lo,hi),max(lo,hi));
        alpha*=de_tex2d(texture).SampleLevel(de_sampler(sampler),uv,0).a;
    }
    return alpha>=material.pbr.w;
}
