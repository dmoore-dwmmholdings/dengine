#include "stylesheet_internal.h"
#include <math.h>
#include <string.h>
f32 de_ui__ease(de_ui_easing easing, f32 t) {
    t = fmaxf(0, fminf(1, t));
    switch (easing) {
    case DE_UI_EASE_QUAD_IN:
        return t * t;
    case DE_UI_EASE_QUAD_OUT:
        return 1 - (1 - t) * (1 - t);
    case DE_UI_EASE_QUAD_IN_OUT:
        return t < .5f ? 2 * t * t : 1 - powf(-2 * t + 2, 2) * .5f;
    case DE_UI_EASE_CUBIC_IN:
        return t * t * t;
    case DE_UI_EASE_CUBIC_OUT:
        return 1 - powf(1 - t, 3);
    case DE_UI_EASE_CUBIC_IN_OUT:
        return t < .5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) * .5f;
    case DE_UI_EASE_BACK_OUT: {
        f32 x = t - 1;
        return 1 + 2.70158f * x * x * x + 1.70158f * x * x;
    }
    case DE_UI_EASE_ELASTIC_OUT:
        return t == 0 || t == 1
                   ? t
                   : powf(2, -10 * t) * sinf((t * 10 - .75f) * (2 * 3.14159265358979323846f / 3)) +
                         1;
    default:
        return t;
    }
}
de_result de_ui__tween_field(de_ui_style_system *state, de_entity entity, de_component component,
                             u32 offset, f32 value, f32 seconds, de_ui_easing easing,
                             bool stylesheet) {
    if (!state || !isfinite(value) || !isfinite(seconds) || seconds < 0 ||
        (u32)easing > DE_UI_EASE_ELASTIC_OUT)
        return DE_ERR_INVALID_ARGUMENT;
    const u8 *object = de_entity_get_const(state->ui->world, entity, component);
    if (!object)
        return DE_ERR_NOT_FOUND;
    f32 current;
    memcpy(&current, object + offset, sizeof current);
    u32 free_slot = UINT32_MAX;
    for (u32 i = 0; i < DE_UI_STYLE_TWEENS; ++i) {
        de_ui_property_tween *t = &state->tweens[i];
        if (t->active && t->entity.bits == entity.bits && t->component.id == component.id &&
            t->offset == offset) {
            free_slot = i;
            break;
        }
        if (!t->active && free_slot == UINT32_MAX)
            free_slot = i;
    }
    if (free_slot == UINT32_MAX)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "UI tween pool exhausted");
    de_ui_property_tween *t = &state->tweens[free_slot];
    *t = (de_ui_property_tween){.entity = entity,
                                .component = component,
                                .offset = offset,
                                .from = current,
                                .to = value,
                                .duration = seconds,
                                .easing = easing,
                                .active = seconds > 0,
                                .stylesheet = stylesheet};
    if (seconds == 0) {
        u8 *dst = de_entity_get(state->ui->world, entity, component);
        memcpy(dst + offset, &value, sizeof value);
    }
    return DE_OK;
}
void de_ui__tweens_tick(de_ui_style_system *state, f32 dt) {
    state->active_tweens = 0;
    if (!isfinite(dt) || dt < 0)
        dt = 0;
    for (u32 i = 0; i < DE_UI_STYLE_TWEENS; ++i) {
        de_ui_property_tween *t = &state->tweens[i];
        if (!t->active)
            continue;
        if (!de_entity_has(state->ui->world, t->entity, t->component)) {
            t->active = false;
            continue;
        }
        t->elapsed = fminf(t->duration, t->elapsed + dt);
        f32 value = t->from + (t->to - t->from) * de_ui__ease(t->easing, t->elapsed / t->duration);
        u8 *dst = de_entity_get(state->ui->world, t->entity, t->component);
        memcpy(dst + t->offset, &value, sizeof value);
        /* Intermediate automatic values are applied output, never new authored defaults. */
        if (t->stylesheet)
            de_ui__style_tween_sample(state, t->entity, t->component, t->offset, value);
        if (t->elapsed >= t->duration)
            t->active = false;
        else
            ++state->active_tweens;
    }
}
de_result de_ui__tween_path(de_ui *ui, de_entity entity, const char *property, f32 to, f32 seconds,
                            de_ui_easing easing) {
    if (!ui || !ui->style || !ui->world || !property)
        return DE_ERR_INVALID_ARGUMENT;
    const char *dot = strchr(property, '.');
    char component_name[64];
    const char *path = property;
    if (dot) {
        size_t length = (size_t)(dot - property);
        if (!length || length >= sizeof component_name)
            return DE_ERR_INVALID_ARGUMENT;
        memcpy(component_name, property, length);
        component_name[length] = 0;
        path = dot + 1;
    } else {
        memcpy(component_name, "DeUiStyle", 10);
        if (!strcmp(property, "radius"))
            path = "corner_radius";
    }
    de_type_registry *types = de_world_types(ui->world);
    de_type_id type = de_type_find(types, component_name);
    de_component component = de_component_find(ui->world, component_name);
    const de_field_info *field = NULL;
    u32 offset = 0;
    de_result result = de_reflect_resolve_path(types, type, path, &field, &offset);
    if (result != DE_OK)
        return result;
    if (!field || field->kind != DE_FK_F32)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "UI tween target must be f32");
    return de_ui__tween_field(ui->style, entity, component, offset, to, seconds, easing, false);
}

void de_ui_tween(de_ui *ui, de_entity entity, const char *property, f32 to, f32 seconds,
                 u32 easing) {
    de_result result = de_ui__tween_path(ui, entity, property, to, seconds, (de_ui_easing)easing);
    if (result != DE_OK)
        DE_FAIL_WRAP(result, "UI tween %s rejected", property ? property : "(null)");
}
