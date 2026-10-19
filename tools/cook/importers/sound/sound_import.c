#include "cook/importers/sound/sound_import.h"
#include "audio/dsp/resample.h"
#include <dr_wav.h>
#include <dr_flac.h>
#include <de_stb_vorbis.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define DECODE_LIMIT (256u * 1024u * 1024u)
static void *codec_alloc(size_t n, void *v) {
    const de_allocator *a = v;
    if (n > SIZE_MAX - 16)
        return NULL;
    u8 *p = DE_ALLOC(a, n + 16, 16, DE_MEM_AUDIO);
    if (p) {
        memcpy(p, &n, sizeof n);
        return p + 16;
    }
    return NULL;
}
static void codec_free(void *p, void *v) {
    if (p) {
        size_t n;
        memcpy(&n, (u8 *)p - 16, sizeof n);
        DE_FREE((const de_allocator *)v, (u8 *)p - 16, n + 16);
    }
}
static void *codec_realloc(void *p, size_t n, void *v) {
    if (!p)
        return codec_alloc(n, v);
    if (!n) {
        codec_free(p, v);
        return NULL;
    }
    size_t old;
    memcpy(&old, (u8 *)p - 16, sizeof old);
    void *result = codec_alloc(n, v);
    if (result) {
        memcpy(result, p, old < n ? old : n);
        codec_free(p, v);
    }
    return result;
}
typedef struct decoded {
    f32 *samples;
    u64 frames, loop_start, loop_end;
    u32 channels, rate;
    bool ogg;
} decoded;
static de_result allocate_pcm(const de_allocator *a, decoded *d) {
    if (!d->frames || d->channels < 1 || d->channels > 8 || d->rate < 8000 || d->rate > 384000 ||
        d->frames > DECODE_LIMIT / (d->channels * 4))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "sound exceeds decoded limit or has invalid format");
    d->samples = DE_ALLOC(a, (size_t)d->frames * d->channels * 4, 16, DE_MEM_AUDIO);
    return d->samples ? DE_OK : DE_FAIL(DE_ERR_OUT_OF_MEMORY, "sound PCM");
}
static de_result decode(const de_allocator *a, const u8 *bytes, u64 size, decoded *d) {
    memset(d, 0, sizeof *d);
    de_result result = DE_OK;
    if (size < 12 || size > INT_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid sound source extent");
    if (!memcmp(bytes, "RIFF", 4) && !memcmp(bytes + 8, "WAVE", 4)) {
        if ((u64)de__snd_u32(bytes + 4) + 8 > size)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "truncated RIFF");
        drwav_allocation_callbacks cb = {(void *)a, codec_alloc, codec_realloc, codec_free};
        drwav wav;
        if (!drwav_init_memory(&wav, bytes, (size_t)size, &cb))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid WAV");
        d->frames = wav.totalPCMFrameCount;
        d->channels = wav.channels;
        d->rate = wav.sampleRate;
        result = allocate_pcm(a, d);
        if (result == DE_OK && drwav_read_pcm_frames_f32(&wav, d->frames, d->samples) != d->frames)
            result = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "truncated WAV samples");
        drwav_uninit(&wav);
        for (u64 offset = 12; result == DE_OK && offset + 8 <= size;) {
            u32 n = de__snd_u32(bytes + offset + 4);
            if (n > size - offset - 8) {
                result = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "truncated WAV chunk");
                break;
            }
            if (!memcmp(bytes + offset, "smpl", 4) && n >= 60 &&
                de__snd_u32(bytes + offset + 8 + 28) > 0) {
                d->loop_start = de__snd_u32(bytes + offset + 8 + 44);
                d->loop_end = (u64)de__snd_u32(bytes + offset + 8 + 48) + 1;
            }
            offset += 8 + (u64)n + (n & 1u);
        }
    } else if (!memcmp(bytes, "fLaC", 4)) {
        drflac_allocation_callbacks cb = {(void *)a, codec_alloc, codec_realloc, codec_free};
        drflac *flac = drflac_open_memory(bytes, (size_t)size, &cb);
        if (!flac)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid FLAC");
        d->frames = flac->totalPCMFrameCount;
        d->channels = flac->channels;
        d->rate = flac->sampleRate;
        result = allocate_pcm(a, d);
        if (result == DE_OK && drflac_read_pcm_frames_f32(flac, d->frames, d->samples) != d->frames)
            result = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "truncated FLAC samples");
        drflac_close(flac);
    } else if (!memcmp(bytes, "OggS", 4)) {
        void *memory = DE_ALLOC(a, 16 * 1024 * 1024, 16, DE_MEM_AUDIO);
        if (!memory)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "Vorbis scratch");
        stb_vorbis_alloc buffer = {(char *)memory, 16 * 1024 * 1024};
        int error = 0;
        stb_vorbis *vorbis = stb_vorbis_open_memory(bytes, (int)size, &error, &buffer);
        if (!vorbis) {
            DE_FREE(a, memory, 16 * 1024 * 1024);
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid Ogg Vorbis");
        }
        stb_vorbis_info info = stb_vorbis_get_info(vorbis);
        d->channels = (u32)info.channels;
        d->rate = info.sample_rate;
        d->frames = stb_vorbis_stream_length_in_samples(vorbis);
        d->ogg = true;
        result = allocate_pcm(a, d);
        if (result == DE_OK) {
            u64 read = 0;
            while (read < d->frames) {
                int n = stb_vorbis_get_samples_float_interleaved(
                    vorbis, (int)d->channels, d->samples + read * d->channels,
                    (int)((d->frames - read) * d->channels));
                if (n <= 0)
                    break;
                read += (u32)n;
            }
            if (read != d->frames)
                result = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "truncated Vorbis samples");
            static const u8 map[9][8] = {{0},
                                         {0},
                                         {0, 1},
                                         {0, 2, 1},
                                         {0, 1, 2, 3},
                                         {0, 2, 1, 3, 4},
                                         {0, 2, 1, 5, 3, 4},
                                         {0, 2, 1, 6, 5, 3, 4},
                                         {0, 2, 1, 7, 5, 6, 3, 4}};
            for (u64 f = 0; f < d->frames; ++f) {
                f32 sample[8];
                memcpy(sample, d->samples + f * d->channels, d->channels * 4);
                for (u32 c = 0; c < d->channels; ++c)
                    d->samples[f * d->channels + c] = sample[map[d->channels][c]];
            }
            stb_vorbis_comment comments = stb_vorbis_get_comment(vorbis);
            for (int i = 0; i < comments.comment_list_length; ++i) {
                const char *c = comments.comment_list[i];
                if (!strncmp(c, "LOOPSTART=", 10))
                    d->loop_start = strtoull(c + 10, NULL, 10);
                if (!strncmp(c, "LOOPEND=", 8))
                    d->loop_end = strtoull(c + 8, NULL, 10);
            }
        }
        stb_vorbis_close(vorbis);
        DE_FREE(a, memory, 16 * 1024 * 1024);
    } else
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "expected WAV, FLAC or Ogg Vorbis");
    if (result != DE_OK) {
        if (d->samples)
            DE_FREE(a, d->samples, (size_t)d->frames * d->channels * 4);
        d->samples = NULL;
        return result;
    }
    for (u64 i = 0; i < d->frames * d->channels; ++i) {
        u32 bits;
        memcpy(&bits, d->samples + i, 4);
        if ((bits & 0x7f800000u) == 0x7f800000u) {
            DE_FREE(a, d->samples, (size_t)d->frames * d->channels * 4);
            d->samples = NULL;
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "non-finite sound samples");
        }
    }
    return DE_OK;
}
static u64 aligned(u64 n) {
    return (n + 15) & ~(u64)15;
}
de_result de__sound_cook(const de_allocator *a, const void *source, u64 size,
                         const de__sound_settings *settings, void **output, u64 *output_size) {
    if (output)
        *output = NULL;
    if (output_size)
        *output_size = 0;
    if (!source || !settings || !output || !output_size)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid sound cook arguments");
    if (!a)
        a = de_heap_allocator();
    decoded d;
    DE_TRY(decode(a, source, size, &d));
    de_result result = DE_OK;
    u64 original_bytes = d.frames * d.channels * 4;
    f32 *samples = d.samples;
    u64 frames = d.frames;
    u32 channels = d.channels, rate = d.rate;
    f32 *resampled = NULL;
    u64 resampled_frames = 0;
    u8 *encoded = NULL;
    u32 encoded_size = 0, *offsets = NULL, packets = 0, preskip = 0;
    if (settings->mono && channels > 1) {
        for (u64 f = 0; f < frames; ++f) {
            f64 sum = 0;
            for (u32 c = 0; c < channels; ++c)
                sum += samples[f * channels + c];
            samples[f] = (f32)(sum / channels);
        }
        channels = 1;
    }
    f64 threshold = settings->stream_above_seconds > 0 ? settings->stream_above_seconds : 10;
    bool stream = settings->stream_set ? settings->stream : ((f64)frames / rate > threshold);
    const char *compression = settings->compression ? settings->compression : "pcm";
    const char *codec =
        settings->codec ? settings->codec : ((d.ogg && !settings->mono) ? "vorbis" : "opus");
    if (strcmp(compression, "pcm") && strcmp(compression, "adpcm")) {
        result = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "unknown sound compression");
        goto done;
    }
    if (strcmp(codec, "opus") && strcmp(codec, "vorbis")) {
        result = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "unknown stream codec");
        goto done;
    }
    u32 kind = stream ? (!strcmp(codec, "vorbis") ? DE__DSND_VORBIS : DE__DSND_OPUS)
                      : (!strcmp(compression, "adpcm") ? DE__DSND_ADPCM : DE__DSND_PCM_F32);
    u64 loop_start = settings->loop_start ? settings->loop_start : d.loop_start;
    u64 loop_end = settings->loop_end ? settings->loop_end : d.loop_end;
    /* Explicit false overrides authored loop metadata without discarding its points. */
    bool looping = settings->loop_set ? settings->loop : (settings->loop || loop_end > loop_start);
    if (looping && !loop_end)
        loop_end = frames;
    if (looping && (loop_start >= loop_end || loop_end > frames)) {
        result = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "sound loop outside source");
        goto done;
    }
    if (kind == DE__DSND_VORBIS && (!d.ogg || settings->mono)) {
        result = DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                         "Vorbis encoding is unsupported; use Opus or Vorbis pass-through");
        goto done;
    }
    if (kind == DE__DSND_OPUS) {
        if (rate != 48000) {
            result = de__resample_buffer(a, samples, frames, channels, rate, 48000, &resampled,
                                         &resampled_frames);
            if (result != DE_OK)
                goto done;
            loop_start = loop_start * 48000 / rate;
            loop_end = loop_end * 48000 / rate;
            frames = resampled_frames;
            rate = 48000;
            samples = resampled;
        }
        result = de__sound_opus(a, samples, frames, channels, settings->bitrate, &encoded,
                                &encoded_size, &offsets, &packets, &preskip);
        if (result != DE_OK)
            goto done;
    }
    u64 data_size = kind == DE__DSND_PCM_F32  ? frames * channels * 4
                    : kind == DE__DSND_ADPCM  ? (frames + 1016) / 1017 * 512 * channels
                    : kind == DE__DSND_VORBIS ? size
                                              : encoded_size;
    u32 seeks = stream ? (u32)((frames + (rate / 2) - 1) / (rate / 2)) : 0;
    u64 table = 128, data = aligned(table + (packets ? ((u64)packets + 1) * 4 : 0));
    u64 seek_offset = aligned(data + data_size), total = seek_offset + (u64)seeks * 16;
    u8 *out = DE_ALLOC(a, (size_t)total, 16, DE_MEM_AUDIO);
    if (!out) {
        result = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "DSND output");
        goto done;
    }
    memset(out, 0, (size_t)total);
    memcpy(out, "DSND", 4);
    de__snd_w32(out + 4, 1);
    de__snd_w32(out + 8, kind);
    de__snd_w32(out + 12, channels);
    de__snd_w32(out + 16, rate);
    de__snd_w32(out + 20, (stream ? 1u : 0u) | (looping ? 2u : 0u));
    de__snd_w64(out + 24, frames);
    de__snd_w64(out + 32, loop_start);
    de__snd_w64(out + 40, loop_end);
    de__snd_w64(out + 48, data);
    de__snd_w64(out + 56, data_size);
    if (kind == DE__DSND_ADPCM) {
        de__snd_w32(out + 76, 1017);
        de__snd_w32(out + 80, 512);
        de__sound_adpcm(samples, frames, channels, out + data);
    } else if (kind == DE__DSND_PCM_F32) {
        for (u64 i = 0; i < frames * channels; ++i) {
            u32 bits;
            memcpy(&bits, samples + i, 4);
            de__snd_w32(out + data + i * 4, bits);
        }
    } else if (kind == DE__DSND_VORBIS)
        memcpy(out + data, source, (size_t)size);
    else {
        memcpy(out + data, encoded, encoded_size);
        de__snd_w64(out + 64, table);
        de__snd_w32(out + 72, packets);
        de__snd_w32(out + 84, preskip);
        de__snd_w32(out + 88, channels);
        for (u32 c = 0; c < channels; ++c)
            out[96 + c] = (u8)c;
        for (u32 i = 0; i <= packets; ++i)
            de__snd_w32(out + table + (u64)i * 4, offsets[i]);
    }
    if (seeks) {
        de__snd_w64(out + 104, seek_offset);
        de__snd_w32(out + 112, seeks);
        u64 page = 0, previous_page = 0;
        for (u32 i = 0; i < seeks; ++i) {
            u64 frame = (u64)i * (rate / 2), position = frame / 960;
            if (kind == DE__DSND_VORBIS) {
                while (page + 27 <= size) {
                    u32 segments = ((const u8 *)source)[page + 26];
                    if (page + 27 + segments > size)
                        break;
                    u64 granule = de__snd_u64((const u8 *)source + page + 6);
                    if (granule != UINT64_MAX && granule >= frame)
                        break;
                    u32 bytes = 0;
                    for (u32 j = 0; j < segments; ++j)
                        bytes += ((const u8 *)source)[page + 27 + j];
                    previous_page = page;
                    page += 27 + segments + bytes;
                }
                position = previous_page;
            }
            de__snd_w64(out + seek_offset + (u64)i * 16, frame);
            de__snd_w64(out + seek_offset + (u64)i * 16 + 8, position);
        }
    }
    *output = out;
    *output_size = total;
