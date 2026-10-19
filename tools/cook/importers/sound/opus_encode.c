#include "cook/importers/sound/sound_import.h"
#include <opus_multistream.h>
#include <string.h>
de_result de__sound_opus(const de_allocator *a, const f32 *samples, u64 frames, u32 channels,
                         u32 bitrate, u8 **data, u32 *size, u32 **offsets, u32 *packets,
                         u32 *preskip) {
    *data = NULL;
    *offsets = NULL;
    *packets = 0;
    *size = 0;
    u8 mapping[8];
    for (u32 c = 0; c < channels; ++c)
        mapping[c] = (u8)c;
    int state_size = opus_multistream_encoder_get_size((int)channels, 0);
    if (state_size <= 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid Opus channel count");
    OpusMSEncoder *enc = DE_ALLOC(a, (size_t)state_size, 16, DE_MEM_AUDIO);
    if (!enc)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "Opus encoder");
    int err = opus_multistream_encoder_init(enc, 48000, (int)channels, (int)channels, 0, mapping,
                                            OPUS_APPLICATION_AUDIO);
    if (err == OPUS_OK)
        err = opus_multistream_encoder_ctl(
            enc, OPUS_SET_BITRATE((int)(bitrate ? bitrate : 48000 * channels)));
    if (err == OPUS_OK)
        err = opus_multistream_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));
    int skip = 0;
    if (err == OPUS_OK)
        err = opus_multistream_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&skip));
    if (err != OPUS_OK) {
        DE_FREE(a, enc, (size_t)state_size);
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "Opus encoder settings");
    }
    u64 count64 = (frames + (u64)skip + 959) / 960;
    if (count64 > UINT32_MAX / (1280 * channels)) {
        DE_FREE(a, enc, (size_t)state_size);
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "Opus stream too large");
    }
    u32 count = (u32)count64, capacity = count * 1280 * channels;
    u8 *bytes = DE_ALLOC(a, capacity, 16, DE_MEM_AUDIO);
    u32 *table = DE_ALLOC(a, ((size_t)count + 1) * 4, 4, DE_MEM_AUDIO);
    if (!bytes || !table) {
        if (bytes)
            DE_FREE(a, bytes, capacity);
        if (table)
            DE_FREE(a, table, ((size_t)count + 1) * 4);
        DE_FREE(a, enc, (size_t)state_size);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "Opus packets");
    }
    i16 pcm[960 * 8];
    u32 used = 0;
    table[0] = 0;
    for (u32 packet = 0; packet < count; ++packet) {
        for (u32 n = 0; n < 960; ++n)
            for (u32 c = 0; c < channels; ++c) {
                u64 frame = (u64)packet * 960 + n;
                pcm[n * channels + c] =
                    frame < frames ? de__sound_i16(samples[frame * channels + c]) : 0;
            }
        int n = opus_multistream_encode(enc, pcm, 960, bytes + used, (opus_int32)(capacity - used));
        if (n < 0) {
            DE_FREE(a, bytes, capacity);
            DE_FREE(a, table, ((size_t)count + 1) * 4);
            DE_FREE(a, enc, (size_t)state_size);
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "Opus encode failed");
        }
        used += (u32)n;
        table[packet + 1] = used;
    }
    /* Exact-sized ownership for the engine allocator's size-aware frees. */
    u8 *exact = DE_ALLOC(a, used, 16, DE_MEM_AUDIO);
    if (!exact) {
        DE_FREE(a, bytes, capacity);
        DE_FREE(a, table, ((size_t)count + 1) * 4);
        DE_FREE(a, enc, (size_t)state_size);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "Opus output");
    }
    memcpy(exact, bytes, used);
    DE_FREE(a, bytes, capacity);
    DE_FREE(a, enc, (size_t)state_size);
    *data = exact;
    *size = used;
    *offsets = table;
    *packets = count;
    *preskip = (u32)skip;
    return DE_OK;
}
