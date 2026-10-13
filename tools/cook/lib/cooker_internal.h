/*
 * cooker_internal.h: what the files of the cooker library share (DENG-209): the cooker struct,
 * its type table, and the import context. Private to tools/cook/lib/.
 */
#ifndef DE_COOKER_INTERNAL_H
#define DE_COOKER_INTERNAL_H

#include "cook/lib/de_cooker.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "asset/manifest_internal.h"
#include "platform/de_atomic.h"

DE_EXTERN_C_BEGIN

/** Most types one cooker holds (as the runtime registry). */
#define DE__COOKER_MAX_TYPES 256u
/** Longest type name, without the NUL (as the runtime registry). */
#define DE__COOKER_TYPE_NAME_MAX 63u
/** Longest import failure message, with the NUL. */
#define DE__COOKER_MSG_MAX 512u

/** One registered type. Written once before the count that publishes it; never changed. */
typedef struct de__cooker_type {
    char name[DE__COOKER_TYPE_NAME_MAX + 1u];
    u64 name_hash;
    u32 importer_version;
    de_result (*import)(de_asset_import_ctx *ctx);
    void *user;
} de__cooker_type;

struct de_cooker {
    const de_allocator *alloc;
    de_task_system *tasks;
    de_cook_platform platform; /**< resolved */
    bool verify;
    char *cache_dir; /**< copy, or NULL */
    size_t cache_dir_size;

    de__cooker_type *types;   /**< DE__COOKER_MAX_TYPES entries */
    de_atomic_u32 type_count; /**< release on register, acquire on read */

    struct de__decache *cache; /**< DENG-210: the `.decache` of cache_dir (decache.h), or NULL */
};

/** The registered type `name`, or NULL (importer_registry.c). Any thread. */
const de__cooker_type *de__cooker_find_type(const de_cooker *c, const char *name);

/* ---- Import context (import_ctx.c) ---------------------------------------------------------- */

/** One block of the scratch allocator: a doubly linked header in front of the memory. */
typedef struct de__scratch_block de__scratch_block;

/** A sub-asset as the importer declared it (owned by the context). */
typedef struct de__import_sub {
    char *name;
    char *type;
    de_asset_id id;
    u8 *data;
    u64 size;
} de__import_sub;

struct de_asset_import_ctx {
    de_cooker *c;
    const de__cooker_type *type;

    /* Inputs (borrowed for the import). */
    de_asset_id id;
    const char *id_str;
    const char *asset_root; /**< normalised virtual directory */
    const char *source_rel; /**< normalised, relative to asset_root */
    const char *source_dir; /**< normalised virtual directory of the source */
    const void *source;
    u64 source_size;
    const de__manifest_kv *settings;
    u32 setting_count;

    /* Scratch allocator (de_asset_import_allocator). */
    de_allocator scratch;
    de__scratch_block *scratch_head;

    /* Outputs, from c->alloc. */
    u8 *out;
    u64 out_size, out_cap;
    de_asset_id *deps;
    u32 dep_count, dep_cap;
    de__import_sub *subs;
    u32 sub_count, sub_cap;
    char **extra; /**< relative to asset_root */
    u32 extra_count, extra_cap;

    /* Failure. */
    bool failed;
    de_result fail_code;
    char msg[DE__COOKER_MSG_MAX];
};

/** Prepares a context for one import (outputs empty). */
void de__import_ctx_init(de_asset_import_ctx *ctx, de_cooker *c, const de__cooker_type *type);
/** Frees every output and every scratch block that is left. */
void de__import_ctx_release(de_asset_import_ctx *ctx);
/** Frees the scratch blocks that are left (after the import returned). */
void de__import_ctx_scratch_release(de_asset_import_ctx *ctx);
/** Records a failure unless one is recorded (as de_asset_import_fail, from cooker code). */
void de__import_ctx_fail(de_asset_import_ctx *ctx, de_result code, const char *fmt, ...)
    DE_PRINTF_FMT(3, 4);
/** XXH3-64 over every output of the context: bytes, dependencies, sub-assets, extra sources. */
u64 de__import_ctx_output_hash(const de_asset_import_ctx *ctx);

/** Runs the importer of ctx->type (or the pass-through copy) on a prepared context and frees its
 * scratch blocks: DE_OK, or the failure recorded in ctx->fail_code and ctx->msg (cooker.c). */
de_result de__cooker_run_import(de_asset_import_ctx *ctx);

/** True if normalised virtual path `path` is `root` or below it; `*rel` gets the part below
 * (without the leading '/'; "" for the root itself). */
bool de__cook_path_below(const char *root, const char *path, const char **rel);

DE_EXTERN_C_END

#endif /* DE_COOKER_INTERNAL_H */
