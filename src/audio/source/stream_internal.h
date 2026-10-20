#ifndef DE_AUDIO_STREAM_INTERNAL_H
#define DE_AUDIO_STREAM_INTERNAL_H
#include "audio/audio_internal.h"
#include "audio/sound_format.h"
#include <dengine/de_task.h>
/* Stream assets retain metadata and a content-addressed extracted cache location; pack
 * compression is removed once at asset load. PCM/ADPCM retain the cooked bytes. */
typedef struct de__sound_asset {
    const de_allocator *alloc;
    de_atomic_u32 refs;
    de__sound_view view;
    u8 *resident, *metadata;
    u64 resident_size, metadata_size;
    char path[1024];
} de__sound_asset;
void de__sound_asset_ref(de__sound_asset *s);
void de__sound_asset_release(de__sound_asset *s);
de_result de__sound_asset_create(const de_allocator *a, const void *bytes, u64 size,
                                 const char *cache_dir, de__sound_asset **out);
de_result de__source_resident(de_audio *a, de__sound_asset *asset, de__audio_source **out);
de_result de__source_stream(de_audio *a, de__sound_asset *asset, de__audio_source **out);
void de__stream_service(de_audio *a);
void de__stream_shutdown(de_audio *a);
void de__stream_test_starve(de_audio *a, bool starve);
/* Deterministic interleaving seams. Set only while the reader is quiescent.
 * refill_now blocks and is exclusively a test-control operation, never an RT callback. */
void de__stream_test_read_hook(de__audio_source *s, void (*hook)(de__audio_source *, void *),
                               void *user);
void de__stream_test_refill_now(de__audio_source *s);
/* Testable real async reader, decoder state is worker-owned. */
#define DE__SOUND_MAX_PACKET_FRAMES 8192u
typedef struct de__sound_decoder {
    const de_allocator *alloc;
    de__sound_asset *asset;
    de_file *file;
    de_io_request request;
    de_sem io_complete;
    bool io_complete_initialized;
    de_result io_result;
    u64 io_bytes;
    u64 io_requests; /* Worker-owned count of successfully submitted compressed reads. */
    u8 *input, *scratch;
    u32 scratch_bytes;
    u32 used, filled;
    u64 file_offset;
    void *codec;
    u32 packet;
    u64 frame;
    u32 skip;
    f32 pcm[DE__SOUND_MAX_PACKET_FRAMES * 8];
    u32 pcm_count, pcm_at;
} de__sound_decoder;
de_result de__decoder_io(de__sound_decoder *d, u64 offset, void *out, u32 bytes);
de_result de__decoder_create(const de_allocator *a, de__sound_asset *asset,
                             de__sound_decoder **out);
void de__decoder_destroy(de__sound_decoder *d);
u32 de__decoder_read(de__sound_decoder *d, f32 *out, u32 frames);
bool de__decoder_seek(de__sound_decoder *d, u64 frame);
de_result de__vorbis_init(de__sound_decoder *d);
u32 de__vorbis_read(de__sound_decoder *d, f32 *out, u32 frames);
bool de__vorbis_seek(de__sound_decoder *d, u64 frame);
de_result de__opus_init(de__sound_decoder *d);
u32 de__opus_read(de__sound_decoder *d, f32 *out, u32 frames);
bool de__opus_seek(de__sound_decoder *d, u64 frame);
#endif
