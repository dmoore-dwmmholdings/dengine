// shaders/volumetrics/froxel_inject_light.slang: the light scattered in each froxel (DENG-207,
// S34). Engine shader `fog_inject_light`, entry cs_fog_light. Per froxel, at its jittered point:
// - the directional lights (the sun's colour carries the sky's transmittance, DENG-206) through
//   the cascaded shadow maps (S15 csm_shadow);
// - the froxel's cluster (S14 lists) of point and spot lights through the shadow atlas
//   (atlas_shadow); rect and disk lights are skipped;
// - the ambient: the mean radiance of the view's IBL (the sky's SH, DENG-838);
// each with the Henyey-Greenstein phase of the froxel's anisotropy. Output (pre-exposed):
// rgb = scattering x light, a = extinction; blended with last frame's reprojected value
// (weight FogHeader.ray00.w), rejected off-screen, outside the slices and where the medium
// changed (disocclusion).
import bindless;
import push_constants;
import "../lighting/lighting_common";
import "../shadows/csm_sample";
import "../shadows/atlas_sample";
import "froxel_common";

struct LightParams
{
    uint header, entry, media0, media1;
    uint history_cur;  // storage: this frame's result
    uint history_prev; // sampled: last frame's
    uint lview, lentry; // DeLightingView ring and entry
    uint lights, dir_lights, grid, list;
    uint counters, width, height;
    uint flags; // 1: direct lights, 2: ambient
};

[shader("compute")]
[numthreads(4, 4, 4)]
void cs_fog_light(uint3 id : SV_DispatchThreadID, uniform LightParams pc)
{
    FogHeader h = fog_header(pc.header, pc.entry);
    if (any(id >= h.grid.xyz))
        return;
    uint2 t = fog_texel(h, id);
    float4 m0 = de_tex2d(pc.media0).Load(int3(int2(t), 0));
    float g = de_tex2d(pc.media1).Load(int3(int2(t), 0)).x;
    float2 uv = (float2(id.xy) + 0.5) / float2(h.grid.xy);
    float z = fog_slice_depth(h, float(id.z) + h.depth.w);
    float3 org, ray;
    fog_ray(h, uv, org, ray);
    float3 rel = org + ray * z;
    float3 p = rel + h.camera.xyz; // render-origin space, as the lights
    float3 v = normalize(ray);     // from the camera through the froxel
    float3 light = float3(0.0, 0.0, 0.0);
    if (any(m0.rgb > 0.0) && (pc.flags & 1u) != 0u)
    {
        DeLightingView lv = de_robuffer<DeLightingView>(pc.lview)[pc.lentry];
        DeBytes counters = de_bytes(pc.counters);
        uint ndir = min(counters.load(DE_LIGHTING_COUNTER_DIRECTIONAL), DE_LIGHTING_MAX_DIRECTIONAL);
        for (uint i = 0; i < ndir; ++i)
        {
            DeClusterLight l = de_robuffer<DeClusterLight>(pc.dir_lights)[i];
            DeLightHit hit = de_light_punctual(l, p);
            float vis = (lv.inputs.reserved0 & DE_INPUT_SHADOW_METADATA) != 0u ?
                csm_shadow(lv.inputs.shadow_directional, p, hit.wi, asuint(l.bound.xy)) : 1.0;
            // DENG-208 marker: the clouds' shadow on the fog multiplies the sun here.
            float sun_visibility = de_input_cloud_shadow(lv.inputs, p, hit.wi);
            vis *= sun_visibility;
            light += hit.illuminance * vis * fog_phase_hg(g, dot(hit.wi, v));
        }
        uint2 px = min(uint2(uv * float2(pc.width, pc.height)), uint2(pc.width - 1u, pc.height - 1u));
        uint cluster = de_cluster_index(lv, px, z);
        if (cluster < lv.cluster_count)
        {
            uint2 cell = de_robuffer<uint2>(pc.grid)[cluster];
            for (uint k = 0; k < cell.y; ++k)
            {
                uint index = de_robuffer<uint>(pc.list)[cell.x + k];
                DeClusterLight l = de_robuffer<DeClusterLight>(pc.lights)[index];
                uint kind = de_light_kind(l);
                if (kind == DE_LIGHT_KIND_RECT || kind == DE_LIGHT_KIND_DISK)
                    continue;
                DeLightHit hit = de_light_punctual(l, p);
                if (!any(hit.illuminance > 0.0))
                    continue;
                float vis = (lv.inputs.reserved0 & DE_INPUT_ATLAS_METADATA) != 0u ?
                    atlas_shadow(lv.inputs.shadow_local, (asuint(l.axis.w) >> 8u) & 255u, p, hit.wi) :
                    1.0;
                light += hit.illuminance * vis * fog_phase_hg(g, dot(hit.wi, v));
            }
        }
        if ((pc.flags & 2u) != 0u)
        {
            // The mean radiance over the sphere: the SH's band 0 (the six axes cancel the rest).
            float3 e = de_ibl_irradiance(lv.ibl, float3(1, 0, 0)) + de_ibl_irradiance(lv.ibl, float3(-1, 0, 0)) +
                       de_ibl_irradiance(lv.ibl, float3(0, 1, 0)) + de_ibl_irradiance(lv.ibl, float3(0, -1, 0)) +
                       de_ibl_irradiance(lv.ibl, float3(0, 0, 1)) + de_ibl_irradiance(lv.ibl, float3(0, 0, -1));
            light += max(e, float3(0.0, 0.0, 0.0)) / (6.0 * FOG_PI);
        }
    }
    float4 cur = float4(min(m0.rgb * light * h.ray_du.w, float3(60000.0, 60000.0, 60000.0)), m0.a);
    float w = h.ray00.w;
    if (w > 0.0)
    {
        float4 c = float4(p, 1.0);
        float4 clip = float4(dot(h.prev_vp[0], c), dot(h.prev_vp[1], c), dot(h.prev_vp[2], c),
                             dot(h.prev_vp[3], c));
        bool ok = clip.w > 0.0;
        float2 puv = ok ? float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5) : float2(-1.0, -1.0);
        float ps = fog_slice_of(h, clip.w) - 0.5; // texel k sits at slice coordinate about k + 0.5
        ok = ok && all(puv >= 0.0) && all(puv <= 1.0) && ps >= -0.5 && ps <= float(h.grid.z) - 0.5;
        float4 hist = fog_sample(h, pc.history_prev, saturate(puv), ps);
        // Disocclusion: the medium there is not the one this froxel now holds.
        ok = ok && abs(hist.a - cur.a) <= 0.5 * max(max(hist.a, cur.a), 1e-6) &&
             all(hist == hist);
        if (ok)
            cur = lerp(cur, hist, w);
    }
    de_rwtex2d(pc.history_cur)[t] = cur;
}
