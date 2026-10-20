#include "text/loc/string_table_format.h"
#include "text/loc/loc_internal.h"
#include "core/de_toml_internal.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
/* Locale strings use first-wins duplicates; all other TOML remains strict. */
static bool locale_duplicate(const toml_duplicate_t *d, void *user) {
    toml_datum_t strings = toml_get(*d->root, "strings");
    if (strings.type != TOML_TABLE || strings.u.tab.value != d->table->u.tab.value ||
        d->previous->type != TOML_STRING || d->replacement->type != TOML_STRING)
        return false;
    de_log_category log = de_log_register_category("text.loc", DE_LOG_INFO);
    DE_WARN(log, "%s:%d: duplicate string key '%.*s'; keeping first value from line %d",
            (const char *)user, d->lineno, d->keylen, d->key, d->previous->lineno);
    return true;
}
static de_result import_locale(de_asset_import_ctx *ctx, const char *file, const void *source,
                               u64 size, void **out, u64 *bytes) {
    if (size > INT_MAX)
        return de_asset_import_fail(ctx, DE_ERR_PARSE, "%s: locale source too large", file);
    const de_allocator *a = de_asset_import_allocator(ctx);
    toml_result_t parsed =
        de__toml_parse_with_duplicates(a, source, (int)size, locale_duplicate, (void *)file);
    if (!parsed.ok) {
        de_result r = de_asset_import_fail(ctx, DE_ERR_PARSE, "%s: %s", file, parsed.errmsg);
        de__toml_free(parsed);
        return r;
    }
    toml_datum_t strings = toml_get(parsed.toptab, "strings");
    if (strings.type != TOML_TABLE) {
        de__toml_free(parsed);
        return de_asset_import_fail(ctx, DE_ERR_PARSE, "%s: missing [strings] table", file);
    }
    u32 count = (u32)strings.u.tab.size;
    de__loc_pair *pairs = DE_ALLOC(a, (size_t)(count ? count : 1) * sizeof *pairs,
                                   DE_ALIGNOF(de__loc_pair), DE_MEM_UI);
    if (!pairs) {
        de__toml_free(parsed);
        return DE_ERR_OUT_OF_MEMORY;
    }
    de_result result = DE_OK;
    for (u32 i = 0; i < count; ++i) {
        const toml_datum_t *value = &strings.u.tab.value[i];
        if (value->type != TOML_STRING) {
            result = de_asset_import_fail(ctx, DE_ERR_PARSE, "%s:%d: string '%s' must be a string",
                                          file, value->lineno, strings.u.tab.key[i]);
            break;
        }
        if (value->u.str.len < 0 || (size_t)value->u.str.len != strlen(value->u.s) ||
            strings.u.tab.len[i] < 0 ||
            (size_t)strings.u.tab.len[i] != strlen(strings.u.tab.key[i])) {
            result = de_asset_import_fail(
                ctx, DE_ERR_PARSE, "%s:%d: embedded NUL in string key/value", file, value->lineno);
            break;
        }
        size_t error = 0;
        if (de__loc_validate(value->u.s, strlen(value->u.s), &error) != DE_OK) {
            result =
                de_asset_import_fail(ctx, DE_ERR_PARSE, "%s:%d: invalid message '%s' at byte %zu",
                                     file, value->lineno, strings.u.tab.key[i], error);
            break;
        }
        pairs[i] = (de__loc_pair){strings.u.tab.key[i], value->u.s, strlen(value->u.s), 0};
    }
    if (result == DE_OK)
        result = de__loc_table_build(a, pairs, count, out, bytes);
    DE_FREE(a, pairs, (size_t)(count ? count : 1) * sizeof *pairs);
    de__toml_free(parsed);
    return result;
}
static de_result import_table(de_asset_import_ctx *ctx) {
    u64 size = 0;
    const u8 *snapshot = de_asset_import_source(ctx, &size);
    if (size < 8 || memcmp(snapshot, "DDIR\1\0\0\0", 8))
        return de_asset_import_fail(
            ctx, DE_ERR_PARSE,
            "string_table source must be a nonrecursive directory of locale TOML files");
    const de_allocator *a = de_asset_import_allocator(ctx);
    u32 count = 0;
    for (u64 at = 8; at < size;) {
        size_t n = strlen((const char *)snapshot + at);
        if (n >= size - at)
            return DE_ERR_PARSE;
        at += n + 1;
        ++count;
    }
    if (!count)
        return de_asset_import_fail(ctx, DE_ERR_PARSE, "string table directory has no locales");
    de__loc_pair *index =
        DE_ALLOC(a, (size_t)count * sizeof *index, DE_ALIGNOF(de__loc_pair), DE_MEM_UI);
    de__loc_table *tables =
        DE_ALLOC(a, (size_t)count * sizeof *tables, DE_ALIGNOF(de__loc_table), DE_MEM_UI);
    if (!index || !tables)
        return DE_ERR_OUT_OF_MEMORY;
    memset(tables, 0, (size_t)count * sizeof *tables);
    const char *path = de_asset_import_source_path(ctx), *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *default_locale = de_asset_import_setting_string(ctx, "default_locale", "en");
    i32 default_index = -1;
    u32 made = 0;
    de_result result = DE_OK;
    for (u64 at = 8; at < size && result == DE_OK;) {
        const char *name = (const char *)snapshot + at;
        size_t n = strlen(name);
        at += n + 1;
        if (n < 6 || strcmp(name + n - 5, ".toml")) {
            result =
                de_asset_import_fail(ctx, DE_ERR_PARSE, "%s: expected a locale.toml file", name);
            break;
        }
        char *locale = DE_ALLOC(a, n - 4, 1, DE_MEM_UI);
        if (!locale) {
            result = DE_ERR_OUT_OF_MEMORY;
            break;
        }
        memcpy(locale, name, n - 5);
        locale[n - 5] = 0;
        if (!de__loc_valid_locale(locale)) {
            result = de_asset_import_fail(ctx, DE_ERR_PARSE, "%s: invalid locale identifier", name);
            break;
        }
        char relative[DE_FS_PATH_MAX];
        int length = snprintf(relative, sizeof relative, "%s/%s", base, name);
        if (length < 0 || (size_t)length >= sizeof relative) {
            result = DE_ERR_INVALID_ARGUMENT;
            break;
        }
        const void *source = NULL;
        u64 source_size = 0;
        result = de_asset_import_read_file(ctx, relative, &source, &source_size);
        void *cooked = NULL;
        u64 cooked_size = 0;
        if (result == DE_OK)
            result = import_locale(ctx, name, source, source_size, &cooked, &cooked_size);
        if (result == DE_OK)
            result = de_asset_import_add_subasset(ctx, locale, "string_table", cooked, cooked_size);
        if (result == DE_OK)
            result = de__loc_table_parse(cooked, cooked_size, &tables[made]);
        if (result == DE_OK) {
            index[made] = (de__loc_pair){locale, locale, strlen(locale), 0};
            if (!strcmp(locale, default_locale))
                default_index = (i32)made;
            ++made;
        }
    }
    if (result == DE_OK && default_index < 0)
        result = de_asset_import_fail(ctx, DE_ERR_PARSE, "default locale '%s.toml' is missing",
                                      default_locale);
    if (result == DE_OK) {
        de_log_category log = de_log_register_category("text.loc", DE_LOG_INFO);
        for (u32 i = 0; i < made; ++i)
            for (u32 k = 0; k < tables[i].count; ++k) {
                u64 hash = de__loc_u64(tables[i].data + 32 + (u64)k * 16);
                if (!de__loc_table_lookup(&tables[default_index], (de_name){hash}).ptr)
                    DE_WARN(log, "locale '%s': key hash %016llx missing from default locale '%s'",
                            index[i].key, (unsigned long long)hash, default_locale);
            }
        void *cooked = NULL;
        u64 bytes = 0;
        result = de__loc_table_build(a, index, made, &cooked, &bytes);
        if (result == DE_OK)
            result = de_asset_import_write(ctx, cooked, bytes);
    }
    return result; /* scratch allocator owns locale strings and cooked temporaries */
}
de_asset_type_desc de_string_table_import_type_desc(void) {
    de_asset_type_desc d = {0};
    d.name = "string_table";
    d.importer_version = 2;
    d.import = import_table;
    return d;
}
