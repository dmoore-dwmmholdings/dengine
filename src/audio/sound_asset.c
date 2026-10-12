/* sound_asset.c: stub until DENG-313 (sound asset type, sources). DENG-313 owns this file and
 * replaces the body; de__sound_source_open keeps its signature (audio_internal.h). */
#include "audio/audio_internal.h"

de_result de__sound_source_open(de_audio *a, de_sound sound, de__audio_source **out) {
    (void)a;
    (void)sound;
    if (out != NULL)
        *out = NULL;
    return DE_FAIL(DE_ERR_UNSUPPORTED,
                   "audio: sound assets cannot be played yet (sources come with DENG-313)");
}
