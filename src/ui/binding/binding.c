#include "binding_internal.h"
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static de_ui_binding_cache *cache(de_ui_binding_system *s, de_entity entity) {
    u64 h = entity.bits * 11400714819323198485ull;
    u32 mask = s->cache_capacity - 1;
    de_ui_binding_cache *available = NULL;
    for (u32 i = (u32)(h ^ (h >> 32)) & mask, n = 0; n <= mask; ++n, i = (i + 1) & mask) {
        de_ui_binding_cache *c = &s->cache[i];
        if (c->entity.bits == entity.bits)
            return c;
        if (!c->entity.bits) {
            if (!available)
                available = c;
            break;
        }
        /* Keep probing beyond tombstones: a live binding may follow a despawned collision. */
        if (!available && !de_entity_alive(s->ui->world, c->entity))
            available = c;
    }
    if (!available)
        return NULL;
    u8 *previous = available->previous;
    u32 previous_capacity = available->previous_capacity;
    char *text = available->text;
    u32 text_capacity = available->text_capacity;
    memset(available, 0, sizeof *available);
    available->entity = entity;
    available->previous = previous;
    available->previous_capacity = previous_capacity;
    available->text = text;
    available->text_capacity = text_capacity;
    return available;
}
static void fail(de_ui_binding_system *s, de_ui_binding_cache *c, const char *reason) {
    c->valid = false;
    if (!c->warned) {
        c->warned = true;
        ++s->warnings;
        DE_WARN(s->log, "binding '%.*s' -> '%.*s': %s", (int)sizeof c->descriptor.path,
                c->descriptor.path, (int)sizeof c->descriptor.property, c->descriptor.property,
                reason);
    }
}
static bool numeric(de_field_kind k) {
    return k >= DE_FK_BOOL && k <= DE_FK_F64;
}
static bool number(const de_field_info *f, const void *ptr, long double *out) {
#define READ(kind_, type_)                                                                         \
    case kind_: {                                                                                  \
        type_ n;                                                                                   \
        memcpy(&n, ptr, sizeof n);                                                                 \
        *out = n;                                                                                  \
        return true;                                                                               \
    }
    switch (f->kind) {
        READ(DE_FK_BOOL, bool)
        READ(DE_FK_I8, i8)
        READ(DE_FK_I16, i16)
        READ(DE_FK_I32, i32)
        READ(DE_FK_I64, i64)
        READ(DE_FK_U8, u8)
        READ(DE_FK_U16, u16) READ(DE_FK_U32, u32) READ(DE_FK_U64, u64) READ(DE_FK_F32, f32)
            READ(DE_FK_F64, f64) default : return false;
    }
#undef READ
}
static bool write_number(const de_field_info *f, long double x, u8 *out) {
    if (!isfinite(x))
        return false;
#define WRITE(kind_, type_, lo_, hi_)                                                              \
    case kind_: {                                                                                  \
        type_ n = x <= (long double)(lo_)   ? (type_)(lo_)                                         \
                  : x >= (long double)(hi_) ? (type_)(hi_)                                         \
                                            : (type_)x;                                            \
        memcpy(out, &n, sizeof n);                                                                 \
        return true;                                                                               \
    }
    switch (f->kind) {
    case DE_FK_BOOL: {
        bool n = x != 0;
        memcpy(out, &n, sizeof n);
        return true;
    }
        WRITE(DE_FK_I8, i8, INT8_MIN, INT8_MAX)
        WRITE(DE_FK_I16, i16, INT16_MIN, INT16_MAX)
        WRITE(DE_FK_I32, i32, INT32_MIN, INT32_MAX)
        WRITE(DE_FK_I64, i64, INT64_MIN, INT64_MAX)
        WRITE(DE_FK_U8, u8, 0, UINT8_MAX)
        WRITE(DE_FK_U16, u16, 0, UINT16_MAX)
        WRITE(DE_FK_U32, u32, 0, UINT32_MAX) WRITE(DE_FK_U64, u64, 0, UINT64_MAX)
            WRITE(DE_FK_F32, f32, -FLT_MAX, FLT_MAX)
                WRITE(DE_FK_F64, f64, -DBL_MAX, DBL_MAX) default : return false;
    }
#undef WRITE
}
static bool target(de_ui_binding_system *s, de_ui_binding_cache *c, const char *component,
                   u32 offset, u32 size) {
    c->target = de_component_find(s->ui->world, component);
    c->target_offset = offset;
    c->target_size = size;
    if (c->target.id == s->ui->ids.style.id && !de_entity_has(s->ui->world, c->entity, c->target) &&
        de_entity_has(s->ui->world, c->entity, s->ui->ids.node))
        (void)de_entity_add(s->ui->world, c->entity, c->target);
    return c->target.id && de_entity_has(s->ui->world, c->entity, c->target);
}
/* Only adapt the issue's dotted numeric array shorthand. Reflection remains the sole path
 * resolver/validator, including all nested schemas, bounds, leaf kinds and readonly flags. */
