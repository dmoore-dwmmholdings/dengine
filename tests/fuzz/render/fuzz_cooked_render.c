/*
 * fuzz_cooked_render.c: fuzz target fuzz_cooked_render (DENG-106, SPEC §19.1, §21.3): the
 * runtime parsers of the cooked render formats, which read untrusted packs at load time (mods):
 *   - DTEX  de_cooked_texture_parse   (render/formats/de_cooked_texture.h, DENG-98)
 *   - DTEX + ENVS de_cooked_env_parse (render/formats/de_cooked_env.h, DENG-100)
 *   - DMSH v2 de_cooked_mesh_parse    (render/formats/de_cooked_mesh.h, DENG-103)
 *   - DMAT  de_cooked_material_parse  (render/formats/de_cooked_material.h, DENG-101)
 *   - DMDL  de_cooked_model_parse     (render/formats/de_cooked_model.h, DENG-101)
 * Every input goes to every parser (each refuses a wrong magic at once). When one accepts it,
 * the walker reads all of it the way a loader would and checks the parser's promises
 * (tests/fuzz/import/fuzz_cooked_walk.c): a broken promise aborts.
 *
 * Seed corpus: corpus/cooked_render/, blobs the importers cooked from the import fixtures
 * (tests/fuzz/import/make_cooked_corpus.c writes them and checks they still parse).
 */
#include "de_fuzz.h"
#include "fuzz_cooked_walk.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    (void)fz_walk_texture(data, size, NULL);
    (void)fz_walk_env(data, size, NULL);
    (void)fz_walk_mesh(data, size, NULL);
    (void)fz_walk_material(data, size, NULL);
    (void)fz_walk_model(data, size, NULL);
    return 0;
}
