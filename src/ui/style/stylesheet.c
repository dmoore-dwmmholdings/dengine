#include "stylesheet_internal.h"
#include "asset/slots_internal.h"
#include <limits.h>
#include <math.h>
#include <string.h>
#define P(T, member, target_, kind_, label) {label, target_, kind_, offsetof(T, member)}
const de_ui_style_property de_ui__style_properties[] = {
    P(DeUiStyle, background, DE_UI_TARGET_STYLE, DE_UI_PROP_COLOR, "background"),
    P(DeUiStyle, background_image, DE_UI_TARGET_STYLE, DE_UI_PROP_ASSET, "background_image"),
    P(DeUiStyle, nine_slice, DE_UI_TARGET_STYLE, DE_UI_PROP_VEC4, "nine_slice"),
    P(DeUiStyle, border_color, DE_UI_TARGET_STYLE, DE_UI_PROP_COLOR, "border_color"),
    P(DeUiStyle, border_width, DE_UI_TARGET_STYLE, DE_UI_PROP_FLOAT, "border_width"),
    P(DeUiStyle, corner_radius, DE_UI_TARGET_STYLE, DE_UI_PROP_FLOAT, "radius"),
    P(DeUiStyle, opacity, DE_UI_TARGET_STYLE, DE_UI_PROP_FLOAT, "opacity"),
    P(DeUiStyle, text_color, DE_UI_TARGET_STYLE, DE_UI_PROP_COLOR, "text_color"),
    P(DeUiNode, padding, DE_UI_TARGET_NODE, DE_UI_PROP_EDGES, "padding"),
    P(DeUiNode, margin, DE_UI_TARGET_NODE, DE_UI_PROP_EDGES, "margin"),
    P(DeUiNode, width, DE_UI_TARGET_NODE, DE_UI_PROP_LENGTH, "width"),
    P(DeUiNode, height, DE_UI_TARGET_NODE, DE_UI_PROP_LENGTH, "height"),
    P(DeUiNode, min_width, DE_UI_TARGET_NODE, DE_UI_PROP_LENGTH, "min_width"),
    P(DeUiNode, min_height, DE_UI_TARGET_NODE, DE_UI_PROP_LENGTH, "min_height"),
    P(DeUiNode, max_width, DE_UI_TARGET_NODE, DE_UI_PROP_LENGTH, "max_width"),
    P(DeUiNode, max_height, DE_UI_TARGET_NODE, DE_UI_PROP_LENGTH, "max_height"),
    P(DeUiNode, gap, DE_UI_TARGET_NODE, DE_UI_PROP_FLOAT, "gap"),
    P(de_text_style, font, DE_UI_TARGET_TEXT, DE_UI_PROP_ASSET, "font"),
    P(de_text_style, size_px, DE_UI_TARGET_TEXT, DE_UI_PROP_FLOAT, "font_size")};
