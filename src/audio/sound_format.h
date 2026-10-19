#ifndef DE_SOUND_FORMAT_H
#define DE_SOUND_FORMAT_H
#include <dengine/de_base.h>
#include <string.h>
/* DSND v1, 128-byte little-endian header. All section starts are 16-byte aligned.
 * PCM is interleaved SMPTE/WAV channel order. ADPCM: 1017 frames per block, one
 * independent 512-byte IMA block per channel (predictor i16,index u8,reserved u8,
 * low nibble first). Opus uses 20 ms packets; a u32[count+1] byte offset table
 * and 16-byte {frame:u64,packet_index:u64} seek entries every 0.5 seconds.
 * Vorbis payload is the original Ogg byte stream; seek positions are page offsets. */
enum { DE__DSND_PCM16 = 1, DE__DSND_PCM_F32, DE__DSND_ADPCM, DE__DSND_VORBIS, DE__DSND_OPUS };
enum { DE__DSND_STREAM = 1, DE__DSND_LOOP = 2, DE__DSND_HEADER = 128 };
typedef struct de__sound_view {
    const u8 *bytes, *data, *packets, *seeks;
    u64 size, frames, loop_start, loop_end, data_size;
    u32 codec, channels, rate, flags, packet_count, seek_count, block_frames, block_bytes, preskip,
        streams, coupled;
    u8 mapping[8];
} de__sound_view;
static inline u32 de__snd_u32(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static inline u64 de__snd_u64(const u8 *p) {
    return de__snd_u32(p) | ((u64)de__snd_u32(p + 4) << 32);
}
static inline void de__snd_w32(u8 *p, u32 v) {
    for (u32 i = 0; i < 4; ++i)
        p[i] = (u8)(v >> (8 * i));
}
static inline void de__snd_w64(u8 *p, u64 v) {
    de__snd_w32(p, (u32)v);
    de__snd_w32(p + 4, (u32)(v >> 32));
}
static inline bool de__snd_range(u64 size, u64 start, u64 length) {
    return start >= 128 && (start & 15) == 0 && start <= size && length <= size - start;
}
static inline de_result de__sound_parse(const void *bytes, u64 size, de__sound_view *v) {
    if (!bytes || !v || size < 128)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "truncated DSND header");
    const u8 *p = bytes;
    memset(v, 0, sizeof *v);
    if (memcmp(p, "DSND", 4) || de__snd_u32(p + 4) != 1)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid DSND signature/version");
    v->bytes = p;
    v->size = size;
    v->codec = de__snd_u32(p + 8);
    v->channels = de__snd_u32(p + 12);
    v->rate = de__snd_u32(p + 16);
    v->flags = de__snd_u32(p + 20);
    v->frames = de__snd_u64(p + 24);
    v->loop_start = de__snd_u64(p + 32);
    v->loop_end = de__snd_u64(p + 40);
    u64 data = de__snd_u64(p + 48);
    v->data_size = de__snd_u64(p + 56);
    u64 packets = de__snd_u64(p + 64);
    v->packet_count = de__snd_u32(p + 72);
    v->block_frames = de__snd_u32(p + 76);
    v->block_bytes = de__snd_u32(p + 80);
    v->preskip = de__snd_u32(p + 84);
    v->streams = de__snd_u32(p + 88);
    v->coupled = de__snd_u32(p + 92);
    memcpy(v->mapping, p + 96, 8);
    u64 seeks = de__snd_u64(p + 104);
    v->seek_count = de__snd_u32(p + 112);
    if (v->codec < 1 || v->codec > 5 || v->channels < 1 || v->channels > 8 || v->rate < 8000 ||
        v->rate > 384000 || !v->frames || !de__snd_range(size, data, v->data_size) ||
        (v->flags & ~3u) ||
        ((v->flags & DE__DSND_LOOP) && (v->loop_start >= v->loop_end || v->loop_end > v->frames)))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid DSND fields");
    v->data = p + data;
    if (v->codec == DE__DSND_PCM16 || v->codec == DE__DSND_PCM_F32) {
        u64 stride = v->channels * (v->codec == DE__DSND_PCM16 ? 2u : 4u);
        if (v->frames > UINT64_MAX / stride || v->data_size != v->frames * stride)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid PCM extent");
    }
    if (v->codec == DE__DSND_ADPCM) {
        if (v->block_frames != 1017 || v->block_bytes != 512 || v->frames > UINT64_MAX - 1016 ||
            (v->frames + 1016) / 1017 > UINT64_MAX / (512 * v->channels) ||
            v->data_size != (v->frames + 1016) / 1017 * 512 * v->channels)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid ADPCM extent");
    }
    if (v->codec == DE__DSND_OPUS) {
        if (!v->packet_count || v->rate != 48000 || v->streams < 1 || v->streams > 8 ||
            v->coupled > v->streams || v->streams + v->coupled > v->channels ||
            !de__snd_range(size, packets, ((u64)v->packet_count + 1) * 4))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid Opus table");
        v->packets = p + packets;
        u32 previous = 0;
        for (u32 i = 0; i <= v->packet_count; ++i) {
            u32 offset = de__snd_u32(v->packets + i * 4u);
            if (offset < previous || offset > v->data_size || (i == 0 && offset))
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid Opus packet offset");
            previous = offset;
        }
        if (previous != v->data_size)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "incomplete Opus packet data");
        for (u32 c = 0; c < v->channels; ++c)
            if (v->mapping[c] >= v->streams + v->coupled)
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid Opus channel mapping");
    }
    if (v->seek_count) {
        if (!de__snd_range(size, seeks, (u64)v->seek_count * 16))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid seek table");
        v->seeks = p + seeks;
    }
    return DE_OK;
}
/* Shared IMA tables and reference decoder for cooker tests/runtime sources. */
static const i32 de__ima_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static const i32 de__ima_step[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static inline i16 de__ima_decode(u32 nibble, i32 *predictor, i32 *index) {
    i32 step = de__ima_step[*index], diff = step / 8;
    if (nibble & 1)
        diff += step / 4;
    if (nibble & 2)
        diff += step / 2;
    if (nibble & 4)
        diff += step;
    *predictor += (nibble & 8) ? -diff : diff;
    if (*predictor > 32767)
        *predictor = 32767;
    if (*predictor < -32768)
        *predictor = -32768;
    *index += de__ima_index[nibble & 15];
    if (*index < 0)
        *index = 0;
    if (*index > 88)
        *index = 88;
    return (i16)*predictor;
}
#endif
