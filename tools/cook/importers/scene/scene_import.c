/* Scene/prefab TOML import belongs to the tools; runtime consumes DSCN only. */
#include "cook/importers/scene/scene_import.h"
#include "asset/scene/scene_format.h"
#include "asset/asset_internal.h"
#include "core/de_array.h"
#include "core/de_toml_internal.h"
#include <dengine/de_reflect.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct imp_entity {
    u64 id;
    u64 parent_id;              /* 0: none */
    u32 entry;                  /* index in [[entity]] */
    u32 parent;                 /* sorted index, or DE__SCENE_NO_PARENT */
    u32 first_cand, cand_count; /* its candidates in imp.cands */
    int line;
} imp_entity;

/* A component table of an entity that names a known struct type. */
typedef struct imp_cand {
    const char *name; /* the TOML key (lives in the parsed document) */
    de_type_id type;
} imp_cand;

/* A component in output order. */
typedef struct imp_comp {
    const char *name;
    de_type_id type;
    u32 type_index;
    u64 data_off;
    u32 data_size;
} imp_comp;

typedef struct imp {
    de_asset_import_ctx *ctx;
    de_asset_system *s;
    const de_type_registry *reg;
    const de_allocator *a; /* the import's scratch allocator */
    const char *source;    /* the source path, for messages */
    imp_entity *ents;      /* sorted by id after imp_sort */
    u32 ent_count;
    de_array refs; /* u64 asset IDs, in first-seen order, unique */
    de_result cb_err;
} imp;

static void imp_warn(imp *m, int line, const char *fmt, ...) DE_PRINTF_FMT(3, 4);
static void imp_warn(imp *m, int line, const char *fmt, ...) {
    char msg[384];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (line > 0)
        DE_WARN(m->s->log, "%s:%d: %s", m->source, line, msg);
    else
        DE_WARN(m->s->log, "%s: %s", m->source, msg);
}

/* A scene ID: a positive integer or a "0x" string of 1 to 16 hex digits. 0 when invalid. */
static u64 imp_scene_id(const toml_datum_t *v) {
    if (v->type == TOML_INT64)
        return v->u.int64 > 0 ? (u64)v->u.int64 : 0u;
    if (v->type != TOML_STRING)
        return 0;
    const char *p = v->u.str.ptr;
    int n = v->u.str.len;
    if (n < 3 || n > 18 || p[0] != '0' || (p[1] != 'x' && p[1] != 'X'))
        return 0;
    u64 x = 0;
    for (int i = 2; i < n; ++i) {
        char c = p[i];
        u32 d;
        if (c >= '0' && c <= '9')
            d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = (u32)(c - 'A' + 10);
        else
            return 0;
        x = (x << 4) | d;
    }
    return x;
}

