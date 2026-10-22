#include "anim/graph_internal.h"
#include "anim/anim_math.h"
#include <dengine/de_reflect.h>
#include <string.h>

de_component de_anim_params_component(const de_anim_system *s) {
    return s ? s->params_component : (de_component){0};
}
static de_component component(de_world *w) {
    return w ? de_component_find(w, "DeAnimParams") : (de_component){0};
}
static i32 find(const DeAnimParams *p, de_name name, de_anim_param_kind kind) {
    if (p && p->graph && p->count <= DE_ANIM_MAX_PARAMS)
        for (u32 i = 0; i < p->count; ++i)
            if (p->names[i].hash == name.hash && p->kinds[i] == (u32)kind)
                return (i32)i;
    return -1;
}
static DeAnimParams *edit(de_world *w, de_entity e, de_name name, de_anim_param_kind kind,
                          i32 *index) {
    de_component c = component(w);
    DeAnimParams *p = c.id ? de_entity_get(w, e, c) : NULL;
    *index = find(p, name, kind);
    if (*index < 0) {
        if (p && p->graph && !p->warned_unknown) {
            p->warned_unknown = true;
            DE_WARN(de_log_register_category("anim", DE_LOG_INFO),
                    "unknown or wrong-type animation parameter 0x%016llx",
                    (unsigned long long)name.hash);
        }
        return NULL;
    }
    return p;
}
de_result de_anim_graph_bind(de_world *w, de_entity e, de_anim_graph graph) {
    de_anim_system *s = de__anim_for_world(w);
    de__anim_graph *g = de__anim_graph_get(s, graph);
    if (!s || !de_entity_alive(w, e) || !s->params_component.id || (graph.id && !g) ||
        !de_entity_get_const(w, e, s->component))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "animation graph binding");
    DeAnimParams *p = de_entity_get(w, e, s->params_component);
    if (graph.id && !p)
        p = de_entity_add(w, e, s->params_component);
    if (graph.id && !p)
        return DE_ERR_OUT_OF_MEMORY;
    if (p && p->graph != graph.id) {
        memset(p, 0, sizeof *p);
        p->graph = graph.id;
        if (g) {
            p->count = g->desc.param_count;
            for (u32 i = 0; i < p->count; ++i) {
                const de_anim_param_desc *d = &g->params[i];
                p->names[i] = d->name;
                p->kinds[i] = (u32)d->kind;
                if (d->kind == DE_ANIM_PARAM_FLOAT)
                    p->values[i] = d->float_default;
                else if (d->kind == DE_ANIM_PARAM_BOOL && d->bool_default)
                    p->bools |= 1u << i;
            }
        }
    }
    /* Adding storage can move the animator to another archetype. */
    DeAnimator *a = de_entity_get(w, e, s->component);
    a->graph = graph;
    return DE_OK;
}
void de_anim_set_float(de_world *w, de_entity e, de_name name, f32 value) {
    if (!de__anim_finite(value)) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "animation parameter must be finite");
        return;
    }
    i32 i;
    DeAnimParams *p = edit(w, e, name, DE_ANIM_PARAM_FLOAT, &i);
    if (p)
        p->values[i] = value;
}
void de_anim_set_bool(de_world *w, de_entity e, de_name name, bool value) {
    i32 i;
    DeAnimParams *p = edit(w, e, name, DE_ANIM_PARAM_BOOL, &i);
    if (p) {
        u32 bit = 1u << (u32)i;
        p->bools = value ? p->bools | bit : p->bools & ~bit;
    }
}
void de_anim_trigger(de_world *w, de_entity e, de_name name) {
    i32 i;
    DeAnimParams *p = edit(w, e, name, DE_ANIM_PARAM_TRIGGER, &i);
    if (p)
        p->triggers |= 1u << (u32)i;
}
bool de_anim_get_float(de_world *w, de_entity e, de_name name, f32 *out) {
    de_component c = component(w);
    const DeAnimParams *p = c.id ? de_entity_get_const(w, e, c) : NULL;
    i32 i = find(p, name, DE_ANIM_PARAM_FLOAT);
    if (i < 0 || !out)
        return false;
    *out = p->values[i];
    return true;
}
bool de_anim_get_bool(de_world *w, de_entity e, de_name name, bool *out) {
    de_component c = component(w);
    const DeAnimParams *p = c.id ? de_entity_get_const(w, e, c) : NULL;
    i32 i = find(p, name, DE_ANIM_PARAM_BOOL);
    if (i < 0 || !out)
        return false;
    *out = (p->bools & (1u << (u32)i)) != 0;
    return true;
}
bool de_anim_consume_trigger(de_world *w, de_entity e, de_name name) {
    i32 i;
    DeAnimParams *p = edit(w, e, name, DE_ANIM_PARAM_TRIGGER, &i);
    if (!p)
        return false;
    u32 bit = 1u << (u32)i;
    bool set = (p->triggers & bit) != 0;
    p->triggers &= ~bit;
    return set;
}
de_result de_anim__graph_init(de_anim_system *s, de_world *w) {
    const u32 flags = DE_FIELD_DEFAULT | DE_FIELD_EDITOR_HIDDEN;
    const de_field_info fields[] = {DE_FIELD(DeAnimParams, graph, DE_FK_U32, flags),
                                    DE_FIELD(DeAnimParams, count, DE_FK_U32, flags),
                                    DE_FIELD_EX(DeAnimParams, names, DE_FK_FIXED_ARRAY, flags,
                                                {0, 0, 0, 0, "name", DE_ANIM_MAX_PARAMS}),
                                    DE_FIELD_EX(DeAnimParams, kinds, DE_FK_FIXED_ARRAY, flags,
                                                {0, 0, 0, 0, "u32", DE_ANIM_MAX_PARAMS}),
                                    DE_FIELD_EX(DeAnimParams, values, DE_FK_FIXED_ARRAY, flags,
                                                {0, 0, 0, 0, "f32", DE_ANIM_MAX_PARAMS}),
                                    DE_FIELD(DeAnimParams, bools, DE_FK_U32, flags),
                                    DE_FIELD(DeAnimParams, triggers, DE_FK_U32, flags),
                                    DE_FIELD(DeAnimParams, warned_unknown, DE_FK_BOOL, flags)};
    de_type_registry *types = de_world_types(w);
    de_type_desc desc = DE_TYPE_DESC(DeAnimParams, 1, fields);
    const de_type_desc *old = de_type_get(types, de_type_find(types, desc.name));
    if (old && (old->size != desc.size || old->align != desc.align || old->version != desc.version))
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "animation parameter reflection conflict");
    de_type_id type = de_type_register(types, &desc);
    if (!type.id)
        return DE_ERR_INTERNAL;
    s->params_component = de_component_register(w, type, DE_COMP_SNAPSHOT);
    return s->params_component.id ? DE_OK : DE_ERR_INTERNAL;
}
