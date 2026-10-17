// DENG-43: public 60-byte row-major instances -> Metal 72-byte indirect descriptors.
// Embedded by src/gpu/metal/embed_accel.cmake. Runtime Metal compilation is the same bootstrap
// policy as the ICB encoder: no optional Slang installation is needed by the native backend.
#include <metal_stdlib>
using namespace metal;
kernel void translate(device const uint *src [[buffer(2)]], device uint *dst [[buffer(3)]],
                      device const uint4 *table [[buffer(4)]], constant uint2 &p [[buffer(5)]],
                      uint tid [[thread_position_in_grid]], uint stride [[threads_per_grid]]) {
    for (uint i = tid; i < p.x; i += stride) {
        uint b = i * 15, o = i * 18;
        uint handle = src[b + 14], index = handle & 0xfffff;
        uint4 entry = uint4(0);
        if (handle && index < p.y) entry = table[index];
        bool valid = entry.x == handle && (entry.z | entry.w) != 0;
        // MTLPackedFloat4x3 stores four columns of three scalars.
        for (uint col = 0; col < 4; ++col)
            for (uint row = 0; row < 3; ++row)
                dst[o + col * 3 + row] = src[b + row * 4 + col];
        dst[o + 12] = src[b + 13] >> 24;       // options (same four low flag bits)
        dst[o + 13] = valid ? src[b + 12] >> 24 : 0;
        dst[o + 14] = src[b + 13] & 0xffffff;  // intersection table offset
        dst[o + 15] = src[b + 12] & 0xffffff;  // user ID
        dst[o + 16] = valid ? entry.z : 0;    // MTLResourceID, low/high
        dst[o + 17] = valid ? entry.w : 0;
    }
}
