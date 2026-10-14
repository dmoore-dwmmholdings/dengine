/*
 * engine_shaders.h: the renderer's own shaders in every build type (SPEC §12.2; DENG-421).
 * Internal to de_render; include as "render/shaders/engine_shaders.h".
 *
 * The list is shaders/engine_shaders.cmake, one line per shader:
 *
 *     de_engine_shader(vis_cull  culling/instance_cull.slang)
 *
 * and C code asks for it by that name:
 *
 *     de__engine_shader_code code;
 *     static const char *const entries[] = {"cs_main"};
 *     DE_TRY(de__engine_shader_load(dev, alloc, "vis_cull", 0, entries, 1, &code, why, sizeof
 * why));
 *     ... de_gpu_pipeline_create with code.entries[0].blob, .threadgroup, .push_constant_bytes ...
 *     de__engine_shader_code_free(&code);    // after the pipelines are made
 *
 * Where the code comes from, first that works (de__engine_shader_code.origin):
 *   1. The null backend: a placeholder blob (it compiles nothing), with the reflection of the
 *      embedded SPIR-V when this build has it.
 *   2. Debug and Dev builds with the cooker (DE_HAS_COOKER): the source under shaders/ cooked
 *      in-process through the cooker's built-in shader importer and the build tree's `.decache`
 *      (DE_RENDER_ENGINE_SHADER_CACHE). An unchanged source is a cache hit (logged at Info); an
 *      edited one is cooked again, without a rebuild. The build-time cook writes the same entries,
 *      so the first create after a build is a hit too.
 *   3. The blob cooked at build time and embedded in de_render (every build whose host could run
 *      tools/engine_shaders/, Release and Shipping included), for this platform's default
 *      targets: macOS metallib + SPIR-V, Linux SPIR-V, Windows DXIL (when DXC was found) + SPIR-V.
 *
 * Threads: loads mount `deengshd:/` and `deengcache:/` once per process (never unmounted); call
 * them on the main thread (renderer and GPU-scene create). The table is constant data.
 */
#ifndef DE_RENDER_ENGINE_SHADERS_H
#define DE_RENDER_ENGINE_SHADERS_H

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>

DE_EXTERN_C_BEGIN

/** One line of shaders/engine_shaders.cmake, with its embedded blob (generated table). */
typedef struct de__engine_shader_info {
    const char *name;          /**< "composite" */
    const char *source;        /**< relative to shaders/: "common/composite.slang" */
    const char *permutations;  /**< "A=1,B/C": permutations split by '/', defines by ','; "" none */
    const unsigned char *blob; /**< the build-time cook (DSHD), NULL when this build has none */
    u32 blob_size;
} de__engine_shader_info;

/** The generated table (engine_shaders_data.c, from shaders/engine_shaders.cmake). */
extern const de__engine_shader_info de__engine_shader_table[];
extern const u32 de__engine_shader_count;

/** The table entry `name`, or NULL. */
const de__engine_shader_info *de__engine_shader_find(const char *name);

/** Where de__engine_shader_load got the code. */
typedef enum de__engine_shader_origin {
    DE__ENGINE_SHADER_NONE = 0,
    DE__ENGINE_SHADER_PLACEHOLDER, /**< the null backend */
    DE__ENGINE_SHADER_COOKED,      /**< cooked from source now (a cache miss) */
    DE__ENGINE_SHADER_CACHED,      /**< from the Dev `.decache` (no cook ran) */
    DE__ENGINE_SHADER_EMBEDDED     /**< the build-time cook embedded in the library */
} de__engine_shader_origin;

/** Name of an origin, for messages: "none", "placeholder", "cooked", "cached", "embedded". */
const char *de__engine_shader_origin_name(de__engine_shader_origin o);

/** Entry points one load returns at most. */
#define DE__ENGINE_SHADER_MAX_ENTRIES 8u

/** One entry point's code for the device's backend and its reflection. */
typedef struct de__engine_shader_entry {
    de_gpu_shader_blob blob; /**< `entry` is the name asked for */
    u32 stage;               /**< de__shader_stage (0 for a placeholder without reflection) */
    u32 threadgroup[3];      /**< compute, mesh and amplification (0 when unknown) */
    u32 push_constant_bytes;
} de__engine_shader_entry;

/** What a load returns. The code lives in `block` (or in static data) until the free. */
typedef struct de__engine_shader_code {
    de__engine_shader_origin origin;
    u32 count;
    de__engine_shader_entry entries[DE__ENGINE_SHADER_MAX_ENTRIES];
    u32 push_constant_bytes; /**< the largest of the entries' (a pipeline's block) */
    void *block;
    size_t block_size;
    const de_allocator *alloc;
} de__engine_shader_code;

/**
 * The code of entry points `entries[0 .. count)` of permutation `permutation` of engine shader
 * `name` for `dev`'s backend, in that order (see the header for where it comes from). On failure
 * `*out` is zeroed and `why` (may be NULL) says what was tried.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL argument, more than DE__ENGINE_SHADER_MAX_ENTRIES);
 *         DE_ERR_NOT_FOUND (no such shader, entry point or permutation, or no code for the
 *         backend's target in this build); DE_ERR_OUT_OF_MEMORY.
 */
de_result de__engine_shader_load(de_gpu_device *dev, const de_allocator *alloc, const char *name,
                                 u32 permutation, const char *const *entries, u32 count,
                                 de__engine_shader_code *out, char *why, size_t why_cap);
/** Frees what a load allocated (zeroes `c`). NULL and a zeroed `c` are ignored. */
void de__engine_shader_code_free(de__engine_shader_code *c);

/* ---------- Test hooks ---------- */

/** Tests: skip the in-process cook (step 2), so a Dev build loads what Release would. */
void de__engine_shaders_set_source_cook(bool on);
/** Tests: the native directory of the Dev cache (NULL: DE_RENDER_ENGINE_SHADER_CACHE). Mounted at
 * the next load. */
void de__engine_shaders_set_cache_dir(const char *native_dir);

DE_EXTERN_C_END

#endif /* DE_RENDER_ENGINE_SHADERS_H */
