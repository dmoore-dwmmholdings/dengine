#include "aseprite_import.h"
#include "anim/sprite_clip_format.h"
#include "cook/lib/de_cooker.h"
#include <dengine/de_platform.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUTPUT_LIMIT (256u * 1024u * 1024u)
static int hash_order(const void *left, const void *right) {
    u64 a, b;
    memcpy(&a, left, sizeof a);
    memcpy(&b, right, sizeof b);
    return a < b ? -1 : a > b;
}
static int record_order(const void *left, const void *right) {
    const u8 *a = left, *b = right;
    u64 x = de_cooked_get_u64(a), y = de_cooked_get_u64(b);
    if (x != y)
        return x < y ? -1 : 1;
    u32 af = de_cooked_get_u32(a + 8), bf = de_cooked_get_u32(b + 8);
    return af < bf ? -1 : af > bf;
}
static u64 name_hash(de_aseprite_string name) {
    return name.size ? de_hash64(name.data, name.size, 0) : 0;
}
static const de_aseprite_slice_key *key_at(const de_aseprite_slice *slice, u32 frame) {
    u32 lo = 0, hi = slice->key_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (slice->keys[mid].frame <= frame)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo ? &slice->keys[lo - 1] : NULL;
}
static bool same_name(de_aseprite_string name, const char *text) {
    return strlen(text) == name.size && !memcmp(name.data, text, name.size);
}
static bool clip_size(const de_aseprite_doc *doc, u64 *size, u32 *keys) {
    if (doc->tag_count > DE__SPRITE_TAG_LIMIT || doc->slice_count > DE__SPRITE_SLICE_LIMIT)
        return false;
    u64 count = 0;
    for (u32 i = 0; i < doc->slice_count; ++i)
        count += doc->slices[i].key_count;
    if (count > DE__SPRITE_SLICE_LIMIT)
        return false;
    *keys = (u32)count;
    *size = 96u + (u64)doc->frame_count * 64u + (u64)doc->tag_count * 24u + count * 64u;
    return true;
}
de_result de_aseprite_clip_encode(const de_aseprite_doc *doc, const de_aseprite_atlas *atlas,
                                  u32 set, de_asset_id texture, f32 ppm, const char *pivot_slice,
                                  const de_allocator *allocator, void **data, u64 *bytes,
                                  const char **why) {
    if (data)
        *data = NULL;
    if (bytes)
        *bytes = 0;
    if (why)
        *why = "invalid Aseprite clip arguments";
    if (!doc || !atlas || !allocator || !data || !bytes || !texture.hash || !atlas->sets ||
        !atlas->frames || !atlas->width || !atlas->height || !de__sprite_finite(ppm) ||
        !(ppm > 0) || set >= atlas->set_count || !doc->frame_count ||
        atlas->frames_per_set != doc->frame_count || atlas->canvas_width != doc->width ||
        atlas->canvas_height != doc->height ||
        (u64)atlas->sets[set].first_frame + doc->frame_count > atlas->frame_count ||
        (doc->tag_count && !doc->tags) || (doc->slice_count && !doc->slices))
        return DE_ERR_INVALID_ARGUMENT;
    u64 size;
    u32 keys;
    if (!clip_size(doc, &size, &keys)) {
        if (why)
            *why = "Aseprite tags/slice keys exceed sprite clip limits";
        return DE_ERR_UNSUPPORTED;
    }
    const char *message = "Aseprite clip allocation failed";
    de_result result = DE_ERR_OUT_OF_MEMORY;
    u8 *out = DE_ALLOC(allocator, (size_t)size, 8, DE_MEM_ASSET);
    u64 *slice_names = NULL;
    size_t names_bytes = (size_t)doc->slice_count * sizeof *slice_names;
    if (!out)
        goto done;
    memset(out, 0, (size_t)size);
    const de_aseprite_slice *pivot = NULL;
    if (names_bytes) {
        slice_names = DE_ALLOC(allocator, names_bytes, 8, DE_MEM_ASSET);
        if (!slice_names)
            goto done;
    }
    for (u32 i = 0; i < doc->slice_count; ++i) {
        const de_aseprite_slice *slice = &doc->slices[i];
        slice_names[i] = name_hash(slice->name);
        if ((slice->flags & 2u) && (pivot_slice ? same_name(slice->name, pivot_slice) : !pivot))
            pivot = slice;
    }
    if (doc->slice_count)
        qsort(slice_names, doc->slice_count, sizeof *slice_names, hash_order);
    for (u32 i = 0; i < doc->slice_count; ++i)
        if (!slice_names[i] || (i && slice_names[i] == slice_names[i - 1])) {
            message = "Aseprite slice names must be nonempty and have unique hashes";
            result = DE_ERR_INVALID_ARGUMENT;
            goto done;
        }
    if (pivot_slice && !pivot) {
        message = "pivot_slice must name a slice with pivot information";
        result = DE_ERR_INVALID_ARGUMENT;
        goto done;
    }
    memcpy(out, "DSCP", 4);
    u32 tags = 96u + (u32)doc->frame_count * 64u;
    u32 slices = tags + doc->tag_count * 24u;
    de_cooked_put_u32(out + 4, DE__SPRITE_CLIP_GEOMETRY_VERSION);
    de_cooked_put_u32(out + 8, (u32)size);
    de_cooked_put_u32(out + 12, doc->frame_count);
    de_cooked_put_u32(out + 16, doc->tag_count);
    de_cooked_put_u32(out + 24, 96);
    de_cooked_put_u32(out + 28, tags);
    de_cooked_put_u32(out + 32, slices); /* no generated frame events */
    de_cooked_put_u32(out + 36, atlas->width);
    de_cooked_put_u32(out + 40, atlas->height);
    de_cooked_put_u64(out + 48, texture.hash);
    de_cooked_put_u32(out + 64, doc->width);
    de_cooked_put_u32(out + 68, doc->height);
    de_cooked_put_f32(out + 72, ppm);
    de_cooked_put_u32(out + 76, keys);
    de_cooked_put_u32(out + 80, slices);
    u64 duration = 0;
    for (u32 i = 0; i < doc->frame_count; ++i) {
        const de_aseprite_atlas_frame *f = &atlas->frames[atlas->sets[set].first_frame + i];
        u8 *p = out + 96u + i * 64u;
        f32 origin[2] = {(f32)doc->width * .5f, (f32)doc->height * .5f};
        const de_aseprite_slice_key *key = pivot ? key_at(pivot, i) : NULL;
        if (key && key->width && key->height) {
            origin[0] = (f32)((i64)key->x + key->pivot_x);
            origin[1] = (f32)((i64)key->y + key->pivot_y);
        }
        de_cooked_put_f32(p, (f32)f->x / atlas->width);
        de_cooked_put_f32(p + 4, (f32)f->y / atlas->height);
        de_cooked_put_f32(p + 8, (f32)f->width / atlas->width);
        de_cooked_put_f32(p + 12, (f32)f->height / atlas->height);
        de_cooked_put_u32(p + 16, f->ms);
        u32 trim[] = {f->trim_x, f->trim_y, f->trim_width, f->trim_height};
        for (u32 k = 0; k < 4; ++k)
            de_cooked_put_u32(p + 32u + k * 4u, trim[k]);
        for (u32 k = 0; k < 2; ++k) {
            f32 normalized = trim[k + 2] ? (origin[k] - trim[k]) / trim[k + 2] : .5f;
            /* Source pixels grow down from the top; the renderer's unit quad grows up. */
            if (k == 1)
                normalized = 1 - normalized;
            de_cooked_put_f32(p + 20u + k * 4u, normalized);
            de_cooked_put_f32(p + 48u + k * 4u, (f32)trim[k + 2] / ppm);
            de_cooked_put_f32(p + 56u + k * 4u, origin[k] / (k ? doc->height : doc->width));
        }
        duration += f->ms;
    }
    de_cooked_put_u64(out + 56, duration);
    for (u32 i = 0; i < doc->tag_count; ++i) {
        const de_aseprite_tag *tag = &doc->tags[i];
        u8 *p = out + tags + i * 24u;
        de_cooked_put_u64(p, name_hash(tag->name));
        de_cooked_put_u32(p + 8, tag->from);
        de_cooked_put_u32(p + 12, tag->to);
        de_cooked_put_u32(p + 16, tag->direction);
        de_cooked_put_u32(p + 20, tag->repeats);
    }
    if (doc->tag_count)
        qsort(out + tags, doc->tag_count, 24, record_order);
    u32 entry = 0;
    for (u32 i = 0; i < doc->slice_count; ++i) {
        const de_aseprite_slice *slice = &doc->slices[i];
        for (u32 j = 0; j < slice->key_count; ++j) {
            const de_aseprite_slice_key *key = &slice->keys[j];
            u8 *p = out + slices + entry++ * 64u;
            de_cooked_put_u64(p, name_hash(slice->name));
            de_cooked_put_u32(p + 8, key->frame);
            de_cooked_put_u32(p + 12, slice->flags & 3u);
            de_cooked_put_u32(p + 16, (u32)key->x);
            de_cooked_put_u32(p + 20, (u32)key->y);
            de_cooked_put_u32(p + 24, key->width);
            de_cooked_put_u32(p + 28, key->height);
            de_cooked_put_u32(p + 32, (u32)key->center_x);
            de_cooked_put_u32(p + 36, (u32)key->center_y);
            de_cooked_put_u32(p + 40, key->center_width);
            de_cooked_put_u32(p + 44, key->center_height);
            de_cooked_put_u32(p + 48, (u32)key->pivot_x);
            de_cooked_put_u32(p + 52, (u32)key->pivot_y);
        }
    }
    if (keys)
        qsort(out + slices, keys, 64, record_order);
    de__sprite_clip_view checked;
    if (!de__sprite_clip_parse(out, size, &checked, NULL)) {
        message = "Aseprite metadata cannot form a valid sprite clip";
        result = DE_ERR_INVALID_ARGUMENT;
        goto done;
    }
    *data = out;
    *bytes = size;
    out = NULL;
    message = NULL;
    result = DE_OK;
done:
    DE_FREE(allocator, out, (size_t)size);
    DE_FREE(allocator, slice_names, names_bytes);
    if (why)
        *why = message;
    return result;
}

