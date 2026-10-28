/* editor_reflect.c: reflection queries (DENG-624): types, asset types, cvars, debug outputs. */
#include "host/editor/editor_internal.h"

#include <dengine/de_debug.h>
#include <dengine/de_render.h>

#include "asset/slots_internal.h"
#include "ecs/ecs_internal.h"
#include "host/host_internal.h"
#include "reflect/reflect_internal.h"

#include <string.h>

typedef struct de__ed_reflect {
    u64 revision; /* the registry revision last announced */
} de__ed_reflect;

static de__ed_reflect g_reflect;

const char *de__ed_kind_name(de_field_kind k) {
    static const char *const names[DE_FIELD_KIND_COUNT] = {
        "bool", "i8",   "i16",  "i32",  "i64",    "u8",    "u16",  "u32",   "u64",
        "f32",  "f64",  "vec2", "vec3", "vec4",   "dvec3", "quat", "mat4",  "color",
        "name", "string", "entity", "asset", "enum", "flags", "struct", "array"};
    return (u32)k < DE_FIELD_KIND_COUNT ? names[k] : "unknown";
}

/* What a struct type is in the world: component, resource, event or a plain struct. */
static const char *type_role(const de_world *w, de_type_id id) {
    for (u32 i = 0; i < w->components.count; ++i)
        if (((const ecs_component_info *)de_array_at(&w->components, i))->type.id == id.id)
            return "component";
    for (u32 i = 0; i < w->resources.count; ++i)
        if (((const ecs_resource *)de_array_at(&w->resources, i))->type.id == id.id)
            return "resource";
    for (u32 i = 0; i < w->events.count; ++i)
        if (((const ecs_event_queue *)de_array_at(&w->events, i))->payload.id == id.id)
            return "event";
    return "struct";
}

static de_ep_list_str field_flags(de_ep_arena *a, u32 flags) {
    static const struct {
        u32 bit;
        const char *name;
    } k[] = {{DE_FIELD_SERIALIZE, "serialize"},   {DE_FIELD_REPLICATE, "replicate"},
             {DE_FIELD_SNAPSHOT, "snapshot"},     {DE_FIELD_EDITOR_HIDDEN, "editor_hidden"},
             {DE_FIELD_READONLY, "readonly"},     {DE_FIELD_ASSET_REF, "asset_ref"}};
    de_ep_list_str l = {0};
    l.items = (de_ep_str *)de__ed_alloc(a, sizeof(de_ep_str) * 6);
    if (l.items == NULL)
        return l;
    for (u32 i = 0; i < 6; ++i)
        if (flags & k[i].bit)
            l.items[l.count++] = de_ep_cstr(k[i].name);
    return l;
}

/* Fills `out` from a reflected field. */
void de__ed_field_info(de_ep_arena *a, const char *owner, const de_field_info *f, de_ep_field_info *out) {
    memset(out, 0, sizeof *out);
    out->name = de_ep_cstr(f->name);
    out->type = de_ep_cstr(de__ed_kind_name(f->kind));
    out->offset = f->offset;
    out->count = f->kind == DE_FK_FIXED_ARRAY ? f->attrs.array_count : 1u;
    out->flags = field_flags(a, f->flags);
    de_ep_kv_raw *kv = (de_ep_kv_raw *)de__ed_alloc(a, sizeof(de_ep_kv_raw) * 10);
    if (kv == NULL)
        return;
    u32 n = 0;
    if (f->attrs.nested_type != NULL)
        kv[n++] = (de_ep_kv_raw){de_ep_cstr("nestedType"), de__ed_raw_str(a, f->attrs.nested_type)};
    if (f->attrs.min < f->attrs.max)
        kv[n++] = (de_ep_kv_raw){de_ep_cstr("range"),
                                 de__ed_raw_range(a, f->attrs.min, f->attrs.max)};
    if (f->attrs.units != NULL)
        kv[n++] = (de_ep_kv_raw){de_ep_cstr("units"), de__ed_raw_str(a, f->attrs.units)};
    if (f->attrs.tooltip != NULL)
        kv[n++] = (de_ep_kv_raw){de_ep_cstr("tooltip"), de__ed_raw_str(a, f->attrs.tooltip)};
    /* Inspector hints (DENG-555 comments), from the hints table. */
    const de_field_hints *h = owner != NULL ? de_field_hints_get(owner, f->name) : NULL;
    if (h != NULL && h->group != NULL)
        kv[n++] = (de_ep_kv_raw){de_ep_cstr("group"), de__ed_raw_str(a, h->group)};
    if (h != NULL && h->order != 0)
        kv[n++] = (de_ep_kv_raw){de_ep_cstr("order"), de__ed_raw_f64(a, h->order)};
    if (h != NULL && h->show_if != NULL)
        kv[n++] = (de_ep_kv_raw){de_ep_cstr("showIf"), de__ed_raw_str(a, h->show_if)};
    if (h != NULL && h->slider_min < h->slider_max)
        kv[n++] = (de_ep_kv_raw){de_ep_cstr("slider"), de__ed_raw_range(a, h->slider_min, h->slider_max)};
    if (h != NULL && h->multiline)
        kv[n++] = (de_ep_kv_raw){de_ep_cstr("multiline"), de__ed_raw_bool(a, true)};
    if ((h != NULL && h->color) || f->kind == DE_FK_COLOR)
        kv[n++] = (de_ep_kv_raw){de_ep_cstr("color"), de__ed_raw_bool(a, true)};
    out->attributes.items = kv;
    out->attributes.count = n;
}

