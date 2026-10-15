// tests/unit/render/data/transmission_kernels.slang: test_transmission_unlit.c's GPU side
// (DENG-301). Runs the Standard model's transmission lobes, the volume helpers and the Unlit model
// on cases the test wrote. Cooked at test time with the engine's shaders/ on the same root.
//
//   cs_sample   thread (case c, sample k): one StandardModel.sample(): weight, pdf, evaluate /
//               pdf, wi.z
//   cs_quad     thread (case c, grid point g): evaluate, pdf and the diffuse share at a direction
//               of a uniform grid over the sphere (cos(theta) x phi)
//   cs_volume   thread c: volume_transmittance and de_volume_sigma_a
//   cs_resolve  thread c: StandardModel and UnlitModel resolve_gbuffer of a material made from the
//               case (no textures): feature bits, G-buffer flags, emission
import bindless;
import push_constants;
import "brdf/bsdf_common";
import "brdf/sampling";
import "materials/material_interface";
import "materials/standard/volume";
import "materials/standard/lobe_transmission";
import "materials/standard/standard";
import "materials/unlit/unlit";
import "visbuffer/gbuffer";

/// Floats each thread writes.
static const uint TK_OUT = 4;

struct TkParams
{
    uint cases;    // float4 x 5 per case (test_transmission_unlit.c sk_case)
    uint out_;     // TK_OUT floats per thread
    uint count;    // cases
    uint lut;      // the energy LUT (sampled bindless index)
    uint per_case; // threads per case
    uint grid_x;   // cs_quad: phi steps
    uint first;    // the first thread of this dispatch
    uint reserved;
};

/// The case's surface: p0 = (roughness, metallic, base, mu_o), p1 = (transmission, thickness,
/// ior, diffuse transmission), p2 = (attenuation colour, attenuation distance), p3 =
/// (diffuse transmission colour, -), p4 = (wo, 1 when wo is given; else from mu_o). The features
/// come from the inputs, as the resolve sets them.
SurfaceData case_surface(float4 p0, float4 p1, float4 p2, float4 p3, uint lut)
{
    SurfaceData s = de_surface_default();
    s.roughness = p0.x;
    s.metallic = p0.y;
    s.base_color = float3(p0.z, p0.z, p0.z);
    s.tangent = float4(1.0, 0.0, 0.0, 1.0);
    s.brdf_lut = lut;
    s.transmission = p1.x;
    s.thickness = p1.y;
    s.ior = p1.z;
    s.diffuse_transmission = p1.w;
    s.attenuation_color = p2.xyz;
    s.attenuation_distance = p2.w;
    s.diffuse_transmission_color = p3.xyz;
    de_transmission_set_features(s);
    return s;
}

void load_case(TkParams pc, uint c, out float4 p0, out float4 p1, out float4 p2, out float4 p3,
               out float4 p4)
{
    p0 = de_robuffer<float4>(pc.cases)[c * 5u + 0u];
    p1 = de_robuffer<float4>(pc.cases)[c * 5u + 1u];
    p2 = de_robuffer<float4>(pc.cases)[c * 5u + 2u];
    p3 = de_robuffer<float4>(pc.cases)[c * 5u + 3u];
    p4 = de_robuffer<float4>(pc.cases)[c * 5u + 4u];
}

float3 case_wo(float4 p0, float4 p4)
{
    return p4.w > 0.5 ? normalize(p4.xyz) : float3(sqrt(1.0 - p0.w * p0.w), 0.0, p0.w);
}

