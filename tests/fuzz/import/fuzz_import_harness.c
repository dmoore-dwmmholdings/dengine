/*
 * fuzz_import_harness.c: the in-memory import harness of the importer fuzz targets (DENG-106).
 * See fuzz_import_harness.h.
 *
 * Setup is done once, lazily (the standalone corpus runner has no LLVMFuzzerInitialize): one
 * cooker with the `model` and `texture` importers, and the `cook` log category silenced so the
 * importers' warnings do not slow the fuzzer down. The cooker is never destroyed; it is reachable
 * from a static, so LeakSanitizer does not report it. Everything an import allocates is freed
 * before fz_import_run returns.
 */
#include "fuzz_import_harness.h"

#include "fuzz_cooked_walk.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "anim/formats/de_cooked_clip.h"
#include "asset/manifest_internal.h"
#include "cook/importers/model/model_import.h"
#include "cook/importers/texture/texture_import.h"
#include "cook/lib/cooker_internal.h"
#include "cook/lib/de_cooker.h"
#include "render/formats/de_cooked_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Setup ----------------------------------------------------------------------------------- */

static de_cooker *g_cooker;

/* The texture importer as the harness registers it: every texture cook, the model's nested ones
 * included (they get only the model's settings), runs with `compression = "none"` and
 * `max_size` 64 unless the settings say otherwise, so fuzzing stays on the decoders and the mip
 * pipeline rather than on the BC encoders (DENG-99), and one input stays fast. */
static de_result fz_texture_import(de_asset_import_ctx *ctx) {
    de__manifest_kv kv[24];
    u32 n = 0;
    bool has_compression = false, has_max_size = false;
    for (u32 i = 0; i < ctx->setting_count && n < 22u; ++i) {
        kv[n] = ctx->settings[i];
        has_compression = has_compression || strcmp(kv[n].key, "compression") == 0;
        has_max_size = has_max_size || strcmp(kv[n].key, "max_size") == 0;
        ++n;
    }
    if (!has_compression) {
        memset(&kv[n], 0, sizeof kv[n]);
        kv[n].key = "compression";
        kv[n].value.type = DE__MANIFEST_STRING;
        kv[n++].value.u.s = "none";
    }
    if (!has_max_size) {
        memset(&kv[n], 0, sizeof kv[n]);
        kv[n].key = "max_size";
        kv[n].value.type = DE__MANIFEST_INT;
        kv[n++].value.u.i = FZ_IMPORT_MAX_SIZE;
    }
    const de__manifest_kv *saved = ctx->settings;
    u32 saved_count = ctx->setting_count;
    ctx->settings = kv;
    ctx->setting_count = n;
    de_result r = de_texture_import(ctx);
    ctx->settings = saved;
    ctx->setting_count = saved_count;
    return r;
}

static de_cooker *fz_cooker(void) {
    if (g_cooker != NULL)
        return g_cooker;
    de_log_set_level(de_log_register_category("cook", DE_LOG_INFO), DE_LOG_FATAL);
    de_cooker_desc d;
    memset(&d, 0, sizeof d);
    d.verify_determinism = DE_COOK_VERIFY_OFF;
    de_cooker *c = NULL;
    if (de_cooker_create(&d, &c) != DE_OK)
        abort();
    de_asset_type_desc model;
    memset(&model, 0, sizeof model);
    model.name = "model";
    model.extensions = de_model_import_extensions;
    model.extension_count = DE_MODEL_IMPORT_EXTENSION_COUNT;
    model.importer_version = DE_MODEL_IMPORTER_VERSION;
    model.import = de_model_import;
    de_asset_type_desc texture = de_texture_import_type_desc();
    texture.import = fz_texture_import;
    if (de_cooker_register_type(c, &model) != DE_OK ||
        de_cooker_register_type(c, &texture) != DE_OK)
        abort();
    g_cooker = c;
    return c;
}

const char *fz_import_kind_name(fz_import_kind kind) {
    static const char *const names[FZ_IMPORT_KIND_COUNT] = {"gltf", "fbx", "image", "ktx2"};
    return (u32)kind < (u32)FZ_IMPORT_KIND_COUNT ? names[kind] : "?";
}

