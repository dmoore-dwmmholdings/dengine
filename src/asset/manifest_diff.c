/*
 * manifest_diff.c: what changed between two manifests, per asset (SPEC §11.9; DENG-214). See
 * hot_reload_internal.h. A digest holds, per asset ID, hashes of the parts that change what a
 * load produces: the type, the sources (asset root, source, locale overrides) and the effective
 * settings. Two digests sorted by ID are merged into the added, removed and changed lists.
 * Order: everything is sorted by ID; nothing depends on hash map order.
 */

#include "asset/hot_reload_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include <string.h>

/* Chains `size` bytes into hash `h`. */
static u64 mix(u64 h, const void *data, size_t size) {
    return de_hash64(data, size, h);
}

/* Chains a string with its length, so "ab" + "c" and "a" + "bc" differ. */
static u64 mix_str(u64 h, const char *s) {
    if (s == NULL)
        s = "";
    u64 n = (u64)strlen(s);
    h = mix(h, &n, sizeof n);
    return mix(h, s, (size_t)n);
}

static u64 mix_u64(u64 h, u64 v) {
    return mix(h, &v, sizeof v);
}

static u64 value_hash(u64 h, const de__manifest_value *value) {
    h = mix_u64(h, (u64)value->type);
    switch (value->type) {
    case DE__MANIFEST_BOOL:
        return mix_u64(h, value->u.b ? 1u : 0u);
    case DE__MANIFEST_INT:
        return mix_u64(h, (u64)value->u.i);
    case DE__MANIFEST_FLOAT: {
        u64 bits;
        memcpy(&bits, &value->u.f, sizeof bits);
        return mix_u64(h, bits);
    }
    case DE__MANIFEST_STRING:
        return mix_str(h, value->u.s);
    case DE__MANIFEST_STRING_ARRAY:
        h = mix_u64(h, value->u.arr.count);
        for (u32 i = 0; i < value->u.arr.count; ++i)
            h = mix_str(h, value->u.arr.items[i]);
        return h;
    case DE__MANIFEST_ARRAY:
        h = mix_u64(h, value->u.values.count);
        for (u32 i = 0; i < value->u.values.count; ++i)
            h = value_hash(h, &value->u.values.items[i]);
        return h;
    }
    return h;
}
static u64 settings_hash(const de__manifest_asset *a) {
    u64 h = mix_u64(0x5e771265ull, a->setting_count);
    for (u32 i = 0; i < a->setting_count; ++i)
        h = value_hash(mix_str(h, a->settings[i].key), &a->settings[i].value);
    return h;
}

static u64 source_hash(const de__manifest *m, const de__manifest_asset *a) {
    u64 h = mix_str(0x50a2ce5ull, m->asset_root);
    h = mix_str(h, a->source);
    h = mix_u64(h, a->locale_count);
    for (u32 i = 0; i < a->locale_count; ++i) {
        h = mix_str(h, a->locales[i].locale);
        h = mix_str(h, a->locales[i].source);
    }
    return h;
}

/* Insertion sort by ID: manifests are small, and the input is mostly in file order. */
static void sort_entries(de__manifest_digest_entry *e, u32 n) {
    for (u32 i = 1; i < n; ++i) {
        de__manifest_digest_entry x = e[i];
        u32 j = i;
        while (j > 0 && e[j - 1u].id.hash > x.id.hash) {
            e[j] = e[j - 1u];
            --j;
        }
        e[j] = x;
    }
}

de_result de__manifest_digest_build(const de__manifest *m, const de__resolved *res,
                                    const de_allocator *alloc, de__manifest_digest *out) {
    memset(out, 0, sizeof *out);
    out->alloc = alloc;
    if (m == NULL || res == NULL || m->asset_count == 0)
        return DE_OK;
    de__manifest_digest_entry *e = (de__manifest_digest_entry *)DE_ALLOC(
        alloc, sizeof(de__manifest_digest_entry) * m->asset_count,
        DE_ALIGNOF(de__manifest_digest_entry), DE_MEM_ASSET);
    if (e == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset manifest diff: out of memory");
    u32 n = 0;
    for (u32 i = 0; i < m->asset_count; ++i) {
        de_asset_id id = res->asset_ids[i];
        if (id.hash == 0)
            continue; /* an invalid ID string: not loadable */
        const de__manifest_asset *a = &m->assets[i];
        e[n].id = id;
        e[n].type_hash = de_hash64_str(a->type);
        e[n].source_hash = source_hash(m, a);
        e[n].settings_hash = settings_hash(a);
        ++n;
    }
    sort_entries(e, n);
    out->entries = e;
    out->count = n;
    out->capacity = m->asset_count;
    return DE_OK;
}

void de__manifest_digest_free(de__manifest_digest *d) {
    if (d->entries != NULL)
        DE_FREE(d->alloc, d->entries, sizeof(de__manifest_digest_entry) * d->capacity);
    d->entries = NULL;
    d->count = d->capacity = 0;
}

de_result de__manifest_diff(const de__manifest_digest *old_d, const de__manifest_digest *new_d,
                            const de_allocator *alloc, de__manifest_change **out, u32 *count) {
    *out = NULL;
    *count = 0;
    u32 cap = old_d->count + new_d->count;
    if (cap == 0)
        return DE_OK;
    de__manifest_change *c = (de__manifest_change *)DE_ALLOC(
        alloc, sizeof(de__manifest_change) * cap, DE_ALIGNOF(de__manifest_change), DE_MEM_ASSET);
    if (c == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset manifest diff: out of memory");
    u32 i = 0, j = 0, n = 0;
    while (i < old_d->count || j < new_d->count) {
        const de__manifest_digest_entry *a = i < old_d->count ? &old_d->entries[i] : NULL;
        const de__manifest_digest_entry *b = j < new_d->count ? &new_d->entries[j] : NULL;
        if (b == NULL || (a != NULL && a->id.hash < b->id.hash)) {
            c[n++] = (de__manifest_change){a->id, DE__MANIFEST_REMOVED, 0};
            ++i;
        } else if (a == NULL || b->id.hash < a->id.hash) {
            c[n++] = (de__manifest_change){b->id, DE__MANIFEST_ADDED, 0};
            ++j;
        } else {
            u32 what = 0;
            if (a->type_hash != b->type_hash)
                what |= DE__MANIFEST_CHANGED_TYPE;
            if (a->source_hash != b->source_hash)
                what |= DE__MANIFEST_CHANGED_SOURCE;
            if (a->settings_hash != b->settings_hash)
                what |= DE__MANIFEST_CHANGED_SETTINGS;
            if (what != 0)
                c[n++] = (de__manifest_change){a->id, DE__MANIFEST_CHANGED, what};
            ++i;
            ++j;
        }
    }
    if (n == 0) {
        DE_FREE(alloc, c, sizeof(de__manifest_change) * cap);
        return DE_OK;
    }
    /* Exact size for the caller's free. */
    if (n != cap) {
        void *p =
            DE_REALLOC(alloc, c, sizeof(de__manifest_change) * cap, sizeof(de__manifest_change) * n,
                       DE_ALIGNOF(de__manifest_change), DE_MEM_ASSET);
        if (p == NULL) {
            DE_FREE(alloc, c, sizeof(de__manifest_change) * cap);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset manifest diff: out of memory");
        }
        c = (de__manifest_change *)p;
    }
    *out = c;
    *count = n;
    return DE_OK;
}
