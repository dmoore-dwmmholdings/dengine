// shaders/common/color.slang: color-space helpers (SPEC §12.11; DENG-289). Shading happens in
// linear Rec.709 primaries; sRGB textures and swapchains convert in hardware where the format
// allows, these functions are for the cases where it does not.
module color;

/// One sRGB-encoded channel to linear (IEC 61966-2-1, exact piecewise curve).
public float de_srgb_to_linear(float c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

/// One linear channel to sRGB encoding (IEC 61966-2-1, exact piecewise curve).
public float de_linear_to_srgb(float c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

/// An sRGB-encoded color to linear; alpha is linear already.
public float4 de_srgb_to_linear(float4 c)
{
    return float4(de_srgb_to_linear(c.r), de_srgb_to_linear(c.g), de_srgb_to_linear(c.b), c.a);
}

/// A linear color to sRGB encoding; alpha stays linear.
public float4 de_linear_to_srgb(float4 c)
{
    return float4(de_linear_to_srgb(c.r), de_linear_to_srgb(c.g), de_linear_to_srgb(c.b), c.a);
}

/// Relative luminance of a linear Rec.709 color.
public float de_luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}
