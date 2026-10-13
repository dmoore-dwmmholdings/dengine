/*
 * fuzz_cooked_walk.h: checks of the cooked formats for the fuzz targets (DENG-106, SPEC §19.1).
 *
 * Each walker parses one blob with its runtime parser and, when the parser accepts it, reads every
 * part a loader reads (every vertex of every stream, every LOD and meshlet range, every
 * subresource, every track key, ...) and checks the promises the parser makes in its header
 * comment: indices below the vertex count, ranges inside their buffers, meshlet local indices
 * below the meshlet's vertex count, and so on. A broken promise is a parser bug: the walker calls
 * abort(), so the fuzzer reports it.
 *
 * Two kinds of caller:
 *   - fuzz_cooked_render / fuzz_cooked_anim (tests/fuzz/render, tests/fuzz/anim) feed hostile
 *     bytes: a refusal is fine (the walker returns false);
 *   - the importer harness (fuzz_import_harness.c) feeds what an importer wrote: a refusal there is
 *     an importer bug (the importer wrote a blob its own loader rejects), so it aborts on false.
 *
 * Allocation-free; no global state.
 */
#ifndef DE_FUZZ_COOKED_WALK_H
#define DE_FUZZ_COOKED_WALK_H

#include <dengine/de_base.h>

#ifdef __cplusplus
extern "C" {
#endif

/** DMSH v2 (render/formats/de_cooked_mesh.h). True when the parser accepts the blob. */
bool fz_walk_mesh(const void *data, u64 size, const char **why);
/** DMAT v1 (render/formats/de_cooked_material.h). */
bool fz_walk_material(const void *data, u64 size, const char **why);
/** DMDL v1 (render/formats/de_cooked_model.h). */
bool fz_walk_model(const void *data, u64 size, const char **why);
/** DTEX v1 (render/formats/de_cooked_texture.h). */
bool fz_walk_texture(const void *data, u64 size, const char **why);
/** A cooked environment (render/formats/de_cooked_env.h, DENG-100): a DTEX cube followed by an
 * ENVS trailer. The DTEX part is walked like fz_walk_texture, then the trailer is evaluated. */
bool fz_walk_env(const void *data, u64 size, const char **why);
/** DSKL v1 (anim/formats/de_cooked_skeleton.h). */
bool fz_walk_skeleton(const void *data, u64 size, const char **why);
/**
 * DCLP v1 (anim/formats/de_cooked_clip.h): every track sampled at every time of the table and
 * between them. With a skeleton blob (`skel`, nullable) that parses, also the reference sampler
 * de__anim_clip_sample at the same times; it must succeed when the joint counts match.
 */
bool fz_walk_clip(const void *data, u64 size, const void *skel, u64 skel_size, const char **why);

/** The four magic bytes of a blob, as a NUL-terminated string ("" when shorter). */
void fz_magic(const void *data, u64 size, char out[5]);

#ifdef __cplusplus
}
#endif

#endif /* DE_FUZZ_COOKED_WALK_H */
