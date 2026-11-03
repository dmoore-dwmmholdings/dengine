#ifndef DE_AUDIO_STREAM_RING_H
#define DE_AUDIO_STREAM_RING_H
#include <dengine/de_base.h>
#include <string.h>
/* Copy only: cursor ownership/publication remains with the SPSC stream caller.
 * Buffers do not overlap; capacity/channels are positive and frames <= capacity. */
static inline void de__stream_ring_write(f32 *ring, u32 capacity, u32 channels, u64 position,
                                         const f32 *pcm, u32 frames) {
    if (!frames)
        return;
    u32 at = (u32)(position % capacity), first = capacity - at;
    if (first > frames)
        first = frames;
    size_t stride = (size_t)channels * sizeof *pcm;
    memcpy(ring + (size_t)at * channels, pcm, (size_t)first * stride);
    if (frames > first)
        memcpy(ring, pcm + (size_t)first * channels, (size_t)(frames - first) * stride);
}
static inline void de__stream_ring_read(f32 *pcm, const f32 *ring, u32 capacity, u32 channels,
                                        u64 position, u32 frames) {
    if (!frames)
        return;
    u32 at = (u32)(position % capacity), first = capacity - at;
    if (first > frames)
        first = frames;
    size_t stride = (size_t)channels * sizeof *pcm;
    memcpy(pcm, ring + (size_t)at * channels, (size_t)first * stride);
    if (frames > first)
        memcpy(pcm + (size_t)first * channels, ring, (size_t)(frames - first) * stride);
}
#endif