done:
    if (encoded)
        DE_FREE(a, encoded, encoded_size);
    if (offsets)
        DE_FREE(a, offsets, ((size_t)packets + 1) * 4);
    if (resampled)
        DE_FREE(a, resampled, (size_t)resampled_frames * channels * 4);
    DE_FREE(a, d.samples, (size_t)original_bytes);
    return result;
}
u32 de__sound_importer_version(void) {
    return 3; /* Rebuild resampled audio with the corrected rational phase clock. */
}
de_result de__sound_import(de_asset_import_ctx *ctx) {
    de__sound_settings s = {0};
    s.stream_set = de_asset_import_setting_has(ctx, "stream");
    s.stream = de_asset_import_setting_bool(ctx, "stream", false);
    s.mono = de_asset_import_setting_bool(ctx, "mono", false);
    s.loop_set = de_asset_import_setting_has(ctx, "loop");
    s.loop = de_asset_import_setting_bool(ctx, "loop", false);
    i64 start = de_asset_import_setting_int(ctx, "loop_start", 0),
        end = de_asset_import_setting_int(ctx, "loop_end", 0),
        bitrate = de_asset_import_setting_int(ctx, "bitrate", 0);
    if (start < 0 || end < 0 || bitrate < 0 || bitrate > INT_MAX)
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                    "invalid sound loop/bitrate setting");
    s.loop_start = (u64)start;
    s.loop_end = (u64)end;
    s.bitrate = (u32)bitrate;
    s.stream_above_seconds = de_asset_import_setting_float(ctx, "stream_above_seconds", 10);
    s.compression = de_asset_import_setting_string(ctx, "compression", "pcm");
    s.codec = de_asset_import_setting_string(ctx, "codec", NULL);
    u64 size = 0, out_size = 0;
    const void *source = de_asset_import_source(ctx, &size);
    void *out = NULL;
    const de_allocator *a = de_asset_import_allocator(ctx);
    de_result r = de__sound_cook(a, source, size, &s, &out, &out_size);
    if (r == DE_OK)
        r = de_asset_import_write(ctx, out, out_size);
    if (out)
        DE_FREE(a, out, (size_t)out_size);
    return r;
}
de_asset_type_desc de_sound_import_type_desc(void) {
    static const char *extensions[] = {"wav", "ogg", "flac"};
    de_asset_type_desc d = {0};
    d.name = "sound";
    d.extensions = extensions;
    d.extension_count = 3;
    d.importer_version = de__sound_importer_version();
    d.import = de__sound_import;
    return d;
}
