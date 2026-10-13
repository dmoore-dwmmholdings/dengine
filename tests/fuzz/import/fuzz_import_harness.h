/*
 * fuzz_import_harness.h: the in-memory import harness of the importer fuzz targets (DENG-106,
 * SPEC §19.1, §21.3). fuzz_gltf.c, fuzz_fbx.c, fuzz_image.c and fuzz_ktx2.c call it from
 * LLVMFuzzerTestOneInput; the CTest import_fuzz_regressions replays tests/fuzz/import/regressions/
 * through it, without libFuzzer.
 *
 * One input runs through the real importer entry (de_model_import, de_texture_import) inside a
 * cooker import context, the way de_cooker_cook runs it, but from memory: there is no file system
 * and no cache. External files a model names (glTF buffers and images, FBX textures) are never
 * found. Every output the import writes then goes through its runtime parser and the walkers of
 * fuzz_cooked_walk.h; a blob the parser refuses is an importer bug and aborts.
 */
#ifndef DE_FUZZ_IMPORT_HARNESS_H
#define DE_FUZZ_IMPORT_HARNESS_H

#include <dengine/de_base.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum fz_import_kind {
    FZ_IMPORT_GLTF = 0, /**< the `model` importer, source `fuzz.gltf` (GLB by its magic) */
    FZ_IMPORT_FBX,      /**< the `model` importer, source `fuzz.fbx` */
    FZ_IMPORT_IMAGE,    /**< the `texture` importer, source `fuzz.png` (PNG, JPEG, EXR) */
    FZ_IMPORT_KTX2,     /**< the `texture` importer, source `fuzz.ktx2` */
    FZ_IMPORT_KIND_COUNT
} fz_import_kind;

/** The `max_decoded_mib` setting every fuzz import gets: small, so CI's -malloc_limit_mb=256
 * holds and one input stays fast. */
#define FZ_IMPORT_MAX_DECODED_MIB 24
/** The texture `max_size` setting of every texture cook, nested ones included (the mip pipeline
 * still runs). Texture cooks also get `compression = "none"` unless an input asks otherwise. */
#define FZ_IMPORT_MAX_SIZE 64

/** The `anim_sample_rate` setting (Hz) of the model targets: keeps the clip time table to the
 * source key times, so clip compression stays fast (fz_settings). */
#define FZ_IMPORT_ANIM_SAMPLE_RATE 0.001

/** "gltf", "fbx", "image", "ktx2". */
const char *fz_import_kind_name(fz_import_kind kind);

/**
 * Imports `data` as `kind` and checks every output. Returns the importer's result; `msg` (nullable,
 * `msg_size` bytes) gets the failure message ("" on success). Never returns when an output breaks
 * its format: it aborts.
 */
de_result fz_import_run(fz_import_kind kind, const u8 *data, size_t size, char *msg,
                        size_t msg_size);

/** One output of an import: the main bytes (`name` NULL, `type` the asset type) or a sub-asset. */
typedef void (*fz_import_output_fn)(const char *name, const char *type, const void *data, u64 size,
                                    void *user);

struct de__manifest_kv;

/**
 * Imports `data` as `kind` with exactly `settings` (nothing derived from the input), checks every
 * output as fz_import_run does, then hands each one to `fn` (nullable) before it is freed. The
 * cooked-format corpus generator (make_cooked_corpus.c) uses it.
 */
de_result fz_import_cook(fz_import_kind kind, const u8 *data, size_t size,
                         const struct de__manifest_kv *settings, u32 setting_count,
                         fz_import_output_fn fn, void *user, char *msg, size_t msg_size);

#ifdef __cplusplus
}
#endif

#endif /* DE_FUZZ_IMPORT_HARNESS_H */
