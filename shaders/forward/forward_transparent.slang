import "../materials/standard/lobe_transmission";
import "../materials/standard/standard_base";
// shaders/forward/forward_transparent.slang: the forward transparent pass (SPEC §12.5 step 6;
// DENG-305). Engine shader `forward_transparent`; src/render/passes/forward/forward_transparent.c
// makes one DE_PIPE_GRAPHICS pipeline of vs_forward and ps_forward (pre-warmed at renderer
// creation): the view's RGBA16F HDR lighting target with DE_BLEND_PREMUL, the view's D32 reverse-Z
// depth tested (GREATER_EQUAL) and not written, no hardware cull (the vertex shader culls).
//
// One instanced draw per listed instance and side (forward.h has the list and its order):
// instance `i` of the draw is meshlet meshlet_first + i of LOD 0, vertex `v` is corner v % 3 of
// triangle slot v / 3 (DE_VIS_MESHLET_MAX_TRIANGLES slots per meshlet). The vertex shader drops a
// triangle (all three corners at the same point outside the clip volume) past the meshlet's
// count, of a submesh whose material is not forward (forward_routing.slang: those are in the vis
// buffer), or facing the other way than the draw's side: DE_FWD_SIDE_FRONT keeps front faces of
// every forward material, DE_FWD_SIDE_BACK back faces of double-sided ones (drawn first, so a
// closed double-sided surface composites its far side under its near side). Front is the
// engine's winding rule in world space: the triangle's geometric normal (object winding, flipped
// by a mirroring transform) towards the eye.
//
// The pixel shader resolves the surface like the material resolve (VertexAttribs from the
// interpolated attributes, hardware UV derivatives, the material's model) and
// shades it like deferred lighting: evaluate_direct_clustered over the cluster of the fragment's
// own view depth, plus evaluate_indirect_split (IBL) and the emission, times pre_exposure. Coverage
// alpha = base colour alpha x vertex colour alpha x base-colour texture alpha; the output is
// premultiplied. A transmissive surface (KHR transmission) also passes the background through:
// current opaque HDR Gaussian pyramid along a bounded two-interface volume proxy and opaque
// depth trace (DENG-342), with raw incident probe/sky fallback, times transmission x (1 - metallic)
// x base colour x (1 - Fresnel) x Beer over travelled world distance. r.refraction=0 preserves
// the historical same-pixel/nominal-thickness path. Unlit (model 1) blends its emission only. Game
// models shade as Standard (as deferred lighting does).
import bindless;
import push_constants;
import "../scene/scene_types";
import "../visbuffer/vis_types";
import "../visbuffer/vis_decode";
import "../visbuffer/resolve_model";
import "../lighting/lighting_common";
import "../materials/standard/standard";
import "../materials/standard/volume";
import "../materials/unlit/unlit";
import "../sprite/sprite_vis";
import "../forward/forward_routing";
import ss_refraction;
import scene_color_pyramid;
import "../rt/rt_refraction_types";
import "../rt/light_sampling";

/// ForwardParams.side
public static const uint DE_FWD_SIDE_FRONT = 0;
public static const uint DE_FWD_SIDE_BACK = 1;
/// Vertices per meshlet instance of a draw (3 x DE_VIS_MESHLET_MAX_TRIANGLES).
public static const uint DE_FWD_VERTICES = 372;
/// Bytes per pixel of the scene-colour copy (RGBA16F).
public static const uint DE_FWD_SCENE_COLOR_BPP = 8;

/// Push constants of the vertex / pixel pair (112 bytes; forward.h de__fwd_params).
public struct ForwardParams
{
    public uint view;          // DeLightingView ring (de_robuffer)
    public uint entry;         // this frame's entry
    public uint scene;         // DeSceneConstants table
    public uint view_slot;
    public uint instance;      // the instance slot drawn
    public uint meshlet_first; // LOD 0's first meshlet
    public uint side;          // DE_FWD_SIDE_*
    public uint sampler;       // the material sampler (linear, wrap)
    public uint scene_color;   // raw RGBA16F rows of the lighting target before the pass (0: none)
    public uint scene_color_pitch; // its row pitch in bytes
    public uint width;         // the view's internal size
    public uint height;
    public uint scene_depth;
    public uint scene_depth_pitch;
    public uint scene_color_levels;
    public uint refraction;
    public DeClusterLists cl;
    public uint rt_capture;
    public uint rt_result;
    public uint rt_path;
    public uint rt_reserved;
};