static int imp_cmp_entity(const void *a, const void *b) {
    u64 x = ((const imp_entity *)a)->id, y = ((const imp_entity *)b)->id;
    return x < y ? -1 : x > y ? 1 : 0;
}
static int imp_cmp_cand(const void *a, const void *b) {
    return strcmp(((const imp_cand *)a)->name, ((const imp_cand *)b)->name);
}
static int imp_cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static int imp_cmp_u64(const void *a, const void *b) {
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static u32 imp_find(const imp *m, u64 id) {
    u32 lo = 0, hi = m->ent_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        if (m->ents[mid].id == id)
            return mid;
        if (m->ents[mid].id < id)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return DE__SCENE_NO_PARENT;
}

/* Serializer context while cooking: entity fields keep their scene IDs (checked against the
 * file); asset fields become indices + 1 into `refs` (each one a cook dependency). */
static u64 imp_scene_id_to_entity(u64 scene_id, void *user) {
    imp *m = (imp *)user;
    if (scene_id != 0 && imp_find(m, scene_id) == DE__SCENE_NO_PARENT) {
        imp_warn(m, 0,
                 "an entity field names scene ID 0x%016llx, which is not in the file; it "
                 "will be null",
                 (unsigned long long)scene_id);
        return 0;
    }
    return scene_id;
}
static u64 imp_entity_to_scene_id(u64 bits, void *user) {
    (void)user;
    return bits;
}
static u32 imp_id_to_asset(u64 id, const char *asset_type, void *user) {
    (void)asset_type;
    imp *m = (imp *)user;
    if (id == 0)
        return 0;
    const u64 *ids = DE_ARRAY_DATA(&m->refs, u64);
    for (u32 i = 0; i < m->refs.count; ++i)
        if (ids[i] == id)
            return i + 1u;
    if (de_array_push(&m->refs, &id) != DE_OK) {
        m->cb_err = DE_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return m->refs.count;
}
static u64 imp_asset_to_id(u32 handle, const char *asset_type, void *user) {
    (void)asset_type;
    imp *m = (imp *)user;
    return handle != 0 && handle <= m->refs.count ? DE_ARRAY_AT(&m->refs, u64, handle - 1u) : 0u;
}

/* Pass 1: entity tables (`id`, `parent`, component candidates). */
static de_result imp_collect(imp *m, const toml_datum_t *arr, imp_cand **out_cands,
                             u32 *out_cand_count) {
    u32 n = (u32)arr->u.arr.size;
    u32 cand_cap = 0;
    for (u32 i = 0; i < n; ++i)
        cand_cap += (u32)arr->u.arr.elem[i].u.tab.size;
    m->ents = (imp_entity *)DE_ALLOC(m->a, sizeof(imp_entity) * (n ? n : 1u),
                                     DE_ALIGNOF(imp_entity), DE_MEM_SCRATCH);
    imp_cand *cands = (imp_cand *)DE_ALLOC(m->a, sizeof(imp_cand) * (cand_cap ? cand_cap : 1u),
                                           DE_ALIGNOF(imp_cand), DE_MEM_SCRATCH);
    if (m->ents == NULL || cands == NULL)
        return de_asset_import_fail(m->ctx, DE_ERR_OUT_OF_MEMORY, "no scratch memory");
    u32 nc = 0;
    for (u32 i = 0; i < n; ++i) {
        const toml_datum_t *t = &arr->u.arr.elem[i];
        imp_entity *e = &m->ents[i];
        memset(e, 0, sizeof *e);
        e->entry = i;
        e->line = t->lineno;
        e->parent = DE__SCENE_NO_PARENT;
        e->first_cand = nc;
        bool has_id = false;
        for (int32_t k = 0; k < t->u.tab.size; ++k) {
            const char *key = t->u.tab.key[k];
            const toml_datum_t *v = &t->u.tab.value[k];
            if (strcmp(key, "id") == 0) {
                e->id = imp_scene_id(v);
                if (e->id == 0)
                    return de_asset_import_fail(
                        m->ctx, DE_ERR_PARSE,
                        "%s:%d: entity %u: `id` must be a positive integer or a \"0x...\" "
                        "string of 1 to 16 hex digits",
                        m->source, v->lineno, (unsigned)i);
                has_id = true;
            } else if (strcmp(key, "parent") == 0) {
                e->parent_id = imp_scene_id(v);
                if (e->parent_id == 0)
                    return de_asset_import_fail(
                        m->ctx, DE_ERR_PARSE,
                        "%s:%d: entity %u: `parent` must be a scene ID of the file", m->source,
                        v->lineno, (unsigned)i);
            } else if (v->type != TOML_TABLE) {
                imp_warn(m, v->lineno,
                         "entity %u: key `%s` is not `id`, `parent` or a "
                         "component table; skipped",
                         (unsigned)i, key);
            } else if (strcmp(key, "DeChildOf") == 0 || strcmp(key, "DeChildren") == 0) {
                imp_warn(m, v->lineno,
                         "entity %u: `%s` is skipped; the hierarchy comes from "
                         "`parent`",
                         (unsigned)i, key);
            } else {
                de_type_id tid = de_type_find(m->reg, key);
                if (de_type_get(m->reg, tid) == NULL) {
                    imp_warn(m, v->lineno, "entity %u: unknown component type `%s`; skipped",
                             (unsigned)i, key);
                } else if (strlen(key) > DE__SCENE_MAX_NAME) {
                    imp_warn(m, v->lineno,
                             "entity %u: type name `%.40s...` is too long; "
                             "skipped",
                             (unsigned)i, key);
                } else {
                    cands[nc].name = key;
                    cands[nc].type = tid;
                    nc++;
                }
            }
        }
        if (!has_id)
            return de_asset_import_fail(m->ctx, DE_ERR_PARSE, "%s:%d: entity %u has no `id`",
                                        m->source, t->lineno, (unsigned)i);
        e->cand_count = nc - e->first_cand;
        /* Components of one entity in type-name order. */
        if (e->cand_count > 1)
            qsort(cands + e->first_cand, e->cand_count, sizeof(imp_cand), imp_cmp_cand);
    }
    m->ent_count = n;
    *out_cands = cands;
    *out_cand_count = nc;
    return DE_OK;
}

/* Sorts the entities by scene ID and resolves and checks the parents. */
static de_result imp_sort(imp *m, bool prefab, u32 *out_roots) {
    if (m->ent_count > 1)
        qsort(m->ents, m->ent_count, sizeof(imp_entity), imp_cmp_entity);
    for (u32 i = 1; i < m->ent_count; ++i)
        if (m->ents[i].id == m->ents[i - 1u].id)
            return de_asset_import_fail(m->ctx, DE_ERR_PARSE,
                                        "%s:%d: scene ID 0x%016llx is used twice (line %d)",
                                        m->source, m->ents[i].line,
                                        (unsigned long long)m->ents[i].id, m->ents[i - 1u].line);
    u32 roots = 0;
    for (u32 i = 0; i < m->ent_count; ++i) {
        imp_entity *e = &m->ents[i];
        if (e->parent_id == 0) {
            roots++;
            continue;
        }
        e->parent = imp_find(m, e->parent_id);
        if (e->parent == DE__SCENE_NO_PARENT)
            return de_asset_import_fail(m->ctx, DE_ERR_PARSE,
                                        "%s:%d: parent 0x%016llx is not an entity of the file",
                                        m->source, e->line, (unsigned long long)e->parent_id);
        if (e->parent == i)
            return de_asset_import_fail(m->ctx, DE_ERR_PARSE, "%s:%d: an entity is its own parent",
                                        m->source, e->line);
    }
    for (u32 i = 0; i < m->ent_count; ++i) {
        u32 at = i, steps = 0;
        while (at != DE__SCENE_NO_PARENT) {
            if (++steps > m->ent_count)
                return de_asset_import_fail(
                    m->ctx, DE_ERR_PARSE, "%s:%d: entity 0x%016llx is in a parent cycle", m->source,
                    m->ents[i].line, (unsigned long long)m->ents[i].id);
            at = m->ents[at].parent;
        }
    }
    if (prefab && roots != 1)
        return de_asset_import_fail(m->ctx, DE_ERR_PARSE,
                                    "%s: a prefab needs exactly one root entity, it has %u",
                                    m->source, (unsigned)roots);
    *out_roots = roots;
    return DE_OK;
}

/* Reads one component table and appends its binary object to `data`. `*kept` false: skipped. */
static de_result imp_component(imp *m, const de_text_doc *doc, const imp_entity *e,
                               const imp_cand *c, de_mem_writer *data, bool *kept) {
    *kept = false;
    const de_type_desc *td = de_type_get(m->reg, c->type);
    size_t align = td->align ? td->align : 1u;
    void *obj = DE_ALLOC(m->a, td->size ? td->size : 1u, align, DE_MEM_SCRATCH);
    if (obj == NULL)
        return de_asset_import_fail(m->ctx, DE_ERR_OUT_OF_MEMORY, "no scratch memory");
    DE_TRY(de_type_construct(m->reg, c->type, obj));
    de_ser_context sc = {imp_entity_to_scene_id, imp_scene_id_to_entity, imp_asset_to_id,
                         imp_id_to_asset, m};
    de_ser_options topt = {DE_SER_TEXT, DE_FIELD_SERIALIZE, &sc};
    de_result r =
        de_text_doc_read_member(doc, "entity", e->entry, c->name, m->reg, c->type, obj, &topt);
    if (r == DE_OK && m->cb_err != DE_OK)
        r = m->cb_err;
    if (r == DE_ERR_VERSION_MISMATCH) {
        /* A newer stored version, or `migrate` dropped it (SPEC §9.2: skip the object). */
        const de_error *er = de_error_last();
        imp_warn(m, e->line, "entity 0x%016llx: component `%s` skipped: %s",
                 (unsigned long long)e->id, c->name, er != NULL ? er->message : "");
        de_type_destruct(m->reg, c->type, obj);
        de_error_clear();
        return DE_OK;
    }
    if (r != DE_OK) {
        de_type_destruct(m->reg, c->type, obj);
        const de_error *er = de_error_last();
        return de_asset_import_fail(m->ctx, r, "entity 0x%016llx, component %s: %s",
                                    (unsigned long long)e->id, c->name,
                                    er != NULL ? er->message : "read failed");
    }
    de_ser_options bopt = {DE_SER_BINARY, DE_FIELD_SERIALIZE, &sc};
    de_ser_binary_options bin = {DE_SER_BINARY_SELF_DESCRIBING, NULL, false};
    de_writer w = de_writer_from_mem(data);
    r = de_serialize_binary(m->reg, c->type, obj, &bopt, &bin, &w);
    de_type_destruct(m->reg, c->type, obj);
    if (r != DE_OK)
        return de_asset_import_fail(m->ctx, r,
                                    "entity 0x%016llx, component %s: binary write "
                                    "failed",
                                    (unsigned long long)e->id, c->name);
    *kept = true;
    return DE_OK;
}

static u32 imp_type_index(const char *const *names, u32 n, const char *name) {
    u32 lo = 0, hi = n;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        int c = strcmp(names[mid], name);
        if (c == 0)
            return mid;
        if (c < 0)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return DE__SCENE_NO_PARENT;
}

static de_result scene_import_kind(de_asset_import_ctx *ctx, bool prefab) {
    imp m;
    memset(&m, 0, sizeof m);
    m.ctx = ctx;
    m.s = (de_asset_system *)de_asset_import_user(ctx);
    m.a = de_asset_import_allocator(ctx);
    m.source = de_asset_import_source_path(ctx);
    m.reg = m.s != NULL ? m.s->desc.types : NULL;
    if (m.reg == NULL)
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                    "no type registry for scene components (set "
                                    "de_asset_system_desc.types)");
    u64 len = 0;
    const char *src = (const char *)de_asset_import_source(ctx, &len);
    if (len > (u64)INT32_MAX)
        return de_asset_import_fail(ctx, DE_ERR_PARSE, "%s is too large", m.source);
    DE_ARRAY_INIT_TAG(&m.refs, u64, m.a, DE_MEM_SCRATCH);

    toml_result_t tr = de__toml_parse(m.a, src, (int)len);
    if (!tr.ok) {
        de_result fr = de_asset_import_fail(ctx, DE_ERR_PARSE, "%s: %s", m.source, tr.errmsg);
        de__toml_free(tr);
        return fr;
    }
    de_text_doc *doc = NULL;
    de_mem_writer data;
    de_mem_writer_init(&data, m.a);
    de_result r = DE_OK;
    imp_cand *cands = NULL;
    imp_comp *comps = NULL;
    const char **names = NULL;
    u32 cand_count = 0, comp_count = 0, type_count = 0, roots = 0;

    /* Top level: `[[entity]]`; anything else is ignored with a warning. */
    const toml_datum_t *arr = NULL;
    for (int32_t k = 0; k < tr.toptab.u.tab.size; ++k) {
        const toml_datum_t *v = &tr.toptab.u.tab.value[k];
        if (strcmp(tr.toptab.u.tab.key[k], "entity") != 0) {
            imp_warn(&m, v->lineno, "top-level key `%s` is not `entity`; skipped",
                     tr.toptab.u.tab.key[k]);
            continue;
        }
        bool ok = v->type == TOML_ARRAY;
        for (int32_t i = 0; ok && i < v->u.arr.size; ++i)
            ok = v->u.arr.elem[i].type == TOML_TABLE;
        if (!ok) {
            r = de_asset_import_fail(ctx, DE_ERR_PARSE,
                                     "%s:%d: `entity` must be an array of "
                                     "tables ([[entity]])",
                                     m.source, v->lineno);
            goto done;
        }
        arr = v;
    }
    if (arr != NULL) {
        r = imp_collect(&m, arr, &cands, &cand_count);
        if (r != DE_OK)
            goto done;
    }
    r = imp_sort(&m, prefab, &roots);
    if (r != DE_OK)
        goto done;

    /* Components in output order: entities by scene ID, then type name. */
    r = de_text_doc_parse(m.a, src, (size_t)len, m.source, &doc);
    if (r != DE_OK) {
        r = de_asset_import_fail(ctx, r, "%s: the text reader refused the file", m.source);
        goto done;
    }
    comps = (imp_comp *)DE_ALLOC(m.a, sizeof(imp_comp) * (cand_count ? cand_count : 1u),
                                 DE_ALIGNOF(imp_comp), DE_MEM_SCRATCH);
    names = (const char **)DE_ALLOC(m.a, sizeof(char *) * (cand_count ? cand_count : 1u),
                                    DE_ALIGNOF(char *), DE_MEM_SCRATCH);
    if (comps == NULL || names == NULL) {
        r = de_asset_import_fail(ctx, DE_ERR_OUT_OF_MEMORY, "no scratch memory");
        goto done;
    }
    for (u32 i = 0; i < m.ent_count; ++i) {
        imp_entity *e = &m.ents[i];
        u32 first = comp_count;
        for (u32 k = 0; k < e->cand_count; ++k) {
            const imp_cand *c = &cands[e->first_cand + k];
            u64 before = data.size;
            bool kept = false;
            r = imp_component(&m, doc, e, c, &data, &kept);
            if (r != DE_OK)
                goto done;
            if (!kept)
                continue;
            if (data.size - before > UINT32_MAX) {
                r = de_asset_import_fail(ctx, DE_ERR_PARSE, "component %s is too large", c->name);
                goto done;
            }
            comps[comp_count].name = c->name;
            comps[comp_count].type = c->type;
            comps[comp_count].data_off = before;
            comps[comp_count].data_size = (u32)(data.size - before);
            names[comp_count] = c->name;
            comp_count++;
        }
        /* Reuse the candidate range for the kept components. */
        e->first_cand = first;
        e->cand_count = comp_count - first;
    }

    /* Type table: the used names, sorted and unique. */
    if (comp_count > 1)
        qsort(names, comp_count, sizeof(char *), imp_cmp_str);
    for (u32 i = 0; i < comp_count; ++i)
        if (type_count == 0 || strcmp(names[type_count - 1u], names[i]) != 0)
            names[type_count++] = names[i];
    for (u32 i = 0; i < comp_count; ++i)
        comps[i].type_index = imp_type_index(names, type_count, comps[i].name);

    /* Asset references: sorted, unique; each one a cook dependency. */
    u64 *refs = DE_ARRAY_DATA(&m.refs, u64);
    if (m.refs.count > 1)
        qsort(refs, m.refs.count, sizeof(u64), imp_cmp_u64);

    /* Assemble the output. */
    u32 string_bytes = 0;
    for (u32 i = 0; i < type_count; ++i)
        string_bytes += (u32)strlen(names[i]) + 1u;
    u64 head = (u64)DE__SCENE_HEADER_SIZE + (u64)type_count * DE__SCENE_TYPE_SIZE +
               (u64)m.ent_count * DE__SCENE_ENTITY_SIZE + (u64)comp_count * DE__SCENE_COMP_SIZE +
               (u64)m.refs.count * DE__SCENE_ASSET_SIZE + string_bytes;
    if (m.ent_count > DE__SCENE_MAX_COUNT || comp_count > DE__SCENE_MAX_COUNT ||
        m.refs.count > DE__SCENE_MAX_COUNT || head > (u64)SIZE_MAX) {
        r = de_asset_import_fail(ctx, DE_ERR_PARSE, "%s: too many entities or components",
                                 m.source);
        goto done;
    }
    u8 *out = (u8 *)DE_ALLOC(m.a, (size_t)head, 8, DE_MEM_SCRATCH);
    if (out == NULL) {
        r = de_asset_import_fail(ctx, DE_ERR_OUT_OF_MEMORY, "no scratch memory");
        goto done;
    }
    memset(out, 0, (size_t)head);
    memcpy(out, DE__SCENE_MAGIC, 4);
    de__scene_put_u32(out + 4, DE__SCENE_VERSION);
    de__scene_put_u32(out + 8, prefab ? (u32)DE__SCENE_KIND_PREFAB : (u32)DE__SCENE_KIND_SCENE);
    de__scene_put_u32(out + 12, type_count);
    de__scene_put_u32(out + 16, m.ent_count);
    de__scene_put_u32(out + 20, comp_count);
    de__scene_put_u32(out + 24, m.refs.count);
    de__scene_put_u32(out + 28, string_bytes);
    de__scene_put_u64(out + 32, (u64)data.size);
    de__scene_put_u32(out + 40, roots);
    u8 *p = out + DE__SCENE_HEADER_SIZE;
    u32 str_off = 0;
    for (u32 i = 0; i < type_count; ++i, p += DE__SCENE_TYPE_SIZE) {
        de_type_id tid = de_type_find(m.reg, names[i]);
        const de_type_desc *td = de_type_get(m.reg, tid);
        u32 nlen = (u32)strlen(names[i]);
        de__scene_put_u32(p, str_off);
        de__scene_put_u32(p + 4, nlen);
        de__scene_put_u32(p + 8, td != NULL ? td->version : 0u);
        de__scene_put_u64(p + 16, de_type_schema_hash(m.reg, tid));
        str_off += nlen + 1u;
    }
    for (u32 i = 0; i < m.ent_count; ++i, p += DE__SCENE_ENTITY_SIZE) {
        const imp_entity *e = &m.ents[i];
        de__scene_put_u64(p, e->id);
        de__scene_put_u32(p + 8, e->parent);
        de__scene_put_u32(p + 12, e->first_cand);
        de__scene_put_u32(p + 16, e->cand_count);
    }
    for (u32 i = 0; i < comp_count; ++i, p += DE__SCENE_COMP_SIZE) {
        de__scene_put_u32(p, comps[i].type_index);
        de__scene_put_u32(p + 4, comps[i].data_size);
        de__scene_put_u64(p + 8, comps[i].data_off);
    }
    for (u32 i = 0; i < m.refs.count; ++i, p += DE__SCENE_ASSET_SIZE)
        de__scene_put_u64(p, refs[i]);
    for (u32 i = 0; i < type_count; ++i) {
        size_t nlen = strlen(names[i]) + 1u;
        memcpy(p, names[i], nlen);
        p += nlen;
    }
    DE_ASSERT((u64)(p - out) == head);
    r = de_asset_import_write(ctx, out, head);
    if (r == DE_OK && data.size != 0)
        r = de_asset_import_write(ctx, data.data, data.size);
    for (u32 i = 0; r == DE_OK && i < m.refs.count; ++i)
        r = de_asset_import_add_dependency(ctx, (de_asset_id){refs[i]});

done:
    de_text_doc_destroy(doc);
    de_mem_writer_free(&data);
    de_array_free(&m.refs);
    de__toml_free(tr);
    /* The scratch arrays (entities, candidates, components, names, output) go with the import. */
    return r;
}

static de_result scene_import(de_asset_import_ctx *ctx) {
    return scene_import_kind(ctx, false);
}
static de_result prefab_import(de_asset_import_ctx *ctx) {
    return scene_import_kind(ctx, true);
}

de_asset_type_desc de_scene_import_type_desc(void) {
    return (de_asset_type_desc){
        .name = "scene", .importer_version = DE__SCENE_IMPORTER_VER, .import = scene_import};
}
de_asset_type_desc de_prefab_import_type_desc(void) {
    return (de_asset_type_desc){
        .name = "prefab", .importer_version = DE__SCENE_IMPORTER_VER, .import = prefab_import};
}
