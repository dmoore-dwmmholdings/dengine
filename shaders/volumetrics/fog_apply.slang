// shaders/volumetrics/fog_apply.slang: the froxel fog on the opaque surfaces and the sky
// (DENG-207, S34). Engine shader `fog_apply`, entry cs_fog_apply, after the sky and before
// transparency. With the sky's aerial perspective (DENG-840) behind the fog:
//   L' = S_fog + T_fog (S_ap + T_ap L)
// The fog stops at its far distance and the aerial perspective covers the whole path, so there is
// no seam where the slices end. Sky pixels (reverse-Z depth 0) take the fog to its far distance.
import bindless;
import push_constants;
import "../sky/sky_common";
import "froxel_common";

struct ApplyParams
{
    uint header, entry, integrated, depth;
    uint target, aerial, aerial_sampler, aerial_range_bits;
    uint width, height, ortho, pad;
    float4 ray;  // the view-space ray at z = -1: top-left (x, y), its change (dx, dy)
    float4 proj; // projection m[10], m[14], m[11], m[15] (column-major): linear depth
};

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_fog_apply(uint3 id : SV_DispatchThreadID, uniform ApplyParams pc)
{
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    int2 p = int2(id.xy);
    float2 uv = (float2(id.xy) + 0.5) / float2(pc.width, pc.height);
    float d = de_tex2d(pc.depth).Load(int3(p, 0)).r;
    RWTexture2D<float4> target = de_rwtex2d(pc.target);
    float4 c = target[uint2(p)];
    FogHeader h = fog_header(pc.header, pc.entry);
    float depth = h.depth.y * 2.0; // the sky: the whole fog
    if (d > 0.0)
    {
        float zv = (d * pc.proj.w - pc.proj.y) / (pc.proj.x - d * pc.proj.z);
        depth = max(-zv, 0.0);
        if (pc.aerial != DE_BINDLESS_INVALID)
        {
            float dist = depth;
            if (pc.ortho == 0u)
                dist *= length(float3(pc.ray.xy + pc.ray.zw * uv, -1.0));
            float4 ap = sky_aerial_perspective(pc.aerial, pc.aerial_sampler, uv, dist * 0.001,
                                               asfloat(pc.aerial_range_bits));
            c.rgb = sky_apply_aerial(c.rgb, ap);
        }
    }
    float4 f = froxel_apply(pc.header, pc.entry, uv, depth, c.rgb, 1.0);
    target[uint2(p)] = float4(f.rgb, c.a);
}
