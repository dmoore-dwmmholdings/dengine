// shaders/visbuffer/feedback.slang: the texture-streaming feedback of the material resolve (SPEC
// §11.8, §12.5 step 4; DENG-238). The residency manager (S20, DENG-274) reads it back.
//
// The feedback buffer is a raw buffer of one u32 per sampled-texture bindless index: the finest
// mip any recorded pixel needed this frame, DE_FEEDBACK_NONE where no pixel sampled the texture.
// resolve_standard.slang's cs_feedback_clear fills it with DE_FEEDBACK_NONE each frame; the
// resolve records one pixel per tile (de_feedback_pixel: jittered by the frame index, so over
// tile x tile frames every pixel of a tile is seen), with an atomic min, and the frame's buffer is
// copied into a readback ring (src/render/passes/vis/resolve.c). The values are hints: the min is
// order independent, and a frame without feedback is fine.
//
// (DENG-238 stores a u32 per texture rather than a u8: an atomic min needs a whole word.)
module feedback;

import bindless;

/// The value of a texture no recorded pixel sampled.
public static const uint DE_FEEDBACK_NONE = 255;

/// The pixel of a `tile` x `tile` tile (index in the tile) that records feedback this frame.
public uint de_feedback_pixel(uint tile_index, uint frame, uint tile)
{
    uint h = (tile_index * 0x9E3779B1u) ^ (frame * 0x85EBCA6Bu);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h % (tile * tile);
}

/// The mip a sample with UV derivatives (dx, dy) needs from sampled texture `texture` (the
/// standard LOD: log2 of the longer footprint axis in texels), and records it.
public void de_feedback_record(uint feedback, uint capacity, uint texture, float2 dx, float2 dy)
{
    if (texture == DE_BINDLESS_INVALID || texture >= capacity)
        return;
    uint w, h, levels;
    de_tex2d(texture).GetDimensions(0, w, h, levels);
    float2 size = float2(float(w), float(h));
    float rho = max(length(dx * size), length(dy * size));
    float lod = rho > 1.0 ? log2(rho) : 0.0;
    uint mip = min(uint(lod), min(levels > 0u ? levels - 1u : 0u, DE_FEEDBACK_NONE - 1u));
    de_rwbytes(feedback).atomic_min(texture * 4u, mip);
}
