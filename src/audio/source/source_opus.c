#include "audio/source/stream_internal.h"
#include <opus_multistream.h>
#include <string.h>
de_result de__opus_init(de__sound_decoder *d) {
    const de__sound_view *v = &d->asset->view;
    int bytes = opus_multistream_decoder_get_size((int)v->streams, (int)v->coupled);
    if (bytes <= 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid Opus decoder configuration");
    d->scratch = DE_ALLOC(d->alloc, (size_t)bytes, 16, DE_MEM_AUDIO);
    if (!d->scratch)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "Opus decoder state");
    d->scratch_bytes = (u32)bytes;
    d->codec = d->scratch;
    if (opus_multistream_decoder_init(d->codec, 48000, (int)v->channels, (int)v->streams,
                                      (int)v->coupled, v->mapping) != OPUS_OK)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid Opus mapping");
    d->skip = v->preskip;
    return DE_OK;
}
static bool next(de__sound_decoder *d) {
    const de__sound_view *v = &d->asset->view;
    while (d->packet < v->packet_count) {
        u32 at = de__snd_u32(v->packets + (u64)d->packet * 4),
            end = de__snd_u32(v->packets + ((u64)d->packet + 1) * 4);
        if (end <= at || end - at > 1024 * 1024)
            return false;
        if (de__decoder_io(d, de__snd_u64(v->bytes + 48) + at, d->input, end - at) != DE_OK)
            return false;
        i16 samples[5760 * 8];
        int count =
            opus_multistream_decode(d->codec, d->input, (opus_int32)(end - at), samples, 5760, 0);
        ++d->packet;
        if (count <= 0 || count > 8192)
            return false;
        u32 skip = d->skip < (u32)count ? d->skip : (u32)count;
        d->skip -= skip;
        d->pcm_count = (u32)count - skip;
        d->pcm_at = 0;
        for (u32 f = 0; f < d->pcm_count; ++f)
            for (u32 c = 0; c < v->channels; ++c)
                d->pcm[f * v->channels + c] = samples[(f + skip) * v->channels + c] / 32768.0f;
        if (d->pcm_count)
            return true;
    }
    return false;
}
u32 de__opus_read(de__sound_decoder *d, f32 *out, u32 frames) {
    u32 done = 0, channels = d->asset->view.channels;
    while (done < frames && d->frame < d->asset->view.frames) {
        if (d->pcm_at == d->pcm_count && !next(d))
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
bool de__opus_seek(de__sound_decoder *d, u64 frame) {
    const de__sound_view *v = &d->asset->view;
    if (frame > v->frames)
        return false;
    u32 packet = 0;
    for (u32 i = 0; i < v->seek_count; ++i) {
        u64 f = de__snd_u64(v->seeks + (u64)i * 16), p = de__snd_u64(v->seeks + (u64)i * 16 + 8);
        if (f > frame)
            break;
        if (p < v->packet_count)
            packet = (u32)p;
    }
    /* Four 20ms packets meet the minimum preroll, but tonal predictor state
     * still differs measurably from linear decoding. Sixteen settle that state while
     * keeping work bounded by 320ms plus the seek table's half-second spacing. */
    enum { PREROLL_PACKETS = 16 };
    packet = packet > PREROLL_PACKETS ? packet - PREROLL_PACKETS : 0;
    if ((u64)packet * 960 > frame + v->preskip)
        packet = 0;
    if (opus_multistream_decoder_ctl(d->codec, OPUS_RESET_STATE) != OPUS_OK)
        return false;
    d->packet = packet;
    d->skip = (u32)(frame + v->preskip - (u64)packet * 960);
    d->frame = frame;
    d->pcm_at = d->pcm_count = 0;
    return true;
}
