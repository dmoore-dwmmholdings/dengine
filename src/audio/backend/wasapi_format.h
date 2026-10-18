#ifndef DE_AUDIO_WASAPI_FORMAT_H
#define DE_AUDIO_WASAPI_FORMAT_H
#include <dengine/de_base.h>
typedef enum de__wasapi_sample { DE__WASAPI_F32, DE__WASAPI_I24, DE__WASAPI_I16 } de__wasapi_sample;
typedef struct de__wasapi_format {
    u32 rate, channels, mask;
    de__wasapi_sample sample;
} de__wasapi_format;
u32 de__wasapi_mask(u32 channels);
void de__wasapi_candidates(u32 device_rate, u32 channels, de__wasapi_format out[3]);
bool de__wasapi_utf8(const u16 *wide, char *out, u32 bytes);
void de__wasapi_convert(const f32 *in, void *out, u32 samples, de__wasapi_sample sample);
#endif
