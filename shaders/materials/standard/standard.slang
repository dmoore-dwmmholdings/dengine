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
//   1. base          diffuse + GGX single and multiple scattering (DENG-299); iridescence makes
//                    its Fresnel a thin film's, anisotropy its GGX lobe anisotropic (DENG-300;
//                    standard_base.slang, inputs in layer_iridescence / layer_anisotropy.slang)
//   2. sheen         [DENG-300: insertion point S]  scales what is below by its albedo
//                    (layer_sheen.slang)
//   3. clearcoat     [DENG-300: insertion point C]  over everything below (layer_clearcoat.slang)
//   4. transmission  [DENG-301: insertion point T]  transmission, diffuse transmission, volume
//                    (lobe_transmission.slang, volume.slang): they take the reflected diffuse's
//                    place, under the sheen and the coat's attenuation along the view; seen from
//                    inside a volume (wo.z < 0) only the dielectric interface remains
//   5. subsurface    [DENG-306: insertion point SSS] the diffuse goes to the SSS target
module standard;

import bindless;
__exported import "../material_interface";
import "../../brdf/bsdf_common";
import "../../brdf/energy";
import "../../brdf/ggx";
import "../../brdf/sampling";
import "../../scene/scene_types";
import "../../visbuffer/resolve_model";
import standard_base;
import layer_sheen;
import layer_clearcoat;
import layer_anisotropy;
import layer_iridescence;
import lobe_transmission;
import volume;

/// Every lobe the Standard model has (DENG-300 / DENG-301 / DENG-306 add their bits here).
public static const uint DE_STANDARD_FEATURES_ALL = 0xFFFFFFFF;

/// The lobes of one surface and view direction (StandardModelT.stack).
public struct StandardStack
{
    public StandardBase base;
    public bool has_sheen;
    public SheenLayer sheen; // set when has_sheen
    public bool has_coat;
    public ClearcoatLayer coat; // set when has_coat
    public bool has_trans;                 // DENG-301: specular or diffuse transmission
    public TransmissionLobes trans;        // set when has_trans
    public float keep_o;                   // the sheen and coat's attenuation along the view
    public float p_base, p_sheen, p_coat; // sample's lobe probabilities
    public float p_trans, p_dt;           // DENG-301
};

