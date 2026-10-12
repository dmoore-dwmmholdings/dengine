/*
 * registry.c: the type registry behind de_reflect.h (DENG-294, SPEC §9.1, §10.1).
 *
 * Layout:
 *   - `entries`: a dense de_array of de__type_entry (struct types and enums share it and one
 *     namespace). An id is (generation << 20) | (index + 1). Entries are never removed, so the
 *     generation stays 1; it exists so a later unregister can invalidate stale ids.
 *   - `by_name`: de_hashmap from the u64 de_hash64_str of the name to the entry index. A hash
 *     that matches a different stored name is reported as DE_ERR_ALREADY_EXISTS.
 *   - Descriptions, field arrays, enum values and every string are deep-copied into bump blocks
 *     taken from the registry's allocator. Blocks are freed only by destroy, so each pointer the
 *     registry hands out stays valid, also across re-registration (hot reload).
 *
 * Registration is single-threaded; lookups only read (de_hashmap_get, array indexing), so any
 * number of threads may look up at once while nobody registers.
 */
#include "reflect_internal.h"

#include <string.h>

#define REG_BLOCK_MIN ((size_t)16 * 1024)
#define REG_ALIGN     ((size_t)16)

/* ---- Bump blocks ----------------------------------------------------------------------------- */

static size_t align_up(size_t v, size_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

static void *reg_alloc(de_type_registry *r, size_t size, size_t align) {
    if (align < REG_ALIGN)
        align = REG_ALIGN;
    de__reg_block *b = r->blocks;
    if (b) {
        size_t at = align_up(b->used, align);
        if (at + size <= b->size) {
            b->used = at + size;
            return (u8 *)b + at;
        }
    }
    size_t header = align_up(sizeof(de__reg_block), REG_ALIGN);
    size_t need = align_up(header, align) + size;
    size_t bytes = need > REG_BLOCK_MIN ? need : REG_BLOCK_MIN;
    de__reg_block *nb = (de__reg_block *)DE_ALLOC(r->a, bytes, REG_ALIGN, DE_MEM_GENERAL);
    if (!nb) {
        DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "type registry: out of memory (%zu bytes)", bytes);
        return NULL;
    }
    nb->next = b;
    nb->size = bytes;
    nb->used = align_up(header, align) + size;
    r->blocks = nb;
    return (u8 *)nb + align_up(header, align);
}

static const char *reg_strdup(de_type_registry *r, const char *s, bool *oom) {
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *d = (char *)reg_alloc(r, n, 1);
    if (!d) {
        *oom = true;
        return NULL;
    }
    memcpy(d, s, n);
    return d;
}

/* ---- Lookup ---------------------------------------------------------------------------------- */

static de__type_entry *entry_at(const de_type_registry *r, u32 index) {
    return &((de__type_entry *)r->entries.data)[index];
}

const de__type_entry *de__reflect_entry(const de_type_registry *r, de_type_id id) {
    if (!r || id.id == 0)
        return NULL;
    u32 slot = id.id & DE__TYPE_INDEX_MASK;
    u32 gen = id.id >> DE__TYPE_INDEX_BITS;
    if (slot == 0 || slot > r->entries.count)
        return NULL;
    const de__type_entry *e = entry_at(r, slot - 1);
    return e->generation == gen ? e : NULL;
}

/* Index of the entry called `name` with hash `h`, or -1. */
static i64 find_index(const de_type_registry *r, const char *name, u64 h) {
    const u32 *idx = DE_HASHMAP_GET(&r->by_name, u32, h, &h);
    if (!idx)
        return -1;
    return strcmp(entry_at(r, *idx)->name, name) == 0 ? (i64)*idx : -1;
}

const de__type_entry *de__reflect_find_entry(const de_type_registry *r, const char *name) {
    if (!r || !name)
        return NULL;
    i64 i = find_index(r, name, de_hash64_str(name));
    return i < 0 ? NULL : entry_at(r, (u32)i);
}

