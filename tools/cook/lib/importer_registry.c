/*
 * importer_registry.c: the cooker's type table (DENG-209, SPEC §11.2, §11.6) and the engine's
 * asset type names.
 *
 * Each cooker has its own table of up to 256 types, mirroring de_asset_type_register: a name, the
 * importer version (part of the `.decache` key, DENG-210), the importer and its user pointer.
 * Entries are written before the release store of the count that publishes them and never change,
 * so cooks on other threads look types up without a lock while one thread registers more.
 */

#include "cooker_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

#include "platform/de_atomic.h"

#include <string.h>

/* ---- Engine type names --------------------------------------------------------------------- */

static const char *const k_engine_types[] = {DE_COOKER_ENGINE_TYPE_NAMES};

const char *const *de_cooker_engine_type_names(u32 *count) {
    if (count != NULL)
        *count = (u32)(sizeof k_engine_types / sizeof k_engine_types[0]);
    return k_engine_types;
}

bool de_cooker_is_engine_type(const char *name) {
    if (name == NULL)
        return false;
    for (u32 i = 0; i < (u32)(sizeof k_engine_types / sizeof k_engine_types[0]); ++i)
        if (strcmp(k_engine_types[i], name) == 0)
            return true;
    return false;
}

/* ---- Type table ------------------------------------------------------------------------------ */

/* The runtime registry's name rule (src/asset/asset_types.c): 1 to 63 printable ASCII bytes,
 * no space or quote. */
static bool name_valid(const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n > DE__COOKER_TYPE_NAME_MAX)
        return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (ch <= 0x20 || ch >= 0x7F || ch == '"' || ch == '\'' || ch == '\\')
            return false;
    }
    return true;
}

const de__cooker_type *de__cooker_find_type(const de_cooker *c, const char *name) {
    if (c == NULL || name == NULL)
        return NULL;
    u32 n = de_atomic_load_u32(&c->type_count, DE_ATOMIC_ACQUIRE);
    u64 h = de_hash64_str(name);
    for (u32 i = 0; i < n; ++i)
        if (c->types[i].name_hash == h && strcmp(c->types[i].name, name) == 0)
            return &c->types[i];
    return NULL;
}

de_result de_cooker_register_type(de_cooker *c, const de_asset_type_desc *desc) {
    if (c == NULL || desc == NULL || desc->name == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cooker_register_type: NULL argument");
    if (!name_valid(desc->name))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_cooker_register_type: invalid type name \"%s\" (1 to %u printable "
                       "ASCII bytes, no space or quote)",
                       desc->name, DE__COOKER_TYPE_NAME_MAX);
    if (de__cooker_find_type(c, desc->name) != NULL)
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "de_cooker_register_type: type \"%s\" exists",
                       desc->name);
    u32 n = de_atomic_load_u32(&c->type_count, DE_ATOMIC_RELAXED);
    if (n >= DE__COOKER_MAX_TYPES)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_cooker_register_type: %u types registered",
                       DE__COOKER_MAX_TYPES);
    de__cooker_type *t = &c->types[n];
    memset(t, 0, sizeof *t);
    memcpy(t->name, desc->name, strlen(desc->name) + 1u);
    t->name_hash = de_hash64_str(desc->name);
    t->importer_version = desc->importer_version;
    t->import = desc->import;
    t->user = desc->user;
    de_atomic_store_u32(&c->type_count, n + 1u, DE_ATOMIC_RELEASE);
    return DE_OK;
}

bool de_cooker_has_type(const de_cooker *c, const char *name) {
    return de__cooker_find_type(c, name) != NULL;
}

u32 de_cooker_type_names(const de_cooker *c, const char **out, u32 max) {
    if (c == NULL)
        return 0;
    u32 n = de_atomic_load_u32(&c->type_count, DE_ATOMIC_ACQUIRE);
    for (u32 i = 0; i < n && i < max && out != NULL; ++i)
        out[i] = c->types[i].name;
    return n;
}

u32 de_cooker_importer_version(const de_cooker *c, const char *name) {
    const de__cooker_type *t = de__cooker_find_type(c, name);
    return t != NULL ? t->importer_version : 0u;
}
