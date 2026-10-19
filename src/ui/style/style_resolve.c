#include "stylesheet_internal.h"
#include <math.h>
#include <string.h>
static const char *const widget_names[] = {"panel",   "button",   "toggle", "slider", "text",
                                           "image",   "progress", "input",  "scroll", "dropdown",
                                           "tooltip", "list",     "tabs",   "tree",   "canvas"};
static u32 state_bits(const DeUiInteractionState *s) {
    return s ? (s->hovered ? 1u : 0u) | (s->focused ? 2u : 0u) | (s->pressed ? 4u : 0u) |
                   (s->disabled ? 8u : 0u)
             : 0u;
}
static void collect(const de_query_chunk *chunk, void *user) {
    de_ui_style_system *state = user;
    for (u32 i = 0; i < chunk->count && state->entity_count < state->ui->capacity; ++i)
        state->entities[state->entity_count++] = chunk->entities[i];
}
static de_ui_style_cache *cached(de_ui_style_system *state, de_entity entity, bool create) {
    u64 h = entity.bits;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    u32 mask = state->ui->hash_capacity - 1, index = (u32)h & mask;
    for (u32 i = 0; i <= mask; ++i, index = (index + 1) & mask) {
        de_ui_style_cache *cache = &state->cache[index];
        if (cache->entity.bits == entity.bits)
            return cache;
        if (!cache->entity.bits || !de_entity_alive(state->ui->world, cache->entity)) {
            if (!create)
                return NULL;
            memset(cache, 0, sizeof *cache);
            cache->entity = entity;
            return cache;
        }
    }
    return NULL;
}
static u32 property_size(const de_ui_style_property *p) {
    return p->kind == DE_UI_PROP_COLOR || p->kind == DE_UI_PROP_VEC4 ? sizeof(de_vec4)
           : p->kind == DE_UI_PROP_LENGTH                            ? sizeof(de_ui_len)
           : p->kind == DE_UI_PROP_EDGES                             ? sizeof(de_ui_len) * 4
                                                                     : sizeof(f32);
}
static u8 *target(const de_ui_style_property *p, DeUiNode *node, DeUiStyle *style,
                  de_text_style *text) {
    return (p->target == DE_UI_TARGET_NODE    ? (u8 *)node
            : p->target == DE_UI_TARGET_STYLE ? (u8 *)style
                                              : (u8 *)text) +
           p->offset;
}
static void restore_authored(de_ui_style_cache *cache, DeUiNode *node, DeUiStyle *style,
                             de_text_style *text) {
    for (u32 p = 0; p < de_ui__style_property_count; ++p) {
        const de_ui_style_property *property = &de_ui__style_properties[p];
        u32 bytes = property_size(property);
        u8 *current = target(property, node, style, text),
           *base = target(property, &cache->authored_node, &cache->authored_style,
                          &cache->authored_text),
           *applied =
               target(property, &cache->applied_node, &cache->applied_style, &cache->applied_text);
        if (memcmp(current, applied, bytes))
            memcpy(base, current, bytes);
        memcpy(current, base, bytes);
    }
}
static void apply_value(de_ui *ui, const de_ui_style_property *p, const de_ui_style_value *value,
                        DeUiNode *node, DeUiStyle *style, de_text_style *text) {
    u8 *dst = target(p, node, style, text);
    switch (p->kind) {
    case DE_UI_PROP_LENGTH: {
        de_ui_len l = {value->unit, value->number[0]};
        memcpy(dst, &l, sizeof l);
        break;
    }
    case DE_UI_PROP_EDGES:
        for (u32 i = 0; i < 4; ++i) {
            de_ui_len l = {DE_UI_PX, value->number[i]};
            memcpy(dst + i * sizeof l, &l, sizeof l);
        }
        break;
    case DE_UI_PROP_ASSET: {
        u32 handle = 0;
        if (ui->desc.assets)
            handle = p->target == DE_UI_TARGET_TEXT
                         ? de_asset_font(ui->desc.assets, value->asset).id
                         : de_asset_texture(ui->desc.assets, value->asset).id;
        memcpy(dst, &handle, sizeof handle);
        break;
    }
    case DE_UI_PROP_COLOR:
    case DE_UI_PROP_VEC4:
        memcpy(dst, value->number, sizeof(de_vec4));
        break;
    default:
        memcpy(dst, value->number, sizeof(f32));
        break;
    }
}
static void transition(de_ui_style_system *state, de_entity entity, const de_ui_style_property *p,
                       const void *from, const void *to, f32 seconds, de_ui_easing easing) {
    de_ui *ui = state->ui;
    de_component component = p->target == DE_UI_TARGET_STYLE  ? ui->ids.style
                             : p->target == DE_UI_TARGET_NODE ? ui->ids.node
                                                              : ui->ids.text;
    u32 base = p->offset + (p->target == DE_UI_TARGET_TEXT ? offsetof(DeUiText, style) : 0);
    if (!de_entity_has(ui->world, entity, component))
        return;
    u32 count = p->kind == DE_UI_PROP_COLOR || p->kind == DE_UI_PROP_VEC4 ? 4
                : p->kind == DE_UI_PROP_EDGES                             ? 4
                                                                          : 1;
    if (p->kind == DE_UI_PROP_ASSET)
        return;
    if (p->kind == DE_UI_PROP_LENGTH &&
        ((const de_ui_len *)from)->unit != ((const de_ui_len *)to)->unit)
        return;
    for (u32 i = 0; i < count; ++i) {
        u32 sub = p->kind == DE_UI_PROP_LENGTH  ? offsetof(de_ui_len, value)
                  : p->kind == DE_UI_PROP_EDGES ? i * sizeof(de_ui_len) + offsetof(de_ui_len, value)
                                                : i * sizeof(f32);
        f32 a, b;
        memcpy(&a, (const u8 *)from + sub, sizeof a);
        memcpy(&b, (const u8 *)to + sub, sizeof b);
        if (a == b)
            continue;
        u8 *dst = de_entity_get(ui->world, entity, component);
        memcpy(dst + base + sub, &a, sizeof a);
        (void)de_ui__tween_field(state, entity, component, base + sub, b, seconds, easing);
    }
}
static void resolve(de_ui_style_system *state, de_entity entity) {
    de_ui *ui = state->ui;
    const DeUiNode *current_node = de_entity_get_const(ui->world, entity, ui->ids.node);
    if (!current_node)
        return;
    const DeUiWidgetType *widget = de_entity_get_const(ui->world, entity, state->widget);
    u32 type = widget ? (u32)widget->type : DE_UI_WIDGET_PANEL;
    const DeUiInteractionState *interaction = de_entity_get_const(ui->world, entity, ui->ids.state);
    u32 bits = state_bits(interaction);
#if defined(DE_UI_NAVIGATION)
    /* Logical focus remains available to editors while mouse mode suppresses only its ring.
     * Keep explicitly authored focus states working when navigation owns no focused entity. */
    if (de_ui_focused(ui).bits == entity.bits && !de_ui_focus_visible(ui))
        bits &= ~2u;
#endif
    de_name widget_name =
        de_name_from(widget_names[type < DE_UI_STYLE_COUNT(widget_names) ? type : 0]);
    bool matching_rule = false;
    for (u32 s = 0; s < state->sheet_count && !matching_rule; ++s)
        for (u32 r = 0; r < state->sheets[s].sheet->rule_count; ++r) {
            const de_ui_style_rule *rule = &state->sheets[s].sheet->rules[r];
            if ((!rule->widget.hash || rule->widget.hash == widget_name.hash) &&
                (!rule->class_name.hash ||
                 rule->class_name.hash == current_node->style_class.hash) &&
                (!rule->state || (bits & rule->state) == rule->state)) {
                matching_rule = true;
                break;
            }
        }
    /* Untouched nodes with no matching rules need no authored/applied snapshot. Existing
     * caches must still resolve when their last matching rule disappears. */
    de_ui_style_cache *cache = cached(state, entity, matching_rule);
    if (!cache)
        return;
    if (cache->initialized && cache->class_name.hash == current_node->style_class.hash &&
        cache->widget == type && cache->state == bits && cache->revision == state->revision)
        return;
    ++state->entities_restyled;
    DeUiNode node = *current_node;
    DeUiStyle style = {.opacity = 1, .text_color = {1, 1, 1, 1}};
    const DeUiStyle *current_style = de_entity_get_const(ui->world, entity, ui->ids.style);
    if (current_style)
        style = *current_style;
    de_text_style text = {0};
    const DeUiText *current_text = de_entity_get_const(ui->world, entity, ui->ids.text);
    if (current_text)
        text = current_text->style;
    DeUiNode before_node = node;
    DeUiStyle before_style = style;
    de_text_style before_text = text;
    if (!cache->initialized) {
        cache->authored_node = node;
        cache->authored_style = style;
        cache->authored_text = text;
    } else
        restore_authored(cache, &node, &style, &text);
    i32 best[24];
    for (u32 i = 0; i < DE_UI_STYLE_COUNT(best); ++i)
        best[i] = -1;
    de_ui_style_value winning[24];
    f32 duration = 0;
    de_ui_easing easing = DE_UI_EASE_LINEAR;
    i32 motion_specificity = -1;
    for (u32 s = 0; s < state->sheet_count; ++s) {
        de_ui_style_sheet_slot *slot = &state->sheets[s];
        for (u32 r = 0; r < slot->sheet->rule_count; ++r) {
            de_ui_style_rule *rule = &slot->sheet->rules[r];
            if ((rule->widget.hash && rule->widget.hash != widget_name.hash) ||
                (rule->class_name.hash && rule->class_name.hash != node.style_class.hash) ||
                (rule->state && (bits & rule->state) != rule->state))
                continue;
            de_ui_compiled_rule *compiled = &slot->compiled[r];
            for (u32 p = 0; p < de_ui__style_property_count; ++p)
                if ((compiled->mask & (1u << p)) && (i32)rule->specificity >= best[p]) {
                    best[p] = (i32)rule->specificity;
                    winning[p] = compiled->values[p];
                }
            toml_datum_t transition_value = toml_get(rule->properties, "transition"),
                         easing_value = toml_get(rule->properties, "easing");
            if ((transition_value.type != TOML_UNKNOWN || easing_value.type != TOML_UNKNOWN) &&
                (i32)rule->specificity >= motion_specificity) {
                motion_specificity = (i32)rule->specificity;
                if (transition_value.type != TOML_UNKNOWN)
                    duration = compiled->transition;
                if (easing_value.type != TOML_UNKNOWN)
                    easing = compiled->easing;
            }
        }
    }
    for (u32 p = 0; p < de_ui__style_property_count; ++p)
        if (best[p] >= 0)
            apply_value(ui, &de_ui__style_properties[p], &winning[p], &node, &style, &text);
    if (memcmp(&node, &before_node, sizeof node))
        *(DeUiNode *)de_entity_get(ui->world, entity, ui->ids.node) = node;
    /* Absent style already means these defaults. Avoid moving neutral panels to a new
     * archetype just to publish an identical value (the common large-tree case). */
    const DeUiStyle default_style = {.opacity = 1, .text_color = {1, 1, 1, 1}};
    if (!current_style && memcmp(&style, &default_style, sizeof style)) {
        DeUiStyle *dst = de_entity_add(ui->world, entity, ui->ids.style);
        if (dst)
            *dst = style;
    } else if (current_style && memcmp(&style, &before_style, sizeof style))
        *(DeUiStyle *)de_entity_get(ui->world, entity, ui->ids.style) = style;
    if (current_text && memcmp(&text, &before_text, sizeof text)) {
        DeUiText *dst = de_entity_get(ui->world, entity, ui->ids.text);
        dst->style = text;
    }
    if (cache->initialized && duration > 0)
        for (u32 p = 0; p < de_ui__style_property_count; ++p) {
            const de_ui_style_property *property = &de_ui__style_properties[p];
            transition(state, entity, property,
                       target(property, &before_node, &before_style, &before_text),
                       target(property, &node, &style, &text), duration, easing);
        }
    cache->initialized = true;
    cache->class_name = node.style_class;
    cache->widget = type;
    cache->state = bits;
    cache->revision = state->revision;
    cache->applied_node = node;
    cache->applied_style = style;
    cache->applied_text = text;
}
void de_ui__style_tick(de_system_ctx *ctx) {
    de_ui_style_system *state = ctx->user;
    state->entities_restyled = 0;
    de_ui__sheets_refresh(state);
    state->entity_count = 0;
    if (state->sheet_count)
        de_query_each(state->ui->world, ctx->queries[0], collect, state);
    for (u32 i = 0; i < state->entity_count; ++i)
        resolve(state, state->entities[i]);
    de_ui__tweens_tick(state, ctx->dt);
}
de_ui_style_stats de_ui_get_style_stats(const de_ui *ui) {
    return ui && ui->style
               ? (de_ui_style_stats){ui->style->entities_restyled, ui->style->active_tweens}
               : (de_ui_style_stats){0};
}
de_result de_ui__style_init(de_ui *ui) {
    de_ui_style_system *state = DE_NEW(ui->alloc, de_ui_style_system, DE_MEM_UI);
    if (!state)
        return DE_ERR_OUT_OF_MEMORY;
    memset(state, 0, sizeof *state);
    state->ui = ui;
    ui->style = state;
    state->cache = DE_ALLOC(ui->alloc, (u64)ui->hash_capacity * sizeof *state->cache,
                            _Alignof(de_ui_style_cache), DE_MEM_UI);
    state->entities = DE_ALLOC(ui->alloc, (u64)ui->capacity * sizeof *state->entities,
                               _Alignof(de_entity), DE_MEM_UI);
    state->tweens = DE_ALLOC(ui->alloc, (u64)DE_UI_STYLE_TWEENS * sizeof *state->tweens,
                             _Alignof(de_ui_property_tween), DE_MEM_UI);
    if (!state->cache || !state->entities || !state->tweens)
        return DE_ERR_OUT_OF_MEMORY;
    memset(state->cache, 0, (size_t)ui->hash_capacity * sizeof *state->cache);
    memset(state->tweens, 0, (size_t)DE_UI_STYLE_TWEENS * sizeof *state->tweens);
    state->revision = 1;
    return de_ui__style_asset_register(ui->desc.assets);
}
void de_ui__style_detach(de_ui *ui) {
    if (!ui->style)
        return;
    if (ui->world && ui->style->system.id)
        de_system_remove(ui->world, ui->style->system);
    ui->style->system = (de_system){0};
    if (ui->style->cache)
        memset(ui->style->cache, 0, (size_t)ui->hash_capacity * sizeof *ui->style->cache);
    if (ui->style->tweens)
        memset(ui->style->tweens, 0, (size_t)DE_UI_STYLE_TWEENS * sizeof *ui->style->tweens);
}
void de_ui__style_shutdown(de_ui *ui) {
    de_ui_style_system *state = ui->style;
    if (!state)
        return;
    de_ui__style_detach(ui);
    de_ui__sheets_shutdown(state);
    if (state->cache)
        DE_FREE(ui->alloc, state->cache, (u64)ui->hash_capacity * sizeof *state->cache);
    if (state->entities)
        DE_FREE(ui->alloc, state->entities, (u64)ui->capacity * sizeof *state->entities);
    if (state->tweens)
        DE_FREE(ui->alloc, state->tweens, (u64)DE_UI_STYLE_TWEENS * sizeof *state->tweens);
    DE_FREE(ui->alloc, state, sizeof *state);
    ui->style = NULL;
}
void de_ui__style_attach(de_ui *ui) {
    de_type_registry *types = de_world_types(ui->world);
    static const de_enum_value values[] = {
        {"panel", DE_UI_WIDGET_PANEL},       {"button", DE_UI_WIDGET_BUTTON},
        {"toggle", DE_UI_WIDGET_TOGGLE},     {"slider", DE_UI_WIDGET_SLIDER},
        {"text", DE_UI_WIDGET_TEXT},         {"image", DE_UI_WIDGET_IMAGE},
        {"progress", DE_UI_WIDGET_PROGRESS}, {"input", DE_UI_WIDGET_INPUT},
        {"scroll", DE_UI_WIDGET_SCROLL},     {"dropdown", DE_UI_WIDGET_DROPDOWN},
        {"tooltip", DE_UI_WIDGET_TOOLTIP},   {"list", DE_UI_WIDGET_LIST},
        {"tabs", DE_UI_WIDGET_TABS},         {"tree", DE_UI_WIDGET_TREE},
        {"canvas", DE_UI_WIDGET_CANVAS}};
    if (!de_type_find(types, "de_ui_widget_type").id &&
        de_enum_register(types, "de_ui_widget_type", values, DE_UI_STYLE_COUNT(values), false) !=
            DE_OK)
        return;
    static const de_field_info fields[] = {
        DE_FIELD_EX(DeUiWidgetType, type, DE_FK_ENUM, DE_FIELD_SERIALIZE | DE_FIELD_SNAPSHOT,
                    {0, 14, NULL, "Widget selector tag", "de_ui_widget_type", 0})};
    de_type_id id = de_type_find(types, "DeUiWidgetType");
    if (!id.id && de_type_register_ex(types,
                                      &(de_type_desc){.name = "DeUiWidgetType",
                                                      .size = sizeof(DeUiWidgetType),
                                                      .align = _Alignof(DeUiWidgetType),
                                                      .version = 1,
                                                      .is_pod = true,
                                                      .fields = fields,
                                                      .field_count = 1},
                                      &id, NULL) != DE_OK)
        return;
    ui->style->widget = de_component_register(ui->world, id, DE_COMP_SERIALIZE | DE_COMP_SNAPSHOT);
    de_query_term term = {ui->ids.node, DE_READ};
    de_query_desc query = {.terms = &term, .term_count = 1};
    const char *before[] = {DE_UI_LAYOUT_SYSTEM};
    ui->style->system = de_system_add(
        ui->world, &(de_system_desc){.name = DE_UI_STYLE_SYSTEM,
                                     .schedule = DE_SCHEDULE_POST_UPDATE,
                                     .fn = de_ui__style_tick,
                                     .user = ui->style,
                                     .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE,
                                     .queries = &query,
                                     .query_count = 1,
                                     .before = before,
                                     .before_count = 1});
}
