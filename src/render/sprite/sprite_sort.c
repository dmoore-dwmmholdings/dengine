#include "sprite_sort.h"
#include <math.h>
#include <string.h>
u64 de__sprite_sort_key(i16 layer, i16 order, f32 depth) {
    if (isnan(depth))
        depth = INFINITY;
    if (depth == 0)
        depth = 0; /* canonicalise negative zero */
    u32 bits;
    memcpy(&bits, &depth, sizeof bits);
    u32 monotonic = (bits & 0x80000000u) ? ~bits : (bits ^ 0x80000000u);
    return ((u64)((u16)layer ^ 0x8000u) << 48) | ((u64)((u16)order ^ 0x8000u) << 32) |
           (u64)~monotonic;
}
