// shaders/common/math.slang: small math helpers and the engine's clip-space rules (SPEC §5.5,
// §12.5; DENG-289).
//
// Clip space is the same on every backend: x right, y up, z from 1 (near) to 0 (far), reverse-Z
// with an infinite far plane. Shaders write D3D-style clip positions; the Vulkan backend flips Y
// with a negative viewport height, Metal and D3D12 need nothing. Depth tests use GREATER_EQUAL
// and depth clears to 0.
module math;

public static const float DE_PI = 3.14159265358979323846;
public static const float DE_TAU = 6.28318530717958647692;
public static const float DE_INV_PI = 0.31830988618379067154;

/// x * x.
public float de_sq(float x)
{
    return x * x;
}

/// `x` mapped from [a, b] to [c, d] (not clamped).
public float de_remap(float x, float a, float b, float c, float d)
{
    return c + (x - a) * (d - c) / (b - a);
}

/// View-space distance from a reverse-Z depth of an infinite perspective projection with near
/// plane `near` (depth 1 at `near`, 0 at infinity). Depth 0 gives +infinity.
public float de_linear_depth_reverse_z(float depth, float near)
{
    return near / depth;
}

/// The reverse-Z depth of view-space distance `dist` (the inverse of de_linear_depth_reverse_z).
public float de_reverse_z_depth(float dist, float near)
{
    return near / dist;
}

/// Texture coordinates (0,0 top-left) to the clip-space xy of the same point (y up).
public float2 de_uv_to_clip(float2 uv)
{
    return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

/// Clip-space xy (y up) to texture coordinates (0,0 top-left).
public float2 de_clip_to_uv(float2 clip)
{
    return float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
}
