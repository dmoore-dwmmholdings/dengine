#ifndef DE_SOUND_IMPORT_H
#define DE_SOUND_IMPORT_H
#include <dengine/de_asset.h>
#include "audio/sound_format.h"
typedef struct de__sound_settings {
    bool stream, stream_set, mono, loop, loop_set;
    u64 loop_start, loop_end;
    f64 stream_above_seconds;
    const char *compression, *codec;
    u32 bitrate;
} de__sound_settings;
de_result de__sound_cook(const de_allocator *a, const void *source, u64 size,
                         const de__sound_settings *settings, void **output, u64 *output_size);
de_result de__sound_import(de_asset_import_ctx *ctx);
de_asset_type_desc de_sound_import_type_desc(void);
u32 de__sound_importer_version(void);
i16 de__sound_i16(f32 value);
void de__sound_adpcm(const f32 *samples, u64 frames, u32 channels, u8 *output);
de_result de__sound_opus(const de_allocator *a, const f32 *samples, u64 frames, u32 channels,
                         u32 bitrate, u8 **data, u32 *size, u32 **offsets, u32 *packets,
                         u32 *preskip);
#endif
