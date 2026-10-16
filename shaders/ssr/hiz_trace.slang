// Shared closest-depth traversal: projected segment UV/reverse-Z, no BRDF or history policy.
// Conservative MAX cells reject a segment only when every point is in front of every surface.
// Potential intersections descend; leaf DDA tests the original full-resolution depth footprint.
module hiz_trace;
import bindless;
public struct DeHiZHit {
  public float2 uv;
  public float depth;
  public float confidence;
};
float de_hiz_boundary(float o, float d, float t, uint cell, float cells) {
    if (abs(d) < 1e-10)
        return 1e20;
    float boundary = float(cell) + (d > 0 ? 1 : 0);
    return max((boundary / cells - o) / d, t + 1e-7);
}
public DeHiZHit de_hiz_trace(uint pyramid, uint depth, uint2 pyramid_size, uint mips, uint2 screen_size,
                      float3 origin, float3 delta, float near_z, float thickness, float depth_scale,
                      uint max_iterations) {
    DeHiZHit hit = (DeHiZHit)0;
    if (any(origin.xy < 0) || any(origin.xy >= 1) || !all(isfinite(origin)) ||
        !all(isfinite(delta)))
        return hit;
    float end = 1;
    for (uint axis = 0; axis < 3; ++axis) {
        if (delta[axis] > 1e-10)
            end = min(end, (1 - origin[axis]) / delta[axis]);
        else if (delta[axis] < -1e-10)
            end = min(end, -origin[axis] / delta[axis]);
    }
    float pixels = max(abs(delta.x) * screen_size.x, abs(delta.y) * screen_size.y);
    if (!(pixels > 1e-5) || !(end > 0))
        return hit;
    float t = min(1.05 / pixels, end); // reject the launching pixel without a world-scale bias
    int level = int(min(mips - 1, 5u));
    for (uint iteration = 0; iteration < max_iterations && t < end; ++iteration) {
        float3 p = origin + delta * t;
        if (any(p.xy < 0) || any(p.xy >= 1))
            break;
        uint2 size = level == 0 ? screen_size : max(pyramid_size >> uint(level), uint2(1, 1));
        // A boundary belongs to the cell the ray enters, for both signs. Use that same cell
        // for sampling and the exit plane. Two representable ULPs work at 4K and beyond;
        // clamp the positive-domain bit step at zero so exiting rays cannot produce NaN.
        float2 owner = asfloat(
            uint2(max(int2(asuint(p.xy * float2(size))) + int2(sign(delta.xy)) * 2, int2(0, 0))));
        uint2 cell = min(uint2(max(floor(owner), float2(0, 0))), size - 1);
        float next = min(end, min(de_hiz_boundary(origin.x, delta.x, t, cell.x, float(size.x)),
                                  de_hiz_boundary(origin.y, delta.y, t, cell.y, float(size.y))));
        float closest = level == 0 ? de_tex2d(depth).Load(int3(cell, 0)).r
                                   : de_tex2d(pyramid).Load(int3(cell, level)).r;
        float z1 = origin.z + delta.z * next;
        if (!(closest > 0) || min(p.z, z1) > closest + 1e-7) {
            t = next;
            level = min(level + 1, int(mips) - 1);
            continue;
        }
        if (level > 0) {
            --level;
            continue;
        }
        float dz = depth_scale > 0 ? thickness * depth_scale
                                   : closest - near_z / (near_z / max(closest, 1e-8) + thickness);
        float lower = closest - max(dz, 1e-7);
        float enter = t, exit = next;
        if (abs(delta.z) > 1e-10) {
            float a = (lower - origin.z) / delta.z, b = (closest - origin.z) / delta.z;
            enter = max(enter, min(a, b));
            exit = min(exit, max(a, b));
        } else if (p.z < lower || p.z > closest)
            exit = -1;
        if (enter <= exit + 1e-7 && enter < end) {
            // Consumers use ordinary UV-to-texel conversion. Keep the returned coordinate
            // inside the leaf whose depth we tested, including negative-direction entry
            // boundaries where directional DDA ownership differs from floor(UV * size).
            hit.uv = clamp(origin.xy + delta.xy * max(enter, t),
                           (float2(cell) + 1.0 / 64) / float2(screen_size),
                           (float2(cell) + 1 - 1.0 / 64) / float2(screen_size));
            hit.depth = closest;
            hit.confidence = 1;
            return hit;
        }
        t = next;
        level = min(1, int(mips) - 1);
    }
    return hit;
}
