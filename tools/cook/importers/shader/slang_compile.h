/*
 * slang_compile.h: the C interface of the Slang shim (slang_compile.cpp; DENG-289). Slang is C++
 * with a COM-style API and slang.h does not compile as C11, so the shader importer reaches it only
 * through these functions (DENG-2 Q1: C++ third-party code behind a C API inside one module).
 *
 * The shim opens the Slang compiler library at run time (de_dylib_open on DE_SLANG_LIBRARY, the
 * prebuilt library of third_party/slang/) on first use and keeps one Slang global session for the
 * life of the process. Slang's front end is not thread-safe, so every call takes one process-wide
 * lock: shader cooks run one at a time.
 *
 * One call compiles one source for one target and one set of defines, and returns the code and
 * the reflection of each requested entry point. Imports are served by `read`: the shim never opens
 * a file itself, so the importer can record every file it reads (de_asset_import_read_file) and
 * no absolute path reaches Slang or its output.
 */
#ifndef DE_COOK_SLANG_COMPILE_H
#define DE_COOK_SLANG_COMPILE_H

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include <stddef.h>

DE_EXTERN_C_BEGIN

/** What a compile produces. */
typedef enum de__slang_target {
    DE__SLANG_SPIRV = 0,    /**< SPIR-V 1.6 binary (Vulkan 1.3) */
    DE__SLANG_DXIL = 1,     /**< DXIL, shader model 6.6 (needs DXC on the host) */
    DE__SLANG_METALLIB = 2, /**< Metal library (needs the Metal toolchain: macOS) */
    DE__SLANG_METAL = 3,    /**< Metal source text (tests and debugging) */
    DE__SLANG_TARGET_COUNT_
} de__slang_target;

/** "spirv", "dxil", "metallib", "metal". */
const char *de__slang_target_name(de__slang_target t);

/**
 * Loads Slang if it is not loaded yet and creates the global session. True when Slang can be
 * used; otherwise false with the reason in `msg` (no library in this build, the library does not
 * open, the session fails). The result of the first call is kept. Any thread.
 */
bool de__slang_available(char *msg, size_t cap);

/** True when Slang is available and can produce `t` on this host (DXC for DXIL, the Metal
 * toolchain for metallib). Any thread. */
bool de__slang_target_supported(de__slang_target t);

/** The Slang version the shim loaded ("2026.19"), or "" when Slang is not available. */
const char *de__slang_version(void);

/**
 * Reads a file that Slang asks for, `path` as Slang names it (relative to the asset root, `/`
 * separators: the source's own directory joined with the import name). Returns DE_OK with the
 * bytes (valid until the compile returns), DE_ERR_NOT_FOUND when there is no such file (Slang then
 * tries its next search location), or another error.
 */
typedef de_result (*de__slang_read_fn)(void *user, const char *path, const void **data, u64 *size);

/** A module the shim serves from memory (shaders/common/), found by `import <name>;`. */
typedef struct de__slang_module {
    const char *name; /**< "bindless": served as "<name>.slang" */
    const char *source;
    size_t size;
} de__slang_module;

/** One entry point to compile. */
typedef struct de__slang_entry {
    const char *name;
    u32 stage; /**< de__shader_stage; DE__SHADER_STAGE_NONE: from its [shader("...")] attribute */
} de__slang_entry;

/** What one entry point compiled to. `code` comes from the call's allocator. */
typedef struct de__slang_entry_out {
    void *code;
    u64 code_size;
    u32 stage; /**< de__shader_stage */
    u32 threadgroup[3];
    u32 push_constant_bytes; /**< entry-point uniforms and push-constant buffers, this layout */
} de__slang_entry_out;

typedef struct de__slang_compile_desc {
    de__slang_target target;
    const char *path; /**< the source, relative to the asset root ("shaders/blur.slang") */
    const char *source;
    u64 source_size;
    /** Preprocessor defines, each "NAME" or "NAME=VALUE". The target define
     * (DE_SHADER_TARGET_SPIRV, _DXIL or _METAL = 1) is added by the shim. */
    const char *const *defines;
    u32 define_count;
    /** In-memory modules, found by `import <name>;`. The first `preload_count` are loaded into
     * every program before the source, so their parameters (the Metal bindless heap) get the
     * first slots whether the source imports them or not. */
    const de__slang_module *modules;
    u32 module_count;
    u32 preload_count;
    de__slang_read_fn read;
    void *read_user;
    /** Entry points; NULL with entry_count 0 asks for every [shader("...")] entry point of the
     * source, in source order (de__slang_compile_result.entry_names then lists them). */
    const de__slang_entry *entries;
    u32 entry_count;
    /** Code buffers, entry names and the result arrays; must be valid. */
    const de_allocator *allocator;
} de__slang_compile_desc;

typedef struct de__slang_compile_result {
    de__slang_entry_out *entries; /**< one per entry point */
    u32 entry_count;
    /** The entry names, in the order of `entries` (copies). */
    char **entry_names;
    /** Slang's warnings (also on success), or the error. NUL-terminated, possibly cut. */
    char diagnostics[2048];

    /* Private. */
    const de_allocator *alloc_;
} de__slang_compile_result;

/**
 * Compiles `d->source` for `d->target`. On success `out` holds the code and the reflection of
 * each entry point (free it with de__slang_compile_result_free). On failure `out->diagnostics`
 * says why and nothing needs freeing.
 * @return DE_OK; DE_ERR_UNSUPPORTED (Slang or the target is not available); DE_ERR_PARSE (the
 *         source does not compile, or an entry point is missing); DE_ERR_INVALID_ARGUMENT;
 *         DE_ERR_OUT_OF_MEMORY.
 */
de_result de__slang_compile(const de__slang_compile_desc *d, de__slang_compile_result *out);

/** Frees what a successful compile returned and zeroes `r`. A zeroed result is ignored. */
void de__slang_compile_result_free(de__slang_compile_result *r);

DE_EXTERN_C_END

#endif /* DE_COOK_SLANG_COMPILE_H */
