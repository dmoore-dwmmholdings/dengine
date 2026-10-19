#include "text/loc/loc_internal.h"
#include "text/loc/string_table_format.h"
#include "asset/asset_internal.h"
#include "asset/slots_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MISSING_SLOTS 1024u
#define LOCALE_CHAIN  20u
typedef struct loaded_table {
    const de_allocator *alloc;
    de_atomic_u32 refs;
    de__loc_table table;
    u64 bytes;
    u8 data[];
} loaded_table;
typedef struct loc_entry {
    bool parent;
    de_load_handle request;
    de_asset_id id;
    loaded_table *object;
    de__loc_table table;
    void *pseudo;
    u64 pseudo_bytes;
} loc_entry;
typedef struct loc_set {
    struct loc_set *next;
    u64 expires;
    char locale[36];
    u32 count, tables;
    bool reported, children_requested;
    bool pseudo;
    loc_entry entries[];
} loc_set;
typedef struct missing_string {
    de_atomic_u64 hash;
    de_atomic_u32 ready;
    char text[512];
} missing_string;
struct de__loc {
    const de_allocator *alloc;
    de_asset_system *assets;
    de_atomic_ptr active;
    de_atomic_u64 generation;
    loc_set *pending, *retired;
    const de__manifest *seen_manifest;
    u64 frame;
    char desired[36], default_locale[36];
    bool pseudo, refresh;
    de_log_category log;
    missing_string missing[MISSING_SLOTS];
};
static void table_ref(loaded_table *table) {
    de_atomic_fetch_add_u32(&table->refs, 1, DE_ATOMIC_RELAXED);
}
static void table_release(loaded_table *table) {
    if (table && de_atomic_fetch_sub_u32(&table->refs, 1, DE_ATOMIC_ACQ_REL) == 1)
        DE_FREE(table->alloc, table, (size_t)table->bytes);
}
static de_result load(de_asset_load_ctx *ctx) {
    de_asset_system *assets = de_asset_load_ctx_user(ctx);
    u64 size = 0;
    const void *data = de_asset_load_ctx_data(ctx, &size);
    de__loc_table parsed;
    DE_TRY(de__loc_table_parse(data, size, &parsed));
    if (size > SIZE_MAX - sizeof(loaded_table))
        return DE_ERR_OUT_OF_MEMORY;
    loaded_table *object =
        DE_ALLOC(assets->alloc, sizeof *object + (size_t)size, DE_ALIGNOF(loaded_table), DE_MEM_UI);
    if (!object)
        return DE_ERR_OUT_OF_MEMORY;
    memset(object, 0, sizeof *object);
    object->alloc = assets->alloc;
    object->bytes = sizeof *object + size;
    de_atomic_init_u32(&object->refs, 1);
    memcpy(object->data, data, (size_t)size);
    object->table = parsed;
    object->table.data = object->data;
    de_asset_load_ctx_set_object(ctx, object, object->bytes);
    return DE_OK;
}
static void unload(void *object, void *user) {
    (void)user;
    table_release(object);
}
de_result de__loc_register(de_asset_system *assets) {
    if (!assets)
        return DE_OK;
    if (de__asset_type_find(assets, "string_table").id)
        return DE_OK;
    de_asset_type_desc d = {0};
    d.name = "string_table";
    d.load = load;
    d.unload = unload;
    d.user = assets;
    d.importer_version = 1;
    de_asset_type_desc builtin;
    if (de__asset_builtin_importer("string_table", &builtin)) {
        d.import = builtin.import;
        d.importer_version = builtin.importer_version;
    }
    return de_asset_type_register(assets, &d).id
               ? DE_OK
               : DE_FAIL(DE_ERR_INVALID_ARGUMENT, "string table registration failed");
}
static void free_set(de__loc *loc, loc_set *set) {
    if (!set)
        return;
    for (u32 i = 0; i < set->count; ++i) {
        loc_entry *e = &set->entries[i];
        if (e->request.id)
            de_asset_release(loc->assets, e->request);
        table_release(e->object);
        if (e->pseudo)
            DE_FREE(loc->alloc, e->pseudo, (size_t)e->pseudo_bytes);
    }
    DE_FREE(loc->alloc, set, sizeof *set + (size_t)set->count * sizeof(loc_entry));
}
static void normalize(const char *in, char out[36]) {
    size_t n = strlen(in);
    memcpy(out, in, n + 1);
    u32 part = 0;
    for (size_t at = 0; at < n;) {
        size_t end = at;
        while (end < n && out[end] != '-')
            ++end;
        size_t len = end - at;
        for (size_t i = at; i < end; ++i) {
            char c = out[i];
            if (c >= 'A' && c <= 'Z')
                c += 32;
            if (part && ((len == 2) || (len == 4 && i == at)) && c >= 'a' && c <= 'z')
                c -= 32;
            out[i] = c;
        }
        ++part;
        at = end + 1;
    }
}
static u32 chain(de__loc *loc, char locales[LOCALE_CHAIN][36]) {
    u32 count = 0;
    char tag[36];
    memcpy(tag, loc->desired, sizeof tag);
    for (;;) {
        memcpy(locales[count++], tag, sizeof tag);
        char *dash = strrchr(tag, '-');
        if (!dash)
            break;
        *dash = 0;
    }
    bool found = false;
    for (u32 i = 0; i < count; ++i)
        if (!strcmp(locales[i], loc->default_locale))
            found = true;
    if (!found)
        memcpy(locales[count++], loc->default_locale, 36);
    return count;
}
static int table_order(const void *a, const void *b) {
    return strcmp((*(const de__manifest_asset *const *)a)->id,
                  (*(const de__manifest_asset *const *)b)->id);
}
static de_result request(de__loc *loc) {
    const de__manifest *manifest = loc->assets ? loc->assets->manifest : NULL;
    u32 tables = 0;
    if (manifest)
        for (u32 i = 0; i < manifest->asset_count; ++i)
            if (!strcmp(manifest->assets[i].type, "string_table"))
                ++tables;
    const de__manifest_asset **parents = NULL;
    if (tables) {
        parents =
            DE_ALLOC(loc->alloc, (size_t)tables * sizeof *parents, DE_ALIGNOF(void *), DE_MEM_UI);
        if (!parents)
            return DE_ERR_OUT_OF_MEMORY;
        u32 n = 0;
        for (u32 i = 0; i < manifest->asset_count; ++i)
            if (!strcmp(manifest->assets[i].type, "string_table"))
                parents[n++] = &manifest->assets[i];
        qsort(parents, tables, sizeof *parents, table_order);
    }
    char locales[LOCALE_CHAIN][36] = {{0}};
    u32 languages = chain(loc, locales);
    u64 entries = (u64)tables * (languages + 1);
    if (entries > UINT32_MAX || entries > (SIZE_MAX - sizeof(loc_set)) / sizeof(loc_entry)) {
        if (parents)
            DE_FREE(loc->alloc, parents, (size_t)tables * sizeof *parents);
        return DE_ERR_OUT_OF_MEMORY;
    }
    loc_set *set = DE_ALLOC(loc->alloc, sizeof *set + (size_t)entries * sizeof(loc_entry),
                            DE_ALIGNOF(loc_set), DE_MEM_UI);
    if (!set) {
        if (parents)
            DE_FREE(loc->alloc, parents, (size_t)tables * sizeof *parents);
        return DE_ERR_OUT_OF_MEMORY;
    }
    memset(set, 0, sizeof *set + (size_t)entries * sizeof(loc_entry));
    set->count = (u32)entries;
    set->tables = tables;
    memcpy(set->locale, loc->desired, 36);
    set->pseudo = loc->pseudo;
    de_result result = DE_OK;
    for (u32 t = 0; t < tables; ++t) {
        loc_entry *e = &set->entries[t];
        e->parent = true;
        e->id = de_asset_id_from(parents[t]->id);
        e->request = de_asset_load(loc->assets, e->id, DE_LOAD_NO_EVENTS);
        if (!e->request.id)
            result = DE_ERR_OUT_OF_MEMORY;
    }
    for (u32 l = 0; l < languages && result == DE_OK; ++l)
        for (u32 t = 0; t < tables; ++t) {
            char id[1024];
            int n = snprintf(id, sizeof id, "%s#%s", parents[t]->id, locales[l]);
            if (n < 0 || (size_t)n >= sizeof id) {
                result = DE_ERR_INVALID_ARGUMENT;
                break;
            }
            loc_entry *e = &set->entries[(l + 1) * tables + t];
            e->id = de_asset_id_from(id);
        }
    if (parents)
        DE_FREE(loc->alloc, parents, (size_t)tables * sizeof *parents);
    if (result != DE_OK) {
        free_set(loc, set);
        return result;
    }
    free_set(loc, loc->pending);
    loc->pending = set;
    loc->seen_manifest = manifest;
    loc->refresh = false;
    return DE_OK;
}
static de_result pseudo_table(de__loc *loc, loc_entry *e) {
    const de__loc_table *table = &e->object->table;
    u64 blob = 32 + (u64)table->count * 16, total = blob;
    for (u32 i = 0; i < table->count; ++i) {
        const u8 *entry = table->data + 32 + (u64)i * 16;
        u32 offset = de__loc_u32(entry + 8), length = de__loc_u32(entry + 12);
        total += (u64)de__loc_pseudo_text((const char *)table->data + table->blob + offset, length,
                                          NULL, 0) +
                 1;
    }
    if (total > UINT32_MAX)
        return DE_ERR_OUT_OF_MEMORY;
    u8 *data = DE_ALLOC(loc->alloc, (size_t)total, 16, DE_MEM_UI);
    if (!data)
        return DE_ERR_OUT_OF_MEMORY;
    memcpy(data, table->data, (size_t)blob);
    de__loc_w32(data + 20, (u32)(total - blob));
    de__loc_w32(data + 24, (u32)total);
    u32 at = 0;
    for (u32 i = 0; i < table->count; ++i) {
        const u8 *old = table->data + 32 + (u64)i * 16;
        u8 *entry = data + 32 + (u64)i * 16;
        u32 length = de__loc_pseudo_text(
            (const char *)table->data + table->blob + de__loc_u32(old + 8), de__loc_u32(old + 12),
            (char *)data + blob + at, (u32)(total - blob - at));
        de__loc_w32(entry + 8, at);
        de__loc_w32(entry + 12, length);
        at += length + 1;
    }
    de_result r = de__loc_table_parse(data, total, &e->table);
    if (r != DE_OK) {
        DE_FREE(loc->alloc, data, (size_t)total);
        return r;
    }
    e->pseudo = data;
    e->pseudo_bytes = total;
    return DE_OK;
}
de_result de__loc_create(const de_allocator *a, de_asset_system *assets, de__loc **out) {
    *out = NULL;
    if (!a)
        a = de_heap_allocator();
    DE_TRY(de__loc_register(assets));
    de__loc *loc = DE_NEW(a, de__loc, DE_MEM_UI);
    if (!loc)
        return DE_ERR_OUT_OF_MEMORY;
    memset(loc, 0, sizeof *loc);
    loc->alloc = a;
    loc->assets = assets;
    memcpy(loc->desired, "en", 3);
    memcpy(loc->default_locale, "en", 3);
    loc->refresh = true;
    de_atomic_init_ptr(&loc->active, NULL);
    de_atomic_init_u64(&loc->generation, 0);
    loc->log = de_log_register_category("text.loc", DE_LOG_INFO);
    for (u32 i = 0; i < MISSING_SLOTS; ++i) {
        de_atomic_init_u64(&loc->missing[i].hash, 0);
        de_atomic_init_u32(&loc->missing[i].ready, 0);
    }
    *out = loc;
    return DE_OK;
}
void de__loc_destroy(de__loc *loc) {
    if (!loc)
        return;
    free_set(loc, de_atomic_load_ptr(&loc->active, DE_ATOMIC_RELAXED));
    free_set(loc, loc->pending);
    while (loc->retired) {
        loc_set *next = loc->retired->next;
        free_set(loc, loc->retired);
        loc->retired = next;
    }
    DE_FREE(loc->alloc, loc, sizeof *loc);
}
de_result de__loc_set_locale(de__loc *loc, const char *locale) {
    if (!loc || !de__loc_valid_locale(locale))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid locale identifier");
    char normalized[36] = {0};
    normalize(locale, normalized);
    if (!strcmp(loc->desired, normalized) && !loc->refresh)
        return DE_OK;
    memcpy(loc->desired, normalized, 36);
    return request(loc);
}
de_result de__loc_set_default_locale(de__loc *loc, const char *locale) {
    if (!loc || !de__loc_valid_locale(locale))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "invalid default locale");
    char normalized[36] = {0};
    normalize(locale, normalized);
    if (!strcmp(loc->default_locale, normalized))
        return DE_OK;
    memcpy(loc->default_locale, normalized, 36);
    loc->refresh = true;
    return request(loc);
}
void de__loc_update(de__loc *loc) {
    if (!loc)
        return;
    ++loc->frame;
    loc_set **at = &loc->retired;
    while (*at) {
        if ((*at)->expires < loc->frame) {
            loc_set *old = *at;
            *at = old->next;
            free_set(loc, old);
        } else
            at = &(*at)->next;
    }
    const de__manifest *manifest = loc->assets ? loc->assets->manifest : NULL;
    if (loc->refresh || loc->seen_manifest != manifest) {
        if (request(loc) != DE_OK)
            return;
    }
    loc_set *set = loc->pending;
    if (!set)
        return;
    if (!set->children_requested) {
        for (u32 i = 0; i < set->tables; ++i)
            if (de_asset_status(loc->assets, set->entries[i].request, NULL) != DE_LOAD_READY)
                return;
        /* Source-mode sub-assets become resolvable only after the parent cook publishes them. */
        for (u32 i = set->tables; i < set->count; ++i) {
            loc_entry *e = &set->entries[i];
            e->request = de_asset_load(loc->assets, e->id, DE_LOAD_NO_EVENTS);
        }
        set->children_requested = true;
    }
    for (u32 i = 0; i < set->count; ++i) {
        de_load_state state = de_asset_status(loc->assets, set->entries[i].request, NULL);
        if (state == DE_LOAD_PENDING || state == DE_LOAD_LOADING)
            return;
    }
    bool ready = true;
    for (u32 i = 0; i < set->count; ++i) {
        loc_entry *e = &set->entries[i];
        if (e->parent)
            continue;
        if (e->object) {
            if (set->pseudo && !e->pseudo && pseudo_table(loc, e) != DE_OK) {
                ready = false;
                break;
            }
            continue;
        }
        if (de_asset_status(loc->assets, e->request, NULL) != DE_LOAD_READY)
            continue;
        const de__slot *slot = de__slot_get(loc->assets, de__slot_find(loc->assets, e->id));
        loaded_table *object = slot ? de__slot_object(slot) : NULL;
        if (!object)
            continue;
        table_ref(object);
        e->object = object;
        e->table = object->table;
        if (set->pseudo && pseudo_table(loc, e) != DE_OK) {
            ready = false;
            break;
        }
    }
    if (!ready)
        return;
    for (u32 base = 0; base < set->tables; ++base) {
        bool present = false;
        for (u32 i = base + set->tables; i < set->count; i += set->tables)
            if (set->entries[i].object)
                present = true;
        if (!present) {
            if (!set->reported) {
                de_load_info info = {0};
                de_asset_status(loc->assets, set->entries[base].request, &info);
                DE_WARN(loc->log, "locale set not ready: %s", info.first_error_msg);
                set->reported = true;
            }
            return;
        }
    }
    loc_set *old = de_atomic_exchange_ptr(&loc->active, set, DE_ATOMIC_ACQ_REL);
    loc->pending = NULL;
    if (old) {
        old->expires = loc->frame + 2;
        old->next = loc->retired;
        loc->retired = old;
    }
    if (loc->assets)
        de_asset_set_locale(loc->assets, set->locale);
    de_atomic_fetch_add_u64(&loc->generation, 1, DE_ATOMIC_RELEASE);
}
const char *de__loc_locale(const de__loc *loc) {
    loc_set *set = loc ? de_atomic_load_ptr(&loc->active, DE_ATOMIC_ACQUIRE) : NULL;
    return set ? set->locale : "en";
}
u64 de__loc_generation(const de__loc *loc) {
    return loc ? de_atomic_load_u64(&loc->generation, DE_ATOMIC_ACQUIRE) : 0;
}
de_str de__loc_lookup(const de__loc *constant, de_name id) {
    if (!constant)
        return (de_str){"", 0};
    de__loc *loc = (de__loc *)constant;
    loc_set *set = de_atomic_load_ptr(&loc->active, DE_ATOMIC_ACQUIRE);
    for (u32 i = 0; set && i < set->count; ++i) {
        de_str s = de__loc_table_lookup(&set->entries[i].table, id);
        if (s.ptr)
            return s;
    }
#if DE_NAME_LOOKUP
    for (u32 probe = 0; probe < MISSING_SLOTS; ++probe) {
        missing_string *missing = &loc->missing[(id.hash + probe) % MISSING_SLOTS];
        u64 hash = de_atomic_load_u64(&missing->hash, DE_ATOMIC_ACQUIRE);
        if (hash == id.hash && hash) {
            if (de_atomic_load_u32(&missing->ready, DE_ATOMIC_ACQUIRE))
                return (de_str){missing->text, strlen(missing->text)};
            return (de_str){"[[missing]]", 11};
        }
        if (!hash && id.hash) {
            u64 expected = 0;
            if (de_atomic_cas_u64(&missing->hash, &expected, id.hash, DE_ATOMIC_ACQ_REL,
                                  DE_ATOMIC_ACQUIRE)) {
                const char *name = de_name_str(id);
                if (name)
                    snprintf(missing->text, sizeof missing->text, "[[%s]]", name);
                else
                    snprintf(missing->text, sizeof missing->text, "[[%016llx]]",
                             (unsigned long long)id.hash);
                DE_WARN(loc->log, "missing localized string %s", missing->text);
                de_atomic_store_u32(&missing->ready, 1, DE_ATOMIC_RELEASE);
                return (de_str){missing->text, strlen(missing->text)};
            }
        }
    }
    return (de_str){"[[missing]]", 11};
#else
    (void)id;
    return (de_str){"", 0};
#endif
}
u32 de__loc_format(const de__loc *loc, de_name id, const de_loc_arg *args, u32 count, char *out,
                   u32 capacity) {
    de_str pattern = de__loc_lookup(loc, id);
    u32 needed = 0;
    if (de__loc_message(pattern.ptr, pattern.len, de__loc_locale(loc), args, count, out, capacity,
                        &needed) != DE_OK)
        return 0;
    return needed;
}
void de__loc_pseudo(de__loc *loc, bool enabled) {
#if DE_NAME_LOOKUP
    if (loc && loc->pseudo != enabled) {
        loc->pseudo = enabled;
        loc->refresh = true;
        (void)request(loc);
    }
#else
    (void)loc;
    (void)enabled;
#endif
}
