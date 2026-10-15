// shaders/materials/material_params.slang: the material parameter block (SPEC §12.6; DENG-302).
// One block per material instance in the GPU scene's material table (one bindless buffer, stride
// 256 bytes), read through its bindless index:
//
//     DeGpuMaterial m = de_robuffer<DeGpuMaterial>(sc.materials)[slot];
//
// scene/scene_types.slang re-exports this module, so every pass that imports scene_types sees
// DeGpuMaterial. The C mirror is the X-macro table of src/render/materials/material_params.h; the
// test render_material_instances.material_params_layout_match parses this file and compares every
// field and constant with it: change both together.
//
// Layout rules (as scene_types.slang): uint, float, uint4 and float4 fields, every 16-byte field
// at a multiple of 16, the struct a multiple of 16 bytes.
//
// The header comes first: `model` at offset 0 (material classification and the resolve read it
// there, DENG-237 / DENG-298), then the feature bits the CPU derives from the parameter values
// (DE_MATERIAL_FEATURE_* of material_interface.slang; they select the resolve permutation), the
// flags and the alpha mode. The ten float4 rows are the Standard parameters (de_standard_material),
// the last row holds the texture slots (bindless index, bit 31 the UV set) and the diffuse
// transmission colour (unorm 10:10:10). Read them with the accessors below.
module material_params;

/// DeGpuMaterial.flags
public static const uint DE_GPU_MATERIAL_READY = 1; // the material is loaded
public static const uint DE_GPU_MATERIAL_DOUBLE_SIDED = 2;
public static const uint DE_GPU_MATERIAL_UNLIT = 4;

/// DeGpuMaterial.alpha_mode (de_cooked_alpha_mode)
public static const uint DE_GPU_MATERIAL_ALPHA_OPAQUE = 0;
public static const uint DE_GPU_MATERIAL_ALPHA_MASK = 1;
public static const uint DE_GPU_MATERIAL_ALPHA_BLEND = 2;

/// Texture slots per material (de_cooked_tex_slot): 19 words in textures[0..4], each the slot's
/// sampled bindless index (0 = none) with DE_GPU_MATERIAL_TEXTURE_UV1 set when it samples UV1.
/// The 20th word (textures[4].w) holds the diffuse transmission colour as 1 - colour in unorm
/// 10:10:10 (r in bits 0-9), so a zeroed block has the KHR default, white.
public static const uint DE_GPU_MATERIAL_TEXTURES = 19;
public static const uint DE_GPU_MATERIAL_TEXTURE_UV1 = 0x80000000;

/// One material instance (256 bytes).
public struct DeGpuMaterial
{
    public uint model;         // material model id (DE_GPU_MATERIAL_MODEL_*, < DE_GPU_MATERIAL_MODELS)
    public uint features;      // DE_MATERIAL_FEATURE_* the parameters use (derived on the CPU)
    public uint flags;         // DE_GPU_MATERIAL_*
    public uint alpha_mode;    // DE_GPU_MATERIAL_ALPHA_*
    public float4 base_color;
    public float4 emissive;    // rgb, strength
    public float4 pbr;         // metallic, roughness, normal_scale, alpha_cutoff
    public float4 specular;    // specular_color rgb, specular
    public float4 clearcoat;   // clearcoat, clearcoat_roughness, ior, transmission
    public float4 sheen;       // sheen_color rgb, sheen_roughness
    public float4 volume;      // attenuation_color rgb, attenuation_distance
    public float4 extra;       // thickness, diffuse_transmission, subsurface, anisotropy
    public float4 iridescence; // iridescence, ior, thickness_min, thickness_max
    public float4 subsurface;  // subsurface_radius rgb, anisotropy_rotation
    public uint4 textures[5];  // per de_cooked_tex_slot: bindless index | UV1 bit; [4].w dt colour
};

/// The bindless index of texture slot `s` (< DE_GPU_MATERIAL_TEXTURES) of `m`, 0 = none.
public uint de_material_texture(DeGpuMaterial m, uint s)
{
    return m.textures[s >> 2][s & 3u] & ~DE_GPU_MATERIAL_TEXTURE_UV1;
}

/// The UV set (0 or 1) texture slot `s` of `m` samples.
public uint de_material_texcoord(DeGpuMaterial m, uint s)
{
    return m.textures[s >> 2][s & 3u] >> 31;
}

/// The diffuse transmission colour factor (KHR_materials_diffuse_transmission), linear RGB.
public float3 de_material_diffuse_transmission_color(DeGpuMaterial m)
{
    uint p = m.textures[4].w;
    return 1.0 - float3(float(p & 1023u), float((p >> 10) & 1023u), float((p >> 20) & 1023u)) /
                     1023.0;
}