static void warn(void *user, de_aseprite_string layer, const char *message) {
    (void)user;
    de_log_category category = de_log_register_category("aseprite", DE_LOG_INFO);
    DE_WARN(category, "%.*s: %s", (int)layer.size, layer.data ? layer.data : "", message);
}
static bool setting_type(de_asset_import_ctx *ctx, const char *key, de_asset_setting_type type) {
    const de_asset_setting_value *value = de_asset_import_setting_value(ctx, key);
    return !value || value->type == type;
}
de_result de_aseprite_import(de_asset_import_ctx *ctx) {
    const de_allocator *allocator = de_asset_import_allocator(ctx);
    const char *layers = de_asset_import_setting_string(ctx, "layers", "flatten");
    const de_asset_setting_value *density = de_asset_import_setting_value(ctx, "pixels_per_meter");
    f64 requested_ppm = de_asset_import_setting_float(ctx, "pixels_per_meter", 100);
    f32 ppm = requested_ppm > 0 && requested_ppm <= FLT_MAX ? (f32)requested_ppm : 0;
    i64 maximum = de_asset_import_setting_int(ctx, "atlas_max_size", 4096);
    if (!setting_type(ctx, "layers", DE_ASSET_SETTING_STRING) ||
        (strcmp(layers, "flatten") && strcmp(layers, "keep")) ||
        !setting_type(ctx, "pixel_art", DE_ASSET_SETTING_BOOL) ||
        !setting_type(ctx, "pivot_slice", DE_ASSET_SETTING_STRING) ||
        !setting_type(ctx, "atlas_max_size", DE_ASSET_SETTING_INT) || maximum < 3 ||
        maximum > 16384 ||
        (density && density->type != DE_ASSET_SETTING_INT &&
         density->type != DE_ASSET_SETTING_FLOAT) ||
        !de__sprite_finite(ppm) || !(ppm > 0))
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                    "invalid Aseprite import settings");
    de_aseprite_doc *doc = NULL;
    de_aseprite_atlas *atlas = NULL;
    void *texture = NULL, *clip = NULL;
    u64 source_bytes, texture_bytes = 0, clip_bytes = 0;
    const void *source = de_asset_import_source(ctx, &source_bytes);
    const char *why = NULL;
    de_result result = de_aseprite_parse(source, source_bytes, allocator, 0, &doc, &why);
    if (result != DE_OK)
        goto done;
    de_aseprite_atlas_desc options = {
        .keep_layers = !strcmp(layers, "keep"), .max_size = (u32)maximum, .warning = warn};
    result = de_aseprite_atlas_build(doc, allocator, &options, &atlas, &why);
    if (result != DE_OK)
        goto done;
    char full[DE_FS_PATH_MAX], name[DE_FS_PATH_MAX];
    const char *id = de_cooker_import_id_str(ctx);
    int n = snprintf(full, sizeof full, "%s#atlas", id);
    de_asset_id texture_id =
        n > 0 && (size_t)n < sizeof full ? de_asset_id_from(full) : (de_asset_id){0};
    if (!texture_id.hash) {
        result = DE_ERR_INVALID_ARGUMENT;
        why = "Aseprite atlas asset ID is invalid or too long";
        goto done;
    }
    result = de_aseprite_atlas_encode(atlas, allocator,
                                      de_asset_import_setting_bool(ctx, "pixel_art", false),
                                      &texture, &texture_bytes);
    if (result != DE_OK)
        goto done;
    u64 one_clip;
    u32 keys;
    if (!clip_size(doc, &one_clip, &keys) ||
        one_clip * atlas->set_count + texture_bytes > OUTPUT_LIMIT) {
        result = DE_ERR_UNSUPPORTED;
        why = "Aseprite aggregate cooked output exceeds 256 MiB";
        goto done;
    }
    result = de_asset_import_add_subasset(ctx, "atlas", "texture", texture, texture_bytes);
    DE_FREE(allocator, texture, (size_t)texture_bytes);
    texture = NULL;
    if (result != DE_OK)
        goto done;
    for (u32 set = 0; set < atlas->set_count; ++set) {
        result = de_aseprite_clip_encode(doc, atlas, set, texture_id, ppm,
                                         de_asset_import_setting_string(ctx, "pivot_slice", NULL),
                                         allocator, &clip, &clip_bytes, &why);
        if (result != DE_OK)
            goto done;
        if (!set)
            result = de_asset_import_write(ctx, clip, clip_bytes);
        else {
            de_aseprite_string layer = atlas->sets[set].name;
            n = snprintf(name, sizeof name, "layer/%.*s", (int)layer.size, layer.data);
            if (!layer.size || n < 0 || (size_t)n >= sizeof name) {
                result = DE_ERR_INVALID_ARGUMENT;
                why = "Aseprite visible layer needs a nonempty, bounded sub-asset name";
            } else
                result = de_asset_import_add_subasset(ctx, name, "sprite_clip", clip, clip_bytes);
        }
        DE_FREE(allocator, clip, (size_t)clip_bytes);
        clip = NULL;
        if (result != DE_OK)
            goto done;
    }
    result = de_asset_import_add_dependency(ctx, texture_id);
done:
    DE_FREE(allocator, clip, (size_t)clip_bytes);
    DE_FREE(allocator, texture, (size_t)texture_bytes);
    de_aseprite_atlas_free(atlas);
    de_aseprite_free(doc);
    if (result != DE_OK)
        return de_asset_import_fail(ctx, result, "%s: %s", de_asset_import_source_path(ctx),
                                    why ? why : "Aseprite output failed");
    return DE_OK;
}
de_asset_type_desc de_aseprite_import_type_desc(void) {
    static const char *const extensions[] = {".aseprite", ".ase"};
    return (de_asset_type_desc){.name = "aseprite",
                                .extensions = extensions,
                                .extension_count = 2,
                                .importer_version = 1,
                                .import = de_aseprite_import};
}