public struct FwdVertex
{
    public float4 pos : SV_Position;
    public float3 world : WORLDPOS;
    public float3 normal : NORMAL;
    public float4 tangent : TANGENT;
    public float4 color : COLOR;
    public float2 uv0 : TEXCOORD0;
    public float2 uv1 : TEXCOORD1;
    public nointerpolation float3 geometric : GEONORMAL;
    public nointerpolation uint material : MATERIAL;
    public nointerpolation float object_scale : OBJSCALE;
    public nointerpolation uint has_tangent : HASTANGENT;
};

/// An object-space normal to world space (the inverse transpose of the 3x3, up to scale).
float3 fwd_normal_to_world(float4 rows[3], float3 n)
{
    float3 r0 = rows[0].xyz, r1 = rows[1].xyz, r2 = rows[2].xyz;
    float3 c0 = cross(r1, r2), c1 = cross(r2, r0), c2 = cross(r0, r1);
    return normalize(float3(dot(c0, n), dot(c1, n), dot(c2, n)));
}

/// True for an orthographic projection (its last row is (0, 0, 0, 1)).
bool fwd_ortho(DeSceneConstants sc)
{
    return sc.proj[3].w != 0.0;
}

[shader("vertex")]
FwdVertex vs_forward(uint vid : SV_VertexID, uint iid : SV_InstanceID, uniform ForwardParams pc)
{
    FwdVertex o;
    o.pos = float4(2.0, 2.0, 2.0, 1.0); // dropped: outside the clip volume, zero area
    o.world = float3(0.0, 0.0, 0.0);
    o.normal = float3(0.0, 0.0, 1.0);
    o.tangent = float4(1.0, 0.0, 0.0, 1.0);
    o.color = float4(1.0, 1.0, 1.0, 1.0);
    o.uv0 = float2(0.0, 0.0);
    o.uv1 = float2(0.0, 0.0);
    o.geometric = float3(0.0, 0.0, 1.0);
    o.material = DE_GPU_NONE;
    o.object_scale = 1.0;
    o.has_tangent = 0u;
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    if (pc.instance >= sc.instance_count)
        return o;
    DeGpuInstance inst = de_robuffer<DeGpuInstance>(sc.instances)[pc.instance];
    if (inst.mesh == DE_GPU_NONE || inst.mesh >= sc.mesh_count)
        return o;
    DeGpuMesh mesh = de_robuffer<DeGpuMesh>(sc.meshes)[inst.mesh];
    uint meshlet = pc.meshlet_first + iid;
    if (!de_vis_mesh_drawable(mesh) || meshlet >= mesh.meshlet_count)
        return o;
    DeVisMeshletInfo m = de_vis_meshlet_info(mesh, meshlet);
    uint tri = vid / 3u;
    uint corner = vid - tri * 3u;
    if (tri >= m.triangle_count)
        return o;
    uint slot = de_vis_instance_material(inst, m.submesh);
    if (slot == DE_GPU_NONE || slot >= sc.material_count)
        return o;
    DeGpuMaterial mat = de_robuffer<DeGpuMaterial>(sc.materials)[slot];
    if (!de_material_forward(mat))
        return o;
    bool double_sided = (mat.flags & DE_GPU_MATERIAL_DOUBLE_SIDED) != 0u;
    if (pc.side == DE_FWD_SIDE_BACK && !double_sided)
        return o;
    uint3 lv = de_vis_meshlet_triangle(mesh, m, tri);
    uint3 vi = uint3(de_vis_meshlet_vertex(mesh, m, lv.x), de_vis_meshlet_vertex(mesh, m, lv.y),
                     de_vis_meshlet_vertex(mesh, m, lv.z));
    float3 w0 = de_vis_transform_point(inst.transform, de_vis_mesh_position(mesh, vi.x));
    float3 w1 = de_vis_transform_point(inst.transform, de_vis_mesh_position(mesh, vi.y));
    float3 w2 = de_vis_transform_point(inst.transform, de_vis_mesh_position(mesh, vi.z));
    float3 c = cross(w1 - w0, w2 - w0);
    if (!(dot(c, c) > 0.0))
        return o;
    float det = de_vis_transform_det(inst.transform);
    float3 gn = normalize(c) * (det < 0.0 ? -1.0 : 1.0);
    float3 to_eye = fwd_ortho(sc) ? sc.view[2].xyz : sc.camera_position.xyz - w0;
    bool front = dot(gn, to_eye) > 0.0;
    if (front != (pc.side == DE_FWD_SIDE_FRONT))
        return o;
    uint vtx = corner == 0u ? vi.x : (corner == 1u ? vi.y : vi.z);
    float3 wp = corner == 0u ? w0 : (corner == 1u ? w1 : w2);
    o.pos = de_vis_clip(sc.view_proj, wp);
    o.world = wp;
    o.normal = de_vis_mesh_has_stream(mesh, DE_VIS_STREAM_NORMAL)
                   ? fwd_normal_to_world(inst.transform, de_vis_mesh_normal(mesh, vtx))
                   : gn;
    float4 t = de_vis_mesh_tangent(mesh, vtx);
    float3 tw = float3(dot(inst.transform[0].xyz, t.xyz), dot(inst.transform[1].xyz, t.xyz),
                       dot(inst.transform[2].xyz, t.xyz));
    o.tangent = float4(tw, t.w * (det < 0.0 ? -1.0 : 1.0));
    o.color = de_vis_mesh_color0(mesh, vtx);
    o.uv0 = de_vis_mesh_uv0(mesh, vtx);
    o.uv1 = de_vis_mesh_uv1(mesh, vtx);
    o.geometric = gn;
    o.material = slot;
    o.object_scale = pow(abs(det), 1.0 / 3.0);
    o.has_tangent = de_vis_mesh_has_stream(mesh, DE_VIS_STREAM_TANGENT) ? 1u : 0u;
    return o;
}