/* The registry entry at `index` as a TypeInfo; false when it is no type. */
static bool type_info(de__editor *ed, de_ep_arena *a, const de__type_entry *te, u32 index,
                      de_ep_type_info *out) {
    memset(out, 0, sizeof *out);
    if (te->name == NULL)
        return false;
    out->name = de_ep_cstr(te->name);
    out->schema_hash = te->hash;
    if (te->kind == DE__ENTRY_ENUM && te->enum_info != NULL) {
        const de_enum_info *ei = te->enum_info;
        out->kind = de_ep_cstr(ei->is_flags ? "flags" : "enum");
        out->size = 8;
        out->align = 8;
        out->version = 1;
        out->has_enum_values = true;
        out->enum_values.items = (de_ep_str *)de__ed_alloc(a, sizeof(de_ep_str) * (ei->count + 1));
        if (out->enum_values.items == NULL)
            return false;
        for (u32 i = 0; i < ei->count; ++i)
            out->enum_values.items[i] = de_ep_cstr(ei->values[i].name);
        out->enum_values.count = ei->count;
        return true;
    }
    if (te->kind != DE__ENTRY_STRUCT || te->cur == NULL)
        return false;
    const de_type_desc *d = &te->cur->desc;
    const de_type_id id = {(index + 1u) | (te->generation << DE__TYPE_INDEX_BITS)};
    out->kind = de_ep_cstr(type_role(ed->e->world, id));
    out->size = d->size;
    out->align = d->align != 0 ? d->align : 1u;
    out->version = d->version;
    out->fields.items =
        (de_ep_field_info *)de__ed_alloc(a, sizeof(de_ep_field_info) * (d->field_count + 1));
    if (out->fields.items == NULL)
        return false;
    for (u32 i = 0; i < d->field_count; ++i)
        de__ed_field_info(a, te->name, &d->fields[i], &out->fields.items[i]);
    out->fields.count = d->field_count;
    return true;
}

static void on_types(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    const de_type_registry *r = ed->e->types;
    const u32 n = r->entries.count;
    de_ep_type_list list;
    memset(&list, 0, sizeof list);
    list.types.items = (de_ep_type_info *)de__ed_alloc(a, sizeof(de_ep_type_info) * (n + 1));
    if (list.types.items == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "reflect.types: out of scratch memory");
        return;
    }
    for (u32 i = 0; i < n; ++i) {
        const de__type_entry *te = (const de__type_entry *)de_array_at(&r->entries, i);
        if (type_info(ed, a, te, i, &list.types.items[list.types.count]))
            ++list.types.count;
    }
    de_ep_write_response_reflect_types(w, req->id, &list);
}

static void on_asset_types(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                           mpack_writer_t *w) {
    de_ep_asset_type_list list;
    memset(&list, 0, sizeof list);
    u32 n = 0;
    const char *const *names = ed->e->assets ? de__asset_type_names(ed->e->assets, &n) : NULL;
    list.types.items = (de_ep_asset_type_info *)de__ed_alloc(a, sizeof(de_ep_asset_type_info) * (n + 1));
    for (u32 i = 0; i < n && list.types.items != NULL; ++i) {
        const de_asset_type t = de__asset_type_find(ed->e->assets, names[i]);
        const de_asset_type_desc *d = de__asset_type_get(ed->e->assets, t);
        de_ep_asset_type_info *o = &list.types.items[list.types.count++];
        memset(o, 0, sizeof *o);
        o->name = de_ep_cstr(names[i]);
        if (d == NULL)
            continue;
        o->extensions.items = (de_ep_str *)de__ed_alloc(a, sizeof(de_ep_str) * (d->extension_count + 1));
        for (u32 x = 0; x < d->extension_count && o->extensions.items != NULL; ++x)
            o->extensions.items[o->extensions.count++] = de_ep_cstr(d->extensions[x]);
        if (d->import != NULL) {
            o->has_importer = true;
            o->importer = de_ep_cstr(names[i]);
        }
    }
    de_ep_write_response_reflect_asset_types(w, req->id, &list);
}

typedef struct cvar_walk {
    de_ep_arena *a;
    de_ep_c_var *items;
    u32 count, cap;
} cvar_walk;

