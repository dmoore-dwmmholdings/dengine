// tests/unit/render/data/standard_kernels.slang: test_standard_base.c's GPU side (DENG-299). Runs
// the Standard model (materials/standard/) on cases the test wrote. Cooked at test time with the
// engine's shaders/ on the same root.
//
//   cs_sample  thread (case c, sample k): one sample() of the case's surface and view, its
//              weight, pdf and evaluate / pdf at the sampled direction (the furnace and the
//              estimator)
//   cs_quad    thread (case c, grid point g): evaluate and pdf at a direction of a uniform grid
//              over the sphere (cos(theta) x phi), for the quadratures
//   cs_f0      thread c: the Fresnel parameters of the case's surface
//   cs_resolve thread c: resolve_gbuffer of a material made from the case (no textures)
import bindless;
import push_constants;
import "brdf/bsdf_common";
import "brdf/sampling";
import "materials/material_interface";
import "materials/standard/standard_base";
import "materials/standard/standard";

/// Floats each thread writes.
static const uint SK_OUT = 4;

struct SkParams
{
    uint cases;    // float4 x 3 per case (test_standard_base.c sk_case)
    uint out_;     // SK_OUT floats per thread
    uint count;    // cases
    uint lut;      // the energy LUT (sampled bindless index) or DE_BINDLESS_INVALID
    uint per_case; // threads per case (samples, or grid points)
    uint grid_x;   // cs_quad: phi steps (grid_y = per_case / grid_x cos(theta) steps)
    uint first;    // the first thread of this dispatch (dispatches have at most 65535 groups)
    uint reserved1;
};

/// The case's surface: p0 = (roughness, metallic, base, specular), p1 = (specular colour, ior);
/// p2 = (mu_o, multiscatter 0/1, -, -).
SurfaceData case_surface(float4 p0, float4 p1, uint lut)
{
    SurfaceData s = de_surface_default();
    s.roughness = p0.x;
    s.metallic = p0.y;
    s.base_color = float3(p0.z, p0.z, p0.z);
    s.specular = p0.w;
    s.specular_color = p1.xyz;
    s.ior = p1.w;
    s.brdf_lut = lut;
    return s;
}

void load_case(SkParams pc, uint c, out float4 p0, out float4 p1, out float4 p2)
{
    p0 = de_robuffer<float4>(pc.cases)[c * 3u + 0u];
    p1 = de_robuffer<float4>(pc.cases)[c * 3u + 1u];
    p2 = de_robuffer<float4>(pc.cases)[c * 3u + 2u];
}