/* ---- Settings -------------------------------------------------------------------------------- */

static de__manifest_kv kv_int(const char *k, i64 v) {
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = k;
    kv.value.type = DE__MANIFEST_INT;
    kv.value.u.i = v;
    return kv;
}

static de__manifest_kv kv_str(const char *k, const char *v) {
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = k;
    kv.value.type = DE__MANIFEST_STRING;
    kv.value.u.s = v;
    return kv;
}

static de__manifest_kv kv_float(const char *k, f64 v) {
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = k;
    kv.value.type = DE__MANIFEST_FLOAT;
    kv.value.u.f = v;
    return kv;
}

static de__manifest_kv kv_bool(const char *k, bool v) {
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = k;
    kv.value.type = DE__MANIFEST_BOOL;
    kv.value.u.b = v;
    return kv;
}

/* FNV-1a of the input: picks texture settings, so one input always gets the same ones and the
 * corpus files stay plain image files. */
static u32 fz_hash(const u8 *p, size_t n) {
    u32 h = 2166136261u;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

/* The settings of one import; returns the count. */
static u32 fz_settings(fz_import_kind kind, const u8 *data, size_t size, de__manifest_kv *kv) {
    u32 n = 0;
    kv[n++] = kv_int("max_decoded_mib", FZ_IMPORT_MAX_DECODED_MIB);
    if (kind == FZ_IMPORT_GLTF || kind == FZ_IMPORT_FBX) {
        /* Clip compression (DENG-104) costs up to 256 track evaluations per sample, track and
         * pass, and a hostile duration makes the sampling grid long: a 3.7 KB glTF with a
         * 13584 s animation took 14 s per input at 30 Hz in a Debug ASan build (CI's limit is
         * 10 s). A grid of one sample per 1000 s keeps the time table to the source key times,
         * which a fuzz input can only hold a few thousand of. Smaller error bounds do not help:
         * they add check passes. */
        kv[n++] = kv_float("anim_sample_rate", FZ_IMPORT_ANIM_SAMPLE_RATE);
        return n;
    }
    static const char *const usages[4] = {"color", "normal", "mask", "environment"};
    u32 h = fz_hash(data, size);
    kv[n++] = kv_int("max_size", FZ_IMPORT_MAX_SIZE);
    kv[n++] = kv_str("usage", usages[h & 3u]);
    if ((h & 3u) == 3u) {
        /* An environment (DENG-100): a small cube and few GGX samples keep one input fast. */
        kv[n++] = kv_int("env_size", 8);
        kv[n++] = kv_int("env_samples", 16);
        if ((h & 0x80u) != 0)
            kv[n++] = kv_float("rotation_deg", 90.0);
    }
    kv[n++] = kv_str("address", (h & 4u) != 0 ? "clamp" : "wrap");
    if ((h & 8u) != 0)
        kv[n++] = kv_float("alpha_coverage", 0.5);
    if ((h & 0x70u) == 0)
        kv[n++] = kv_bool("pixel_art", true);
    /* One input in eight goes through the BC encoders (DENG-99), at their fast setting; the
     * others cook uncompressed (fz_texture_import). */
    if ((h & 0x700u) == 0) {
        kv[n++] = kv_str("compression", "auto");
        kv[n++] = kv_str("quality", "fast");
    }
    return n;
}

/* ---- Output checks --------------------------------------------------------------------------- */

static void fz_fail(fz_import_kind kind, const char *what, const char *name, const char *why) {
    fprintf(stderr, "fuzz_%s: the importer wrote a %s that its parser refuses (%s): %s\n",
            fz_import_kind_name(kind), what, name != NULL ? name : "main output",
            why != NULL ? why : "?");
    abort();
}

static const de__import_sub *fz_sub_by_id(const de_asset_import_ctx *ctx, const char *type,
                                          u64 id) {
    for (u32 i = 0; i < ctx->sub_count; ++i)
        if (ctx->subs[i].id.hash == id && strcmp(ctx->subs[i].type, type) == 0)
            return &ctx->subs[i];
    return NULL;
}

static void fz_check_texture_output(fz_import_kind kind, const de_asset_import_ctx *ctx,
                                    bool environment) {
    const char *why = NULL;
    if (environment) {
        if (!fz_walk_env(ctx->out, ctx->out_size, &why))
            fz_fail(kind, "DTEX + ENVS environment", NULL, why);
    } else if (!fz_walk_texture(ctx->out, ctx->out_size, &why)) {
        fz_fail(kind, "DTEX", NULL, why);
    }
    if (ctx->sub_count != 0)
        fz_fail(kind, "sub-asset", ctx->subs[0].name, "the texture importer declares none");
}

static void fz_check_model_output(fz_import_kind kind, const de_asset_import_ctx *ctx) {
    const char *why = NULL;
    u32 counts[5] = {0, 0, 0, 0, 0}; /* mesh, material, texture, skeleton, clip */
    for (u32 i = 0; i < ctx->sub_count; ++i) {
        const de__import_sub *s = &ctx->subs[i];
        why = NULL;
        if (strcmp(s->type, "mesh") == 0) {
            ++counts[0];
            if (!fz_walk_mesh(s->data, s->size, &why))
                fz_fail(kind, "DMSH", s->name, why);
        } else if (strcmp(s->type, "material") == 0) {
            ++counts[1];
            if (!fz_walk_material(s->data, s->size, &why))
                fz_fail(kind, "DMAT", s->name, why);
        } else if (strcmp(s->type, "texture") == 0) {
            ++counts[2];
            if (!fz_walk_texture(s->data, s->size, &why))
                fz_fail(kind, "DTEX", s->name, why);
        } else if (strcmp(s->type, "skeleton") == 0) {
            ++counts[3];
            if (!fz_walk_skeleton(s->data, s->size, &why))
                fz_fail(kind, "DSKL", s->name, why);
        } else if (strcmp(s->type, "anim_clip") == 0) {
            ++counts[4];
            de_cooked_clip_view cv;
            memset(&cv, 0, sizeof cv);
            if (!de_cooked_clip_parse(s->data, s->size, &cv, &why))
                fz_fail(kind, "DCLP", s->name, why);
            const de__import_sub *sk = fz_sub_by_id(ctx, "skeleton", cv.skeleton_id);
            if (sk == NULL)
                fz_fail(kind, "DCLP", s->name, "its skeleton ID names no skeleton sub-asset");
            if (!fz_walk_clip(s->data, s->size, sk->data, sk->size, &why))
                fz_fail(kind, "DCLP", s->name, why);
        } else if (strcmp(s->type, "prefab") != 0) {
            fz_fail(kind, "sub-asset of an unknown type", s->name, s->type);
        }
    }
    de_cooked_model_view mv;
    memset(&mv, 0, sizeof mv);
    if (!fz_walk_model(ctx->out, ctx->out_size, &why) ||
        !de_cooked_model_parse(ctx->out, ctx->out_size, &mv, &why))
        fz_fail(kind, "DMDL", NULL, why);
    if (mv.mesh_count != counts[0] || mv.material_count != counts[1] ||
        mv.texture_count != counts[2] || mv.skeleton_count != counts[3] ||
        mv.clip_count != counts[4])
        fz_fail(kind, "DMDL", NULL, "its counts differ from the sub-assets");
    /* Every ID the model lists names a sub-asset of the right type. */
    for (u32 i = 0; i < mv.mesh_count; ++i)
        if (fz_sub_by_id(ctx, "mesh", de_cooked_model_mesh_id(&mv, i)) == NULL)
            fz_fail(kind, "DMDL", NULL, "a mesh ID names no mesh sub-asset");
    for (u32 i = 0; i < mv.material_count; ++i)
        if (fz_sub_by_id(ctx, "material", de_cooked_model_material_id(&mv, i)) == NULL)
            fz_fail(kind, "DMDL", NULL, "a material ID names no material sub-asset");
    for (u32 i = 0; i < mv.texture_count; ++i)
        if (fz_sub_by_id(ctx, "texture", de_cooked_model_texture_id(&mv, i)) == NULL)
            fz_fail(kind, "DMDL", NULL, "a texture ID names no texture sub-asset");
    for (u32 i = 0; i < mv.skeleton_count; ++i)
        if (fz_sub_by_id(ctx, "skeleton", de_cooked_model_skeleton_id(&mv, i)) == NULL)
            fz_fail(kind, "DMDL", NULL, "a skeleton ID names no skeleton sub-asset");
    for (u32 i = 0; i < mv.clip_count; ++i)
        if (fz_sub_by_id(ctx, "anim_clip", de_cooked_model_clip_id(&mv, i)) == NULL)
            fz_fail(kind, "DMDL", NULL, "a clip ID names no clip sub-asset");
    if (fz_sub_by_id(ctx, "prefab", mv.prefab_id) == NULL)
        fz_fail(kind, "DMDL", NULL, "the prefab ID names no prefab sub-asset");
}

/* ---- Run ------------------------------------------------------------------------------------- */

static bool fz_settings_say_environment(const de__manifest_kv *kv, u32 n) {
    for (u32 i = 0; i < n; ++i)
        if (strcmp(kv[i].key, "usage") == 0 && kv[i].value.type == DE__MANIFEST_STRING &&
            strcmp(kv[i].value.u.s, "environment") == 0)
            return true;
    return false;
}

de_result fz_import_cook(fz_import_kind kind, const u8 *data, size_t size,
                         const de__manifest_kv *settings, u32 setting_count, fz_import_output_fn fn,
                         void *user, char *msg, size_t msg_size) {
    static const char *const sources[FZ_IMPORT_KIND_COUNT] = {"fuzz.gltf", "fuzz.fbx", "fuzz.png",
                                                              "fuzz.ktx2"};
    if (msg != NULL && msg_size != 0)
        msg[0] = '\0';
    if ((u32)kind >= (u32)FZ_IMPORT_KIND_COUNT)
        abort();
    de_cooker *c = fz_cooker();
    bool model = kind == FZ_IMPORT_GLTF || kind == FZ_IMPORT_FBX;
    const de__cooker_type *type = de__cooker_find_type(c, model ? "model" : "texture");
    if (type == NULL)
        abort();

    /* The cooker hands importers the source with a NUL after it (de_file_read_all); so does this
     * copy, and nothing more, so ASan sees a read past it. */
    u8 *source = (u8 *)malloc(size + 1u);
    if (source == NULL)
        abort();
    if (size != 0)
        memcpy(source, data, size);
    source[size] = 0;

    de_asset_import_ctx ctx;
    de__import_ctx_init(&ctx, c, type);
    ctx.id_str = "fuzz/asset";
    ctx.id = de_asset_id_from(ctx.id_str);
    ctx.asset_root = "fuzzmem:/";
    ctx.source_rel = sources[kind];
    ctx.source_dir = "fuzzmem:/";
    ctx.source = source;
    ctx.source_size = size;
    ctx.settings = settings;
    ctx.setting_count = setting_count;
    de_result r = de__cooker_run_import(&ctx);
    if (r == DE_OK) {
        if (model)
            fz_check_model_output(kind, &ctx);
        else
            fz_check_texture_output(kind, &ctx,
                                    fz_settings_say_environment(settings, setting_count));
        if (fn != NULL) {
            fn(NULL, type->name, ctx.out, ctx.out_size, user);
            for (u32 i = 0; i < ctx.sub_count; ++i)
                fn(ctx.subs[i].name, ctx.subs[i].type, ctx.subs[i].data, ctx.subs[i].size, user);
        }
    } else if (msg != NULL && msg_size != 0) {
        snprintf(msg, msg_size, "%s", ctx.msg);
    }
    de__import_ctx_release(&ctx);
    free(source);
    return r;
}

de_result fz_import_run(fz_import_kind kind, const u8 *data, size_t size, char *msg,
                        size_t msg_size) {
    de__manifest_kv settings[12];
    u32 n = fz_settings(kind, data, size, settings);
    return fz_import_cook(kind, data, size, settings, n, NULL, NULL, msg, msg_size);
}