const u32 de_ui__style_property_count = DE_UI_STYLE_COUNT(de_ui__style_properties);
#undef P
static de_result parse_error(int line, const char *reason) {
    return DE_FAIL(DE_ERR_PARSE, "UI stylesheet line %d: %s", line > 0 ? line : 1, reason);
}
static bool known_widget(const char *name, size_t length) {
    static const char *const widgets[] = {"panel",   "button",   "toggle", "slider", "text",
                                          "image",   "progress", "input",  "scroll", "dropdown",
                                          "tooltip", "list",     "tabs",   "tree",   "canvas"};
    for (u32 i = 0; i < DE_UI_STYLE_COUNT(widgets); ++i)
        if (strlen(widgets[i]) == length && !memcmp(name, widgets[i], length))
            return true;
    return false;
}
static de_result selector(const char *text, int line, de_ui_style_rule *rule) {
    const char *end = text + strlen(text), *state = strchr(text, ':');
    if (state) {
        const char *name = state + 1;
        rule->state = !strcmp(name, "hover")      ? 1u
                      : !strcmp(name, "focus")    ? 2u
                      : !strcmp(name, "pressed")  ? 4u
                      : !strcmp(name, "disabled") ? 8u
                                                  : 0u;
        if (!rule->state)
            return parse_error(line, "unknown state selector");
        rule->specificity += 5;
        end = state;
    }
    const char *dot = memchr(text, '.', (size_t)(end - text));
    const char *type_end = dot ? dot : end;
    if (type_end > text) {
        if (!known_widget(text, (size_t)(type_end - text)))
            return parse_error(line, "unknown widget type");
        rule->widget = de_name_from_view((de_str){text, (u64)(type_end - text)});
        rule->specificity += 1;
    }
    if (dot) {
        if (dot + 1 == end)
            return parse_error(line, "empty class selector");
        for (const char *p = dot + 1; p < end; ++p)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
                return parse_error(line, "invalid class selector");
        rule->class_name = de_name_from_view((de_str){dot + 1, (u64)(end - dot - 1)});
        rule->specificity += 10;
    }
    if (type_end == text && !dot)
        return parse_error(line, "empty selector");
    return DE_OK;
}
static bool property_known(const char *name) {
    if (!strcmp(name, "transition") || !strcmp(name, "easing"))
        return true;
    for (u32 i = 0; i < de_ui__style_property_count; ++i)
        if (!strcmp(name, de_ui__style_properties[i].name))
            return true;
    return false;
}
de_result de_ui__sheet_parse(const de_allocator *alloc, const char *bytes, u64 length,
                             de_ui_sheet_document **out) {
    if (!out || (!bytes && length))
        return DE_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (length > 4u * 1024u * 1024u || length > INT_MAX)
        return parse_error(1, "sheet exceeds4MiB limit");
    if (!alloc)
        alloc = de_heap_allocator();
    toml_result_t document = de__toml_parse(alloc, bytes ? bytes : "", (int)length);
    if (!document.ok) {
        de_result result = DE_FAIL(DE_ERR_PARSE, "UI stylesheet: %s", document.errmsg);
        de__toml_free(document);
        return result;
    }
    toml_datum_t style = toml_get(document.toptab, "style");
    if (style.type != TOML_UNKNOWN && style.type != TOML_TABLE) {
        de__toml_free(document);
        return parse_error(style.lineno, "style must be a table");
    }
    if (style.type == TOML_TABLE && style.u.tab.size > (int)DE_UI_STYLE_RULES) {
        de__toml_free(document);
        return parse_error(style.lineno, "too many style rules");
    }
    de_ui_sheet_document *sheet = DE_NEW(alloc, de_ui_sheet_document, DE_MEM_UI);
    if (!sheet) {
        de__toml_free(document);
        return DE_ERR_OUT_OF_MEMORY;
    }
    memset(sheet, 0, sizeof *sheet);
    sheet->alloc = alloc;
    sheet->document = document;
    sheet->references = 1;
    sheet->rule_count = style.type == TOML_TABLE ? (u32)style.u.tab.size : 0;
    if (sheet->rule_count)
        sheet->rules = DE_ALLOC(alloc, (u64)sheet->rule_count * sizeof *sheet->rules,
                                _Alignof(de_ui_style_rule), DE_MEM_UI);
    if (sheet->rule_count && !sheet->rules) {
        de_ui__sheet_release(sheet);
        return DE_ERR_OUT_OF_MEMORY;
    }
    if (sheet->rules)
        memset(sheet->rules, 0, (size_t)sheet->rule_count * sizeof *sheet->rules);
    de_result result = DE_OK;
    for (u32 i = 0; i < sheet->rule_count && result == DE_OK; ++i) {
        de_ui_style_rule *rule = &sheet->rules[i];
        rule->order = i;
        rule->properties = style.u.tab.value[i];
        result = selector(style.u.tab.key[i], rule->properties.lineno, rule);
        if (result != DE_OK)
            break;
        if (rule->properties.type != TOML_TABLE) {
            result = parse_error(rule->properties.lineno, "rule must be a table");
            break;
        }
        for (int j = 0; j < rule->properties.u.tab.size; ++j)
            if (!property_known(rule->properties.u.tab.key[j])) {
                result =
                    parse_error(rule->properties.u.tab.value[j].lineno, "unknown style property");
                break;
            }
    }
    if (result == DE_OK) {
        de_ui_compiled_rule *compiled =
            sheet->rule_count ? DE_ALLOC(alloc, (u64)sheet->rule_count * sizeof *compiled,
                                         _Alignof(de_ui_compiled_rule), DE_MEM_UI)
                              : NULL;
        if (sheet->rule_count && !compiled)
            result = DE_ERR_OUT_OF_MEMORY;
        else {
            de_ui_style_system validation = {0};
            de_ui_style_sheet_slot slot = {.sheet = sheet, .compiled = compiled};
            result = de_ui__compile_sheet(&validation, &slot);
            toml_datum_t themes = toml_get(document.toptab, "theme"),
                         vars = toml_get(document.toptab, "vars");
            if (vars.type != TOML_UNKNOWN && vars.type != TOML_TABLE)
                result = parse_error(vars.lineno, "vars must be a table");
            if (themes.type != TOML_UNKNOWN && themes.type != TOML_TABLE)
                result = parse_error(themes.lineno, "theme must be a table");
            if (themes.type == TOML_TABLE)
                for (int i = 0; i < themes.u.tab.size && result == DE_OK; ++i) {
                    validation.theme = de_name_from(themes.u.tab.key[i]);
                    toml_datum_t theme_vars = toml_get(themes.u.tab.value[i], "vars");
                    if (theme_vars.type != TOML_UNKNOWN && theme_vars.type != TOML_TABLE)
                        result = parse_error(theme_vars.lineno, "theme vars must be a table");
                    else
                        result = de_ui__compile_sheet(&validation, &slot);
                }
        }
        if (compiled)
            DE_FREE(alloc, compiled, (u64)sheet->rule_count * sizeof *compiled);
    }
    if (result != DE_OK) {
        de_ui__sheet_release(sheet);
        return result;
    }
    *out = sheet;
    return DE_OK;
}
void de_ui__sheet_release(de_ui_sheet_document *sheet) {
    if (!sheet || --sheet->references)
        return;
    if (sheet->rules)
        DE_FREE(sheet->alloc, sheet->rules, (u64)sheet->rule_count * sizeof *sheet->rules);
    de__toml_free(sheet->document);
    DE_FREE(sheet->alloc, sheet, sizeof *sheet);
}
/* The custom asset loader is pure: parsed objects never hold UI pointers. */
static de_result sheet_dependencies(de_asset_load_ctx *ctx, de_ui_sheet_document *sheet) {
    de_ui_compiled_rule *compiled =
        sheet->rule_count ? DE_ALLOC(sheet->alloc, (u64)sheet->rule_count * sizeof *compiled,
                                     _Alignof(de_ui_compiled_rule), DE_MEM_UI)
                          : NULL;
    if (sheet->rule_count && !compiled)
        return DE_ERR_OUT_OF_MEMORY;
    de_ui_style_sheet_slot slot = {.sheet = sheet, .compiled = compiled};
    de_ui_style_system state = {0};
    toml_datum_t themes = toml_get(sheet->document.toptab, "theme");
    u32 theme_count = themes.type == TOML_TABLE ? (u32)themes.u.tab.size : 0;
    de_result result = DE_OK;
    for (u32 theme = 0; theme <= theme_count && result == DE_OK; ++theme) {
        state.theme = theme ? de_name_from(themes.u.tab.key[theme - 1]) : (de_name){0};
        result = de_ui__compile_sheet(&state, &slot);
        for (u32 rule = 0; rule < sheet->rule_count && result == DE_OK; ++rule)
            for (u32 p = 0; p < de_ui__style_property_count && result == DE_OK; ++p)
                if ((compiled[rule].mask & (1u << p)) &&
                    de_ui__style_properties[p].kind == DE_UI_PROP_ASSET)
                    result = de_asset_load_ctx_add_dependency(ctx, compiled[rule].values[p].asset);
    }
    if (compiled)
        DE_FREE(sheet->alloc, compiled, (u64)sheet->rule_count * sizeof *compiled);
    return result;
}
static de_result sheet_asset_load(de_asset_load_ctx *ctx) {
    u64 size = 0;
    const char *bytes = de_asset_load_ctx_data(ctx, &size);
    de_ui_sheet_document *sheet = NULL;
    de_result result = de_ui__sheet_parse(NULL, bytes, size, &sheet);
    if (result != DE_OK)
        return result;
    result = sheet_dependencies(ctx, sheet);
    if (result != DE_OK) {
        de_ui__sheet_release(sheet);
        return result;
    }
    de_asset_load_ctx_set_object(ctx, sheet, size);
    return DE_OK;
}
static void sheet_asset_unload(void *object, void *user) {
    (void)user;
    de_ui__sheet_release(object);
}
de_blob de_ui_stylesheet(de_ui *ui, de_asset_id id) {
    return (de_blob){ui && ui->desc.assets ? de__asset_handle_of(ui->desc.assets, id, "ui_style")
                                           : 0};
}
static void slot_clear(de_ui_style_system *state, de_ui_style_sheet_slot *slot) {
    if (slot->compiled)
        DE_FREE(state->ui->alloc, slot->compiled,
                (u64)slot->sheet->rule_count * sizeof *slot->compiled);
    de_ui__sheet_release(slot->sheet);
    memset(slot, 0, sizeof *slot);
}
static de_result slot_build(de_ui_style_system *state, de_blob handle,
                            de_ui_style_sheet_slot *slot) {
    memset(slot, 0, sizeof *slot);
    slot->handle = handle;
    de__slot *asset = de__slot_get(state->ui->desc.assets, handle.id);
    if (!asset)
        return DE_ERR_NOT_FOUND;
    slot->fingerprint = asset->version;
    if (asset->type_hash == de_hash64_str("ui_style")) {
        slot->sheet = de__slot_object(asset);
        if (!slot->sheet)
            return DE_ERR_NOT_FOUND;
        ++slot->sheet->references;
    } else if (asset->type_hash == de_hash64_str("blob")) {
        u64 size = 0;
        const char *bytes = de_blob_data(state->ui->desc.assets, handle, &size);
        if (!bytes)
            return DE_ERR_NOT_FOUND;
        DE_TRY(de_ui__sheet_parse(state->ui->alloc, bytes, size, &slot->sheet));
    } else
        return DE_ERR_INVALID_ARGUMENT;
    if (slot->sheet->rule_count) {
        slot->compiled =
            DE_ALLOC(state->ui->alloc, (u64)slot->sheet->rule_count * sizeof *slot->compiled,
                     _Alignof(de_ui_compiled_rule), DE_MEM_UI);
        if (!slot->compiled) {
            slot_clear(state, slot);
            return DE_ERR_OUT_OF_MEMORY;
        }
    }
    de_result result = de_ui__compile_sheet(state, slot);
    if (result != DE_OK)
        slot_clear(state, slot);
    return result;
}
de_result de_ui_load_stylesheet(de_ui *ui, de_blob stylesheet) {
    if (!ui || !ui->style || !ui->desc.assets || !stylesheet.id)
        return DE_ERR_INVALID_ARGUMENT;
    de_ui_style_system *state = ui->style;
    for (u32 i = 0; i < state->sheet_count; ++i)
        if (state->sheets[i].handle.id == stylesheet.id)
            return DE_OK;
    if (state->sheet_count >= DE_UI_STYLE_SHEETS)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "UI stylesheet limit reached");
    de_ui_style_sheet_slot slot;
    DE_TRY(slot_build(state, stylesheet, &slot));
    state->sheets[state->sheet_count++] = slot;
    ++state->revision;
    return DE_OK;
}
void de_ui__sheets_refresh(de_ui_style_system *state) {
    if (!state->ui->desc.assets)
        return;
    for (u32 i = 0; i < state->sheet_count; ++i) {
        de_ui_style_sheet_slot *slot = &state->sheets[i];
        de__slot *asset = de__slot_get(state->ui->desc.assets, slot->handle.id);
        if (!asset || asset->version == slot->fingerprint)
            continue;
        de_ui_style_sheet_slot replacement;
        de_result result = slot_build(state, slot->handle, &replacement);
        if (result == DE_OK) {
            slot_clear(state, slot);
            *slot = replacement;
            ++state->revision;
        } else {
            slot->fingerprint = asset->version;
            DE_WARN(DE_LOG_CAT_GENERAL, "stylesheet reload rejected: %s", de_error_last()->message);
        }
    }
}
void de_ui__sheets_shutdown(de_ui_style_system *state) {
    for (u32 i = 0; i < state->sheet_count; ++i)
        slot_clear(state, &state->sheets[i]);
}
de_result de_ui__style_asset_register(de_asset_system *assets) {
    if (!assets)
        return DE_OK;
    /* Asset types are system-wide; a second UI shares the existing pure loader. */
    de_asset_type type =
        de_asset_type_register(assets, &(de_asset_type_desc){.name = "ui_style",
                                                             .importer_version = 1,
                                                             .load = sheet_asset_load,
                                                             .unload = sheet_asset_unload});
    if (type.id)
        return DE_OK;
    return de_error_last()->result == DE_ERR_ALREADY_EXISTS ? DE_OK : de_error_last()->result;
}
