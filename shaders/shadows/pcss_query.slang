import bindless;
import push_constants;
import shadow_filter;
import csm_sample;
import atlas_sample;
struct PcssQuery { float4 uvz_radius; uint4 control; };
struct PcssQueryParams { uint maps, queries, output, count; };
[shader("compute")]
[numthreads(64, 1, 1)]
void cs_pcss_query(uint3 id : SV_DispatchThreadID, uniform PcssQueryParams pc)
{
    if (id.x >= pc.count) return;
    PcssQuery q = de_robuffer<PcssQuery>(pc.queries)[id.x];
    ShadowMapRef m = q.control.y == 1u ? de_robuffer<CsmCascade>(pc.maps)[q.control.x].map :
                     q.control.y == 2u ? de_robuffer<AtlasShadow>(pc.maps)[q.control.x].map :
                     de_robuffer<ShadowMapRef>(pc.maps)[q.control.x];
    if (q.control.y != 0u)
    {
        de_buffer<float4>(pc.output)[id.x * 2u] = float4(asfloat(m.texture), m.depth_projection.w,
                                                        m.uv_projection.z, asfloat(m.filter_state.z));
        de_buffer<float4>(pc.output)[id.x * 2u + 1u] = m.uv_clamp;
        return;
    }
    float d = de_shadow_linear_depth(m, q.uvz_radius.z);
    de_buffer<float4>(pc.output)[id.x * 2u] =
        float4(shadow_filter(m, q.uvz_radius.xyz, q.uvz_radius.w),
               shadow_pcf(m, q.uvz_radius.xyz, q.uvz_radius.w), d, 1.0);
    de_buffer<float4>(pc.output)[id.x * 2u + 1u] =
        float4(de_shadow_search_radius(m, d), de_shadow_penumbra_radius(m, d, 2.0));
}
