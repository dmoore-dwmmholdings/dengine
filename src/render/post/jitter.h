#ifndef DE_RENDER_POST_JITTER_H
#define DE_RENDER_POST_JITTER_H
#include <dengine/de_math.h>
/** ceil(8 * output area / input area), clamped to 8..64. Invalid dimensions return zero. */
u32 de__jitter_phase_count(u32 iw, u32 ih, u32 ow, u32 oh);
/** Shared Halton(2,3) pixel offsets, starting at index one. Reset frame is zero. */
de_vec2 de__jitter_sample(u64 frame, u32 phases);
/** Raster sample at pixel centre + jitter (positive Y down). Homogeneous clip offsets
 * support perspective and orthographic projections without modifying reverse-Z depth. */
de_mat4 de__jitter_projection(de_mat4 projection, de_vec2 jitter, u32 width, u32 height);
#endif
