// shaders/lighting/lights.slang: the lights of the clustered pipeline and their physical units
// (SPEC §12.7; DENG-303). src/render/lighting/light_data.c has the same arithmetic in C (the
// tests' reference); lighting.h mirrors DeClusterLight.
//
// Units of DeLight.intensity, as authored (de_render.h):
//   directional       illuminance in lux at normal incidence
//   point             luminous power in lumens -> intensity I = power / (4 pi) candela
//   spot              luminous power in lumens -> I = power / (2 pi (1 - cos outer)) candela: the
//                     power of a point light concentrated into the outer cone (spot_outer is the
//                     outer HALF-angle, radians; spot_inner the half-angle of full intensity)
//   rect, disk        luminance in nits (cd / m^2) of the emitting side
// Illuminance of a punctual light at distance d: E = I / max(d, source_radius)^2 x window, with
// the windowed inverse square window = saturate(1 - (d / r)^4)^2 (Karis 2013); r = DeLight.range,
// or with range 0 the distance where the unwindowed illuminance of the brightest colour channel
// falls to the cutoff (r.light_cutoff_lux): r = sqrt(I x max(colour) / cutoff). Spot falloff:
// t = saturate((cos theta - cos outer) / (cos inner - cos outer)), factor t^2.
//
// Directions: a light points along its transform's -Z (glTF convention; DENG-175 transforms
// relative to the render origin). Rect and disk lights lie in their transform's XY plane and emit
// towards -Z: X and Y span the rectangle (area_size.x along X, .y along Y; a disk's diameter is
// area_size.x).
module lights;

import bindless;
__exported import "../scene/scene_types";

/// de_light_kind (de_render.h).
public static const uint DE_LIGHT_KIND_DIRECTIONAL = 0;
public static const uint DE_LIGHT_KIND_POINT = 1;
public static const uint DE_LIGHT_KIND_SPOT = 2;
public static const uint DE_LIGHT_KIND_RECT = 3;
public static const uint DE_LIGHT_KIND_DISK = 4;
/// DeClusterLight flags (axis.w)
public static const uint DE_CLUSTER_LIGHT_PROXY = 1;  // an emissive mesh's proxy light
public static const uint DE_CLUSTER_LIGHT_SHADOWS = 2; // casts shadows (S15)

public static const float DE_LIGHT_PI = 3.14159265358979323846;

/// A light as the clustered pipeline shades with it (96 bytes): built per view and frame by
/// cs_light_prep (light_cull.slang) from the GPU scene's light table and the emissive proxies.
public struct DeClusterLight
{
    public float4 position;  // render-origin relative xyz; w = range r (m; directional: 0)
    public float4 direction; // the direction the light points (unit); w = source radius (directional: angular radians; local: metres)
    public float4 color;     // colour x intensity in shading units (cd, lux, nits); w = cos(outer)
    public float4 shape;     // x 1 / (cos inner - cos outer); y, z area half extents (m); w kind
    public float4 axis;      // area light X axis (unit) xyz; w flags (DE_CLUSTER_LIGHT_*)
    public float4 bound;     // the culling sphere in view space: centre xyz, radius w
};

public uint de_light_kind(DeClusterLight l)
{
    return asuint(l.shape.w);
}

/// Intensity in candela of `lumens` emitted by a point light.
public float de_point_candela(float lumens)
{
    return lumens / (4.0 * DE_LIGHT_PI);
}

/// Intensity in candela of `lumens` emitted into a spot cone of outer half-angle `outer`.
public float de_spot_candela(float lumens, float outer)
{
    float c = cos(clamp(outer, 1e-3, DE_LIGHT_PI));
    return lumens / (2.0 * DE_LIGHT_PI * max(1.0 - c, 1e-6));
}

/// The windowed inverse-square factor at distance `d` of a light of range `r` (no 1/d^2).
public float de_light_window(float d, float r)
{
    float x = d / r;
    float w = saturate(1.0 - x * x * x * x);
    return w * w;
}

/// The range a light of intensity `intensity` (cd, colour already applied: the brightest
/// channel) reaches before its illuminance falls to `cutoff` lux.
public float de_light_derived_range(float intensity, float cutoff)
{
    return sqrt(max(intensity, 0.0) / max(cutoff, 1e-9));
}

/// The spot factor of direction `to_point` (unit, from the light) for light `l`.
public float de_spot_factor(DeClusterLight l, float3 to_point)
{
    float c = dot(to_point, l.direction.xyz);
    float t = saturate((c - l.color.w) * l.shape.x);
    return t * t;
}

/// What a punctual or directional light delivers to point `p`.
public struct DeLightHit
{
    public float3 wi;          // unit, from `p` towards the light (world)
    public float3 illuminance; // rgb lux at normal incidence (cosine not applied)
    public float distance;     // m (directional: a large value)
};

/// Light `l` (directional, point or spot) at point `p`; illuminance 0 outside its range or cone.
public DeLightHit de_light_punctual(DeClusterLight l, float3 p)
{
    DeLightHit h;
    uint kind = de_light_kind(l);
    if (kind == DE_LIGHT_KIND_DIRECTIONAL)
    {
        h.wi = -l.direction.xyz;
        h.illuminance = l.color.rgb;
        h.distance = 1e30;
        return h;
    }
    float3 v = l.position.xyz - p;
    float d2 = dot(v, v);
    float d = sqrt(d2);
    h.wi = d > 0.0 ? v / d : float3(0.0, 0.0, 1.0);
    h.distance = d;
    float r = l.position.w;
    if (!(d < r))
    {
        h.illuminance = float3(0.0, 0.0, 0.0);
        return h;
    }
    float rs = l.direction.w;
    float e = de_light_window(d, r) / max(d2, max(rs * rs, 1e-8));
    if (kind == DE_LIGHT_KIND_SPOT)
        e *= de_spot_factor(l, -h.wi);
    h.illuminance = l.color.rgb * e;
    return h;
}

/// The perceptual roughness a punctual light of source radius `radius` at distance `d` widens
/// `roughness` to (sphere-light normalisation, Karis 2013: alpha' = alpha + radius / (2 d)).
public float de_light_widened_roughness(float roughness, float radius, float d)
{
    if (!(radius > 0.0) || !(d > 0.0))
        return roughness;
    float a = roughness * roughness;
    float a2 = saturate(a + radius / (2.0 * d));
    return sqrt(a2);
}
