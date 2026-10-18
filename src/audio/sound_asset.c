#include "audio/source/stream_internal.h"
#include "asset/asset_internal.h"
#include "asset/slots_internal.h"
#include <stdio.h>
#include <string.h>
void de__sound_asset_ref(de__sound_asset *s) {
    de_atomic_fetch_add_u32(&s->refs, 1, DE_ATOMIC_RELAXED);
}
void de__sound_asset_release(de__sound_asset *s) {
    if (!s || de_atomic_fetch_sub_u32(&s->refs, 1, DE_ATOMIC_ACQ_REL) != 1)
        return;
    if (s->resident)
        DE_FREE(s->alloc, s->resident, (size_t)s->resident_size);
    if (s->metadata)
        DE_FREE(s->alloc, s->metadata, (size_t)s->metadata_size);
    DE_FREE(s->alloc, s, sizeof *s);
}
de_result de__sound_asset_create(const de_allocator *a, const void *bytes, u64 size,
                                 const char *cache_dir, de__sound_asset **out) {
    *out = NULL;
    de__sound_view view;
    DE_TRY(de__sound_parse(bytes, size, &view));
    de__sound_asset *s = DE_ALLOC(a, sizeof *s, DE_ALIGNOF(de__sound_asset), DE_MEM_AUDIO);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "sound asset");
    memset(s, 0, sizeof *s);
    s->alloc = a;
    s->view = view;
    de_atomic_init_u32(&s->refs, 1);
    de_result result = DE_OK;
    if (view.codec == DE__DSND_VORBIS || view.codec == DE__DSND_OPUS) {
        if (!cache_dir) {
            result = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "stream cache directory required");
            goto fail;
        }
        char directory[1024];
        int length = snprintf(directory, sizeof directory, "%s/audio-streams", cache_dir);
        if (length < 0 || (size_t)length >= sizeof directory) {
            result = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "sound cache path too long");
            goto fail;
        }
        if (!de_path_exists(cache_dir))
            result = de_dir_create(cache_dir);
        if (result == DE_OK && !de_path_exists(directory))
            result = de_dir_create(directory);
        if (result != DE_OK)
            goto fail;
        length = snprintf(s->path, sizeof s->path, "%s/%016llx.dsnd", directory,
                          (unsigned long long)de_hash64(bytes, (size_t)size, 0));
        if (length < 0 || (size_t)length >= sizeof s->path) {
            result = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "sound cache path too long");
            goto fail;
        }
        if (!de_path_exists(s->path)) {
            result = de_file_write_all_atomic(s->path, bytes, size);
            if (result != DE_OK)
                goto fail;
        }
        u64 packet_bytes = view.packet_count ? ((u64)view.packet_count + 1) * 4 : 0;
        s->metadata_size = 128 + packet_bytes + (u64)view.seek_count * 16;
        s->metadata = DE_ALLOC(a, (size_t)s->metadata_size, 16, DE_MEM_AUDIO);
        if (!s->metadata) {
            result = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "sound stream metadata");
            goto fail;
        }
        memcpy(s->metadata, bytes, 128);
        if (packet_bytes)
            memcpy(s->metadata + 128, view.packets, (size_t)packet_bytes);
        if (view.seek_count)
            memcpy(s->metadata + 128 + packet_bytes, view.seeks, (size_t)view.seek_count * 16);
        s->view.bytes = s->metadata;
        s->view.data = NULL;
        s->view.packets = packet_bytes ? s->metadata + 128 : NULL;
        s->view.seeks = view.seek_count ? s->metadata + 128 + packet_bytes : NULL;
    } else {
        s->resident_size = size;
        s->resident = DE_ALLOC(a, (size_t)size, 16, DE_MEM_AUDIO);
        if (!s->resident) {
            result = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "resident sound");
            goto fail;
        }
        memcpy(s->resident, bytes, (size_t)size);
        result = de__sound_parse(s->resident, size, &s->view);
        if (result != DE_OK)
            goto fail;
        if (view.codec == DE__DSND_ADPCM)
            for (u64 i = 0; i < view.data_size; i += 512)
                if (s->view.data[i + 2] > 88) {
                    result = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid ADPCM predictor index");
                    goto fail;
                }
    }
    *out = s;
    return DE_OK;
fail:
    de__sound_asset_release(s);
    return result;
}
static de_result load(de_asset_load_ctx *ctx) {
    de_asset_system *assets = de_asset_load_ctx_user(ctx);
    u64 size = 0;
    const void *bytes = de_asset_load_ctx_data(ctx, &size);
    de__sound_asset *object = NULL;
    DE_TRY(de__sound_asset_create(assets->alloc, bytes, size, assets->desc.cache_dir, &object));
    de_asset_load_ctx_set_object(ctx, object,
                                 sizeof *object + object->resident_size + object->metadata_size);
    return DE_OK;
}
static void unload(void *object, void *user) {
    (void)user;
    de__sound_asset_release(object);
}
de_result de_audio_register_sound_type(de_asset_system *assets) {
    if (!assets)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "NULL sound asset system");
    if (de__asset_type_find(assets, "sound").id)
        return DE_OK;
    static const char *extensions[] = {".wav", ".ogg", ".flac"};
    de_asset_type_desc d = {0};
    d.name = "sound";
    d.extensions = extensions;
    d.extension_count = 3;
    d.load = load;
    d.unload = unload;
    d.user = assets;
    d.category = DE_ASSET_CAT_AUDIO;
    d.importer_version = 1;
#if DE_HAS_COOKER
    de_asset_type_desc builtin;
    if (de__asset_builtin_importer("sound", &builtin)) {
        d.import = builtin.import;
        d.importer_version = builtin.importer_version;
    }
#endif
    return de_asset_type_register(assets, &d).id
               ? DE_OK
               : DE_FAIL(DE_ERR_INVALID_ARGUMENT, "sound registration failed");
}
de_result de__sound_source_open(de_audio *a, de_sound sound, de__audio_source **out) {
    if (out)
        *out = NULL;
    if (!a || !a->assets || !out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "sound asset system required");
    if (!de__slot_ref_asset(a->assets, sound.id))
        return DE_FAIL(DE_ERR_NOT_FOUND, "stale sound handle");
    const de__slot *slot = de__slot_get(a->assets, sound.id);
    de__sound_asset *object = NULL;
    if (slot && slot->type_hash == de_hash64_str("sound"))
        object = de__slot_object(slot);
    if (object)
        de__sound_asset_ref(object);
    (void)de__slot_unref_asset(a->assets, sound.id);
    if (!object)
        return DE_FAIL(DE_ERR_NOT_FOUND, "sound is not ready");
    de_result result = (object->view.codec >= DE__DSND_VORBIS)
                           ? de__source_stream(a, object, out)
                           : de__source_resident(a, object, out);
    de__sound_asset_release(object);
    return result;
}
