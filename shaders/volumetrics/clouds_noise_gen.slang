// shaders/volumetrics/clouds_noise_gen.slang: the clouds' tiling noises and weather map
// (DENG-208, S34). Engine shader `clouds_noise`, entry cs_clouds_noise, run once per renderer
// (src/world/sky/clouds.c). One texel per thread of the RGBA8 target:
//   mode 0  base noise atlas (128^3 in 130-texel tiles, 16 columns): r Perlin-Worley, gba Worley
//           at 4, 8 and 16 cells per tile
//   mode 1  detail noise atlas (32^3 in 34-texel tiles, 8 columns): rgb Worley at 2, 4, 8 cells
//   mode 2  weather map (512^2): r coverage, g cloud type, b precipitation
// Every noise is periodic over its tile, so the textures wrap without seams. The atlas gutters
// hold the wrapped neighbour texels (bilinear filtering inside a tile).
import bindless;
import push_constants;
import "clouds_common";

struct NoiseParams
{
    uint target, mode, width, height;
};

uint clouds_hash(uint3 v)
{
    uint h = v.x * 0x8da6b343u ^ v.y * 0xd8163841u ^ v.z * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

float3 clouds_hash3(uint3 v)
{
    uint a = clouds_hash(v);
    uint b = clouds_hash(v + uint3(17u, 59u, 113u));
    uint c = clouds_hash(v + uint3(211u, 7u, 31u));
    return float3(float(a & 0xFFFFFFu), float(b & 0xFFFFFFu), float(c & 0xFFFFFFu)) / 16777216.0;
}

uint3 wrap_cell(int3 c, int period)
{
    return uint3((c % period + period) % period);
}

/// Worley (cellular) noise of `period` cells per unit, periodic over 1: 1 at a feature point,
/// 0 a cell away.
float worley(float3 p, int period)
{
    float3 q = p * float(period);
    int3 cell = int3(floor(q));
    float3 f = q - float3(cell);
    float d = 1e9;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                int3 o = int3(x, y, z);
                float3 fp = float3(o) + clouds_hash3(wrap_cell(cell + o, period));
                float3 v = fp - f;
                d = min(d, dot(v, v));
            }
    return 1.0 - saturate(sqrt(d));
}

float3 fade(float3 t)
{
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

float3 gradient(uint3 c)
{
    return normalize(clouds_hash3(c) * 2.0 - 1.0 + 1e-4);
}

/// Perlin gradient noise of `period` cells per unit, periodic over 1, in about -1..1.
float perlin(float3 p, int period)
{
    float3 q = p * float(period);
    int3 i = int3(floor(q));
    float3 f = q - float3(i);
    float3 u = fade(f);
    float n[8];
    for (int k = 0; k < 8; ++k)
    {
        int3 o = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        n[k] = dot(gradient(wrap_cell(i + o, period)), f - float3(o));
    }
    float x0 = lerp(lerp(n[0], n[1], u.x), lerp(n[2], n[3], u.x), u.y);
    float x1 = lerp(lerp(n[4], n[5], u.x), lerp(n[6], n[7], u.x), u.y);
    return lerp(x0, x1, u.z) * 1.6;
}

float perlin_fbm(float3 p, int period, int octaves)
{
    float s = 0.0, a = 0.5, norm = 0.0;
    for (int o = 0; o < octaves; ++o)
    {
        s += perlin(p, period << o) * a;
        norm += a;
        a *= 0.5;
    }
    return s / norm;
}

float worley_fbm(float3 p, int period)
{
    return worley(p, period) * 0.625 + worley(p, period * 2) * 0.25 + worley(p, period * 4) * 0.125;
}

/// The position (0..1 periodic) of atlas texel `t`, or false in the unused part of the atlas.
bool atlas_position(uint2 t, uint n, uint tile, uint columns, out float3 p)
{
    uint tx = t.x / tile, ty = t.y / tile;
    uint z = ty * columns + tx;
    p = float3(0.0, 0.0, 0.0);
    if (z >= n)
        return false;
    uint lx = t.x % tile, ly = t.y % tile;
    uint ix = (lx + n - 1u) % n, iy = (ly + n - 1u) % n; // the gutter wraps
    p = (float3(float(ix), float(iy), float(z)) + 0.5) / float(n);
    return true;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_clouds_noise(uint3 id : SV_DispatchThreadID, uniform NoiseParams pc)
{
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    float4 o = float4(0.0, 0.0, 0.0, 0.0);
    float3 p;
    if (pc.mode == 0u)
    {
        if (!atlas_position(id.xy, CLOUDS_BASE_N, CLOUDS_BASE_TILE, CLOUDS_BASE_COLUMNS, p))
            return;
        float w4 = worley_fbm(p, 4);
        float pn = perlin_fbm(p, 4, 3) * 0.5 + 0.5;
        // Perlin-Worley: the Perlin billows with the Worley cells' rounded edges.
        float pw = saturate(clouds_remap(pn, w4 - 1.0, 1.0, 0.0, 1.0));
        o = float4(pw, w4, worley_fbm(p, 8), worley_fbm(p, 16));
    }
    else if (pc.mode == 1u)
    {
        if (!atlas_position(id.xy, CLOUDS_DETAIL_N, CLOUDS_DETAIL_TILE, CLOUDS_DETAIL_COLUMNS, p))
            return;
        o = float4(worley_fbm(p, 2), worley_fbm(p, 4), worley_fbm(p, 8), 1.0);
    }
    else
    {
        p = float3((float2(id.xy) + 0.5) / float2(pc.width, pc.height), 0.5);
        float cov = perlin_fbm(p, 6, 4) * 0.5 + 0.5;
        cov = saturate(clouds_remap(cov, 0.25, 0.75, 0.0, 1.0));
        float type = saturate(perlin_fbm(p + 0.37, 3, 2) * 0.9 + 0.55);
        float rain = saturate(clouds_remap(cov, 0.75, 1.0, 0.0, 1.0)) *
                     saturate(perlin_fbm(p + 0.71, 4, 2) + 0.5);
        o = float4(cov, type, rain, 1.0);
    }
    de_rwtex2d(pc.target)[id.xy] = o;
}