const de__type_layout *de__reflect_find_layout(const de_type_registry *r, const char *name) {
    const de__type_entry *e = de__reflect_find_entry(r, name);
    return (e && e->kind == DE__ENTRY_STRUCT) ? e->cur : NULL;
}

static de_type_id make_id(const de__type_entry *e, u32 index) {
    de_type_id id;
    id.id = (e->generation << DE__TYPE_INDEX_BITS) | (index + 1);
    return id;
}

static de_type_id invalid_id(void) {
    de_type_id id;
    id.id = 0;
    return id;
}

/* ---- Create / destroy ------------------------------------------------------------------------ */

de_result de_type_registry_create(const de_allocator *a, de_type_registry **out) {
    if (!out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_type_registry_create: out is NULL");
    *out = NULL;
    if (!a)
        a = de_heap_allocator();
    de_type_registry *r = DE_NEW(a, de_type_registry, DE_MEM_GENERAL);
    if (!r)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_type_registry_create: out of memory");
    memset(r, 0, sizeof *r);
    r->a = a;
    de_array_init(&r->entries, a, (u32)sizeof(de__type_entry), (u32)DE_ALIGNOF(de__type_entry),
                  DE_MEM_GENERAL);
    de_result res = DE_HASHMAP_INIT(&r->by_name, u64, u32, a);
    if (res != DE_OK) {
        DE_FREE(a, r, sizeof *r);
        return res;
    }
    *out = r;
    return DE_OK;
}

void de_type_registry_destroy(de_type_registry *r) {
    if (!r)
        return;
    const de_allocator *a = r->a;
    de__reg_block *b = r->blocks;
    while (b) {
        de__reg_block *next = b->next;
        DE_FREE(a, b, b->size);
        b = next;
    }
    de_hashmap_free(&r->by_name);
    de_array_free(&r->entries);
    DE_FREE(a, r, sizeof *r);
}

/* ---- Copies ---------------------------------------------------------------------------------- */

static const char *const k_xyzw[4] = {"x", "y", "z", "w"};
static const char *const k_rgba[4] = {"r", "g", "b", "a"};

/* Fills `comps` for vector kind `kind` (no-op for other kinds). */
static u32 make_components(de_field_kind kind, const de_field_info *parent, de_field_info *comps) {
    u32 comp_size = 0;
    u32 n = de__reflect_kind_components(kind, &comp_size);
    for (u32 i = 0; i < n; ++i) {
        de_field_info *c = &comps[i];
        memset(c, 0, sizeof *c);
        c->name = kind == DE_FK_COLOR ? k_rgba[i] : k_xyzw[i];
        c->offset = i * comp_size;
        c->size = comp_size;
        c->kind = comp_size == 8 ? DE_FK_F64 : DE_FK_F32;
        c->flags = parent->flags;
        c->attrs.min = parent->attrs.min;
        c->attrs.max = parent->attrs.max;
        c->attrs.units = parent->attrs.units;
        c->attrs.tooltip = parent->attrs.tooltip;
    }
    return n;
}

/* Deep copy of a validated description. NULL (error set) on out of memory. */
static de__type_layout *copy_layout(de_type_registry *r, const de_type_desc *desc,
                                    const char *owned_name) {
    bool oom = false;
    de__type_layout *l = (de__type_layout *)reg_alloc(r, sizeof *l, DE_ALIGNOF(de__type_layout));
    if (!l)
        return NULL;
    memset(l, 0, sizeof *l);
    u32 n = desc->field_count;
    if (n > 0) {
        l->fields =
            (de_field_info *)reg_alloc(r, sizeof(de_field_info) * n, DE_ALIGNOF(de_field_info));
        l->extra = (de__field_extra *)reg_alloc(r, sizeof(de__field_extra) * n,
                                                DE_ALIGNOF(de__field_extra));
        if (!l->fields || !l->extra)
            return NULL;
        memset(l->extra, 0, sizeof(de__field_extra) * n);
    }
    for (u32 i = 0; i < n; ++i) {
        const de_field_info *src = &desc->fields[i];
        de_field_info *f = &l->fields[i];
        *f = *src;
        f->name = reg_strdup(r, src->name, &oom);
        f->attrs.units = reg_strdup(r, src->attrs.units, &oom);
        f->attrs.tooltip = reg_strdup(r, src->attrs.tooltip, &oom);
        f->attrs.nested_type = reg_strdup(r, src->attrs.nested_type, &oom);
        if (oom)
            return NULL;

        de__field_extra *x = &l->extra[i];
        if (f->kind == DE_FK_FIXED_ARRAY) {
            de_field_kind ek = DE_FK_U8;
            u32 es = 0;
            const char *en = NULL;
            /* Already validated: this cannot fail. */
            if (de__reflect_array_element(r, owned_name, f, &ek, &es, &en) != DE_OK)
                return NULL;
            de_field_info *el = &x->element;
            el->name = f->name;
            el->offset = 0;
            el->size = es;
            el->kind = ek;
            el->flags = f->flags;
            el->attrs = f->attrs;
            el->attrs.nested_type = en;
            el->attrs.array_count = 0;
            x->comp_count = make_components(ek, f, x->comps);
        } else {
            x->comp_count = make_components(f->kind, f, x->comps);
        }
    }
    l->desc = *desc;
    l->desc.name = owned_name;
    l->desc.fields = l->fields;
    return l;
}

static de_enum_info *copy_enum(de_type_registry *r, const char *owned_name,
                               const de_enum_value *values, u32 count, bool is_flags) {
    bool oom = false;
    de_enum_info *e = (de_enum_info *)reg_alloc(r, sizeof *e, DE_ALIGNOF(de_enum_info));
    de_enum_value *v =
        (de_enum_value *)reg_alloc(r, sizeof(de_enum_value) * count, DE_ALIGNOF(de_enum_value));
    if (!e || !v)
        return NULL;
    for (u32 i = 0; i < count; ++i) {
        v[i].name = reg_strdup(r, values[i].name, &oom);
        v[i].value = values[i].value;
    }
    if (oom)
        return NULL;
    e->name = owned_name;
    e->values = v;
    e->count = count;
    e->is_flags = is_flags;
    return e;
}

/* Adds a new entry (name copied). Returns its index, or -1 with the error set. */
static i64 add_entry(de_type_registry *r, const char *name, u64 h, de__entry_kind kind) {
    if (r->entries.count >= DE__TYPE_MAX_ENTRIES) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "type registry: more than %u types", DE__TYPE_MAX_ENTRIES);
        return -1;
    }
    bool oom = false;
    const char *owned = reg_strdup(r, name, &oom);
    if (oom)
        return -1;
    de_result res = de_hashmap_reserve(&r->by_name, r->entries.count + 1);
    if (res == DE_OK)
        res = de_array_reserve(&r->entries, r->entries.count + 1);
    if (res != DE_OK)
        return -1;
    de__type_entry e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.generation = 1;
    e.name_hash = h;
    e.name = owned;
    u32 index = r->entries.count;
    /* Both reserved above: neither can fail now. */
    (void)de_array_push(&r->entries, &e);
    (void)de_hashmap_put(&r->by_name, h, &h, &index);
    return (i64)index;
}

