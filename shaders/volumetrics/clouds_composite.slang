// shaders/volumetrics/clouds_composite.slang: the clouds into the HDR target (DENG-208, S34).
// Engine shader `clouds_composite`, entry cs_clouds_composite, pass clouds.composite, after
// sky.render. The march stopped at the opaque depth, so the clouds are in front of what the pixel
// holds (an opaque surface or the sky, whose sun disk the transmittance occludes):
//   L' = L T_c + S_c T_ap + S_ap (1 - T_c)
// with the sky's aerial perspective (T_ap, S_ap) at the cloud distance. On opaque pixels with
// flags & 1 clear (the froxel fog applies the aerial perspective later): L' = L T_c + S_c.
import bindless;
import push_constants;
import "../sky/sky_common";
import "clouds_common";

struct CompositeParams
{
    uint header, entry, clouds, depth;
    uint target, aerial, aerial_sampler, aerial_range_bits;
    uint width, height, mw, mh;
    uint dist, flags, pad0, pad1; // flags: 1 the aerial perspective on opaque pixels
};

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_clouds_composite(uint3 id : SV_DispatchThreadID, uniform CompositeParams pc)
{
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    int2 p = int2(id.xy);
    float4 c = de_tex2d(pc.clouds).Load(int3(p, 0));
    if (c.a >= 0.9999)
        return; // no cloud in front
    RWTexture2D<float4> target = de_rwtex2d(pc.target);
    float4 L = target[uint2(p)];
    float d = de_tex2d(pc.depth).Load(int3(p, 0)).r;
    float3 o = L.rgb * c.a + c.rgb;
    if (pc.aerial != DE_BINDLESS_INVALID && (d <= 0.0 || (pc.flags & 1u) != 0u))
    {
        int2 b = min(p / 2, int2(int(pc.mw) - 1, int(pc.mh) - 1));
        float dist = de_tex2d(pc.dist).Load(int3(b, 0)).r;
        float2 uv = (float2(p) + 0.5) / float2(pc.width, pc.height);
        float4 ap = sky_aerial_perspective(pc.aerial, pc.aerial_sampler, uv, dist * 0.001,
                                           asfloat(pc.aerial_range_bits));
        o = L.rgb * c.a + c.rgb * ap.a + ap.rgb * (1.0 - c.a);
    }
    target[uint2(p)] = float4(min(o, float3(CLOUDS_MAX_VALUE, CLOUDS_MAX_VALUE, CLOUDS_MAX_VALUE)), L.a);
}
