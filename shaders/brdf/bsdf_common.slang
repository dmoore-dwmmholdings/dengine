// shaders/brdf/bsdf_common.slang: what every BSDF module and material model shares (SPEC §12.2,
// §12.6; DENG-298): constants, the lobe flags, the sample record and the shading frame.
//
// The modules of shaders/brdf/ are pure functions: no resource bindings, no derivatives (no ddx),
// no pass-specific state, so the raster resolve and lighting (S14), RT hit shading (S16) and the
// path tracer (S17) import the same code. tests/unit/render/brdf_ref.c has the same functions in
// C (the GPU-vs-C test).
//
// Shading frame convention: BSDF directions are in a local frame with Z along the shading normal
// (X the tangent, Y the bitangent), unit length, both pointing away from the surface: `wo`
// towards the viewer (or the previous vertex of a path), `wi` towards the light (or the next
// vertex). cos(theta) of a direction is its z. ShadingFrame converts between world and local.
module bsdf_common;

public static const float DE_PI = 3.14159265358979323846;
public static const float DE_INV_PI = 0.31830988618379067154;

/// BSDF lobe flags (IMaterialModel.lobes, BsdfSample.lobe): which kinds of scattering a surface
/// has, for multiple importance sampling.
public static const uint DE_BSDF_LOBE_DIFFUSE = 1;
public static const uint DE_BSDF_LOBE_SPECULAR = 2;
public static const uint DE_BSDF_LOBE_TRANSMISSION = 4;

/// One sampled direction of a BSDF.
public struct BsdfSample
{
    public float3 wi;     // local frame, unit
    public float3 weight; // evaluate(wo, wi) / pdf: BSDF x cos(theta_i) / pdf; 0 without a sample
    public float pdf;     // solid-angle density of wi; 0 = no sample (wi below the horizon, ...)
    public uint lobe;     // the DE_BSDF_LOBE_* the sample came from
};

/// An orthonormal frame: t (tangent, X), b (bitangent, Y), n (normal, Z).
public struct ShadingFrame
{
    public float3 t;
    public float3 b;
    public float3 n;

    /// World direction `v` in this frame's local coordinates.
    public float3 to_local(float3 v)
    {
        return float3(dot(v, t), dot(v, b), dot(v, n));
    }
    /// Local direction `v` in world coordinates.
    public float3 to_world(float3 v)
    {
        return t * v.x + b * v.y + n * v.z;
    }
};

/// A frame around unit normal `n` with an arbitrary but continuous tangent (Duff et al. 2017,
/// "Building an Orthonormal Basis, Revisited").
public ShadingFrame de_frame_from_normal(float3 n)
{
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float b = n.x * n.y * a;
    ShadingFrame f;
    f.t = float3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    f.b = float3(b, s + n.y * n.y * a, -n.y);
    f.n = n;
    return f;
}

/// A frame around unit normal `n` with its X along `tangent.xyz` made perpendicular to `n`
/// (Gram-Schmidt) and its Y = cross(n, t) x `tangent.w` (the bitangent sign). A tangent (nearly)
/// parallel to `n`, or zero, gives de_frame_from_normal.
public ShadingFrame de_frame_from_tangent(float3 n, float4 tangent)
{
    float3 t = tangent.xyz - n * dot(n, tangent.xyz);
    float l2 = dot(t, t);
    if (!(l2 > 1e-12))
        return de_frame_from_normal(n);
    ShadingFrame f;
    f.n = n;
    f.t = t * rsqrt(l2);
    f.b = cross(n, f.t) * (tangent.w < 0.0 ? -1.0 : 1.0);
    return f;
}

/// x^5, for the Schlick-style terms.
public float de_pow5(float x)
{
    float x2 = x * x;
    return x2 * x2 * x;
}

/// True when local directions `a` and `b` are on the same side of the surface.
public bool de_same_hemisphere(float3 a, float3 b)
{
    return a.z * b.z > 0.0;
}
