// tests/unit/render/data/standard_layers_kernels.slang: test_standard_layers.c's GPU side
// (DENG-300). Runs the Standard model with its layers on cases the test wrote. Cooked at test time
// with the engine's shaders/ on the same root.
//
//   cs_sample  thread (case c, sample k): one StandardModel.sample(): weight, pdf, evaluate / pdf
//   cs_quad    thread (case c, grid point g): evaluate and pdf at a direction of a uniform grid
//              over the sphere (cos(theta) x phi)
//   cs_film    thread c: the thin-film Fresnel at normal incidence and the base Fresnel with the
//              case's layers (iridescence identity and hue)
import bindless;
import push_constants;
import "brdf/bsdf_common";
import "brdf/sampling";
import "brdf/thin_film";
import "materials/material_interface";
import "materials/standard/standard_base";
import "materials/standard/standard";

/// Floats each thread writes.
static const uint LK_OUT = 4;

struct LkParams
{
    uint cases;    // float4 x 5 per case (test_standard_layers.c sk_case)
    uint out_;     // LK_OUT floats per thread
    uint count;    // cases
    uint lut;      // the energy LUT (sampled bindless index)
    uint per_case; // threads per case
    uint grid_x;   // cs_quad: phi steps
    uint first;    // the first thread of this dispatch
    uint reserved;
};

/// The case's surface: p0 = (roughness, metallic, base, mu_o), p1 = (clearcoat, clearcoat
/// roughness, sheen roughness, anisotropy), p2 = (sheen colour, -), p3 = (iridescence,
/// iridescence ior, thickness nm, features), p4 = (anisotropy direction, -, -). Normal +Z,
/// tangent +X.
SurfaceData case_surface(float4 p0, float4 p1, float4 p2, float4 p3, float4 p4, uint lut)
{
    SurfaceData s = de_surface_default();
    s.roughness = p0.x;
    s.metallic = p0.y;
    s.base_color = float3(p0.z, p0.z, p0.z);
    s.tangent = float4(1.0, 0.0, 0.0, 1.0);
    s.brdf_lut = lut;
    s.clearcoat = p1.x;
    s.clearcoat_roughness = p1.y;
    s.clearcoat_normal = s.normal;
    s.sheen_roughness = p1.z;
    s.anisotropy = p1.w;
    s.sheen_color = p2.xyz;
    s.anisotropy_direction = p4.xy;
    s.iridescence = p3.x;
    s.iridescence_ior = p3.y;
    s.iridescence_thickness = p3.z;
    s.features = uint(p3.w);
    return s;
}

void load_case(LkParams pc, uint c, out float4 p0, out float4 p1, out float4 p2, out float4 p3,
               out float4 p4)
{
    p0 = de_robuffer<float4>(pc.cases)[c * 5u + 0u];
    p1 = de_robuffer<float4>(pc.cases)[c * 5u + 1u];
    p2 = de_robuffer<float4>(pc.cases)[c * 5u + 2u];
    p3 = de_robuffer<float4>(pc.cases)[c * 5u + 3u];
    p4 = de_robuffer<float4>(pc.cases)[c * 5u + 4u];
}

void write_out(LkParams pc, uint t, float4 v)
{
    de_buffer<float>(pc.out_)[t * LK_OUT + 0u] = v.x;
    de_buffer<float>(pc.out_)[t * LK_OUT + 1u] = v.y;
    de_buffer<float>(pc.out_)[t * LK_OUT + 2u] = v.z;
    de_buffer<float>(pc.out_)[t * LK_OUT + 3u] = v.w;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_sample(uint3 id : SV_DispatchThreadID, uniform LkParams pc)
{
    uint t = id.x + pc.first, c = t / pc.per_case, k = t % pc.per_case;
    if (c >= pc.count)
        return;
    float4 p0, p1, p2, p3, p4;
    load_case(pc, c, p0, p1, p2, p3, p4);
    SurfaceData s = case_surface(p0, p1, p2, p3, p4, pc.lut);
    float3 wo = float3(sqrt(1.0 - p0.w * p0.w), 0.0, p0.w);
    float2 uv = de_hammersley(k, pc.per_case);
    float3 u = float3(uv, frac(0.5 + float(k) * 0.6180339887));
    BsdfSample r = StandardModel.sample(s, wo, u);
    float pdf = StandardModel.pdf(s, wo, r.wi);
    float e = pdf > 0.0 ? StandardModel.evaluate(s, wo, r.wi).x / pdf : 0.0;
    write_out(pc, t, float4(r.weight.x, r.pdf, e, pdf));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_quad(uint3 id : SV_DispatchThreadID, uniform LkParams pc)
{
    uint t = id.x + pc.first, c = t / pc.per_case, g = t % pc.per_case;
    if (c >= pc.count)
        return;
    float4 p0, p1, p2, p3, p4;
    load_case(pc, c, p0, p1, p2, p3, p4);
    SurfaceData s = case_surface(p0, p1, p2, p3, p4, pc.lut);
    float3 wo = float3(sqrt(1.0 - p0.w * p0.w), 0.0, p0.w);
    uint grid_y = pc.per_case / pc.grid_x;
    float mu = 2.0 * (float(g / pc.grid_x) + 0.5) / float(grid_y) - 1.0; // the whole sphere
    float phi = 2.0 * DE_PI * (float(g % pc.grid_x) + 0.5) / float(pc.grid_x);
    float st = sqrt(max(0.0, 1.0 - mu * mu));
    float3 wi = float3(st * cos(phi), st * sin(phi), mu);
    float3 diffuse;
    float3 f = StandardModel.evaluate_split(s, wo, wi, diffuse);
    write_out(pc, t, float4(f.x, StandardModel.pdf(s, wo, wi), diffuse.x, f.y));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_film(uint3 id : SV_DispatchThreadID, uniform LkParams pc)
{
    uint c = id.x;
    if (c >= pc.count)
        return;
    float4 p0, p1, p2, p3, p4;
    load_case(pc, c, p0, p1, p2, p3, p4);
    // The film alone at normal incidence on a dielectric of F0 p0.z.
    float3 film = de_thin_film_fresnel(1.0, p3.y, 1.0, p3.z, float3(p0.z, p0.z, p0.z));
    write_out(pc, c * 2u, float4(film, 0.0));
    // The base's Fresnel at cos(theta_d) p0.w with the case's layers.
    SurfaceData s = case_surface(p0, p1, p2, p3, p4, pc.lut);
    StandardBase b = de_standard_base_t<DE_STANDARD_FEATURES_ALL>(s, float3(0.0, 0.0, 1.0), true);
    write_out(pc, c * 2u + 1u,
              float4(de_standard_base_fresnel_t<DE_STANDARD_FEATURES_ALL>(b, p0.w), 0.0));
}