/// The Standard model with the lobes of `FEATURES` compiled in.
public struct StandardModelT<let FEATURES : uint> : IMaterialSplitModel
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
        // DENG-306: subsurface scattering needs an amount and a mean free path (the screen-space
        // pass, passes/sss/sss.c, reads both from the material; de__material_features agrees).
        if (m.extra.z > 0.0 && max(m.subsurface.x, max(m.subsurface.y, m.subsurface.z)) > 0.0)
            s.features |= DE_MATERIAL_FEATURE_SUBSURFACE;
        if (m.clearcoat.x > 0.0)
            s.features |= DE_MATERIAL_FEATURE_CLEARCOAT;
        // DENG-300: the layers' inputs (each sets or clears its feature bit).
        if ((FEATURES & DE_MATERIAL_FEATURE_IRIDESCENCE) != 0u)
            de_iridescence_resolve(s, m, v, lod);
        if ((FEATURES & DE_MATERIAL_FEATURE_ANISOTROPY) != 0u)
            de_anisotropy_resolve(s, m, v, lod);
        if ((FEATURES & DE_MATERIAL_FEATURE_SHEEN) != 0u)
            de_sheen_resolve(s, m, v, lod); // insertion point S
        if ((FEATURES & DE_MATERIAL_FEATURE_CLEARCOAT) != 0u)
            de_clearcoat_resolve(s, m, v, lod); // insertion point C
        if (HAS_TRANSMISSION || HAS_DIFFUSE_TRANSMISSION)
            de_transmission_resolve(s, m, v, lod); // insertion point T
        // insertion point SSS
        return s;
    }

    /// The layers' inputs from the material record alone, for a pass that rebuilt `s` from the
    /// thin G-buffer (de_surface_from_gb) and has the pixel's material (lighting): factors,
    /// colours, roughness, the coat on the shading normal; no textures.
    public static void layers_from_material(inout SurfaceData s, DeGpuMaterial m)
    {
        if ((FEATURES & DE_MATERIAL_FEATURE_IRIDESCENCE) != 0u)
            de_iridescence_from_material(s, m);
        if ((FEATURES & DE_MATERIAL_FEATURE_ANISOTROPY) != 0u)
            de_anisotropy_from_material(s, m);
        if ((FEATURES & DE_MATERIAL_FEATURE_SHEEN) != 0u)
            de_sheen_from_material(s, m);
        if ((FEATURES & DE_MATERIAL_FEATURE_CLEARCOAT) != 0u)
            de_clearcoat_from_material(s, m);
        if (HAS_TRANSMISSION || HAS_DIFFUSE_TRANSMISSION)
            de_transmission_from_material(s, m); // insertion point T
        // insertion point SSS
    }

    /// The layers compiled in (constants: a pass that leaves one out has none of its code).
    static const bool HAS_SHEEN = (FEATURES & DE_MATERIAL_FEATURE_SHEEN) != 0u;
    static const bool HAS_COAT = (FEATURES & DE_MATERIAL_FEATURE_CLEARCOAT) != 0u;
    static const bool HAS_TRANSMISSION = (FEATURES & DE_MATERIAL_FEATURE_TRANSMISSION) != 0u;
    static const bool HAS_DIFFUSE_TRANSMISSION =
        (FEATURES & DE_MATERIAL_FEATURE_DIFFUSE_TRANSMISSION) != 0u;

    /// True when `s` refracts and `wo` is inside its volume: only the interface remains.
    static bool inside(SurfaceData s, float3 wo)
    {
        return HAS_TRANSMISSION && !(wo.z > 0.0) &&
               (s.features & DE_MATERIAL_FEATURE_TRANSMISSION) != 0u && de_transmission_refracts(s);
    }

    /// The lobes of `s` for view `wo`: the base, then each layer on top, with the probabilities
    /// sample picks them with (by albedo at wo).
    static StandardStack stack(SurfaceData s, float3 wo)
    {
        StandardStack k;
        k.base = de_standard_base_t<FEATURES>(s, wo, true);
        k.has_sheen = HAS_SHEEN && (s.features & DE_MATERIAL_FEATURE_SHEEN) != 0u;
        k.has_coat = HAS_COAT && (s.features & DE_MATERIAL_FEATURE_CLEARCOAT) != 0u;
        float w_base = k.base.albedo, w_sheen = 0.0, w_coat = 0.0, w_trans = 0.0, w_dt = 0.0;
        k.keep_o = 1.0;
        // insertion point S
        if (HAS_SHEEN && k.has_sheen)
        {
            k.sheen = de_sheen_layer(s, wo);
            w_base *= saturate(1.0 - k.sheen.k * k.sheen.e_o);
            k.keep_o *= saturate(1.0 - k.sheen.k * k.sheen.e_o);
            w_sheen = dot(de_sheen_albedo_o(k.sheen), float3(0.2126, 0.7152, 0.0722));
        }
        // insertion point C
        if (HAS_COAT && k.has_coat)
        {
            k.coat = de_clearcoat_layer(s, wo);
            float keep = saturate(1.0 - k.coat.e_o);
            w_base *= keep;
            w_sheen *= keep;
            w_coat = k.coat.e_o;
            k.keep_o *= keep;
        }
        // insertion point T
        bool has_t = HAS_TRANSMISSION && (s.features & DE_MATERIAL_FEATURE_TRANSMISSION) != 0u &&
                     s.transmission > 0.0;
        bool has_dt = HAS_DIFFUSE_TRANSMISSION &&
                      (s.features & DE_MATERIAL_FEATURE_DIFFUSE_TRANSMISSION) != 0u &&
                      s.diffuse_transmission > 0.0;
        k.has_trans = has_t || has_dt;
        if ((HAS_TRANSMISSION || HAS_DIFFUSE_TRANSMISSION) && k.has_trans)
        {
            k.trans = de_transmission_lobes(s, k.base, wo, has_t, has_dt);
            // The reflected diffuse keeps (1 - t)(1 - dt) of itself.
            float df = dot(k.base.diffuse * DE_PI * (1.0 - k.base.ed_o),
                           float3(0.2126, 0.7152, 0.0722));
            w_base = max(w_base - df * (1.0 - k.trans.keep) * k.keep_o, 0.0);
            w_trans = k.trans.albedo_t * k.keep_o;
            w_dt = k.trans.albedo_dt * k.keep_o;
        }
        // insertion point SSS
        float total = w_base + w_sheen + w_coat + w_trans + w_dt;
        k.p_base = total > 0.0 ? w_base / total : 1.0;
        k.p_sheen = total > 0.0 ? w_sheen / total : 0.0;
        k.p_coat = total > 0.0 ? w_coat / total : 0.0;
        k.p_trans = total > 0.0 ? w_trans / total : 0.0;
        k.p_dt = total > 0.0 ? w_dt / total : 0.0;
        return k;
    }

    /// BSDF x cos(theta_i), and the share of it that is the diffuse lobe (`diffuse`; what lighting
    /// separates for subsurface scattering, DENG-303 / DENG-306). The rest is specular: GGX,
    /// multi-scatter, sheen, coat. Splitting by evaluating with base_color 0 does not work:
    /// metals take their F0 from the base colour.
    public static float3 evaluate_split(SurfaceData s, float3 wo, float3 wi, out float3 diffuse)
    {
        diffuse = float3(0.0, 0.0, 0.0);
        if (HAS_TRANSMISSION && inside(s, wo)) // insertion point T: inside the volume
            return de_interface_inside_evaluate(de_ggx_alpha(saturate(s.roughness)), s.ior, wo, wi);
        StandardStack k = stack(s, wo);
        // 1. base (DENG-299; iridescence and anisotropy inside, DENG-300)
        float3 d;
        float3 f = de_standard_base_evaluate_split_t<FEATURES>(k.base, s, wo, wi, d);
        // 2. sheen (DENG-300): insertion point S
        if (HAS_SHEEN && k.has_sheen)
        {
            float keep = de_sheen_scale(k.sheen, wi);
            f = f * keep + de_sheen_evaluate(k.sheen, wo, wi);
            d *= keep;
        }
        // 3. clearcoat (DENG-300): insertion point C
        if (HAS_COAT && k.has_coat)
        {
            float keep = de_clearcoat_scale(k.coat, wi);
            f = f * keep + de_clearcoat_evaluate(k.coat, wi);
            d *= keep;
        }
        // 4. transmission, diffuse transmission, volume (DENG-301): insertion point T
        if ((HAS_TRANSMISSION || HAS_DIFFUSE_TRANSMISSION) && k.has_trans)
        {
            f -= d * (1.0 - k.trans.keep);
            d *= k.trans.keep;
            float3 dt;
            f += de_transmission_evaluate(k.trans, wo, wi, dt) * k.keep_o;
            d += dt * k.keep_o;
        }
        // 5. subsurface (DENG-306): insertion point SSS. No lobe: the diffuse share `d` is what the
        //    screen-space pass (passes/sss/sss.c) blurs with the Burley profile at SUBSURFACE
        //    pixels, and what the path tracer's random walk replaces (S17, DENG-73).
        diffuse = d;
        return f;
    }

    public static float3 evaluate(SurfaceData s, float3 wo, float3 wi)
    {
        float3 unused;
        return evaluate_split(s, wo, wi, unused);
    }

    static float stack_pdf(StandardStack k, float3 wo, float3 wi)
    {
        // The same chain as evaluate: each step mixes its density in.
        float p = k.p_base * de_standard_base_pdf_t<FEATURES>(k.base, wo, wi);
        if (HAS_SHEEN && k.has_sheen) // insertion point S
            p += k.p_sheen * de_sheen_pdf(wi);
        if (HAS_COAT && k.has_coat) // insertion point C
            p += k.p_coat * de_clearcoat_pdf(k.coat, wi);
        if ((HAS_TRANSMISSION || HAS_DIFFUSE_TRANSMISSION) && k.has_trans) // insertion point T
            p += k.p_trans * de_transmission_specular_pdf(k.trans, wo, wi) +
                 k.p_dt * de_transmission_diffuse_pdf(wi);
        // insertion point SSS
        return p;
    }

    public static float pdf(SurfaceData s, float3 wo, float3 wi)
    {
        if (HAS_TRANSMISSION && inside(s, wo)) // insertion point T
            return de_interface_inside_pdf(de_ggx_alpha(saturate(s.roughness)), s.ior, wo, wi);
        if (!(wo.z > 0.0))
            return 0.0;
        return stack_pdf(stack(s, wo), wo, wi);
    }

    public static BsdfSample sample(SurfaceData s, float3 wo, float3 u)
    {
        // The same chain as evaluate: each step takes its share of u.z.
        if (HAS_TRANSMISSION && inside(s, wo)) // insertion point T
            return de_interface_inside_sample(de_ggx_alpha(saturate(s.roughness)), s.ior, wo, u);
        StandardStack k = stack(s, wo);
        BsdfSample r;
        r.wi = float3(0.0, 0.0, 1.0);
        r.weight = float3(0.0, 0.0, 0.0);
        r.pdf = 0.0;
        r.lobe = 0u;
        if (!(wo.z > 0.0))
            return r;
        bool layered = (HAS_SHEEN && k.has_sheen) || (HAS_COAT && k.has_coat) ||
                       ((HAS_TRANSMISSION || HAS_DIFFUSE_TRANSMISSION) && k.has_trans);
        if (!layered || u.z < k.p_base)
        {
            float3 ub = float3(u.xy, k.p_base > 0.0 ? saturate(u.z / k.p_base) : 0.0);
            r = de_standard_base_sample_t<FEATURES>(k.base, s, wo, ub);
            if (!layered)
                return r; // the base alone: its own pdf and weight
        }
        else if (HAS_SHEEN && u.z < k.p_base + k.p_sheen) // insertion point S
        {
            r.wi = de_sample_cosine_hemisphere(u.xy);
            r.lobe = DE_BSDF_LOBE_SPECULAR;
        }
        else if (HAS_COAT && u.z < k.p_base + k.p_sheen + k.p_coat) // insertion point C
        {
            r.wi = de_clearcoat_sample(k.coat, u.xy);
            r.lobe = DE_BSDF_LOBE_SPECULAR;
        }
        else if ((HAS_TRANSMISSION || HAS_DIFFUSE_TRANSMISSION) &&
                 u.z < k.p_base + k.p_sheen + k.p_coat + k.p_trans) // insertion point T
        {
            if (!de_transmission_specular_sample(k.trans, wo, u.xy, r.wi))
                return r; // no direction: weight 0
            r.lobe = DE_BSDF_LOBE_TRANSMISSION;
        }
        else if (HAS_TRANSMISSION || HAS_DIFFUSE_TRANSMISSION)
        {
            r.wi = de_transmission_diffuse_sample(u.xy);
            r.lobe = DE_BSDF_LOBE_TRANSMISSION;
        }
        // insertion point SSS
        r.pdf = stack_pdf(k, wo, r.wi);
        float3 unused;
        r.weight = float3(0.0, 0.0, 0.0);
        if (r.pdf > 0.0)
            r.weight = evaluate_split(s, wo, r.wi, unused) / r.pdf;
        return r;
    }

    public static uint lobes(SurfaceData s)
    {
        uint l = DE_BSDF_LOBE_SPECULAR;
        if (s.metallic < 1.0 || s.roughness > 0.0)
            l |= DE_BSDF_LOBE_DIFFUSE; // the diffuse and the multi-scatter lobes
        // insertion points S, C (both specular)
        if ((HAS_TRANSMISSION &&
             (s.features & DE_MATERIAL_FEATURE_TRANSMISSION) != 0u && s.transmission > 0.0) ||
            (HAS_DIFFUSE_TRANSMISSION &&
             (s.features & DE_MATERIAL_FEATURE_DIFFUSE_TRANSMISSION) != 0u &&
             s.diffuse_transmission > 0.0))
            l |= DE_BSDF_LOBE_TRANSMISSION; // insertion point T
        // insertion point SSS
        return l;
    }
};

/// The Standard model with every lobe.
public typealias StandardModel = StandardModelT<DE_STANDARD_FEATURES_ALL>;
