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
import "../visbuffer/gbuffer";

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

// Same values as de_view_render_mode in de_render.h; append only.
struct ViewDebugParams
{
    uint gb0, gb1, gb2, motion, depth, emission, dst, width, height, mode, reserved0, reserved1;
};
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_view_debug(uint3 id : SV_DispatchThreadID, uniform ViewDebugParams pc)
{
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    int3 pixel = int3(id.xy, 0);
    float4 emission = de_tex2d(pc.emission).Load(pixel);
    float3 color = 0.0;
    if (emission.a > 0.0)
    {
        DeGbSurface surface = gb_decode(de_tex2d(pc.gb0).Load(pixel),
                                        de_tex2d(pc.gb1).Load(pixel), de_tex2d(pc.gb2).Load(pixel));
        switch (pc.mode)
        {
        case 1u: color = surface.base_color + emission.rgb; break;
        case 2u: color = surface.base_color; break;
        case 3u: color = surface.normal * 0.5 + 0.5; break;
        case 4u: color = surface.roughness.xxx; break;
        case 5u: color = surface.metallic.xxx; break;
        case 6u: color = surface.ao.xxx; break;
        case 7u: color = emission.rgb; break;
        case 8u: color = float3(0.5 + de_tex2d(pc.motion).Load(pixel).xy * 10.0, 0.5); break;
        case 9u: color = de_tex2d(pc.depth).Load(pixel).rrr; break;
        }
    }
    de_rwtex2d(pc.dst)[id.xy] = float4(saturate(color), 1.0);
}