/* The existing entry for `name`, a hash collision error, or -1 when the name is free. */
static de_result lookup_for_register(const de_type_registry *r, const char *name, u64 h,
                                     i64 *out_index) {
    *out_index = -1;
    const u32 *idx = DE_HASHMAP_GET(&r->by_name, u32, h, &h);
    if (!idx)
        return DE_OK;
    const de__type_entry *e = entry_at(r, *idx);
    if (strcmp(e->name, name) != 0)
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "type '%s': name hash collides with type '%s'", name,
                       e->name);
    *out_index = (i64)*idx;
    return DE_OK;
}

/* ---- Registration ---------------------------------------------------------------------------- */

de_result de_type_register_ex(de_type_registry *r, const de_type_desc *desc, de_type_id *out_id,
                              bool *out_layout_changed) {
    if (out_id)
        *out_id = invalid_id();
    if (out_layout_changed)
        *out_layout_changed = false;
    if (!r)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_type_register: registry is NULL");
    DE_TRY(de__reflect_validate_type(r, desc));

    u64 h = de_hash64_str(desc->name);
    i64 index;
    DE_TRY(lookup_for_register(r, desc->name, h, &index));
    if (index >= 0 && entry_at(r, (u32)index)->kind != DE__ENTRY_STRUCT)
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "type '%s': the name is a registered enum",
                       desc->name);

    if (index < 0) {
        index = add_entry(r, desc->name, h, DE__ENTRY_STRUCT);
        if (index < 0)
            return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "type '%s': registration failed", desc->name);
    }
    de__type_entry *e = entry_at(r, (u32)index);
    de__type_layout *l = copy_layout(r, desc, e->name);
    if (!l) {
        /* A new entry must not stay visible without a layout: roll it back (it is the last
         * one). A re-registration keeps its current layout. */
        if (!e->cur) {
            u64 key = e->name_hash;
            de_hashmap_remove(&r->by_name, key, &key, NULL);
            de_array_pop(&r->entries, NULL);
        }
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "type '%s': registration failed", desc->name);
    }

    u64 new_hash = de__reflect_hash_type(r, &l->desc, 0);
    bool changed = false;
    if (!e->cur) {
        e->cur = l;
        e->hash = e->registered_hash = new_hash;
    } else {
        changed = new_hash != e->registered_hash;
        if (changed)
            e->prev = e->cur;
        e->cur = l;
        e->registered_hash = new_hash;
        if (changed)
            de__reflect_rehash_all(r); /* types that nest this one hash differently now */
        else
            e->hash = new_hash;
    }
    if (out_id)
        *out_id = make_id(e, (u32)index);
    if (out_layout_changed)
        *out_layout_changed = changed;
    return DE_OK;
}

