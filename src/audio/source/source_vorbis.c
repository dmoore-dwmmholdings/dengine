#include "audio/source/stream_internal.h"
#include <de_stb_vorbis.h>
#include <string.h>
#define INPUT_BYTES   (1024u * 1024u)
#define SCRATCH_BYTES (16u * 1024u * 1024u)
static bool more(de__sound_decoder *d) {
    u32 remain = d->filled - d->used;
    memmove(d->input, d->input + d->used, remain);
    d->filled = remain;
    d->used = 0;
    u64 bytes = d->asset->view.data_size - d->file_offset;
    u32 read = INPUT_BYTES - remain;
    if (read > 65536)
        read = 65536;
    if (read > bytes)
        read = (u32)bytes;
    if (!read)
        return false;
    if (de__decoder_io(d, de__snd_u64(d->asset->view.bytes + 48) + d->file_offset,
                       d->input + remain, read) != DE_OK)
        return false;
    d->file_offset += read;
    d->filled += read;
    return true;
}
de_result de__vorbis_init(de__sound_decoder *d) {
    if (!d->scratch) {
        d->scratch = DE_ALLOC(d->alloc, SCRATCH_BYTES, 16, DE_MEM_AUDIO);
        d->scratch_bytes = SCRATCH_BYTES;
        if (!d->scratch)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "Vorbis scratch");
    }
    d->used = d->filled = 0;
    d->file_offset = 0;
    d->pcm_at = d->pcm_count = 0;
    d->frame = 0;
    stb_vorbis_alloc memory = {(char *)d->scratch, SCRATCH_BYTES};
    int consumed = 0, error = 0;
    do {
        if (!more(d))
            break;
        d->codec = stb_vorbis_open_pushdata(d->input, (int)d->filled, &consumed, &error, &memory);
        if (d->codec) {
            d->used = (u32)consumed;
            stb_vorbis_info info = stb_vorbis_get_info(d->codec);
            if (info.channels != (int)d->asset->view.channels ||
                info.sample_rate != d->asset->view.rate)
                return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "Vorbis format disagrees with DSND");
            return DE_OK;
        }
    } while (error == VORBIS_need_more_data);
    return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid/truncated streaming Vorbis headers");
}
static bool next(de__sound_decoder *d, u32 discard) {
    const u32 channels = d->asset->view.channels;
    static const u8 map[9][8] = {{0},
                                 {0},
                                 {0, 1},
                                 {0, 2, 1},
                                 {0, 1, 2, 3},
                                 {0, 2, 1, 3, 4},
                                 {0, 2, 1, 5, 3, 4},
                                 {0, 2, 1, 6, 5, 3, 4},
                                 {0, 2, 1, 7, 5, 6, 3, 4}};
    for (u32 attempts = 0; attempts < 1024; ++attempts) {
        float **output = NULL;
        int count = 0, got_channels = 0;
        int used = stb_vorbis_decode_frame_pushdata(d->codec, d->input + d->used,
                                                    (int)(d->filled - d->used), &got_channels,
                                                    &output, &count);
        if (used < 0 || (u32)used > d->filled - d->used)
            return false;
        d->used += (u32)used;
        if (count > 0) {
            if (count > (int)DE__SOUND_MAX_PACKET_FRAMES || got_channels != (int)channels)
                return false;
            d->pcm_count = (u32)count;
            d->pcm_at = discard < d->pcm_count ? discard : d->pcm_count;
            d->frame += d->pcm_at;
            /* Seeking still decodes every overlap packet, but discarded PCM
             * need not be interleaved or copied through a temporary buffer. */
            for (u32 f = d->pcm_at; f < d->pcm_count; ++f)
                for (u32 c = 0; c < channels; ++c)
                    d->pcm[f * channels + c] = output[map[channels][c]][f];
            return true;
        }
        if (!used && !more(d))
            return false;
    }
    return false;
}
u32 de__vorbis_read(de__sound_decoder *d, f32 *out, u32 frames) {
    u32 done = 0, channels = d->asset->view.channels;
    while (done < frames && d->frame < d->asset->view.frames) {
        if (d->pcm_at == d->pcm_count && !next(d, 0))
            break;
        u32 n = d->pcm_count - d->pcm_at;
        if (n > frames - done)
            n = frames - done;
        if (n > d->asset->view.frames - d->frame)
            n = (u32)(d->asset->view.frames - d->frame);
        memcpy(out + (size_t)done * channels, d->pcm + (size_t)d->pcm_at * channels,
               (size_t)n * channels * 4);
        done += n;
        d->pcm_at += n;
        d->frame += n;
    }
    return done;
}
bool de__vorbis_seek(de__sound_decoder *d, u64 frame) {
    const de__sound_view *v = &d->asset->view;
    if (frame > v->frames)
        return false;
    /* Push decoding consumes the resynchronization page, then one packet to
     * rebuild overlap. A page ending just before the requested frame can thus
     * produce its first PCM after the target and force a full header restart.
     * Keep one maximum decoded block of preroll when choosing the seek page. */
    u64 seek_frame = frame > DE__SOUND_MAX_PACKET_FRAMES ? frame - DE__SOUND_MAX_PACKET_FRAMES : 0;
    u64 offset = 0;
    for (u32 i = 0; i < v->seek_count; ++i) {
        u64 f = de__snd_u64(v->seeks + (u64)i * 16), at = de__snd_u64(v->seeks + (u64)i * 16 + 8);
        if (f > seek_frame)
            break;
        if (at < v->data_size)
            offset = at;
    }
    bool positioned = false;
    if (offset) {
        stb_vorbis_flush_pushdata(d->codec);
        d->used = d->filled = 0;
        d->file_offset = offset;
        d->pcm_at = d->pcm_count = 0;
        for (u32 attempts = 0; attempts < 256; ++attempts) {
            if (!next(d, 0))
                break;
            int end = stb_vorbis_get_sample_offset(d->codec);
            if (end >= 0 && (u64)end >= d->pcm_count) {
                u64 start = (u64)end - d->pcm_count;
                if (start <= frame) {
                    d->frame = start;
                    positioned = true;
                }
                break;
            }
        }
    }
    if (!positioned) {
        if (d->codec)
            stb_vorbis_close(d->codec);
        d->codec = NULL;
        if (de__vorbis_init(d) != DE_OK)
            return false;
    }
    while (d->frame < frame) {
        u64 remaining = frame - d->frame;
        if (d->pcm_at == d->pcm_count) {
            u32 skip = remaining < DE__SOUND_MAX_PACKET_FRAMES ? (u32)remaining
                                                               : DE__SOUND_MAX_PACKET_FRAMES;
            if (!next(d, skip))
                return false;
        } else {
            u32 skip = d->pcm_count - d->pcm_at;
            if (skip > remaining)
                skip = (u32)remaining;
            d->pcm_at += skip;
            d->frame += skip;
        }
    }
    return true;
}
