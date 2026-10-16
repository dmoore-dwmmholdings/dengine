module scene_color_pyramid;
import bindless;
import push_constants;
public uint scene_color_pitch(uint width) {
    return (width * 8u + 255u) & ~255u;
}
public
uint4 scene_color_level(uint width, uint height, uint level) {
    uint offset = 0;
    for (uint i = 0; i < level; ++i) {
        offset = (offset + scene_color_pitch(width) * height + 511u) & ~511u;
        width = max(width / 2u, 1u);
        height = max(height / 2u, 1u);
    }
    return uint4(width, height, scene_color_pitch(width), offset);
}
public float4 scene_color_load(uint buffer, uint4 level, int2 pixel) {
    pixel = clamp(pixel, int2(0), int2(level.xy) - 1);
    uint2 h = de_bytes(buffer).load2(level.w + uint(pixel.y) * level.z + uint(pixel.x) * 8u);
    return float4(f16tof32(h.x & 65535u), f16tof32(h.x >> 16), f16tof32(h.y & 65535u),
                  f16tof32(h.y >> 16));
}
public float4 scene_color_bilinear(uint buffer, uint4 level, float2 point) {
    int2 p = int2(floor(point));
    float2 f = frac(point);
    return lerp(lerp(scene_color_load(buffer, level, p),
                     scene_color_load(buffer, level, p + int2(1, 0)), f.x),
                lerp(scene_color_load(buffer, level, p + int2(0, 1)),
                     scene_color_load(buffer, level, p + int2(1, 1)), f.x),
                f.y);
}
public float3 scene_color_sample(uint buffer, uint width, uint height, uint levels, float2 uv, float lod) {
    lod = clamp(lod, 0.0, float(levels - 1));
    uint lo = uint(floor(lod)), hi = min(lo + 1, levels - 1);
    uint4 a = scene_color_level(width, height, lo), b = scene_color_level(width, height, hi);
    return lerp(scene_color_bilinear(buffer, a, uv * float2(a.xy) - .5),
                scene_color_bilinear(buffer, b, uv * float2(b.xy) - .5), frac(lod))
        .rgb;
}
struct SceneColorParams {
    uint color;
    uint src_width;
    uint src_height;
    uint src_pitch;
    uint src_offset;
    uint width;
    uint height;
    uint pitch;
    uint offset;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};
[shader("compute")][numthreads(8, 8, 1)] void cs_scene_color_pyramid(uint3 id : SV_DispatchThreadID,
                                                                     uniform SceneColorParams pc) {
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    uint4 level = uint4(pc.src_width, pc.src_height, pc.src_pitch, pc.src_offset);
    float2 center =
        (float2(id.xy) + .5) * float2(pc.src_width, pc.src_height) / float2(pc.width, pc.height) -
        .5;
    const float weights[5] = {1, 4, 6, 4, 1};
    float4 sum = 0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
            sum += scene_color_bilinear(pc.color, level, center + float2(x, y)) * weights[x + 2] *
                   weights[y + 2];
    sum /= 256;
    uint2 h =
        uint2(f32tof16(sum.x) | (f32tof16(sum.y) << 16), f32tof16(sum.z) | (f32tof16(sum.w) << 16));
    de_rwbytes(pc.color).store2(pc.offset + id.y * pc.pitch + id.x * 8u, h);
}
