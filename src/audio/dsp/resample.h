#ifndef DE_AUDIO_RESAMPLE_H
#define DE_AUDIO_RESAMPLE_H
#include <dengine/de_memory.h>
/* Polyphase Kaiser-windowed sinc. Preallocated input ring and coefficient bank; control
 * functions and processing allocate nothing. Ratios are input frames per output frame. */
typedef struct de__resampler de__resampler;
de_result de__resample_create(const de_allocator *a, u32 channels, f64 ratio, de__resampler **out);
void de__resample_destroy(de__resampler *s);
void de__resample_reset(de__resampler *s);
void de__resample_ratio(de__resampler *s, f64 ratio, u32 ramp_frames);
u32 de__resample_push(de__resampler *s, const f32 *input, u32 frames);
void de__resample_finish(de__resampler *s);
u32 de__resample_read(de__resampler *s, f32 *output, u32 frames);
de_result de__resample_buffer(const de_allocator *a, const f32 *input, u64 frames, u32 channels,
                              u32 input_rate, u32 output_rate, f32 **output, u64 *output_frames);
#endif
