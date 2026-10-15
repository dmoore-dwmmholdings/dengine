// shaders/visbuffer/gbuffer.slang: the thin G-buffer of the material resolve (SPEC §12.5 step 4;
// DENG-238). src/render/passes/vis/gbuffer.h has the same layout and arithmetic in C (the layout
// table is there); keep both in step. A contract for S14-S17: change it only by appending.
//
//   GB0      RGBA8_UNORM    rgb base colour sRGB-encoded, a AO
//   GB1      RGB10A2_UNORM  octahedral normal 11 + 11 bits (high 10 in r / g, low bits in a),
//                           b perceptual roughness
//   GB2      RGBA8_UNORM    r metallic, g model id / 255, b flags / 255 (DE_GB_FLAG_*), a specular
//   motion   RG16_FLOAT     NDC xy now minus NDC xy in the previous frame
//   lighting RGBA16_FLOAT   emissive radiance (rgb), a = 1 where a surface is
//
// Writers store through de_rwtex2d (storage views, float4); readers sample or load the targets and
// decode with the gb_* functions below.
module gbuffer;

import bindless;

/// GB2.b flags.
public static const uint DE_GB_FLAG_DOUBLE_SIDED = 1;
public static const uint DE_GB_FLAG_TRANSMISSION = 2;
public static const uint DE_GB_FLAG_SUBSURFACE = 4;
public static const uint DE_GB_FLAG_CLEARCOAT = 8;
public static const uint DE_GB_FLAG_UNLIT = 16;
/// Bits of each octahedral normal component.
public static const uint DE_GB_NORMAL_BITS = 11;

/// What resolve knows of one pixel's surface.
public struct DeGbSurface
{
    public float3 base_color; // linear
    public float ao;
    public float3 normal;     // world space, unit
    public float roughness;   // perceptual
    public float metallic;
    public float specular;
    public uint model;        // DeGpuMaterial.model
    public uint flags;        // DE_GB_FLAG_*
};

/// Linear to sRGB and back (IEC 61966-2-1).
public float3 gb_srgb_encode(float3 c)
{
    c = saturate(c);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(c, float3(1.0 / 2.4, 1.0 / 2.4, 1.0 / 2.4)) - 0.055;
    return float3(c.x <= 0.0031308 ? lo.x : hi.x, c.y <= 0.0031308 ? lo.y : hi.y,
                  c.z <= 0.0031308 ? lo.z : hi.z);
}
public float3 gb_srgb_decode(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, float3(2.4, 2.4, 2.4));
    return float3(c.x <= 0.04045 ? lo.x : hi.x, c.y <= 0.04045 ? lo.y : hi.y,
                  c.z <= 0.04045 ? lo.z : hi.z);
}

float gb_sign(float v)
{
    return v >= 0.0 ? 1.0 : -1.0;
}

public float2 gb_oct_encode(float3 n)
{
    float s = abs(n.x) + abs(n.y) + abs(n.z);
    float x = n.x / s, y = n.y / s;
    if (n.z < 0.0)
    {
        float tx = (1.0 - abs(y)) * gb_sign(x), ty = (1.0 - abs(x)) * gb_sign(y);
        x = tx;
        y = ty;
    }
    return float2(x, y);
}

public float3 gb_oct_decode(float2 e)
{
    float x = e.x, y = e.y;
    float z = 1.0 - abs(x) - abs(y);
    if (z < 0.0)
    {
        float tx = (1.0 - abs(y)) * gb_sign(x), ty = (1.0 - abs(x)) * gb_sign(y);
        x = tx;
        y = ty;
    }
    return normalize(float3(x, y, z));
}

/// The 11-bit codes of unit normal `n`: the best of the 4 roundings.
public uint2 gb_normal_encode(float3 n)
{
    float m = float((1u << DE_GB_NORMAL_BITS) - 1u);
    float2 f = (gb_oct_encode(n) * 0.5 + 0.5) * m;
    uint2 b = uint2(floor(f));
    uint2 q = b;
    float best = -2.0;
    for (uint k = 0; k < 4u; ++k)
    {
        uint2 c = b + uint2(k & 1u, k >> 1);
        if (float(c.x) > m || float(c.y) > m)
            continue;
        float d = dot(n, gb_oct_decode(float2(c) / m * 2.0 - 1.0));
        if (d > best)
        {
            best = d;
            q = c;
        }
    }
    return q;
}

public float3 gb_normal_decode(uint2 q)
{
    float m = float((1u << DE_GB_NORMAL_BITS) - 1u);
    return gb_oct_decode(float2(q) / m * 2.0 - 1.0);
}

/// The values of GB0, GB1 and GB2 for surface `s` (what the UNORM targets store).
public float4 gb0_encode(DeGbSurface s)
{
    return float4(gb_srgb_encode(s.base_color), saturate(s.ao));
}
public float4 gb1_encode(DeGbSurface s)
{
    uint2 q = gb_normal_encode(s.normal);
    float r = floor(saturate(s.roughness) * 1023.0 + 0.5) / 1023.0;
    float a = float((q.x & 1u) | ((q.y & 1u) << 1)) / 3.0;
    return float4(float(q.x >> 1) / 1023.0, float(q.y >> 1) / 1023.0, r, a);
}
public float4 gb2_encode(DeGbSurface s)
{
    return float4(saturate(s.metallic), float(s.model) / 255.0, float(s.flags & 255u) / 255.0,
                  saturate(s.specular));
}

/// The surface stored in GB0..GB2 (values as loaded from the UNORM targets).
public DeGbSurface gb_decode(float4 gb0, float4 gb1, float4 gb2)
{
    DeGbSurface s;
    s.base_color = gb_srgb_decode(gb0.rgb);
    s.ao = gb0.a;
    uint a = uint(gb1.a * 3.0 + 0.5);
    uint2 q = uint2((uint(gb1.r * 1023.0 + 0.5) << 1) | (a & 1u),
                    (uint(gb1.g * 1023.0 + 0.5) << 1) | (a >> 1));
    s.normal = gb_normal_decode(q);
    s.roughness = gb1.b;
    s.metallic = gb2.r;
    s.model = uint(gb2.g * 255.0 + 0.5);
    s.flags = uint(gb2.b * 255.0 + 0.5);
    s.specular = gb2.a;
    return s;
}
