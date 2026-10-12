/*
 * asset_types.c: the asset type registry (SPEC §11.2; DENG-140). Modules and games register the
 * types the manifest's `type = "..."` names, each with an importer (cooker side) and a runtime
 * loader.
 *
 * The table has a fixed capacity (DE__ASSET_MAX_TYPES) and is allocated on the first register, so
 * an entry never moves: loader threads may look types up while the main thread registers more.
 * The count is published with release after the entry is complete. Types are never unregistered,
 * so a type handle's generation is always 1.
 */

#include "asset/slots_internal.h"

#include "asset/asset_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "platform/de_atomic.h"

#include <string.h>

/* Type handles use the slot handle encoding with a fixed generation. */
#define DE__ASSET_TYPE_GEN 1u

typedef struct de__asset_type_entry {
    de_asset_type_desc desc; /* name and extensions point into `block` */
    u64 name_hash;
    void *block; /* the owned name, extension pointer array and extension strings */
    size_t block_size;
} de__asset_type_entry;

typedef struct de__asset_type_registry {
    de_atomic_u32 count; /* published entries */
    de__asset_type_entry entries[DE__ASSET_MAX_TYPES];
    const char *names[DE__ASSET_MAX_TYPES]; /* entries[i].desc.name, for de__asset_type_names */
} de__asset_type_registry;

static u32 types_count(const de__asset_type_registry *r) {
    return r != NULL ? de_atomic_load_u32(&r->count, DE_ATOMIC_ACQUIRE) : 0u;
}

static de_asset_type types_handle(u32 index) {
    return (de_asset_type){de__slot_handle_make(index, DE__ASSET_TYPE_GEN)};
}

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* `ext` without one leading dot. */
static const char *ext_skip_dot(const char *ext) {
    return ext[0] == '.' ? ext + 1 : ext;
}

/* A type name: 1..DE__ASSET_TYPE_NAME_MAX bytes, printable ASCII without space or quotes (it is
 * written as a TOML string and in log lines). */
static bool name_valid(const char *name) {
    size_t n = 0;
    for (; name[n] != '\0'; ++n) {
        unsigned char c = (unsigned char)name[n];
        if (c <= ' ' || c >= 0x7F || c == '"' || c == '\'' || c == '\\')
            return false;
    }
    return n > 0 && n <= DE__ASSET_TYPE_NAME_MAX;
}

/* An extension (after its optional leading dot): 1..31 bytes, printable ASCII without space,
 * dot, slash or backslash. */
static bool ext_valid(const char *ext) {
    size_t n = 0;
    for (; ext[n] != '\0'; ++n) {
        unsigned char c = (unsigned char)ext[n];
        if (c <= ' ' || c >= 0x7F || c == '.' || c == '/' || c == '\\')
            return false;
    }
    return n > 0 && n < 32;
}

static bool ext_equal_nocase(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; ++a, ++b)
        if (ascii_lower(*a) != ascii_lower(*b))
            return false;
    return *a == *b;
}

