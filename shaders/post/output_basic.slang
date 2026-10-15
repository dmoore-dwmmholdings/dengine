// shaders/post/output_basic.slang: the basic output pass (SPEC §12.7; DENG-304). Engine shader
// `post_output_basic`; src/render/passes/output_basic.c runs it per view, where the S18 tonemapper
// stage (DENG-163) will go:
//
//   cs_output_basic   8 x 8 threads per group: the pre-exposed linear HDR lighting target,
//                     clamped to [0, 1] (the equivalent of DE_TM_NONE), into the view's colour.
//                     The swapchains are sRGB formats on every backend, so the hardware applies
//                     the sRGB OETF when the view is composited; with `encode` the kernel applies
//                     it itself, for a target that does not.
import bindless;
import push_constants;

/// Push constants (32 bytes; output_basic.c ob_params).
struct OutputParams
{
    uint src;    // the HDR lighting target (sampled)
    uint dst;    // the output (storage, RGBA16F)
    uint width;
    uint height;
    uint encode; // 1: apply the sRGB OETF
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

/// The sRGB OETF (IEC 61966-2-1) of a linear value in [0, 1].
public float de_srgb_oetf(float c)
{
    return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_output_basic(uint3 id : SV_DispatchThreadID, uniform OutputParams pc)
{
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    float3 c = saturate(de_tex2d(pc.src).Load(int3(int(id.x), int(id.y), 0)).rgb);
    if (pc.encode != 0u)
        c = float3(de_srgb_oetf(c.r), de_srgb_oetf(c.g), de_srgb_oetf(c.b));
    de_rwtex2d(pc.dst)[id.xy] = float4(c, 1.0);
}
