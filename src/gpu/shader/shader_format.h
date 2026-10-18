/*
 * shader_format.h: the cooked `shader` asset (SPEC §11.6, §12.2; DENG-289). The shader importer
 * (tools/cook/importers/shader/) writes it; the runtime type (shader_type.c) reads it. Header only:
 * the importer includes it as "gpu/shader/shader_format.h" and links nothing of de_gpu_shader.
 *
 * One blob holds every compiled variant of one `.slang` source: per target (SPIR-V, DXIL,
 * metallib), per permutation (a set of preprocessor defines) and per entry point, with the
 * reflection of each. Little-endian; every offset counts from the start of the blob.
 *
 *   de__shader_header                     32 bytes
 *   de__shader_entry[entry_count]         8 bytes each: name (string offset), stage
 *   de__shader_permutation[perm_count]    4 bytes each: defines (string offset), "A=1;B=0"
 *   de__shader_record[record_count]       48 bytes each, sorted by (target, permutation, entry)
 *   strings                               NUL-terminated UTF-8, `strings_size` bytes
 *   code                                  each record's code at a 16-byte aligned offset
 *
 * A target the cooking host could not compile (DXIL without DXC, metallib off macOS) has no
 * records: de__shader_blob then fails with DE_ERR_NOT_FOUND. The target enum is the format's own
 * (not de_gpu_backend, which DENG-281 renumbered).
 */
#ifndef DE_GPU_SHADER_FORMAT_H
#define DE_GPU_SHADER_FORMAT_H

#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

/** "DSHD", little-endian. */
#define DE__SHADER_MAGIC 0x44485344u
/** Bump on any layout change; the loader refuses other versions. */
#define DE__SHADER_VERSION 1u
/** Alignment of each code blob inside the cooked asset. */
#define DE__SHADER_CODE_ALIGN 16u
/** The largest push-constant block on every backend (SPEC §12.1). */
#define DE__SHADER_MAX_PUSH_CONSTANT_BYTES 128u

/** Compile targets of the cooked format. */
typedef enum de__shader_target {
    DE__SHADER_TARGET_NONE = 0,
    DE__SHADER_TARGET_SPIRV = 1,    /**< Vulkan 1.3, SPIR-V 1.6 */
    DE__SHADER_TARGET_DXIL = 2,     /**< D3D12, shader model 6.6 */
    DE__SHADER_TARGET_METALLIB = 3, /**< Metal 3 library */
    DE__SHADER_TARGET_COUNT_
} de__shader_target;

/** Pipeline stages. */
typedef enum de__shader_stage {
    DE__SHADER_STAGE_NONE = 0,
    DE__SHADER_STAGE_VERTEX = 1,
    DE__SHADER_STAGE_PIXEL = 2,
    DE__SHADER_STAGE_COMPUTE = 3,
    DE__SHADER_STAGE_MESH = 4,
    DE__SHADER_STAGE_AMPLIFICATION = 5, /**< task shader on Vulkan, object shader on Metal */
    DE__SHADER_STAGE_RAYGEN = 6,
    DE__SHADER_STAGE_MISS = 7,
    DE__SHADER_STAGE_CLOSEST_HIT = 8,
    DE__SHADER_STAGE_ANY_HIT = 9,
    DE__SHADER_STAGE_COUNT_
} de__shader_stage;

/** Bindless tables an entry point reads (shaders/common/bindless.slang). */
typedef enum de__shader_bindless_flags {
    DE__SHADER_USES_TEXTURES = 1u << 0,         /**< de_tex2d */
    DE__SHADER_USES_STORAGE_TEXTURES = 1u << 1, /**< de_rwtex2d */
    DE__SHADER_USES_SAMPLERS = 1u << 2,         /**< de_sampler */
    DE__SHADER_USES_BUFFERS = 1u << 3           /**< de_buffer */
} de__shader_bindless_flags;

typedef struct de__shader_header {
    u32 magic;   /**< DE__SHADER_MAGIC */
    u32 version; /**< DE__SHADER_VERSION */
    u32 entry_count;
    u32 permutation_count;
    u32 record_count;
    u32 strings_offset;
    u32 strings_size;
    u32 reserved; /**< 0 */
} de__shader_header;

typedef struct de__shader_entry {
    u32 name;  /**< string offset (relative to strings_offset) */
    u32 stage; /**< de__shader_stage */
} de__shader_entry;

typedef struct de__shader_permutation {
    u32 defines; /**< string offset: the permutation as written in the settings */
} de__shader_permutation;

/** One compiled variant and its reflection. */
typedef struct de__shader_record {
    u32 target;      /**< de__shader_target */
    u32 permutation; /**< index into the permutation table */
    u32 entry;       /**< index into the entry table */
    u32 stage;       /**< de__shader_stage (the entry's) */
    u32 code_offset; /**< from the start of the blob, DE__SHADER_CODE_ALIGN-aligned */
    u32 code_size;
    u32 threadgroup[3];      /**< compute, mesh, amplification; 0 otherwise */
    u32 push_constant_bytes; /**< on this target's layout; <= 128 */
    u32 bindless_flags;      /**< de__shader_bindless_flags */
    u32 mesh_limits;         /**< mesh: max vertices (low 16 bits), max primitives (high 16) */
} de__shader_record;

DE_STATIC_ASSERT(sizeof(de__shader_header) == 32, "cooked shader header layout");
DE_STATIC_ASSERT(sizeof(de__shader_entry) == 8, "cooked shader entry layout");
DE_STATIC_ASSERT(sizeof(de__shader_permutation) == 4, "cooked shader permutation layout");
DE_STATIC_ASSERT(sizeof(de__shader_record) == 48, "cooked shader record layout");

DE_EXTERN_C_END

#endif /* DE_GPU_SHADER_FORMAT_H */