de_type_id de_type_register(de_type_registry *r, const de_type_desc *desc) {
    de_type_id id;
    if (de_type_register_ex(r, desc, &id, NULL) != DE_OK)
        return invalid_id();
    return id;
}

de_result de_enum_register(de_type_registry *r, const char *name, const de_enum_value *values,
                           u32 count, bool is_flags) {
    if (!r)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_enum_register: registry is NULL");
    DE_TRY(de__reflect_validate_enum(name, values, count));
    u64 h = de_hash64_str(name);
    i64 index;
    DE_TRY(lookup_for_register(r, name, h, &index));
    if (index >= 0 && entry_at(r, (u32)index)->kind != DE__ENTRY_ENUM)
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "enum '%s': the name is a registered struct type",
                       name);
    bool is_new = index < 0;
    if (is_new) {
        index = add_entry(r, name, h, DE__ENTRY_ENUM);
        if (index < 0)
            return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "enum '%s': registration failed", name);
    }
    de__type_entry *e = entry_at(r, (u32)index);
    de_enum_info *info = copy_enum(r, e->name, values, count, is_flags);
    if (!info) {
        if (is_new) {
            u64 key = e->name_hash;
            de_hashmap_remove(&r->by_name, key, &key, NULL);
            de_array_pop(&r->entries, NULL);
        }
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "enum '%s': registration failed", name);
    }
    u64 new_hash = de__reflect_hash_enum(info);
    bool changed = !is_new && new_hash != e->registered_hash;
    e->enum_info = info;
    e->hash = e->registered_hash = new_hash;
    if (changed)
        de__reflect_rehash_all(r);
    return DE_OK;
}

/* ---- Queries --------------------------------------------------------------------------------- */

de_type_id de_type_find(const de_type_registry *r, const char *name) {
    if (!r || !name)
        return invalid_id();
    i64 i = find_index(r, name, de_hash64_str(name));
    return i < 0 ? invalid_id() : make_id(entry_at(r, (u32)i), (u32)i);
}