void write_out(TkParams pc, uint t, float4 v)
{
    de_buffer<float>(pc.out_)[t * TK_OUT + 0u] = v.x;
    de_buffer<float>(pc.out_)[t * TK_OUT + 1u] = v.y;
    de_buffer<float>(pc.out_)[t * TK_OUT + 2u] = v.z;
    de_buffer<float>(pc.out_)[t * TK_OUT + 3u] = v.w;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_sample(uint3 id : SV_DispatchThreadID, uniform TkParams pc)
{
    uint t = id.x + pc.first, c = t / pc.per_case, k = t % pc.per_case;
    if (c >= pc.count)
        return;
    float4 p0, p1, p2, p3, p4;
    load_case(pc, c, p0, p1, p2, p3, p4);
    SurfaceData s = case_surface(p0, p1, p2, p3, pc.lut);
    float3 wo = case_wo(p0, p4);
    float2 uv = de_hammersley(k, pc.per_case);
    float3 u = float3(uv, frac(0.5 + float(k) * 0.6180339887));
    BsdfSample r = StandardModel.sample(s, wo, u);
    float pdf = StandardModel.pdf(s, wo, r.wi);
    float e = pdf > 0.0 ? StandardModel.evaluate(s, wo, r.wi).x / pdf : 0.0;
    write_out(pc, t, float4(r.weight.x, r.pdf, r.pdf > 0.0 ? e : 0.0, r.pdf > 0.0 ? r.wi.z : 0.0));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_quad(uint3 id : SV_DispatchThreadID, uniform TkParams pc)
{
    uint t = id.x + pc.first, c = t / pc.per_case, g = t % pc.per_case;
    if (c >= pc.count)
        return;
    float4 p0, p1, p2, p3, p4;
    load_case(pc, c, p0, p1, p2, p3, p4);
    SurfaceData s = case_surface(p0, p1, p2, p3, pc.lut);
    float3 wo = case_wo(p0, p4);
    uint grid_y = pc.per_case / pc.grid_x;
    float mu = 2.0 * (float(g / pc.grid_x) + 0.5) / float(grid_y) - 1.0; // the whole sphere
    float phi = 2.0 * DE_PI * (float(g % pc.grid_x) + 0.5) / float(pc.grid_x);
    float st = sqrt(max(0.0, 1.0 - mu * mu));
    float3 wi = float3(st * cos(phi), st * sin(phi), mu);
    float3 diffuse;
    float3 f = StandardModel.evaluate_split(s, wo, wi, diffuse);
    write_out(pc, t, float4(f.x, StandardModel.pdf(s, wo, wi), diffuse.x, mu));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_volume(uint3 id : SV_DispatchThreadID, uniform TkParams pc)
{
    uint c = id.x;
    if (c >= pc.count)
        return;
    float4 p0, p1, p2, p3, p4;
    load_case(pc, c, p0, p1, p2, p3, p4);
    // p2: attenuation colour and distance, p0.x: the path length.
    write_out(pc, c * 2u, float4(volume_transmittance(p2.xyz, p2.w, p0.x), 0.0));
    write_out(pc, c * 2u + 1u, float4(de_volume_sigma_a(p2.xyz, p2.w), 0.0));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_resolve(uint3 id : SV_DispatchThreadID, uniform TkParams pc)
{
    uint c = id.x;
    if (c >= pc.count)
        return;
    float4 p0, p1, p2, p3, p4;
    load_case(pc, c, p0, p1, p2, p3, p4);
    DeGpuMaterial m = {};
    m.base_color = float4(p3.xyz, 1.0);                // (the case's base colour, per channel)
    m.emissive = float4(0.25, 0.5, 0.75, 2.0);         // ignored by Unlit
    m.pbr = float4(p0.y, p0.x, 1.0, 0.5);
    m.specular = float4(1.0, 1.0, 1.0, 1.0);
    m.clearcoat = float4(0.0, 0.0, p1.z, p1.x);        // ior, transmission
    m.volume = p2;                                     // attenuation colour, distance
    m.extra = float4(p1.y, p1.w, 0.0, 0.0);            // thickness, diffuse transmission
    m.flags = DE_GPU_MATERIAL_READY;
    m.model = p4.w > 0.5 ? DE_GPU_MATERIAL_MODEL_UNLIT : DE_GPU_MATERIAL_MODEL_STANDARD;
    VertexAttribs v;
    v.position = float3(0.0, 0.0, 0.0);
    v.normal = float3(0.0, 0.0, 1.0);
    v.tangent = float4(1.0, 0.0, 0.0, 1.0);
    v.geometric_normal = float3(0.0, 0.0, 1.0);
    v.view = float3(0.0, 0.0, 1.0);
    v.color = float4(p4.xyz, 1.0);                     // the vertex colour
    v.uv[0] = float2(0.0, 0.0);
    v.uv[1] = float2(0.0, 0.0);
    v.has_tangent = true;
    v.object_scale = p0.z;                             // the instance's scale
    TextureLodInfo lod;
    lod.dx[0] = float2(0.0, 0.0);
    lod.dx[1] = float2(0.0, 0.0);
    lod.dy[0] = float2(0.0, 0.0);
    lod.dy[1] = float2(0.0, 0.0);
    lod.sampler = 0u;
    lod.feedback = 0u;
    lod.feedback_capacity = 0u;
    lod.record = false;
    SurfaceData s;
    float3 wo = float3(0.6, 0.0, 0.8), wi = float3(-0.3, 0.4, 0.866);
    float ev, pdf;
    uint lobes;
    if (p4.w > 0.5)
    {
        s = UnlitModel.resolve_gbuffer(UnlitModel.load_params(m), v, lod);
        ev = UnlitModel.evaluate(s, wo, wi).x + UnlitModel.evaluate(s, wo, -wi).x;
        pdf = UnlitModel.pdf(s, wo, wi) + UnlitModel.sample(s, wo, float3(0.3, 0.6, 0.2)).pdf;
        lobes = UnlitModel.lobes(s);
    }
    else
    {
        s = StandardModel.resolve_gbuffer(StandardModel.load_params(m), v, lod);
        ev = StandardModel.evaluate(s, wo, wi).x;
        pdf = StandardModel.pdf(s, wo, wi);
        lobes = StandardModel.lobes(s);
    }
    DeGbSurface g = de_surface_to_gb(s);
    write_out(pc, c * 3u, float4(s.emissive, s.thickness));
    write_out(pc, c * 3u + 1u, float4(float(s.features), float(g.flags), float(lobes), ev));
    write_out(pc, c * 3u + 2u, float4(pdf, s.transmission, s.diffuse_transmission, s.ior));
}