void fwd_resolve_surface(FwdVertex i,ForwardParams pc,DeSceneConstants sc,out DeGpuMaterial m,out VertexAttribs v,out TextureLodInfo lod,out float curvature,out float alpha) {
    m = de_resolve_material_of(i.material, sc);
    v.position = i.world;
    float nl = dot(i.normal, i.normal);
    v.normal = nl > 0.0 ? i.normal * rsqrt(nl) : i.geometric;
    float tl = dot(i.tangent.xyz, i.tangent.xyz);
    v.tangent = float4(tl > 0.0 ? i.tangent.xyz * rsqrt(tl) : float3(1.0, 0.0, 0.0),
                       i.tangent.w < 0.0 ? -1.0 : 1.0);
    v.geometric_normal = i.geometric;
    float3 base_normal = v.normal;
    curvature = refraction_curvature(ddx(i.world), ddy(i.world),
                                           ddx(base_normal), ddy(base_normal));
    float3 to_eye = fwd_ortho(sc) ? sc.view[2].xyz : sc.camera_position.xyz - i.world;
    float el = dot(to_eye, to_eye);
    v.view = el > 0.0 ? to_eye * rsqrt(el) : v.normal;
    v.color = i.color;
    v.uv[0] = i.uv0;
    v.uv[1] = i.uv1;
    v.has_tangent = i.has_tangent != 0u;
    v.object_scale = i.object_scale;
    lod.dx[0] = ddx(i.uv0);
    lod.dy[0] = ddy(i.uv0);
    lod.dx[1] = ddx(i.uv1);
    lod.dy[1] = ddy(i.uv1);
    lod.sampler = pc.sampler;
    lod.feedback = 0u;
    lod.feedback_capacity = 0u;
    lod.record = false;
    float4 white = float4(1.0, 1.0, 1.0, 1.0);
    VertexAttribs alpha_v = v;
    alpha_v.uv[0] = de_sprite_uv(m, v.uv[0]);
    alpha = saturate(m.base_color.w * i.color.w *
                           de_resolve_sample(m, DE_RESOLVE_TEX_BASE_COLOR, alpha_v, lod, white).w);
}
[shader("fragment")]
float4 ps_capture_refraction(FwdVertex i,uniform ForwardParams pc):SV_Target {
    DeSceneConstants sc=de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    DeGpuMaterial m;VertexAttribs v;TextureLodInfo lod;float curvature,alpha;
    fwd_resolve_surface(i,pc,sc,m,v,lod,curvature,alpha);
    SurfaceData s=StandardModel.resolve_gbuffer(StandardModel.load_params(m),v,lod);
    float t=(s.features&DE_MATERIAL_FEATURE_TRANSMISSION)!=0?saturate(s.transmission)*(1-saturate(s.metallic)):0;
    if(t>0&&alpha>0&&pc.rt_capture!=0) {
        if(s.thickness>0&&dot(v.normal,v.view)<0){m.flags&=~DE_GPU_MATERIAL_DOUBLE_SIDED;s=StandardModel.resolve_gbuffer(StandardModel.load_params(m),v,lod);}
        DeRefractionCapture c=(DeRefractionCapture)0;c.position_thickness=float4(i.world,s.thickness);c.view_ior=float4(v.view,s.ior);c.normal_roughness=float4(s.normal,s.roughness);c.geometry_curvature=float4(v.normal,curvature);
        de_buffer<DeRefractionCapture>(pc.rt_capture)[uint(i.pos.y)*pc.width+uint(i.pos.x)]=c;
    }
    // Premultiplied zero keeps the actual lighting target unchanged during capture.
    return float4(0);
}