void write_out(SkParams pc, uint t, float4 v)
{
    de_buffer<float>(pc.out_)[t * SK_OUT + 0u] = v.x;
    de_buffer<float>(pc.out_)[t * SK_OUT + 1u] = v.y;
    de_buffer<float>(pc.out_)[t * SK_OUT + 2u] = v.z;
    de_buffer<float>(pc.out_)[t * SK_OUT + 3u] = v.w;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_sample(uint3 id : SV_DispatchThreadID, uniform SkParams pc)
{
    uint t = id.x + pc.first, c = t / pc.per_case, k = t % pc.per_case;
    if (c >= pc.count)
        return;
    float4 p0, p1, p2;
    load_case(pc, c, p0, p1, p2);
    SurfaceData s = case_surface(p0, p1, pc.lut);
    float3 wo = float3(sqrt(1.0 - p2.x * p2.x), 0.0, p2.x);
    // u.xy Hammersley, u.z a Kronecker sequence (golden ratio): all three well stratified.
    float2 uv = de_hammersley(k, pc.per_case);
    float3 u = float3(uv, frac(0.5 + float(k) * 0.6180339887));
    StandardBase b = de_standard_base(s, wo, p2.y > 0.5);
    BsdfSample r = de_standard_base_sample(b, s, wo, u);
    float pdf = de_standard_base_pdf(b, wo, r.wi);
    float e = pdf > 0.0 ? de_standard_base_evaluate(b, s, wo, r.wi).x / pdf : 0.0;
    // The model's own entry points must agree with the base for a base-only surface.
    float m = p2.y > 0.5 ? StandardModel.sample(s, wo, u).weight.x : r.weight.x;
    write_out(pc, t, float4(r.weight.x, r.pdf, e, m));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_quad(uint3 id : SV_DispatchThreadID, uniform SkParams pc)
{
    uint t = id.x + pc.first, c = t / pc.per_case, g = t % pc.per_case;
    if (c >= pc.count)
        return;
    float4 p0, p1, p2;
    load_case(pc, c, p0, p1, p2);
    SurfaceData s = case_surface(p0, p1, pc.lut);
    float3 wo = float3(sqrt(1.0 - p2.x * p2.x), 0.0, p2.x);
    uint grid_y = pc.per_case / pc.grid_x;
    float mu = 2.0 * (float(g / pc.grid_x) + 0.5) / float(grid_y) - 1.0; // the whole sphere
    float phi = 2.0 * DE_PI * (float(g % pc.grid_x) + 0.5) / float(pc.grid_x);
    float st = sqrt(max(0.0, 1.0 - mu * mu));
    float3 wi = float3(st * cos(phi), st * sin(phi), mu);
    StandardBase b = de_standard_base(s, wo, p2.y > 0.5);
    write_out(pc, t, float4(de_standard_base_evaluate(b, s, wo, wi).x,
                            de_standard_base_pdf(b, wo, wi), StandardModel.evaluate(s, wo, wi).x,
                            StandardModel.pdf(s, wo, wi)));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_f0(uint3 id : SV_DispatchThreadID, uniform SkParams pc)
{
    uint c = id.x;
    if (c >= pc.count)
        return;
    float4 p0, p1, p2;
    load_case(pc, c, p0, p1, p2);
    SurfaceData s = case_surface(p0, p1, pc.lut);
    s.base_color = float3(p2.x, p2.y, p2.z); // a coloured base for the metal F0
    StandardFresnel f = de_standard_fresnel(s);
    // Two threads' worth of floats per case: f0_d, f90_d.x; f0, f90.x.
    write_out(pc, c * 2u, float4(f.f0_d, f.f90_d.x));
    write_out(pc, c * 2u + 1u, float4(f.f0, f.f90.x));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_resolve(uint3 id : SV_DispatchThreadID, uniform SkParams pc)
{
    uint c = id.x;
    if (c >= pc.count)
        return;
    float4 p0, p1, p2;
    load_case(pc, c, p0, p1, p2);
    DeGpuMaterial m = {};
    m.base_color = float4(p0.z, p0.z, p0.z, 1.0);
    m.emissive = p2;                                 // rgb, strength
    m.pbr = float4(p0.y, p0.x, 1.0, 0.5);            // metallic, roughness, normal scale, cutoff
    m.specular = float4(p1.xyz, p0.w);               // specular colour, specular
    m.clearcoat = float4(0.0, 0.0, p1.w, 0.0);       // ior
    m.flags = DE_GPU_MATERIAL_READY;
    m.model = DE_GPU_MATERIAL_MODEL_STANDARD;
    VertexAttribs v;
    v.position = float3(0.0, 0.0, 0.0);
    v.normal = float3(0.0, 0.0, 1.0);
    v.tangent = float4(1.0, 0.0, 0.0, 1.0);
    v.geometric_normal = float3(0.0, 0.0, 1.0);
    v.view = float3(0.0, 0.0, 1.0);
    v.color = float4(1.0, 1.0, 1.0, 1.0);
    v.uv[0] = float2(0.0, 0.0);
    v.uv[1] = float2(0.0, 0.0);
    v.has_tangent = true;
    v.object_scale = 1.0;
    TextureLodInfo lod;
    lod.dx[0] = float2(0.0, 0.0);
    lod.dx[1] = float2(0.0, 0.0);
    lod.dy[0] = float2(0.0, 0.0);
    lod.dy[1] = float2(0.0, 0.0);
    lod.sampler = 0u;
    lod.feedback = 0u;
    lod.feedback_capacity = 0u;
    lod.record = false;
    SurfaceData s = StandardModel.resolve_gbuffer(StandardModel.load_params(m), v, lod);
    write_out(pc, c * 3u, float4(s.emissive, s.ior));
    write_out(pc, c * 3u + 1u, float4(s.specular_color, s.specular));
    write_out(pc, c * 3u + 2u, float4(s.base_color.x, s.roughness, s.metallic,
                                      float(s.brdf_lut == DE_BINDLESS_INVALID ? 1u : 0u)));
}
