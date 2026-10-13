/*
 * fuzz_cooked_anim.c: fuzz target fuzz_cooked_anim (DENG-106, SPEC §19.1, §21.3): the runtime
 * parsers of the cooked animation formats (src/anim/formats/, DENG-104), which read untrusted
 * packs at load time:
 *   - DSKL  de_cooked_skeleton_parse  (anim/formats/de_cooked_skeleton.h)
 *   - DCLP  de_cooked_clip_parse      (anim/formats/de_cooked_clip.h), then every track sampled
 *           and the reference sampler de__anim_clip_sample (clip_decode.c) against a skeleton.
 *
 * Input: a u32 (little-endian) length L, then L bytes of skeleton, then the clip. The two parts
 * are walked as a pair (the sampler needs both), and the whole input is also tried as a skeleton
 * and as a clip on its own. Walkers: tests/fuzz/import/fuzz_cooked_walk.c (a broken parser
 * promise aborts).
 *
 * Seed corpus: corpus/cooked_anim/, pairs the model importer cooked from the import fixtures
 * (tests/fuzz/import/make_cooked_corpus.c writes them and checks they still parse).
 */
#include "de_fuzz.h"
#include "fuzz_cooked_walk.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    (void)fz_walk_skeleton(data, size, NULL);
    (void)fz_walk_clip(data, size, NULL, 0, NULL);
    if (size < 4)
        return 0;
    u64 split = (u64)data[0] | ((u64)data[1] << 8) | ((u64)data[2] << 16) | ((u64)data[3] << 24);
    if (split > size - 4u)
        return 0;
    const uint8_t *skel = data + 4;
    const uint8_t *clip = skel + split;
    u64 clip_size = size - 4u - split;
    (void)fz_walk_skeleton(skel, split, NULL);
    (void)fz_walk_clip(clip, clip_size, skel, split, NULL);
    return 0;
}