de_asset_type de_asset_type_register(de_asset_system *s, const de_asset_type_desc *desc) {
    const de_asset_type none = {0};
    if (s == NULL || desc == NULL || desc->name == NULL) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_asset_type_register: NULL argument");
        return none;
    }
    if (!name_valid(desc->name)) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                      "de_asset_type_register: bad type name \"%.64s\" (1 to %u printable ASCII "
                      "characters without spaces or quotes)",
                      desc->name, DE__ASSET_TYPE_NAME_MAX);
        return none;
    }
    if (desc->extension_count != 0 && desc->extensions == NULL) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                      "de_asset_type_register(\"%s\"): extension_count %u with no extensions",
                      desc->name, desc->extension_count);
        return none;
    }
    for (u32 i = 0; i < desc->extension_count; ++i) {
        if (desc->extensions[i] == NULL || !ext_valid(ext_skip_dot(desc->extensions[i]))) {
            (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                          "de_asset_type_register(\"%s\"): bad extension %u \"%.40s\"", desc->name,
                          i, desc->extensions[i] != NULL ? desc->extensions[i] : "(null)");
            return none;
        }
    }
    if (de__asset_type_find(s, desc->name).id != 0) {
        (void)DE_FAIL(DE_ERR_ALREADY_EXISTS,
                      "de_asset_type_register: type \"%s\" is already registered", desc->name);
        return none;
    }

    de__asset_type_registry *r = s->types;
    if (r == NULL) {
        r = DE_NEW(s->alloc, de__asset_type_registry, DE_MEM_ASSET);
        if (r == NULL) {
            (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_asset_type_register: out of memory");
            return none;
        }
        memset(r, 0, sizeof *r);
        de_atomic_init_u32(&r->count, 0);
        s->types = r;
    }
    u32 index = de_atomic_load_u32(&r->count, DE_ATOMIC_RELAXED);
    if (index >= DE__ASSET_MAX_TYPES) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                      "de_asset_type_register(\"%s\"): all %u asset types are taken", desc->name,
                      DE__ASSET_MAX_TYPES);
        return none;
    }

    /* One block: extension pointers, then the name, then the extensions (lowercase, no dot). */
    size_t name_size = strlen(desc->name) + 1;
    size_t size = sizeof(const char *) * desc->extension_count + name_size;
    for (u32 i = 0; i < desc->extension_count; ++i)
        size += strlen(ext_skip_dot(desc->extensions[i])) + 1;
    void *block = DE_ALLOC(s->alloc, size, DE_ALIGNOF(const char *), DE_MEM_ASSET);
    if (block == NULL) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_asset_type_register(\"%s\"): out of memory",
                      desc->name);
        return none;
    }
    const char **exts = (const char **)block;
    char *p = (char *)block + sizeof(const char *) * desc->extension_count;
    memcpy(p, desc->name, name_size);
    const char *name = p;
    p += name_size;
    for (u32 i = 0; i < desc->extension_count; ++i) {
        const char *e = ext_skip_dot(desc->extensions[i]);
        size_t n = strlen(e);
        for (size_t k = 0; k < n; ++k)
            p[k] = ascii_lower(e[k]);
        p[n] = '\0';
        exts[i] = p;
        p += n + 1;
    }

    for (u32 i = 0; i < desc->extension_count; ++i) {
        de_asset_type owner = de__asset_type_find_ext(s, exts[i]);
        if (owner.id != 0)
            DE_WARN(s->log,
                    "asset type \"%s\": extension \".%s\" already belongs to type \"%s\", "
                    "which keeps it for lookups by extension",
                    name, exts[i], de__asset_type_get(s, owner)->name);
    }

    de__asset_type_entry *e = &r->entries[index];
    e->desc = *desc;
    e->desc.name = name;
    e->desc.extensions = desc->extension_count != 0 ? exts : NULL;
    e->name_hash = de_hash64_str(name);
    e->block = block;
    e->block_size = size;
    r->names[index] = name;
    /* Release: a thread that reads the new count sees the complete entry. */
    de_atomic_store_u32(&r->count, index + 1u, DE_ATOMIC_RELEASE);
    return types_handle(index);
}

const de_asset_type_desc *de__asset_type_get(const de_asset_system *s, de_asset_type type) {
    if (s == NULL || type.id == 0 || de__slot_handle_generation(type.id) != DE__ASSET_TYPE_GEN)
        return NULL;
    u32 index = de__slot_handle_index(type.id);
    if (index >= types_count(s->types))
        return NULL;
    return &s->types->entries[index].desc;
}

de_asset_type de__asset_type_find(const de_asset_system *s, const char *name) {
    if (s == NULL || name == NULL)
        return (de_asset_type){0};
    u64 h = de_hash64_str(name);
    u32 n = types_count(s->types);
    for (u32 i = 0; i < n; ++i)
        if (s->types->entries[i].name_hash == h &&
            strcmp(s->types->entries[i].desc.name, name) == 0)
            return types_handle(i);
    return (de_asset_type){0};
}

de_asset_type de__asset_type_find_hash(const de_asset_system *s, u64 name_hash) {
    u32 n = s != NULL ? types_count(s->types) : 0u;
    for (u32 i = 0; i < n; ++i)
        if (s->types->entries[i].name_hash == name_hash)
            return types_handle(i);
    return (de_asset_type){0};
}

de_asset_type de__asset_type_find_ext(const de_asset_system *s, const char *ext) {
    if (s == NULL || ext == NULL)
        return (de_asset_type){0};
    ext = ext_skip_dot(ext);
    u32 n = types_count(s->types);
    for (u32 i = 0; i < n; ++i) {
        const de_asset_type_desc *d = &s->types->entries[i].desc;
        for (u32 k = 0; k < d->extension_count; ++k)
            if (ext_equal_nocase(d->extensions[k], ext))
                return types_handle(i);
    }
    return (de_asset_type){0};
}

const char *const *de__asset_type_names(const de_asset_system *s, u32 *count) {
    u32 n = s != NULL ? types_count(s->types) : 0u;
    if (count != NULL)
        *count = n;
    return n != 0 ? s->types->names : NULL;
}

de_asset_type de__slot_type(const de_asset_system *s, const de__slot *slot) {
    if (slot->type.id != 0)
        return slot->type;
    return slot->type_hash != 0 ? de__asset_type_find_hash(s, slot->type_hash) : (de_asset_type){0};
}

void de__asset_types_shutdown(de_asset_system *s) {
    de__asset_type_registry *r = s != NULL ? s->types : NULL;
    if (r == NULL)
        return;
    u32 n = types_count(r);
    for (u32 i = 0; i < n; ++i)
        DE_FREE(s->alloc, r->entries[i].block, r->entries[i].block_size);
    DE_FREE(s->alloc, r, sizeof *r);
    s->types = NULL;
}