static void visit_cvar(de_cvar cv, void *user) {
    cvar_walk *cw = (cvar_walk *)user;
    if (cw->count >= cw->cap)
        return;
    de_ep_c_var *o = &cw->items[cw->count++];
    memset(o, 0, sizeof *o);
    o->name = de_ep_cstr(de_cvar_name(cv));
    o->help = de_ep_cstr(de_cvar_help(cv));
    switch (de_cvar_get_type(cv)) {
    case DE_CVAR_BOOL:
        o->type = de_ep_cstr("bool");
        o->value = de__ed_raw_bool(cw->a, de_cvar_bool(cv));
        break;
    case DE_CVAR_INT: {
        o->type = de_ep_cstr("int");
        mpack_writer_t w;
        de__ed_raw_begin(cw->a, &w);
        mpack_write_i64(&w, de_cvar_int(cv));
        o->value = de__ed_raw_end(cw->a, &w);
        break;
    }
    case DE_CVAR_FLOAT:
        o->type = de_ep_cstr("float");
        o->value = de__ed_raw_f64(cw->a, de_cvar_float(cv));
        break;
    case DE_CVAR_ENUM:
        o->type = de_ep_cstr("enum");
        o->value = de__ed_raw_str(cw->a, de_cvar_string(cv));
        break;
    default:
        o->type = de_ep_cstr("string");
        o->value = de__ed_raw_str(cw->a, de_cvar_string(cv));
        break;
    }
    static const struct {
        u32 bit;
        const char *name;
    } k[] = {{DE_CVAR_READONLY, "readonly"},
             {DE_CVAR_DEV_ONLY, "dev_only"},
             {DE_CVAR_SAVED, "saved"},
             {DE_CVAR_REPLICATED, "replicated"},
             {DE_CVAR_CHEAT, "cheat"}};
    const u32 flags = de_cvar_get_flags(cv);
    o->flags.items = (de_ep_str *)de__ed_alloc(cw->a, sizeof(de_ep_str) * 5);
    for (u32 i = 0; i < 5 && o->flags.items != NULL; ++i)
        if (flags & k[i].bit)
            o->flags.items[o->flags.count++] = de_ep_cstr(k[i].name);
}

static void on_cvars(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    (void)ed;
    cvar_walk cw = {a, NULL, 0, 4096};
    cw.items = (de_ep_c_var *)de__ed_alloc(a, sizeof(de_ep_c_var) * cw.cap);
    if (cw.items == NULL)
        cw.cap = 0;
    de_cvar_foreach_flag(0, visit_cvar, &cw);
    de_ep_c_var_list list;
    memset(&list, 0, sizeof list);
    list.cvars.items = cw.items;
    list.cvars.count = cw.count;
    de_ep_write_response_reflect_cvars(w, req->id, &list);
}

static void on_debug_outputs(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                             mpack_writer_t *w) {
    de_ep_debug_output_list list;
    memset(&list, 0, sizeof list);
#if DE_HAS_RENDER
    if (ed->e->renderer != NULL) {
        const de_render_stats *rs = de_renderer_stats(ed->e->renderer);
        if (rs != NULL && rs->pass_names != NULL) {
            list.outputs.items =
                (de_ep_debug_output *)de__ed_alloc(a, sizeof(de_ep_debug_output) * (rs->pass_count + 1));
            for (u32 i = 0; i < rs->pass_count && list.outputs.items != NULL; ++i) {
                list.outputs.items[list.outputs.count].name = de_ep_cstr(rs->pass_names[i]);
                list.outputs.items[list.outputs.count++].kind = de_ep_cstr("pass");
            }
        }
    }
#else
    (void)ed;
    (void)a;
#endif
    de_ep_write_response_reflect_debug_outputs(w, req->id, &list);
}

/* reflect.changed when the registry changed (a hot reload re-registered types). */
static void tick(de__editor *ed) {
    const u64 rev = de_type_registry_revision(ed->e->types);
    if (rev == g_reflect.revision)
        return;
    g_reflect.revision = rev;
    de_ep_reflect_changed d;
    memset(&d, 0, sizeof d);
    mpack_writer_t w;
    const u64 seq = de__ed_note_begin(ed, &w);
    de_ep_write_notify_reflect_changed(&w, seq, &d);
    de__ed_send(ed, &w, DE_EP_KIND_NOTIFICATION);
}

static void reset(de__editor *ed) {
    g_reflect.revision = de_type_registry_revision(ed->e->types);
}

void de__ed_reflect_register(de__editor *ed) {
    g_reflect.revision = de_type_registry_revision(ed->e->types);
    de__ed_on(ed, DE_EP_METHOD_REFLECT_TYPES, on_types);
    de__ed_on(ed, DE_EP_METHOD_REFLECT_ASSET_TYPES, on_asset_types);
    de__ed_on(ed, DE_EP_METHOD_REFLECT_CVARS, on_cvars);
    de__ed_on(ed, DE_EP_METHOD_REFLECT_DEBUG_OUTPUTS, on_debug_outputs);
    de__ed_on_tick(ed, tick);
    de__ed_on_reset(ed, reset);
}
