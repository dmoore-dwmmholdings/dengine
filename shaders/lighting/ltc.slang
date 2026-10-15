// shaders/lighting/ltc.slang: rect and disk area lights with linearly transformed cosines (Heitz,
// Dupuy, Hill and Neubelt, SIGGRAPH 2016; DENG-303).
//
//   diffuse   the polygon's cosine-weighted solid angle (the identity LTC: exact Lambert)
//   specular  the polygon through M^-1 of the GGX lobe of (roughness, theta_v), from the tables of
//             src/render/lighting/ltc_data.c (our own fit, tools/ltc_fit/ltc_fit.c), times the
//             Schlick reflectance F0 x + F90 y of the same table entry
//
// A disk is a regular 12-gon of the same area. The polygon is clipped to the shading point's
// upper hemisphere (Sutherland-Hodgman) and integrated edge by edge (Heitz 2017's cubic fit of
// theta / sin(theta)). An area light emits only from its front side (towards its -Z).
module ltc;

import bindless;
import "../lighting/lights";

public static const uint DE_LTC_SIZE = 64;
/// Vertices of the disk polygon.
public static const uint DE_LTC_DISK_SIDES = 12;
static const uint LTC_MAX = 16;

/// The z part of the edge integral of (v1, v2) (unit vectors): (v1 x v2).z theta / sin(theta) /
/// (2 pi) (Heitz 2017's rational fit includes the 1 / (2 pi)).
public float de_ltc_edge(float3 v1, float3 v2)
{
    float x = dot(v1, v2);
    float y = abs(x);
    float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    float b = 3.4175940 + (4.1616724 + y) * y;
    float v = a / b;
    float ts = x > 0.0 ? v : 0.5 * rsqrt(max(1.0 - x * x, 1e-7)) - v;
    return cross(v1, v2).z * ts;
}

/// The cosine-weighted solid angle / pi of polygon `p[0..n)` (points relative to the shading
/// point, already in the cosine's frame: z up), clipped to z >= 0. 1 = the whole hemisphere.
public float de_ltc_polygon(float3 p[LTC_MAX], uint n)
{
    // Clip against z = 0.
    float3 c[LTC_MAX];
    uint m = 0;
    for (uint i = 0; i < n; ++i)
    {
        float3 a = p[i];
        float3 b = p[(i + 1u) % n];
        bool ia = a.z >= 0.0, ib = b.z >= 0.0;
        if (ia && m < LTC_MAX)
            c[m++] = a;
        if (ia != ib && m < LTC_MAX)
        {
            float t = a.z / (a.z - b.z);
            c[m++] = lerp(a, b, t);
        }
    }
    if (m < 3u)
        return 0.0;
    float sum = 0.0;
    float3 first = normalize(c[0]);
    float3 prev = first;
    for (uint i = 1; i < m; ++i)
    {
        float3 cur = normalize(c[i]);
        sum += de_ltc_edge(prev, cur);
        prev = cur;
    }
    sum += de_ltc_edge(prev, first);
    return abs(sum); // the fit already holds the 1 / (2 pi)
}

/// The corners of area light `l` (rect: 4, disk: 12-gon of equal area), relative to `p`.
public uint de_ltc_light_polygon(DeClusterLight l, float3 p, out float3 q[LTC_MAX])
{
    float3 x = l.axis.xyz;
    float3 z = -l.direction.xyz;
    float3 y = cross(z, x);
    float3 c = l.position.xyz - p;
    q[0] = c; // initialised (all paths below write every used entry)
    if (de_light_kind(l) == DE_LIGHT_KIND_RECT)
    {
        float hx = l.shape.y, hy = l.shape.z;
        q[0] = c - x * hx - y * hy;
        q[1] = c + x * hx - y * hy;
        q[2] = c + x * hx + y * hy;
        q[3] = c - x * hx + y * hy;
        return 4u;
    }
    // A regular 12-gon with the disk's area: R' = R sqrt(pi / (n/2 sin(2 pi / n))).
    float r = l.shape.y * 1.0233267;
    for (uint i = 0; i < DE_LTC_DISK_SIDES; ++i)
    {
        float a = 2.0 * DE_LIGHT_PI * float(i) / float(DE_LTC_DISK_SIDES);
        q[i] = c + (x * cos(a) + y * sin(a)) * r;
    }
    return DE_LTC_DISK_SIDES;
}

/// The LTC table entry for (perceptual roughness, n.v).
public void de_ltc_fetch(uint tab1, uint tab2, uint smp, float roughness, float nov,
                         out float4 t1, out float2 t2)
{
    float u = saturate(roughness);
    float v = sqrt(saturate(1.0 - nov));
    float2 uv = float2(u, v) * (float(DE_LTC_SIZE - 1u) / float(DE_LTC_SIZE)) +
                0.5 / float(DE_LTC_SIZE);
    // Table rows are roughness, columns sqrt(1 - cos): x = column (view), y = row (roughness).
    t1 = de_tex2d(tab1).SampleLevel(de_sampler(smp), uv.yx, 0.0);
    t2 = de_tex2d(tab2).SampleLevel(de_sampler(smp), uv.yx, 0.0).xy;
}

/// Diffuse and specular radiance from area light `l` at point `p` with normal `n`, view `v`
/// (unit, towards the eye): diffuse = luminance x form factor (multiply by the diffuse albedo);
/// specular = luminance x LTC form factor x (F0 x + F90 y).
public void de_ltc_area_light(DeClusterLight l, float3 p, float3 n, float3 v, float roughness,
                              float3 f0, float f90, uint tab1, uint tab2, uint smp,
                              out float3 diffuse, out float3 specular)
{
    diffuse = float3(0.0, 0.0, 0.0);
    specular = float3(0.0, 0.0, 0.0);
    float3 to_p = p - l.position.xyz;
    if (!(dot(to_p, l.direction.xyz) > 0.0) || !(dot(to_p, to_p) < l.position.w * l.position.w))
        return; // behind the emitting side, or out of range
    float3 q[LTC_MAX];
    uint count = de_ltc_light_polygon(l, p, q);
    // The shading frame: T1 towards the view in the tangent plane.
    float nov = saturate(dot(n, v));
    float3 t1 = v - n * dot(n, v);
    float l2 = dot(t1, t1);
    t1 = l2 > 1e-12 ? t1 * rsqrt(l2) : (abs(n.x) < 0.9 ? normalize(cross(n, float3(1, 0, 0)))
                                                       : normalize(cross(n, float3(0, 1, 0))));
    float3 t2 = cross(n, t1);
    float3 local[LTC_MAX];
    for (uint i = 0; i < LTC_MAX; ++i)
        local[i] = i < count ? float3(dot(q[i], t1), dot(q[i], t2), dot(q[i], n)) : float3(0, 0, 0);
    float window = de_light_window(length(to_p), l.position.w);
    float3 lum = l.color.rgb * window;
    diffuse = lum * de_ltc_polygon(local, count);
    float4 m;
    float2 fr;
    de_ltc_fetch(tab1, tab2, smp, roughness, nov, m, fr);
    // M^-1 = [[m.x, 0, m.y], [0, 1, 0], [m.z, 0, m.w]] (row-major).
    float3 ts[LTC_MAX];
    for (uint i = 0; i < LTC_MAX; ++i)
    {
        float3 a = local[i];
        ts[i] = float3(m.x * a.x + m.y * a.z, a.y, m.z * a.x + m.w * a.z);
    }
    specular = lum * de_ltc_polygon(ts, count) * (f0 * fr.x + f90 * fr.y);
}
