// shaders/materials/standard/standard.slang: the Standard material model (SPEC §12.6; DENG-299),
// material model id 0 and the model of every imported glTF / FBX material. StandardModel
// implements IMaterialModel (materials/material_interface.slang); the vis-buffer resolve
// instantiates de_resolve_tiles<StandardModel> (engine shader vis_resolve), lighting (DENG-303)
// and RT (S16, S17) call evaluate / sample / pdf.
//
//   resolve_gbuffer  the glTF inputs: base colour x factor x vertex colour, metallic-roughness
//                    (B, G) x factors, a tangent-space normal map x normal_scale, occlusion (R),
//                    emissive x emissive_strength x texture (radiance in cd/m^2 before exposure),
//                    the KHR specular colour and factor and the IOR; the material's features
//   evaluate etc.    the composition below: the base lobes of standard_base.slang, then one
//                    insertion point per layer
//
// Feature specialisation: StandardModelT<FEATURES> compiles in the lobes whose
// DE_MATERIAL_FEATURE_* bit is set in FEATURES (a compile-time constant, so a pass can
// instantiate a cheaper model) and runs each one only where SurfaceData.features has it.
// StandardModel compiles every lobe in.
//
// Composition chain. DENG-300 (layers) and DENG-301 (transmission) edit it in wave 3, DENG-306
// (subsurface) in wave 4: each adds its step at its insertion point and nowhere else.
//
//   1. base          diffuse + GGX single and multiple scattering (DENG-299)
//   2. sheen         [DENG-300: insertion point S]  scales what is below by its albedo
//   3. clearcoat     [DENG-300: insertion point C]  over everything below
//   4. transmission  [DENG-301: insertion point T]  transmission, diffuse transmission, volume
//   5. subsurface    [DENG-306: insertion point SSS] the diffuse goes to the SSS target
module standard;

import bindless;
__exported import "../material_interface";
import "../../brdf/bsdf_common";
import "../../brdf/energy";
import "../../scene/scene_types";
import "../../visbuffer/resolve_model";
import standard_base;

/// Every lobe the Standard model has (DENG-300 / DENG-301 / DENG-306 add their bits here).
public static const uint DE_STANDARD_FEATURES_ALL = 0xFFFFFFFF;

/// The Standard model with the lobes of `FEATURES` compiled in.
public struct StandardModelT<let FEATURES : uint> : IMaterialModel
{
    public typealias Params = DeGpuMaterial;

    public static Params load_params(DeGpuMaterial material)
    {
        return material;
    }

    public static SurfaceData resolve_gbuffer(Params m, VertexAttribs v, TextureLodInfo lod)
    {
        float3 n = v.normal;
        float4 tangent = v.tangent;
        // Double-sided materials face the viewer.
        if ((m.flags & DE_GPU_MATERIAL_DOUBLE_SIDED) != 0u && dot(v.geometric_normal, v.view) < 0.0)
        {
            n = -n;
            tangent.w = -tangent.w;
        }
        float4 white = float4(1.0, 1.0, 1.0, 1.0);
        float4 base = m.base_color * v.color *
                      de_resolve_sample(m, DE_RESOLVE_TEX_BASE_COLOR, v, lod, white);
        float4 mr = de_resolve_sample(m, DE_RESOLVE_TEX_METAL_ROUGH, v, lod, white);
        float4 nm = de_resolve_sample(m, DE_RESOLVE_TEX_NORMAL, v, lod, float4(0.5, 0.5, 1.0, 1.0));
        float4 ao = de_resolve_sample(m, DE_RESOLVE_TEX_OCCLUSION, v, lod, white);
        float4 em = de_resolve_sample(m, DE_RESOLVE_TEX_EMISSIVE, v, lod, white);
        uint unused;
        if (de_resolve_texture(m, DE_RESOLVE_TEX_NORMAL, unused) != DE_BINDLESS_INVALID &&
            v.has_tangent)
        {
            float3 tn = (nm.xyz * 2.0 - 1.0) * float3(m.pbr.z, m.pbr.z, 1.0);
            float3 t = normalize(tangent.xyz - n * dot(n, tangent.xyz));
            float3 b = cross(n, t) * tangent.w;
            n = normalize(t * tn.x + b * tn.y + n * tn.z);
        }
        SurfaceData s = de_surface_default();
        s.base_color = base.rgb;
        s.ao = ao.r;
        s.normal = n;
        s.roughness = saturate(m.pbr.y * mr.g);
        s.tangent = v.has_tangent ? tangent : float4(0.0, 0.0, 0.0, 1.0);
        s.metallic = saturate(m.pbr.x * mr.b);
        s.specular = m.specular.w;
        s.specular_color = m.specular.rgb;
        s.ior = m.clearcoat.z;
        s.emissive = m.emissive.rgb * m.emissive.w * em.rgb;
        s.model = m.model & (DE_GPU_MATERIAL_MODELS - 1u);
        s.features = 0u;
        if ((m.flags & DE_GPU_MATERIAL_DOUBLE_SIDED) != 0u)
            s.features |= DE_MATERIAL_FEATURE_DOUBLE_SIDED;
        if (m.clearcoat.w > 0.0)
            s.features |= DE_MATERIAL_FEATURE_TRANSMISSION;
        if (m.extra.y > 0.0)
            s.features |= DE_MATERIAL_FEATURE_DIFFUSE_TRANSMISSION;
        if (m.extra.z > 0.0)
            s.features |= DE_MATERIAL_FEATURE_SUBSURFACE;
        if (m.clearcoat.x > 0.0)
            s.features |= DE_MATERIAL_FEATURE_CLEARCOAT;
        return s;
    }

    public static float3 evaluate(SurfaceData s, float3 wo, float3 wi)
    {
        // 1. base (DENG-299)
        StandardBase b = de_standard_base(s, wo, true);
        float3 f = de_standard_base_evaluate(b, s, wo, wi);
        // 2. sheen (DENG-300): insertion point S
        // 3. clearcoat (DENG-300): insertion point C
        // 4. transmission, diffuse transmission, volume (DENG-301): insertion point T
        // 5. subsurface (DENG-306): insertion point SSS
        return f;
    }

    public static float pdf(SurfaceData s, float3 wo, float3 wi)
    {
        // The same chain as evaluate: each step mixes its density in.
        StandardBase b = de_standard_base(s, wo, true);
        float p = de_standard_base_pdf(b, wo, wi);
        // insertion points S, C, T, SSS
        return p;
    }

    public static BsdfSample sample(SurfaceData s, float3 wo, float3 u)
    {
        // The same chain as evaluate: each step takes its share of u.z.
        StandardBase b = de_standard_base(s, wo, true);
        BsdfSample r = de_standard_base_sample(b, s, wo, u);
        // insertion points S, C, T, SSS
        return r;
    }

    public static uint lobes(SurfaceData s)
    {
        uint l = DE_BSDF_LOBE_SPECULAR;
        if (s.metallic < 1.0 || s.roughness > 0.0)
            l |= DE_BSDF_LOBE_DIFFUSE; // the diffuse and the multi-scatter lobes
        // insertion points S, C, T (DE_BSDF_LOBE_TRANSMISSION), SSS
        return l;
    }
};

/// The Standard model with every lobe.
public typealias StandardModel = StandardModelT<DE_STANDARD_FEATURES_ALL>;