static bool path(const char *input, char *component, char *field) {
    const char *dot = strchr(input, '.');
    if (!dot || dot == input || (size_t)(dot - input) >= 64 || !dot[1])
        return false;
    memcpy(component, input, (size_t)(dot - input));
    component[dot - input] = 0;
    u32 n = 0;
    for (const char *p = dot + 1; *p;) {
        if (*p == '.' && p[1] >= '0' && p[1] <= '9') {
            if (n + 2 >= 64)
                return false;
            field[n++] = '[';
            ++p;
            while (*p >= '0' && *p <= '9') {
                if (n + 2 >= 64)
                    return false;
                field[n++] = *p++;
            }
            field[n++] = ']';
        } else {
            if (n + 1 >= 64)
                return false;
            field[n++] = *p++;
        }
    }
    field[n] = 0;
    return true;
}
static void resolve(de_ui_binding_system *s, de_ui_binding_cache *c, const DeUiBinding *binding,
                    u64 revision) {
    de_ui *ui = s->ui;
    c->descriptor = *binding;
    c->revision = revision;
    c->sampled = false;
    c->warned = false;
    c->valid = false;
    c->editor = (de_component){0};
    c->target = (de_component){0};
    ++s->resolved;
    if (!memchr(binding->path, 0, sizeof binding->path) ||
        !memchr(binding->property, 0, sizeof binding->property) ||
        !memchr(binding->format, 0, sizeof binding->format) ||
        (u32)binding->mode > DE_UI_BIND_TWO_WAY) {
        fail(s, c, "unterminated descriptor or invalid mode");
        return;
    }
    char component[64], field[64];
    const de_field_info *info = NULL;
    de_type_registry *types = de_world_types(ui->world);
    if (!path(binding->path, component, field)) {
        fail(s, c, "invalid reflection path");
        return;
    }
    c->source = de_component_find(ui->world, component);
    de_type_id type = de_type_find(types, component);
    if (!c->source.id || !type.id ||
        de_reflect_resolve_path(types, type, field, &info, &c->offset) != DE_OK) {
        fail(s, c, "reflection path not found");
        return;
    }
    c->field = *info;
    bool compatible = false;
    if (!strcmp(binding->property, "value")) {
        c->property = DE_UI_BIND_VALUE;
        compatible = numeric(info->kind) &&
                     (target(s, c, "DeUiSlider", offsetof(DeUiSlider, value), sizeof(f32)) ||
                      target(s, c, "DeUiProgress", offsetof(DeUiProgress, value), sizeof(f32)));
    } else if (!strcmp(binding->property, "on")) {
        c->property = DE_UI_BIND_ON;
        compatible = numeric(info->kind) &&
                     target(s, c, "DeUiToggle", offsetof(DeUiToggle, on), sizeof(bool));
    } else if (!strcmp(binding->property, "text")) {
        c->property = DE_UI_BIND_TEXT;
        compatible =
            (numeric(info->kind) || info->kind == DE_FK_STRING || info->kind == DE_FK_NAME) &&
            target(s, c, "DeUiText", 0, sizeof(DeUiText));
        de_type_id editor = de_type_find(types, "DeUiTextInput");
        de_component ec = de_component_find(ui->world, "DeUiTextInput");
        if (editor.id && ec.id && de_entity_has(ui->world, c->entity, ec)) {
            const de_field_info *buffer = NULL, *length = NULL;
            u32 bo = 0, lo = 0;
            bool buffer_ok = false;
            u32 buffer_size = 0;
            if (de_reflect_resolve_path(types, editor, "buffer", &buffer, &bo) == DE_OK) {
                buffer_size = buffer->size;
                buffer_ok = buffer->kind == DE_FK_STRING;
                if (buffer->kind == DE_FK_FIXED_ARRAY) {
                    const de_field_info *element = NULL;
                    u32 element_offset = 0;
                    buffer_ok = de_reflect_resolve_path(types, editor, "buffer[0]", &element,
                                                        &element_offset) == DE_OK &&
                                element_offset == bo && element->size == 1 &&
                                (element->kind == DE_FK_U8 || element->kind == DE_FK_I8);
                }
            }
            if (buffer_ok && buffer_size &&
                de_reflect_resolve_path(types, editor, "length", &length, &lo) == DE_OK &&
                length->kind == DE_FK_U32) {
                c->editor = ec;
                c->editor_offset = bo;
                c->editor_size = buffer_size;
                c->editor_length = lo;
            }
        }
    } else if (!strcmp(binding->property, "visible")) {
        c->property = DE_UI_BIND_VISIBLE;
        compatible = numeric(info->kind) && target(s, c, "DeUiNode", offsetof(DeUiNode, display),
                                                   sizeof(((DeUiNode *)0)->display));
    } else if (!strcmp(binding->property, "disabled")) {
        c->property = DE_UI_BIND_DISABLED;
        compatible =
            numeric(info->kind) && target(s, c, "DeUiInteractionState",
                                          offsetof(DeUiInteractionState, disabled), sizeof(bool));
        if (!compatible && numeric(info->kind) &&
            de_entity_has(ui->world, c->entity, ui->ids.node)) {
            (void)de_entity_add(ui->world, c->entity, ui->ids.state);
            compatible = target(s, c, "DeUiInteractionState",
                                offsetof(DeUiInteractionState, disabled), sizeof(bool));
        }
    } else if (!strcmp(binding->property, "opacity")) {
        c->property = DE_UI_BIND_OPACITY;
        compatible = numeric(info->kind) &&
                     target(s, c, "DeUiStyle", offsetof(DeUiStyle, opacity), sizeof(f32));
    } else if (!strcmp(binding->property, "tint")) {
        c->property = DE_UI_BIND_TINT;
        compatible =
            (info->kind == DE_FK_VEC4 || info->kind == DE_FK_COLOR) &&
            (target(s, c, "DeUiImage", offsetof(DeUiImage, tint), sizeof(de_vec4)) ||
             target(s, c, "DeUiText", offsetof(DeUiText, style) + offsetof(de_text_style, color),
                    sizeof(de_vec4)) ||
             target(s, c, "DeUiStyle", offsetof(DeUiStyle, background), sizeof(de_vec4)));
    }
    if (!compatible) {
        fail(s, c, "property or source kind mismatch");
        return;
    }
    if (c->previous_capacity < info->size) {
        c->previous = de_arena_push(s->arena, info->size, 1);
        c->previous_capacity = c->previous ? info->size : 0;
    }
    if (!c->previous) {
        fail(s, c, "source cache arena exhausted");
        return;
    }
    c->valid = true;
}
static bool put(de_ui_binding_system *s, de_entity e, de_component comp, u32 offset,
                const void *value, u32 size) {
    const u8 *old = de_entity_get_const(s->ui->world, e, comp);
    if (!old || !memcmp(old + offset, value, size))
        return false;
    u8 *data = de_entity_get(s->ui->world, e, comp);
    memcpy(data + offset, value, size);
    ++s->writes;
    return true;
}
static bool text_value(de_ui_binding_system *s, de_ui_binding_cache *c, const void *source,
                       char *formatted, u32 cap, de_str *out) {
    de_ui *ui = s->ui;
    if (c->field.kind == DE_FK_STRING) {
        *out = (de_str){source, strnlen(source, c->field.size)};
        return true;
    }
    if (c->field.kind == DE_FK_NAME) {
        de_name id;
        memcpy(&id, source, sizeof id);
        *out = de_loc(ui->desc.text, id);
        return true;
    }
    long double value;
    if (!number(&c->field, source, &value) || !isfinite(value))
        return false;
    if (!c->descriptor.format[0]) {
        int n;
        if (c->field.kind == DE_FK_U64) {
            u64 v;
            memcpy(&v, source, 8);
            n = snprintf(formatted, cap, "%" PRIu64, v);
        } else if (c->field.kind == DE_FK_I64) {
            i64 v;
            memcpy(&v, source, 8);
            n = snprintf(formatted, cap, "%" PRId64, v);
        } else if (c->field.kind == DE_FK_BOOL)
            n = snprintf(formatted, cap, "%s", value ? "true" : "false");
        else
            n = snprintf(formatted, cap, "%.15Lg", value);
        if (n < 0 || (u32)n >= cap)
            return false;
        *out = (de_str){formatted, (size_t)n};
        return true;
    }
    de_loc_arg arg = {.name = "value"};
    if (c->field.kind == DE_FK_I64) {
        arg.kind = DE_LOC_ARG_INT;
        memcpy(&arg.i, source, sizeof arg.i);
    } else if (c->field.kind == DE_FK_U64) {
        u64 v;
        memcpy(&v, source, sizeof v);
        if (v <= INT64_MAX) {
            arg.kind = DE_LOC_ARG_INT;
            arg.i = (i64)v;
        } else {
            arg.kind = DE_LOC_ARG_FLOAT;
            arg.f = (f64)v;
        }
    } else if (c->field.kind == DE_FK_F32 || c->field.kind == DE_FK_F64) {
        arg.kind = DE_LOC_ARG_FLOAT;
        arg.f = (f64)value;
    } else {
        arg.kind = DE_LOC_ARG_INT;
        arg.i = (i64)value;
    }
    de_error_clear();
    u32 n = de_loc_format_pattern(ui->desc.text, de_str_from_cstr(c->descriptor.format), &arg, 1,
                                  formatted, cap);
    if (de_error_last() || n >= cap)
        return false;
    *out = (de_str){formatted, n};
    return true;
}
static void apply_text(de_ui_binding_system *s, de_ui_binding_cache *c, const void *source) {
    char formatted[1024];
    de_str value;
    if (!text_value(s, c, source, formatted, sizeof formatted, &value)) {
        fail(s, c, "text formatting failed");
        return;
    }
    if (c->editor.id) {
        const u8 *editor = de_entity_get_const(s->ui->world, c->entity, c->editor);
        size_t length = value.len < c->editor_size - 1 ? value.len : c->editor_size - 1;
        /* Editor storage is the reflected committed buffer, never password/preedit display text. */
        if (editor && (strnlen((const char *)editor + c->editor_offset, c->editor_size) != length ||
                       memcmp(editor + c->editor_offset, value.ptr, length))) {
            u8 *edit = de_entity_get(s->ui->world, c->entity, c->editor);
            memcpy(edit + c->editor_offset, value.ptr, length);
            edit[c->editor_offset + length] = 0;
            u32 n = (u32)length;
            memcpy(edit + c->editor_length, &n, sizeof n);
            ++s->writes;
            de_ui_widget_table *table = de_ui__widget_table(s->ui);
            if (table && table->refresh_text)
                table->refresh_text(s->ui, c->entity);
        }
        return;
    }
    const DeUiText *text = de_entity_get_const(s->ui->world, c->entity, c->target);
    if (!text)
        return;
    if (!text->loc_id.hash && text->literal.len == value.len &&
        (!value.len || !memcmp(text->literal.ptr, value.ptr, value.len)))
        return;
    if (c->text_capacity < value.len + 1) {
        c->text = de_arena_push(s->arena, value.len + 1, 1);
        c->text_capacity = c->text ? (u32)value.len + 1 : 0;
    }
    if (!c->text) {
        fail(s, c, "text arena exhausted");
        return;
    }
    memcpy(c->text, value.ptr, value.len);
    c->text[value.len] = 0;
    DeUiText *edit = de_entity_get(s->ui->world, c->entity, c->target);
    edit->literal = (de_str){c->text, value.len};
    edit->loc_id = (de_name){0};
    ++s->writes;
}
static void apply(de_ui_binding_system *s, de_ui_binding_cache *c, const void *source) {
    if (c->property == DE_UI_BIND_TEXT) {
        apply_text(s, c, source);
        return;
    }
    if (c->property == DE_UI_BIND_TINT) {
        put(s, c->entity, c->target, c->target_offset, source, sizeof(de_vec4));
        return;
    }
    long double value;
    if (!number(&c->field, source, &value) || !isfinite(value)) {
        fail(s, c, "nonfinite numeric source");
        return;
    }
    if (c->property == DE_UI_BIND_VISIBLE) {
        de_ui_display display = value ? DE_UI_FLEX : DE_UI_NONE;
        put(s, c->entity, c->target, c->target_offset, &display, c->target_size);
    } else if (c->property == DE_UI_BIND_DISABLED || c->property == DE_UI_BIND_ON) {
        bool flag = value != 0;
        put(s, c->entity, c->target, c->target_offset, &flag, sizeof flag);
        if (c->property == DE_UI_BIND_DISABLED &&
            de_entity_has(s->ui->world, c->entity, s->ui->widgets->button))
            put(s, c->entity, s->ui->widgets->button, offsetof(DeUiButton, disabled), &flag,
                sizeof flag);
    } else {
        f32 scalar = (f32)fmaxl(-FLT_MAX, fminl(FLT_MAX, value));
        put(s, c->entity, c->target, c->target_offset, &scalar, sizeof scalar);
    }
}
static void writeback(de_ui_binding_system *s, const de_ui_event *event) {
    if (event->kind != DE_UI_EV_CHANGED)
        return;
    de_ui_binding_cache *c = cache(s, event->target);
    if (!c || !c->valid || c->descriptor.mode != DE_UI_BIND_TWO_WAY ||
        (c->field.flags & DE_FIELD_READONLY))
        return;
    const u8 *source = de_entity_get_const(s->ui->world, c->descriptor.source, c->source);
    if (!source)
        return;
    if (c->property == DE_UI_BIND_TEXT) {
        de_str text = {0};
        if (c->editor.id) {
            const u8 *editor = de_entity_get_const(s->ui->world, c->entity, c->editor);
            if (!editor)
                return;
            text = (de_str){(const char *)editor + c->editor_offset,
                            strnlen((const char *)editor + c->editor_offset, c->editor_size)};
        } else {
            const DeUiText *target_text = de_entity_get_const(s->ui->world, c->entity, c->target);
            if (!target_text)
                return;
            text = target_text->literal;
        }
        if (c->field.kind == DE_FK_STRING) {
            size_t n = text.len < c->field.size - 1 ? text.len : c->field.size - 1;
            if (strnlen((const char *)source + c->offset, c->field.size) == n &&
                !memcmp(source + c->offset, text.ptr, n))
                return;
            u8 *dest = de_entity_get(s->ui->world, c->descriptor.source, c->source);
            memcpy(dest + c->offset, text.ptr, n);
            memset(dest + c->offset + n, 0, c->field.size - n);
            de_entity_mark_changed(s->ui->world, c->descriptor.source, c->source);
            return;
        }
        if (c->field.kind == DE_FK_NAME) {
            de_name name = de_name_from_view(text);
            if (put(s, c->descriptor.source, c->source, c->offset, &name, sizeof name))
                de_entity_mark_changed(s->ui->world, c->descriptor.source, c->source);
            return;
        }
        long double numeric_value;
        f64 n;
        if (de_str_to_f64(text, &n) != DE_OK)
            return;
        numeric_value = n;
        u8 bytes[8];
        if (write_number(&c->field, numeric_value, bytes) &&
            put(s, c->descriptor.source, c->source, c->offset, bytes, c->field.size))
            de_entity_mark_changed(s->ui->world, c->descriptor.source, c->source);
        return;
    }
    u8 bytes[8];
    if (write_number(&c->field, event->value, bytes) &&
        put(s, c->descriptor.source, c->source, c->offset, bytes, c->field.size))
        de_entity_mark_changed(s->ui->world, c->descriptor.source, c->source);
}
static void collect(const de_query_chunk *chunk, void *user) {
    de_ui_binding_system *s = user;
    for (u32 i = 0; i < chunk->count && s->count < s->capacity; ++i)
        s->entities[s->count++] = chunk->entities[i];
}
static void update(de_system_ctx *ctx) {
    de_ui_binding_system *s = ctx->user;
    de_ui *ui = s->ui;
    u64 start = de_time_now_ns();
    s->count = s->writes = 0;
    de_query_each(ui->world, ctx->queries[0], collect, s);
    u64 revision = de_type_registry_revision(de_world_types(ui->world));
    u64 locale = de_loc_generation(ui->desc.text);
    for (u32 i = 0; i < s->count; ++i) {
        de_ui_binding_cache *c = cache(s, s->entities[i]);
        const DeUiBinding *binding = de_entity_get_const(ui->world, s->entities[i], s->component);
        if (c && binding &&
            (c->revision != revision || memcmp(&c->descriptor, binding, sizeof *binding))) {
            DeUiBinding copy = *binding;
            resolve(s, c, &copy, revision);
        }
    }
    const de_ui_event *event;
    while ((event = de_event_next(ui->world, &s->events)))
        writeback(s, event);
    for (u32 i = 0; i < s->count; ++i) {
        de_ui_binding_cache *c = cache(s, s->entities[i]);
        if (!c || !c->valid)
            continue;
        const u8 *source = de_entity_get_const(ui->world, c->descriptor.source, c->source);
        if (!source) {
            c->sampled = false;
            continue;
        }
        bool relocalize = c->property == DE_UI_BIND_TEXT && c->locale_generation != locale;
        if (c->sampled && !relocalize && !memcmp(c->previous, source + c->offset, c->field.size))
            continue;
        /* A target structural add may relocate the source; retain its bytes before applying. */
        memcpy(c->previous, source + c->offset, c->field.size);
        c->sampled = true;
        c->locale_generation = locale;
        apply(s, c, c->previous);
    }
    s->update_ms = (de_time_now_ns() - start) / 1000000.;
}
de_result de_ui__binding_init(de_ui *ui) {
    de_ui_binding_system *s = DE_NEW(ui->alloc, de_ui_binding_system, DE_MEM_UI);
    if (!s)
        return DE_ERR_OUT_OF_MEMORY;
    memset(s, 0, sizeof *s);
    ui->binding = s;
    s->ui = ui;
    s->capacity = ui->capacity;
    s->cache_capacity = ui->hash_capacity;
    s->log = de_log_register_category("ui.binding", DE_LOG_WARN);
    s->cache = DE_ALLOC(ui->alloc, (u64)s->cache_capacity * sizeof *s->cache,
                        _Alignof(de_ui_binding_cache), DE_MEM_UI);
    s->entities =
        DE_ALLOC(ui->alloc, (u64)s->capacity * sizeof *s->entities, _Alignof(de_entity), DE_MEM_UI);
    if (!s->cache || !s->entities)
        return DE_ERR_OUT_OF_MEMORY;
    memset(s->cache, 0, (u64)s->cache_capacity * sizeof *s->cache);
    return de_arena_create(
        &(de_arena_desc){.reserve_bytes = 256ull << 20, .tag = DE_MEM_UI, .name = "ui.binding"},
        &s->arena);
}
void de_ui__binding_detach(de_ui *ui) {
    de_ui_binding_system *s = ui->binding;
    if (!s)
        return;
    if (ui->world && s->system.id)
        de_system_remove(ui->world, s->system);
    s->system = (de_system){0};
    s->count = 0;
    /* Keep text spans alive until UI destruction: existing DeUiText views may outlive detach. */
    for (u32 i = 0; i < s->cache_capacity; ++i) {
        s->cache[i].entity = DE_ENTITY_NULL;
        s->cache[i].valid = false;
    }
}
void de_ui__binding_shutdown(de_ui *ui) {
    de_ui_binding_system *s = ui->binding;
    if (!s)
        return;
    de_ui__binding_detach(ui);
    de_arena_destroy(s->arena);
    if (s->cache)
        DE_FREE(ui->alloc, s->cache, (u64)s->cache_capacity * sizeof *s->cache);
    if (s->entities)
        DE_FREE(ui->alloc, s->entities, (u64)s->capacity * sizeof *s->entities);
    DE_FREE(ui->alloc, s, sizeof *s);
    ui->binding = NULL;
}
void de_ui__binding_attach(de_ui *ui) {
    de_ui_binding_system *s = ui->binding;
    if (!s)
        return;
    de_type_registry *types = de_world_types(ui->world);
    if (!de_type_find(types, "de_ui_binding_mode").id) {
        static const de_enum_value modes[] = {{"ONE_WAY", DE_UI_BIND_ONE_WAY},
                                              {"TWO_WAY", DE_UI_BIND_TWO_WAY}};
        if (de_enum_register(types, "de_ui_binding_mode", modes, 2, false) != DE_OK)
            return;
    }
    static const de_field_info fields[] = {
        DE_FIELD(DeUiBinding, source, DE_FK_ENTITY, DE_FIELD_DEFAULT),
        DE_FIELD(DeUiBinding, path, DE_FK_STRING, DE_FIELD_DEFAULT),
        DE_FIELD(DeUiBinding, property, DE_FK_STRING, DE_FIELD_DEFAULT),
        DE_FIELD_EX(DeUiBinding, mode, DE_FK_ENUM, DE_FIELD_DEFAULT,
                    {0, 1, NULL, NULL, "de_ui_binding_mode", 0}),
        DE_FIELD(DeUiBinding, format, DE_FK_STRING, DE_FIELD_DEFAULT)};
    de_type_id type = de_type_find(types, "DeUiBinding");
    if (!type.id && de_type_register_ex(types,
                                        &(de_type_desc){.name = "DeUiBinding",
                                                        .size = sizeof(DeUiBinding),
                                                        .align = _Alignof(DeUiBinding),
                                                        .version = 1,
                                                        .is_pod = true,
                                                        .fields = fields,
                                                        .field_count = 5},
                                        &type, NULL) != DE_OK)
        return;
    s->component = de_component_register(ui->world, type, DE_COMP_SERIALIZE | DE_COMP_SNAPSHOT);
    if (!s->component.id)
        return;
    de_query_term term = {s->component, DE_READ};
    de_query_desc query = {.terms = &term, .term_count = 1};
    s->events = de_event_reader_create(ui->world, ui->event);
    const char *before[] = {DE_UI_STYLE_SYSTEM, DE_UI_LAYOUT_SYSTEM};
#if defined(DE_UI_NAVIGATION)
    const char *after[] = {"ui.widgets", "ui.focus"};
#else
    const char *after[] = {"ui.widgets"};
#endif
    s->system = de_system_add(
        ui->world, &(de_system_desc){.name = "ui.binding",
                                     .schedule = DE_SCHEDULE_POST_UPDATE,
                                     .fn = update,
                                     .user = s,
                                     .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE,
                                     .queries = &query,
                                     .query_count = 1,
                                     .before = before,
                                     .before_count = 2,
                                     .after = after,
                                     .after_count = (u32)(sizeof after / sizeof after[0])});
}