[shader("fragment")]
float4 ps_forward(FwdVertex i, uniform ForwardParams pc) : SV_Target
{
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    DeLightingView lv = de_robuffer<DeLightingView>(pc.view)[pc.entry];
    // New contact masks describe the opaque G-buffer surface, not this forward receiver.
    // Keep the legacy generic outgoing input contract separate.
    if ((lv.inputs.reserved0 & DE_INPUT_CONTACT_METADATA) != 0u) {
        lv.inputs.contact_shadows = 0u;
        lv.inputs.reserved0 &= ~DE_INPUT_CONTACT_METADATA;
    }
    // SSR is weighted for this pixel's opaque G-buffer surface, not the transparent material.
    if ((lv.inputs.reserved0 & DE_INPUT_SPECULAR_OPAQUE) != 0u)
        lv.inputs.indirect_specular = 0u;
    if ((lv.inputs.reserved0 & DE_INPUT_DIFFUSE_OPAQUE) != 0u)
        lv.inputs.indirect_diffuse = 0u;
    if ((lv.inputs.reserved0 & DE_INPUT_AO_GTAO) != 0u) {
        lv.inputs.ao = 0u;
        lv.inputs.reserved0 &= ~DE_INPUT_AO_GTAO;
    }
    // DENG-45 RT screen masks belong to the opaque receiver; keep raster for forward.
    lv.inputs.rt_shadow_visibility=0u;lv.inputs.rt_shadow_keys=0u;
    lv.inputs.restir_diffuse=0u;lv.inputs.restir_specular=0u;
    uint2 px = uint2(i.pos.xy);
    DeGpuMaterial m;VertexAttribs v;TextureLodInfo lod;float curvature,alpha;
    fwd_resolve_surface(i,pc,sc,m,v,lod,curvature,alpha);
    float3 base_normal=v.normal;
    float e = lv.pre_exposure;
    if ((m.model & (DE_GPU_MATERIAL_MODELS - 1u)) == DE_GPU_MATERIAL_MODEL_UNLIT)
    {
        SurfaceData u = UnlitModel.resolve_gbuffer(UnlitModel.load_params(m), v, lod);
        return float4(u.emissive * e * alpha, alpha);
    }
    SurfaceData s = StandardModel.resolve_gbuffer(StandardModel.load_params(m), v, lod);
    float t = (s.features & DE_MATERIAL_FEATURE_TRANSMISSION) != 0u
                  ? saturate(s.transmission) * (1.0 - saturate(s.metallic))
                  : 0.0;
    // Only an effectively transmitting volume backface represents the glass-to-air
    // interface. Resolve textures/metallicity first so opaque double-sided shading is unchanged.
    if (pc.refraction != 0 && t > 0 && s.thickness > 0 && dot(base_normal, v.view) < 0)
    {
        m.flags &= ~DE_GPU_MATERIAL_DOUBLE_SIDED;
        s = StandardModel.resolve_gbuffer(StandardModel.load_params(m), v, lod);
    }
    s.brdf_lut = lv.brdf_lut;
    bool identity = pc.refraction != 0 && t > 0 && abs(s.ior - 1) < 1e-3;
    // With equal indices there is no dielectric interface, even at grazing incidence.
    // Keep this specialization local to transmissive materials; opaque shading is unchanged.
    if (identity)
        s.specular = 0;
    float view_depth = -dot(sc.view[2], float4(i.world, 1.0));
    uint cluster = de_cluster_index(lv, px, view_depth);
    DeDirect d = evaluate_direct_clustered<StandardModel>(s, i.world, v.view, px, lv, pc.cl,
                                                          cluster, pc.refraction != 0 && t > 0);
    float3 ind_d, ind_s;
    evaluate_indirect_split(s, i.world, v.view, px, lv, ind_d, ind_s);
    // The transmitted part takes the reflected diffuse's place (as the BSDF's keep).
    float3 lit = d.diffuse + d.specular + ind_d * (1.0 - t) + ind_s + s.emissive;
    float3 rgb = lit * e;
    if (t > 0.0 && pc.refraction == 0 && pc.scene_color != DE_BINDLESS_INVALID) {
        float nov = saturate(dot(s.normal, v.view)), r0 = (s.ior - 1) / (s.ior + 1);
        float3 f0 = min(r0 * r0 * s.specular_color, float3(1)) * s.specular;
        float3 f = f0 + (float3(s.specular) - f0) * pow(1 - nov, 5);
        float3 through =
            t * s.base_color * (float3(1) - saturate(f)) *
            volume_transmittance(s.attenuation_color, s.attenuation_distance, s.thickness);
        rgb += scene_color_load(pc.scene_color, uint4(pc.width, pc.height, pc.scene_color_pitch, 0),
                                int2(px))
                   .rgb *
               through;
    } else if (t > 0.0 && pc.refraction != 0) {
        RefractionScene scene;
        scene.color = pc.scene_color;
        scene.depth = pc.scene_depth;
        scene.width = pc.width;
        scene.height = pc.height;
        scene.depth_pitch = pc.scene_depth_pitch;
        scene.levels = pc.scene_color_levels;
        scene.reserved0 = 0;
        scene.reserved1 = 0;
        RefractionResult ray;
        bool rt=pc.rt_result!=0&&de_robuffer<float4>(pc.rt_result)[px.y*pc.width+px.x].w>0;
        if(rt) {
            float4 path=de_robuffer<float4>(pc.rt_path)[px.y*pc.width+px.x];
            ray=(RefractionResult)0;ray.radiance=de_robuffer<float4>(pc.rt_result)[px.y*pc.width+px.x].rgb;
            ray.distance=path.x;ray.tir=path.y!=0;ray.confidence=1;
        } else ray=ss_refract(scene, sc, lv, cluster, px, i.world, v.view, s.normal,
                                          base_normal, curvature, s.thickness, s.ior, s.roughness);
        if (ray.tir) {
            // All energy in the transmitting dielectric lobe is reflected at TIR. Raw incident
            // radiance is already pre-exposed; replace its Schlick-weighted IBL contribution.
            if (dot(v.view, base_normal) < 0)
                rgb = (d.specular + s.emissive) * e + ray.radiance;
            else
                // A proxy's exit-interface TIR replaces only the transmitting portion; retain
                // the independently shaded opaque mixture at its actual entry interface.
                rgb += t * (ray.radiance - ind_s * e);
        } else {
            float nov = saturate(abs(dot(s.normal, v.view)));
            float r0 = (s.ior - 1.0) / (s.ior + 1.0);
            float3 f0 = min(r0 * r0 * s.specular_color, float3(1.0, 1.0, 1.0)) * s.specular;
            float k = pow(1.0 - nov, 5.0);
            float3 f = identity ? float3(0) : f0 + (float3(s.specular) - f0) * k;
            float3 through =
                t * s.base_color * (float3(1) - saturate(f)) *
                volume_transmittance(s.attenuation_color, s.attenuation_distance, ray.distance);
            if(rt&&s.roughness>=.05&&dot(v.view,s.normal)>0) {
                ShadingFrame frame=de_frame_from_normal(s.normal);float3 wo=frame.to_local(v.view);
                float3 wi=frame.to_local(de_robuffer<DeRefractionCapture>(pc.rt_capture)[px.y*pc.width+px.x].sampled.xyz);
                TransmissionLobes transmission=de_transmission_lobes(s,de_standard_base_t<DE_STANDARD_FEATURES_ALL>(s,wo,true),wo,true,false);
                float pdf=de_transmission_specular_pdf(transmission,wo,wi);
                through=(pdf>0?StandardModel.evaluate(s,wo,wi)/pdf:float3(0))*volume_transmittance(s.attenuation_color,s.attenuation_distance,ray.distance);
            }
            rgb += ray.radiance * through;
        }
    }
    return float4(rgb * alpha, alpha);
}
