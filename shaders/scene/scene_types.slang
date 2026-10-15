// shaders/scene/scene_types.slang: the GPU scene's table entries and per-view scene constants
// (SPEC §12.4, §12.1; DENG-177). Every pass that reads the scene imports this module:
//
//     import scene_types;
//     DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
//     DeGpuInstance inst = de_robuffer<DeGpuInstance>(sc.instances)[i];
//
// Between graphs the tables are in DE_ACCESS_SHADER_READ: a pass that reads them declares that
// access and reads through de_robuffer (DENG-410). Each table buffer's stride is its entry size.
//
// The C mirrors are the X-macro tables in src/render/scene/gpu_scene.h. The test
// render_gpu_scene.gpu_scene.layout_match parses this file and checks every struct and constant
// against them (names, types, offsets, sizes): change both together.
//
// Layout rules (so Vulkan std430, Metal and D3D12 structured buffers agree, bindless.slang):
// fields are uint, float, uint4, float4 and the structs below; every 16-byte field sits at a
// multiple of 16 and every struct is a multiple of 16 bytes. No float3, no matrices (their
// layout differs per target): a matrix is four float4 rows (row r = (m[r][0..3])), and a 3x4
// affine transform is three rows; transform a point with dot(row, float4(p, 1)).
//
// Positions are float, relative to the render origin (DENG-2 addendum; DeSceneConstants holds
// the origin). Table entries are indexed by slot; a free slot has flags without ..._LIVE.
module scene_types;

/// "No entry": a mesh or material index that names nothing.
public static const uint DE_GPU_NONE = 0xFFFFFFFF;

/// DeGpuInstance.flags
public static const uint DE_GPU_INSTANCE_LIVE = 1;           // the slot holds an instance
public static const uint DE_GPU_INSTANCE_CAST_SHADOWS = 2;   // DeMeshRenderer.cast_shadows
public static const uint DE_GPU_INSTANCE_VISIBLE_IN_RT = 4;  // DeMeshRenderer.visible_in_rt
public static const uint DE_GPU_INSTANCE_HAS_BOUNDS = 8;     // aabb_* and sphere are valid

/// DeGpuMesh.flags
public static const uint DE_GPU_MESH_READY = 1;    // the mesh is loaded; the fields are valid
public static const uint DE_GPU_MESH_MESHLETS = 2; // it has meshlet buffers

/// DeGpuMaterial.flags
public static const uint DE_GPU_MATERIAL_READY = 1; // the material is loaded
public static const uint DE_GPU_MATERIAL_DOUBLE_SIDED = 2;
public static const uint DE_GPU_MATERIAL_UNLIT = 4;

/// DeGpuMaterial.model: the material model (SPEC §12.6; DENG-237). Built-in ids first (DENG-2
/// Q12), game models (de_material_model_register, S14 DENG-298) after them; at most
/// DE_GPU_MATERIAL_MODELS models, so a tile's model set is one uint (material classification).
public static const uint DE_GPU_MATERIAL_MODEL_STANDARD = 0;
public static const uint DE_GPU_MATERIAL_MODEL_UNLIT = 1;
public static const uint DE_GPU_MATERIAL_MODEL_SPRITE = 2;
public static const uint DE_GPU_MATERIAL_MODEL_TERRAIN = 3;
public static const uint DE_GPU_MATERIAL_MODEL_WATER = 4;
public static const uint DE_GPU_MATERIAL_MODELS = 32;

/// DeGpuLight.flags
public static const uint DE_GPU_LIGHT_LIVE = 1;
public static const uint DE_GPU_LIGHT_CAST_SHADOWS = 2;

/// Materials per instance (DeMeshRenderer.materials).
public static const uint DE_GPU_INSTANCE_MATERIALS = 8;
/// LOD records per mesh (DMSH v2).
public static const uint DE_GPU_MESH_LODS = 8;
/// Texture slots per material (de_cooked_tex_slot; 20 = 5 x uint4, the last one unused).
public static const uint DE_GPU_MATERIAL_TEXTURES = 20;

/// One mesh instance (208 bytes). Slot = the render world's instance slot (DENG-175).
public struct DeGpuInstance
{
    public float4 transform[3];      // model matrix rows, relative to the render origin
    public float4 prev_transform[3]; // the previous frame's (motion vectors)
    public float4 aabb_min;          // world AABB relative to the origin (xyz)
    public float4 aabb_max;
    public float4 sphere;            // bounding sphere of the AABB: center xyz, radius w
    public uint mesh;                // DeGpuMesh slot, DE_GPU_NONE without a mesh
    public uint flags;               // DE_GPU_INSTANCE_*
    public uint layer_mask;
    public uint reserved0;
    public uint entity_lo;           // the entity (picking: vis-buffer id -> instance -> entity)
    public uint entity_hi;
    public uint reserved1;
    public uint reserved2;
    public uint4 materials[2];       // DeGpuMaterial slot per submesh, DE_GPU_NONE = the default
};

