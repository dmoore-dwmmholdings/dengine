// shaders/lighting/lighting_inputs.slang: the optional inputs of direct and indirect lighting
// (SPEC §12.5 steps 5-7; DENG-303). src/render/lighting/lighting_inputs.h is the C side (the
// per-view de__lighting_inputs a producer fills before lighting; a test compares the two layouts).
//
// A CONTRACT, append only: DENG-304 (indirect diffuse and specular) and the S15 tasks (shadows,
// AO, contact shadows: DENG-335, DENG-337, DENG-338, DENG-339, DENG-340, DENG-341) each add one
// field and one accessor. Every field is the bindless index of a sampled texture at the view's
// internal size; 0 (DE_BINDLESS_INVALID) means "no producer this frame", and the accessor then
// returns the neutral value (1 for visibility and occlusion terms, 0 for added light), so a
// missing producer never changes the image. Fields are grouped in blocks of four uints (16 bytes):
// add a whole block when the reserved words run out.
module lighting_inputs;

import bindless;
public static const uint DE_INPUT_DIFFUSE_OPAQUE = 2u;
public static const uint DE_INPUT_SPECULAR_OPAQUE = 1u; // reserved0 flag, DENG-339

/// The inputs of one view and frame (64 bytes; DeLightingView.inputs).
public static const uint DE_INPUT_SHADOW_METADATA = 4;

public struct LightingInputs
{
    public uint ao;                 // R: ambient occlusion (screen space), 1 = none
    public uint shadow_directional; // CSM metadata when SHADOW_METADATA is set; otherwise RGBA: visibility of directional lights 0..3, 1 = lit
    public uint shadow_local;       // R: visibility of the local lights (S15 defines the layout)
    public uint contact_shadows;    // R: contact-shadow visibility of the main directional light
    public uint indirect_diffuse;   // RGB: indirect diffuse irradiance x albedo / pi (DENG-304)
    public uint indirect_specular;  // RGB: indirect specular radiance (DENG-304)
    public uint reserved0;
    public uint reserved1;
    public uint probe_records;
    public uint probe_list;
    public uint probe_entry;
    public uint probe_sampler;
    public float4 probe_view_z;
};

/// Texel `p` of input texture `index`, or `fallback` without one.
public float4 de_input_load(uint index, uint2 p, float4 fallback)
{
    if (index == DE_BINDLESS_INVALID)
        return fallback;
    return de_tex2d(index).Load(int3(int(p.x), int(p.y), 0));
}

/// Ambient occlusion at pixel `p` (1 without a producer).
public float de_input_ao(LightingInputs i, uint2 p)
{
    return de_input_load(i.ao, p, float4(1.0, 1.0, 1.0, 1.0)).r;
}

/// Visibility of directional light `light` (0..3) at pixel `p` (1 without a producer).
public float de_input_shadow_directional(LightingInputs i, uint2 p, uint light)
{
    if ((i.reserved0 & DE_INPUT_SHADOW_METADATA) != 0u) return 1.0;
    float4 v = de_input_load(i.shadow_directional, p, float4(1.0, 1.0, 1.0, 1.0));
    return light == 0u ? v.x : light == 1u ? v.y : light == 2u ? v.z : v.w;
}

/// Visibility of local light `light` (its index in the view's local list) at pixel `p` (1
/// without a producer; S15 replaces the body when it defines the layout).
public float de_input_shadow_local(LightingInputs i, uint2 p, uint light)
{
    return de_input_load(i.shadow_local, p, float4(1.0, 1.0, 1.0, 1.0)).r;
}

/// Contact-shadow visibility of directional light 0 at pixel `p` (1 without a producer).
public float de_input_contact_shadow(LightingInputs i, uint2 p)
{
    return de_input_load(i.contact_shadows, p, float4(1.0, 1.0, 1.0, 1.0)).r;
}

/// DENG-304: the indirect inputs override image-based lighting. rgb: the radiance leaving pixel
/// `p` (diffuse: before AO); a: its weight over the IBL estimate (0 without a producer, so the
/// IBL stays). S15 producers (probes, SSR, SSGI) write a < 1 to blend.
public float4 de_input_indirect_diffuse_override(LightingInputs i, uint2 p)
{
    return de_input_load(i.indirect_diffuse, p, float4(0.0, 0.0, 0.0, 0.0));
}

/// The specular override of pixel `p` (as de_input_indirect_diffuse_override).
public float4 de_input_indirect_specular_override(LightingInputs i, uint2 p)
{
    return de_input_load(i.indirect_specular, p, float4(0.0, 0.0, 0.0, 0.0));
}

/// Material-weighted unexposed outgoing specular, over probe/sky by SSR confidence.
public float3 de_input_compose_specular(LightingInputs i,uint2 p,float3 fallback)
{
    float4 ssr=de_input_indirect_specular_override(i,p);
    return lerp(fallback,ssr.rgb,saturate(ssr.a));
}

/// Indirect diffuse radiance leaving pixel `p` (0 without a producer).
public float3 de_input_indirect_diffuse(LightingInputs i, uint2 p)
{
    return de_input_load(i.indirect_diffuse, p, float4(0.0, 0.0, 0.0, 0.0)).rgb;
}

/// Indirect specular radiance leaving pixel `p` (0 without a producer).
public float3 de_input_indirect_specular(LightingInputs i, uint2 p)
{
    return de_input_load(i.indirect_specular, p, float4(0.0, 0.0, 0.0, 0.0)).rgb;
}
