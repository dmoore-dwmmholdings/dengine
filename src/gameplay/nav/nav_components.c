/* nav_components.c: DeNavLink, DeNavModifier and DeNavGrid2D (DENG-582). See nav_components.h. */
#include "gameplay/nav/nav_components.h"

#include <dengine/de_reflect.h>
#include <string.h>

#define AUTHORED DE_FIELD_DEFAULT
#define HIDDEN   (DE_FIELD_DEFAULT | DE_FIELD_EDITOR_HIDDEN)

static de_result last_error_or(de_result fallback) {
    const de_error *e = de_error_last();
    return e != NULL && e->result != DE_OK ? e->result : fallback;
}

/* The id of `d->name`: the registered type when its layout matches, or a new registration. */
static de_result ensure_type(de_type_registry *reg, const de_type_desc *d, de_type_id *out) {
    *out = de_type_find(reg, d->name);
    if (DE_HANDLE_VALID(*out)) {
        const de_type_desc *cur = de_type_get(reg, *out);
        if (cur == NULL || cur->size != d->size || cur->align != d->align ||
            cur->version != d->version || cur->field_count != d->field_count)
            return DE_FAIL(DE_ERR_ALREADY_EXISTS,
                           "de__nav_components_register: the type registry has another type "
                           "called %s",
                           d->name);
        return DE_OK;
    }
    const de_result r = de_type_register_ex(reg, d, out, NULL);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "de__nav_components_register: registering %s", d->name);
    return DE_OK;
}

/* The defaults match the DIDE mock engine's. */
static void construct_link(void *obj) {
    DeNavLink *l = (DeNavLink *)obj;
    memset(l, 0, sizeof *l);
    l->end.x = 4.0f;
    l->radius = 0.5f;
    l->bidirectional = true;
}

static void construct_modifier(void *obj) {
    DeNavModifier *m = (DeNavModifier *)obj;
    memset(m, 0, sizeof *m);
    m->size.x = m->size.y = m->size.z = 2.0f;
    m->mode = DE_NAV_MODIFIER_AREA;
    m->area = 1;
}

static void construct_grid(void *obj) {
    DeNavGrid2D *g = (DeNavGrid2D *)obj;
    memset(g, 0, sizeof *g);
    g->width = g->height = 16;
    g->cell_size = 1.0f;
    memset(g->cells, 1, sizeof g->cells);
}

static de_result component(de_world *w, de_type_desc d, void (*construct)(void *),
                           de_component *out) {
    *out = de_component_find(w, d.name);
    if (DE_HANDLE_VALID(*out))
        return DE_OK;
    d.hooks.construct = construct;
    de_type_id t;
    DE_TRY(ensure_type(de_world_types(w), &d, &t));
    *out = de_component_register(w, t, DE_COMP_SNAPSHOT | DE_COMP_SERIALIZE);
    if (!DE_HANDLE_VALID(*out))
        return DE_FAIL_WRAP(last_error_or(DE_ERR_INTERNAL),
                            "de__nav_components_register: component %s", d.name);
    return DE_OK;
}

de_result de__nav_components_register(de_world *w, de__nav_component_ids *out) {
    if (w == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__nav_components_register: NULL argument");
    memset(out, 0, sizeof *out);
    de_type_registry *reg = de_world_types(w);

    static const de_enum_value k_mode[] = {
        {"area", DE_NAV_MODIFIER_AREA},
        {"carve", DE_NAV_MODIFIER_CARVE},
    };
    const de_type_id mode = de_type_find(reg, "de_nav_modifier_mode");
    if (DE_HANDLE_VALID(mode)) {
        const de_enum_info *e = de_enum_get(reg, mode);
        if (e == NULL || e->count != 2 || e->is_flags)
            return DE_FAIL(DE_ERR_ALREADY_EXISTS, "de__nav_components_register: the type "
                                                  "registry has another de_nav_modifier_mode");
    } else {
        const de_result r = de_enum_register(reg, "de_nav_modifier_mode", k_mode, 2, false);
        if (r != DE_OK)
            return DE_FAIL_WRAP(r, "de__nav_components_register: registering "
                                   "de_nav_modifier_mode");
    }

    static const de_field_info k_link[] = {
        DE_FIELD_EX(DeNavLink, end, DE_FK_VEC3, AUTHORED,
                    {0, 0, "m", "The far end, from the entity's position", 0, 0}),
        DE_FIELD_EX(DeNavLink, radius, DE_FK_F32, AUTHORED, {0.05, 10, "m", 0, 0, 0}),
        DE_FIELD(DeNavLink, bidirectional, DE_FK_BOOL, AUTHORED),
        DE_FIELD_EX(DeNavLink, area, DE_FK_U32, AUTHORED, {0, 15, 0, 0, 0, 0}),
    };
    DE_TRY(component(w, DE_TYPE_DESC(DeNavLink, 1, k_link), construct_link, &out->link));

    static const de_field_info k_modifier[] = {
        DE_FIELD_EX(DeNavModifier, size, DE_FK_VEC3, AUTHORED, {0, 0, "m", 0, 0, 0}),
        DE_FIELD_EX(DeNavModifier, mode, DE_FK_ENUM, AUTHORED,
                    {0, 0, 0, 0, "de_nav_modifier_mode", 0}),
        DE_FIELD_EX(DeNavModifier, area, DE_FK_U32, AUTHORED, {0, 15, 0, 0, 0, 0}),
    };
    DE_TRY(component(w, DE_TYPE_DESC(DeNavModifier, 1, k_modifier), construct_modifier,
                     &out->modifier));

    static const de_field_info k_grid[] = {
        DE_FIELD_EX(DeNavGrid2D, width, DE_FK_U32, AUTHORED, {1, 64, 0, 0, 0, 0}),
        DE_FIELD_EX(DeNavGrid2D, height, DE_FK_U32, AUTHORED, {1, 64, 0, 0, 0, 0}),
        DE_FIELD_EX(DeNavGrid2D, cell_size, DE_FK_F32, AUTHORED, {0.05, 100, "m", 0, 0, 0}),
        DE_FIELD_EX(DeNavGrid2D, cells, DE_FK_FIXED_ARRAY, HIDDEN,
                    {0, 0, 0, 0, "u8", DE_NAV_GRID2D_CELLS}),
    };
    DE_TRY(component(w, DE_TYPE_DESC(DeNavGrid2D, 1, k_grid), construct_grid, &out->grid));
    return DE_OK;
}