/// One LOD of a mesh (32 bytes): de_cooked_mesh_lod.
public struct DeGpuMeshLod
{
    public uint index_first;
    public uint index_count;
    public uint meshlet_first;
    public uint meshlet_count;
    public uint triangle_count;
    public uint vertex_count; // the LOD uses vertices [0, vertex_count)
    public float error;       // object-space error, metres
    public uint reserved;
};

/// One mesh (480 bytes): de_render_mesh (DENG-105). Slot per mesh handle that some instance uses.
public struct DeGpuMesh
{
    public uint vertices;           // bindless buffer indices (de_render_mesh_buffer order)
    public uint meshlets;
    public uint meshlet_vertices;
    public uint meshlet_triangles;
    public uint indices;
    public uint vertex_count;
    public uint index_count;
    public uint meshlet_count;
    public uint submesh_count;
    public uint lod_count;
    public uint flags;              // DE_GPU_MESH_*
    public uint reserved;
    public float4 position_offset;  // dequantization: p = offset + q * scale; w = lod_error
    public float4 position_scale;   // w = max_position_error
    public float4 uv_transform[2];  // per UV set: offset xy, scale zw
    public float4 bounds_min;       // object space (xyz)
    public float4 bounds_max;
    public float4 sphere;           // object space: center xyz, radius w
    public uint4 stream_offset[2];  // byte offset of each de_cooked_stream in `vertices`, NONE
    public uint4 stream_format[2];  // de_cooked_vertex_format, 0 = none
    public DeGpuMeshLod lods[8];
};

/// One material (272 bytes): the provisional packing of de_standard_material (S14 replaces it
/// with material instances, DENG-302, which must keep `model`).
public struct DeGpuMaterial
{
    public float4 base_color;
    public float4 emissive;         // rgb, strength
    public float4 pbr;              // metallic, roughness, normal_scale, alpha_cutoff
    public float4 specular;         // specular_color rgb, specular
    public float4 clearcoat;        // clearcoat, clearcoat_roughness, ior, transmission
    public float4 sheen;            // sheen_color rgb, sheen_roughness
    public float4 volume;           // attenuation_color rgb, attenuation_distance
    public float4 extra;            // thickness, diffuse_transmission, subsurface, anisotropy
    public float4 iridescence;      // iridescence, ior, thickness_min, thickness_max
    public float4 subsurface;       // subsurface_radius rgb, anisotropy_rotation
    public uint alpha_mode;         // 0 opaque, 1 mask, 2 blend
    public uint flags;              // DE_GPU_MATERIAL_*
    public uint texcoord_bits;      // bit s: texture slot s samples UV1
    public uint cooked_flags;       // DE_COOKED_MAT_* (the KHR extensions used)
    public uint4 textures[5];       // sampled bindless index per de_cooked_tex_slot, 0 = none
    public uint model;              // DE_GPU_MATERIAL_MODEL_* (< DE_GPU_MATERIAL_MODELS; DENG-237)
    public uint reserved0;
    public uint reserved1;
    public uint reserved2;
};

/// One light (112 bytes). Slot = the render world's light slot (DENG-175).
public struct DeGpuLight
{
    public float4 transform[3]; // rows, relative to the render origin; the light points along it
    public float4 color;        // linear rgb, intensity
    public float4 params;       // range, spot_inner, spot_outer, source_radius
    public float4 area;         // area_size xy
    public uint kind;           // de_light_kind
    public uint flags;          // DE_GPU_LIGHT_*
    public uint entity_lo;
    public uint entity_hi;
};

/// The scene constants of one view (432 bytes), one entry per view slot.
public struct DeSceneConstants
{
    public float4 view[4];           // render-origin space -> view space (rows)
    public float4 proj[4];
    public float4 view_proj[4];
    public float4 inv_view_proj[4];
    public float4 prev_view_proj[4]; // the previous frame's, rebased onto this frame's origin
    public float4 camera_position;   // relative to the render origin (xyz)
    public float4 origin_high;       // the render origin (f64) as float (xyz) ...
    public float4 origin_low;        // ... and what float lost (origin - high)
    public float4 viewport;          // width, height, 1 / width, 1 / height
    public uint instances;           // bindless indices of the tables
    public uint meshes;
    public uint materials;
    public uint lights;
    public uint instance_count;      // slots in use (high-water mark); free slots are not LIVE
    public uint mesh_count;
    public uint material_count;
    public uint light_count;
    public uint view_slot;
    public uint flags;
    public float pre_exposure;      // DENG-304: radiance written to HDR targets is x this (0 = 1)
    public float prev_pre_exposure; // the previous frame's (temporal passes rescale history by it)
};
