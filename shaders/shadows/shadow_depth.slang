// DENG-335: depth-only alpha variants reuse the visibility raster and its material test.
#include "../visbuffer/vis_alpha.slang"
[shader("fragment")]
void ps_shadow_alpha_mesh(AlphaVertex v, AlphaPixel p, uniform AlphaParams pc)
{
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    if (!alpha_test(sc, p.material, v.uv, pc.sampler)) discard;
}
[shader("fragment")]
void ps_shadow_alpha_draw(AlphaDrawVertex v, uniform AlphaParams pc)
{
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    if (!alpha_test(sc, v.material, v.uv, pc.sampler)) discard;
}