static const de__type_entry *struct_entry(const de_type_registry *r, de_type_id id) {
    const de__type_entry *e = de__reflect_entry(r, id);
    return (e && e->kind == DE__ENTRY_STRUCT) ? e : NULL;
}

const de_type_desc *de_type_get(const de_type_registry *r, de_type_id id) {
    const de__type_entry *e = struct_entry(r, id);
    return e ? &e->cur->desc : NULL;
}

const de_type_desc *de_type_get_previous(const de_type_registry *r, de_type_id id) {
    const de__type_entry *e = struct_entry(r, id);
    return (e && e->prev) ? &e->prev->desc : NULL;
}

const de_field_info *de_type_fields(const de_type_registry *r, de_type_id id, u32 *count) {
    const de__type_entry *e = struct_entry(r, id);
    if (count)
        *count = e ? e->cur->desc.field_count : 0;
    return (e && e->cur->desc.field_count > 0) ? e->cur->fields : NULL;
}

const de_field_info *de_type_array_element(const de_type_registry *r, de_type_id id,
                                           u32 field_index) {
    const de__type_entry *e = struct_entry(r, id);
    if (!e || field_index >= e->cur->desc.field_count ||
        e->cur->fields[field_index].kind != DE_FK_FIXED_ARRAY)
        return NULL;
    return &e->cur->extra[field_index].element;
}

u64 de_type_schema_hash(const de_type_registry *r, de_type_id id) {
    const de__type_entry *e = de__reflect_entry(r, id);
    return e ? e->hash : 0;
}

const de_enum_info *de_enum_get(const de_type_registry *r, de_type_id id) {
    const de__type_entry *e = de__reflect_entry(r, id);
    return (e && e->kind == DE__ENTRY_ENUM) ? e->enum_info : NULL;
}

bool de_enum_value_of(const de_type_registry *r, de_type_id id, const char *name, i64 *out_value) {
    const de_enum_info *info = de_enum_get(r, id);
    if (!info || !name)
        return false;
    for (u32 i = 0; i < info->count; ++i) {
        if (strcmp(info->values[i].name, name) == 0) {
            if (out_value)
                *out_value = info->values[i].value;
            return true;
        }
    }
    return false;
}

const char *de_enum_name_of(const de_type_registry *r, de_type_id id, i64 value) {
    const de_enum_info *info = de_enum_get(r, id);
    if (!info)
        return NULL;
    for (u32 i = 0; i < info->count; ++i) {
        if (info->values[i].value == value)
            return info->values[i].name;
    }
    return NULL;
}

/* ---- Hooks ----------------------------------------------------------------------------------- */

de_result de_type_construct(const de_type_registry *r, de_type_id id, void *obj) {
    const de__type_entry *e = struct_entry(r, id);
    if (!e || !obj)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_type_construct: invalid type id or NULL obj");
    const de_type_desc *d = &e->cur->desc;
    if (d->hooks.construct)
        d->hooks.construct(obj);
    else
        memset(obj, 0, d->size);
    return DE_OK;
}

void de_type_destruct(const de_type_registry *r, de_type_id id, void *obj) {
    const de__type_entry *e = struct_entry(r, id);
    if (e && obj && e->cur->desc.hooks.destruct)
        e->cur->desc.hooks.destruct(obj);
}

de_result de_type_copy(const de_type_registry *r, de_type_id id, void *dst, const void *src) {
    const de__type_entry *e = struct_entry(r, id);
    if (!e || !dst || !src)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_type_copy: invalid type id or NULL pointer");
    const de_type_desc *d = &e->cur->desc;
    if (d->hooks.copy)
        d->hooks.copy(dst, src);
    else if (d->is_pod)
        memmove(dst, src, d->size);
    else
        return DE_FAIL(DE_ERR_UNSUPPORTED, "type '%s': no copy hook and not POD", d->name);
    return DE_OK;
}
