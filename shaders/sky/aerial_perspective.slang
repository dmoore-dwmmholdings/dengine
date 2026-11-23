// shaders/sky/aerial_perspective.slang: the aerial perspective of Hillaire 2020 §5.5 (SPEC §12.13,
// §12.7; DENG-840). Engine shader `sky_aerial`, two entries:
// - cs_aerial builds the view's froxel volume: SKY_AERIAL_N x SKY_AERIAL_N screen cells x
//   SKY_AERIAL_N depth slices (sky_aerial_distance), as a 2D atlas (slice s at x in
//   [s N, s N + N)). Each texel holds the light scattered toward the camera between the camera and
//   the end of its slice (rgb, pre-exposed) and the mean transmittance over that distance (a).
//   The medium, the transmittance LUT and the multi-scattering LUT are the sky's (sky_common).
// - cs_aerial_apply applies the volume to the opaque surfaces in the HDR lighting target (pixels
//   whose reverse-Z depth is not the far plane): L' = L T + S.
// The forward transparency pass samples the same atlas (sky_aerial_perspective).
import bindless;
import push_constants;
import "sky_common";

struct AerialParams
{
    uint4 idx;     // target (storage), transmittance, multiscatter, sampler (linear clamp)
    float4 ray00;  // the camera ray at the screen's top-left corner (world, unnormalized); w: radius km
    float4 ray_du; // its change across the width; w: range (km)
    float4 ray_dv; // its change down the height; w: mie scale
    float4 sun;    // direction (world, +Z up), weight (illuminance x pre-exposure)
    float4 moon;   // direction, weight
};

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_aerial(uint3 id : SV_DispatchThreadID, uniform AerialParams pc)
{
    uint N = SKY_AERIAL_N;
    if (id.x >= N || id.y >= N)
        return;
    float2 uv = (float2(id.xy) + 0.5) / float(N);
    float3 dir = normalize(pc.ray00.xyz + pc.ray_du.xyz * uv.x + pc.ray_dv.xyz * uv.y);
    float r = pc.ray00.w;
    float range = pc.ray_du.w;
    SkyLight sun, moon;
    sun.dir = pc.sun.xyz;
    sun.weight = pc.sun.w;
    moon.dir = pc.moon.xyz;
    moon.weight = pc.moon.w;
    float c0 = dot(dir, sun.dir), c1 = dot(dir, moon.dir);
    float3 L = float3(0.0, 0.0, 0.0);
    float3 thr = float3(1.0, 1.0, 1.0);
    float t_prev = 0.0;
    RWTexture2D<float4> target = de_rwtex2d(pc.idx.x);
    for (uint i = 0; i < N; ++i)
    {
        float t = sky_aerial_distance((float(i) + 1.0) / float(N), range);
        float dt = t - t_prev;
        float tm = 0.5 * (t + t_prev);
        t_prev = t;
        float3 p = float3(0.0, 0.0, r) + dir * tm;
        float rp = length(p);
        // Below the ground (a ray that looks down): sample just above it, as Hillaire does. The
        // geometry hides those texels in all but the frames before it streams in.
        rp = clamp(rp, SKY_BOTTOM + 0.001, SKY_TOP);
        p = normalize(p) * rp;
        SkyMedium m = sky_medium(rp - SKY_BOTTOM, pc.ray_dv.w);
        float3 S = sky_scattering(pc.idx.y, pc.idx.z, pc.idx.w, p, m, sun, moon, c0, c1);
        float3 ext = max(m.extinction, float3(1e-7, 1e-7, 1e-7));
        float3 st = exp(-ext * dt);
        L += thr * (S - S * st) / ext;
        thr *= st;
        float a = (thr.r + thr.g + thr.b) / 3.0;
        target[uint2(i * N + id.x, id.y)] = float4(min(L, float3(60000.0, 60000.0, 60000.0)), a);
    }
}

struct AerialApplyParams
{
    uint4 idx;   // atlas, sampler (linear clamp), depth, target (storage)
    uint4 size;  // width, height, orthographic (0/1), 0
    float4 ray;  // the view-space ray at z = -1 at the top-left corner (x, y), its change (dx, dy)
    float4 proj; // projection m[10], m[14], m[11], m[15] (column-major): linear depth
    float4 misc; // range (km), km per world unit, 0, 0
};

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_aerial_apply(uint3 id : SV_DispatchThreadID, uniform AerialApplyParams pc)
{
    if (id.x >= pc.size.x || id.y >= pc.size.y)
        return;
    int2 p = int2(id.xy);
    float d = de_tex2d(pc.idx.z).Load(int3(p, 0)).r;
    if (d <= 0.0)
        return; // the far plane: the sky pass draws it
    float zv = (d * pc.proj.w - pc.proj.y) / (pc.proj.x - d * pc.proj.z); // view z (negative)
    float depth = max(-zv, 0.0);
    float2 uv = (float2(id.xy) + 0.5) / float2(pc.size.xy);
    float dist = depth;
    if (pc.size.z == 0)
        dist *= length(float3(pc.ray.xy + pc.ray.zw * uv, -1.0));
    float4 ap = sky_aerial_perspective(pc.idx.x, pc.idx.y, uv, dist * pc.misc.y, pc.misc.x);
    RWTexture2D<float4> target = de_rwtex2d(pc.idx.w);
    float4 c = target[uint2(p)];
    target[uint2(p)] = float4(sky_apply_aerial(c.rgb, ap), c.a);
}
