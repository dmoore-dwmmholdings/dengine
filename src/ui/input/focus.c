#include "focus_internal.h"
#include "ui/style/stylesheet_internal.h"
#include <math.h>
#include <string.h>

bool de_ui__focus_descendant(de_ui *ui, de_entity entity, de_entity ancestor) {
    if (!ancestor.bits)
        return true;
    for (u32 i = 0; entity.bits && i < ui->capacity; ++i) {
        if (entity.bits == ancestor.bits)
            return true;
        entity = de_entity_parent(ui->world, entity);
    }
    return false;
}
bool de_ui__focus_allowed(de_ui *ui, de_entity entity) {
    return ui && ui->world && ui->focus && entity.bits &&
           de_entity_has(ui->world, entity, ui->focus->focusable) &&
           de_entity_has(ui->world, entity, ui->ids.node) && de_ui__widget_enabled(ui, entity) &&
           de_ui__focus_descendant(ui, entity, ui->focus->scope);
}
static void ring_state(de_ui *ui, de_entity e, bool value) {
    if (!e.bits || !de_entity_alive(ui->world, e))
        return;
    const DeUiInteractionState *old = de_entity_get_const(ui->world, e, ui->ids.state);
    if (old && old->focused == value)
        return;
    DeUiInteractionState *state = old ? de_entity_get(ui->world, e, ui->ids.state)
                                      : de_entity_add(ui->world, e, ui->ids.state);
    if (state)
        state->focused = value;
}
static void ring(de_ui *ui, bool visible) {
    ui->focus->ring = visible;
}
void de_ui_set_focus(de_ui *ui, de_entity entity) {
    if (!ui || !ui->world || !ui->focus || (entity.bits && !de_ui__focus_allowed(ui, entity)))
        return;
    de_ui_focus *f = ui->focus;
    if (f->focused.bits == entity.bits)
        return;
    de_entity old = f->focused;
    ring_state(ui, old, false);
    f->focused = entity;
    if (old.bits)
        de_ui__widget_event(ui, DE_UI_EV_BLUR, old, (de_name){0}, 0);
    if (!entity.bits)
        return;
    ring_state(ui, entity, true);
    de_ui__widget_event(ui, DE_UI_EV_FOCUS, entity, (de_name){0}, 1);
    de_ui_widget_table *table = de_ui__widget_table(ui);
    if (table && table->scroll_into_view) {
        de_entity parent = de_entity_parent(ui->world, entity);
        for (u32 depth = 0; parent.bits && depth < ui->capacity; ++depth) {
            const DeUiWidgetType *tag = de_entity_get_const(ui->world, parent, ui->style->widget);
            if (tag && (tag->type == DE_UI_WIDGET_SCROLL || tag->type == DE_UI_WIDGET_LIST)) {
                table->scroll_into_view(ui, parent, entity);
                break;
            }
            parent = de_entity_parent(ui->world, parent);
        }
    }
}
de_entity de_ui_focused(const de_ui *ui) {
    return ui && ui->focus ? ui->focus->focused : DE_ENTITY_NULL;
}
bool de_ui_focus_visible(const de_ui *ui) {
    return ui && ui->focus && ui->focus->focused.bits && ui->focus->ring;
}
void de_ui_set_input_mode(de_ui *ui, de_ui_input_mode mode) {
    if (!ui || !ui->focus || (u32)mode > DE_UI_INPUT_MODAL)
        return;
    ui->focus->mode = mode;
    if (mode == DE_UI_INPUT_NONE) {
        ui->focus->sample_count = 0;
        ui->focus->held = DE_UI_NAV_NONE;
    }
    de_ui__actions_context(ui);
}
static void collect(const de_query_chunk *chunk, void *user) {
    de_ui_focus *f = user;
    for (u32 i = 0; i < chunk->count && f->node_count < f->ui->capacity; ++i)
        f->nodes[f->node_count++] = chunk->entities[i];
}
void de_ui__focus_collect(de_ui *ui) {
    de_ui_focus *f = ui->focus;
    f->count = f->node_count = 0;
    de_query_each(ui->world, ui->all_nodes, collect, f);
    /* ECS query/archetype order changes on component edits. Roots instead follow stable entity
     * order; children follow the public hierarchy's insertion order, including non-UI parents. */
    for (u32 i = 1; i < f->node_count; ++i) {
        de_entity e = f->nodes[i];
        u32 j = i;
        while (j && f->nodes[j - 1].bits > e.bits) {
            f->nodes[j] = f->nodes[j - 1];
            --j;
        }
        f->nodes[j] = e;
    }
    for (u32 i = 0; i < f->node_count; ++i) {
        de_entity root = f->nodes[i], parent = de_entity_parent(ui->world, root);
        bool ancestor_node = false;
        while (parent.bits) {
            if (de_entity_has(ui->world, parent, ui->ids.node)) {
                ancestor_node = true;
                break;
            }
            parent = de_entity_parent(ui->world, parent);
        }
        if (ancestor_node)
            continue;
        de_entity e = root;
        for (;;) {
            if (de_entity_has(ui->world, e, ui->ids.node) &&
                de_entity_has(ui->world, e, f->focusable) && f->count < ui->capacity)
                f->order[f->count++] = e;
            de_entity child = de_entity_children_first(ui->world, e);
            if (child.bits) {
                e = child;
                continue;
            }
            while (e.bits != root.bits && !de_entity_children_next(ui->world, e).bits)
                e = de_entity_parent(ui->world, e);
            if (e.bits == root.bits)
                break;
            e = de_entity_children_next(ui->world, e);
        }
    }
}
static de_entity top_popup(de_ui *ui, bool modal_only) {
    de_entity result = {0};
    i32 z = INT32_MIN;
    de_ui_focus *f = ui->focus;
    for (u32 i = 0; i < f->node_count; ++i) {
        de_entity e = f->nodes[i];
        const DeUiPopup *p = de_entity_get_const(ui->world, e, ui->ids.popup);
        const DeUiNode *n = de_entity_get_const(ui->world, e, ui->ids.node);
        if (!p || (modal_only && !p->modal) || !de_ui__widget_enabled(ui, e))
            continue;
        if (!result.bits || de_ui__focus_descendant(ui, e, result) ||
            (!de_ui__focus_descendant(ui, result, e) && n->z_order >= z)) {
            result = e;
            z = n->z_order;
        }
    }
    return result;
}
static void scopes(de_ui *ui) {
    de_ui_focus *f = ui->focus;
    /* Remove closed scopes from the top. Restore only after the scope restriction is popped. */
    while (f->scope_count) {
        de_ui_focus_scope top = f->scopes[f->scope_count - 1];
        const DeUiPopup *p = de_entity_get_const(ui->world, top.popup, ui->ids.popup);
        if (p && p->modal && de_ui__widget_enabled(ui, top.popup))
            break;
        --f->scope_count;
        f->scope = f->scope_count ? f->scopes[f->scope_count - 1].popup : DE_ENTITY_NULL;
        de_ui_set_focus(ui, de_ui__focus_allowed(ui, top.restore) ? top.restore : DE_ENTITY_NULL);
    }
    de_entity top = top_popup(ui, true);
    if (top.bits && top.bits != f->scope.bits && f->scope_count < ui->capacity) {
        /* A sibling modal replaces the active restriction but retains its restore history. */
        f->scopes[f->scope_count++] = (de_ui_focus_scope){top, f->focused};
        f->scope = top;
    }
    if (f->focused.bits && !de_ui__focus_allowed(ui, f->focused))
        de_ui_set_focus(ui, DE_ENTITY_NULL);
    if (f->scope.bits && !f->focused.bits)
        de_ui_set_focus(ui, de_ui__tab_nav(ui, DE_ENTITY_NULL, false));
    de_ui__actions_context(ui);
}
static de_ui_widget_entry *entry(de_ui *ui) {
    const DeUiWidgetType *tag =
        de_entity_get_const(ui->world, ui->focus->focused, ui->style->widget);
    de_ui_widget_table *table = de_ui__widget_table(ui);
    return table && tag && (u32)tag->type < DE_UI_WIDGET_TYPE_COUNT ? &table->entries[tag->type]
                                                                    : NULL;
}
void de_ui__focus_move(de_ui *ui, de_ui_nav_dir direction) {
    ring(ui, true);
    de_ui_widget_entry *widget = entry(ui);
    /* Adjusters consume horizontal navigation; all other controls use the same spatial graph. */
    if (widget && direction == DE_UI_NAV_RIGHT && widget->increment) {
        widget->increment(ui, ui->focus->focused);
        return;
    }
    if (widget && direction == DE_UI_NAV_LEFT && widget->decrement) {
        widget->decrement(ui, ui->focus->focused);
        return;
    }
    de_entity next = de_ui__spatial_nav(ui, ui->focus->focused, direction);
    if (next.bits)
        de_ui_set_focus(ui, next);
}
static void cancel(de_ui *ui) {
    de_ui_widget_entry *widget = entry(ui);
    if (widget && widget->cancel) {
        widget->cancel(ui, ui->focus->focused);
        return;
    }
    de_entity popup = top_popup(ui, false);
    if (popup.bits) {
        DeUiNode *node = de_entity_get(ui->world, popup, ui->ids.node);
        node->display = DE_UI_NONE;
        scopes(ui);
        return;
    }
    de_entity root = ui->focus->focused;
    for (u32 depth = 0; root.bits && depth < ui->capacity; ++depth) {
        de_entity parent = de_entity_parent(ui->world, root);
        if (!parent.bits)
            break;
        root = parent;
    }
    if (!root.bits && ui->focus->node_count)
        root = ui->focus->nodes[0];
    de_ui__widget_event(ui, DE_UI_EV_SUBMIT, root, de_name_from("cancel"), 0);
}
void de_ui__focus_sample(de_ui *ui, const de_ui_nav_sample *s) {
    de_ui_focus *f = ui->focus;
    if (s->pointer.x || s->pointer.y)
        ring(ui, false);
    if (s->previous || s->next) {
        ring(ui, true);
        de_ui_set_focus(ui, de_ui__tab_nav(ui, f->focused, s->previous));
    }
    de_ui_nav_dir direction = DE_UI_NAV_NONE;
    if (fabsf(s->navigate.x) > .5f || fabsf(s->navigate.y) > .5f) {
        if (fabsf(s->navigate.x) > fabsf(s->navigate.y))
            direction = s->navigate.x < 0 ? DE_UI_NAV_LEFT : DE_UI_NAV_RIGHT;
        else
            direction = s->navigate.y > 0 ? DE_UI_NAV_UP : DE_UI_NAV_DOWN;
    }
    if (direction != f->held) {
        f->held = direction;
        f->held_time = 0;
        f->next_repeat = .4;
        if (direction != DE_UI_NAV_NONE)
            de_ui__focus_move(ui, direction);
    } else if (direction != DE_UI_NAV_NONE && isfinite(s->dt) && s->dt > 0) {
        f->held_time += s->dt;
        while (f->held_time + 1e-8 >= f->next_repeat) {
            de_ui__focus_move(ui, direction);
            f->next_repeat += .08;
        }
    }
    if (s->submit) {
        ring(ui, true);
        de_ui_widget_entry *widget = entry(ui);
        if (widget && widget->submit)
            widget->submit(ui, f->focused);
        else if (f->focused.bits)
            de_ui__widget_event(ui, DE_UI_EV_SUBMIT, f->focused, de_name_from("submit"), 1);
    }
    if (s->cancel)
        cancel(ui);
}
static void update(de_system_ctx *ctx) {
    de_ui_focus *f = ctx->user;
    de_ui *ui = f->ui;
    de_ui__focus_collect(ui);
    scopes(ui);
    /* Pointer state comes from the existing routing layer, never raw device queries. */
    de_vec2 p = ui->widgets->pointer.position;
    if (f->pointer_known && (p.x != f->last_pointer.x || p.y != f->last_pointer.y))
        ring(ui, false);
    f->pointer_known = true;
    f->last_pointer = p;
    if (ui->widgets->pointer.down && ui->widgets->pointer.captured.bits) {
        de_entity e = ui->widgets->pointer.captured;
        for (u32 depth = 0; e.bits && depth < ui->capacity; ++depth) {
            if (de_ui__focus_allowed(ui, e)) {
                de_ui_set_focus(ui, e);
                break;
            }
            e = de_entity_parent(ui->world, e);
        }
    }
    for (u32 i = 0; i < f->sample_count; ++i)
        de_ui__focus_sample(ui, &f->samples[i]);
    f->sample_count = 0;
}
de_result de_ui__focus_init(de_ui *ui) {
    de_ui_focus *f = DE_NEW(ui->alloc, de_ui_focus, DE_MEM_UI);
    if (!f)
        return DE_ERR_OUT_OF_MEMORY;
    memset(f, 0, sizeof *f);
    ui->focus = f;
    f->ui = ui;
    f->ring = true;
    f->mode = DE_UI_INPUT_NON_MODAL;
    f->sample_capacity = ui->capacity > 256 ? ui->capacity : 256;
    f->nodes =
        DE_ALLOC(ui->alloc, (u64)ui->capacity * sizeof(de_entity), _Alignof(de_entity), DE_MEM_UI);
    f->order =
        DE_ALLOC(ui->alloc, (u64)ui->capacity * sizeof(de_entity), _Alignof(de_entity), DE_MEM_UI);
    f->scopes = DE_ALLOC(ui->alloc, (u64)ui->capacity * sizeof(de_ui_focus_scope),
                         _Alignof(de_ui_focus_scope), DE_MEM_UI);
    f->samples = DE_ALLOC(ui->alloc, (u64)f->sample_capacity * sizeof(de_ui_nav_sample),
                          _Alignof(de_ui_nav_sample), DE_MEM_UI);
    return f->nodes && f->order && f->scopes && f->samples ? DE_OK : DE_ERR_OUT_OF_MEMORY;
}
void de_ui__focus_detach(de_ui *ui) {
    de_ui_focus *f = ui->focus;
    if (!f)
        return;
    if (ui->widgets) {
        ui->widgets->table.request_focus = NULL;
        ui->widgets->table.focused = NULL;
    }
    if (ui->world) {
        de_ui_set_focus(ui, DE_ENTITY_NULL);
        if (f->update.id)
            de_system_remove(ui->world, f->update);
    }
    de_ui__actions_detach(ui);
    f->update = (de_system){0};
    f->sample_count = f->scope_count = f->count = f->node_count = 0;
    f->scope = DE_ENTITY_NULL;
    f->held = DE_UI_NAV_NONE;
    f->pointer_known = false;
}
void de_ui__focus_shutdown(de_ui *ui) {
    de_ui_focus *f = ui->focus;
    if (!f)
        return;
    de_ui__focus_detach(ui);
    if (f->nodes)
        DE_FREE(ui->alloc, f->nodes, (u64)ui->capacity * sizeof(de_entity));
    if (f->order)
        DE_FREE(ui->alloc, f->order, (u64)ui->capacity * sizeof(de_entity));
    if (f->scopes)
        DE_FREE(ui->alloc, f->scopes, (u64)ui->capacity * sizeof(de_ui_focus_scope));
    if (f->samples)
        DE_FREE(ui->alloc, f->samples, (u64)f->sample_capacity * sizeof(de_ui_nav_sample));
    DE_FREE(ui->alloc, f, sizeof *f);
    ui->focus = NULL;
}
void de_ui__focus_attach(de_ui *ui) {
    if (!ui->focus || !ui->world)
        return;
    de_ui_focus *f = ui->focus;
    de_type_registry *types = de_world_types(ui->world);
    de_type_id type = de_type_find(types, "DeUiFocusable");
    static const de_field_info fields[] = {
        DE_FIELD(DeUiFocusable, tab_index, DE_FK_I32, DE_FIELD_DEFAULT),
        DE_FIELD(DeUiFocusable, up, DE_FK_ENTITY, DE_FIELD_DEFAULT),
        DE_FIELD(DeUiFocusable, down, DE_FK_ENTITY, DE_FIELD_DEFAULT),
        DE_FIELD(DeUiFocusable, left, DE_FK_ENTITY, DE_FIELD_DEFAULT),
        DE_FIELD(DeUiFocusable, right, DE_FK_ENTITY, DE_FIELD_DEFAULT)};
    if (!type.id && de_type_register_ex(types,
                                        &(de_type_desc){.name = "DeUiFocusable",
                                                        .size = sizeof(DeUiFocusable),
                                                        .align = _Alignof(DeUiFocusable),
                                                        .version = 1,
                                                        .is_pod = true,
                                                        .fields = fields,
                                                        .field_count = 5},
                                        &type, NULL) != DE_OK)
        return;
    f->focusable = de_component_register(ui->world, type, DE_COMP_SERIALIZE | DE_COMP_SNAPSHOT);
    ui->widgets->focusable = f->focusable;
    ui->widgets->table.request_focus = de_ui_set_focus;
    ui->widgets->table.focused = de_ui_focused;
    const char *before[] = {DE_UI_STYLE_SYSTEM, DE_UI_LAYOUT_SYSTEM};
    const char *after[] = {"ui.widgets"};
    f->update = de_system_add(
        ui->world, &(de_system_desc){.name = "ui.focus",
                                     .schedule = DE_SCHEDULE_POST_UPDATE,
                                     .fn = update,
                                     .user = f,
                                     .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE,
                                     .before = before,
                                     .before_count = 2,
                                     .after = after,
                                     .after_count = 1});
    de_ui__actions_attach(ui);
}
