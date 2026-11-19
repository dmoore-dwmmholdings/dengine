/* editor_ui.c: ui.layout (DENG-778; DIDE protocol 1.14, capability ui.editor) and the protocol
 * 1.15 UI editor services (DENG-785).
 *
 * The UI editor asks for the rects of a UI subtree at a preview viewport (a device size, a UI
 * scale, safe-area insets, pixel art) and draws its overlay from the engine's own layout, through
 * de_ui_layout_preview. The live layout is unchanged afterwards. The rects are the root, then its
 * subtree in pre-order with the children in sibling order, so the order the IDE sets with
 * EditOp.index (editor_edit.c) is the order it reads back here.
 *
 * Protocol 1.15 (DENG-785, `unsupported` when the IDE agreed an older version):
 * - PickResult.uiStack: de__ed_ui_pick, called by the viewport pick (editor_views.c). A game view
 *   picks the screen UI at the pixel (de_ui_pick), a scene view the world panels under the camera
 *   ray (de_ui_pick_ray).
 * - ui.nodeCorners: de_ui_node_world_corners, for the outline of a node on a world panel.
 * - ui.themes, ui.themeVars, ui.previewTheme: de_ui_list_themes, de_ui_theme_vars,
 *   de_ui_preview_theme. UiThemeVar.sheet is the stylesheet's load-order index as text: the C API
 *   has no sheet path.
 * - loc.locales, loc.strings, loc.previewLocale: de_loc_list_locales, de_loc_read_strings,
 *   de_loc_preview_locale. LocString.key is the 16-digit lowercase hex of the key hash and
 *   LocString.table the string table's index in asset ID order.
 * A disconnect ends a theme or locale preview the IDE left on (reset hook).
 *
 * Protocol 1.16 (DENG-787, capability ui.stylesheets, `unsupported` below 1.16):
 * - ui.stylesheets: de_ui_list_sheets, with the suffix DE_UI_STYLESHEET_SUFFIX. UiSheet.path is
 *   the manifest path, UiSheet.source the manifest source relative to the asset root, and
 *   UiSheetError.file that same source. A rejected reload is `loaded` with an `error`.
 * - ui.checkStylesheet: de_ui_check_stylesheet on the text, without loading it.
 * A line and column are 1-based; 0 is unknown, and a column is never sent with line 0.
 *
 * Protocol 1.17 (DENG-788, capability ui.textMetrics, `unsupported` below 1.17):
 * - ui.measureText: de_ui_measure_text with the UI's text system, in layout pixels. The capability
 *   is listed only when the engine has a text system; without one a call is `unsupported`.
 *   The font is an asset id (path, or the 16-digit hex of an id the manifest lists) and must be
 *   READY, else `not_found`. sizePx and letterSpacing are multiplied by the scale. lineHeight is
 *   in layout pixels and is not; the engine's de_text_style.line_height is a factor of the font's
 *   own, so the handler measures once with the font's own height and derives the factor. The reply
 *   maps de_ui_text_metrics and de_text_line one to one. Empty text gets what
 *   de_text_layout_lines gives for it.
 */
#include "host/editor/editor_internal.h"

#include "host/host_internal.h"
#include <dengine/de_asset.h>
#include <dengine/de_text.h>
#include <dengine/de_ui.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A safe-area inset in whole pixels; false for a value that is not finite. Negative is 0. */
static bool inset(float v, u32 *out) {
    if (!isfinite(v))
        return false;
    *out = v <= 0.f ? 0u : v >= 65535.f ? 65535u : (u32)lrintf(v);
    return true;
}

static void h_ui_layout(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                        mpack_writer_t *w) {
    DE__ED_PARAMS(ui_layout, de_ep_ui_layout_params, q);
    de_ui_preview_viewport vp = {.width = q.width, .height = q.height};
    if (q.width == 0 || q.height == 0) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "ui.layout: width and height above 0");
        return;
    }
    if (q.has_ui_scale) {
        if (!isfinite(q.ui_scale)) {
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "ui.layout: uiScale not finite");
            return;
        }
        vp.ui_scale = q.ui_scale; /* zero or less: the live ui.scale, as when absent */
    }
    if (q.has_safe &&
        (!inset(q.safe.left, &vp.safe.left) || !inset(q.safe.top, &vp.safe.top) ||
         !inset(q.safe.right, &vp.safe.right) || !inset(q.safe.bottom, &vp.safe.bottom))) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "ui.layout: safe not finite");
        return;
    }
    vp.pixel_art = q.has_pixel_art && q.pixel_art;

    de_ui *ui = ed->e->ui;
    if (ui == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "ui.layout: the game has no UI");
        return;
    }
    const de_entity root = {q.root};
    u32 count = 0;
    f32 scale = 0.f;
    de_result r = de_ui_layout_preview(ui, root, &vp, NULL, 0, &count, &scale);
    de_ui_preview_rect *out = NULL;
    de_ep_ui_layout_rect *items = NULL;
    float *values = NULL;
    if (r == DE_OK && count > 0) {
        out = (de_ui_preview_rect *)de__ed_alloc(a, sizeof *out * count);
        items = (de_ep_ui_layout_rect *)de__ed_alloc(a, sizeof *items * count);
        values = (float *)de__ed_alloc(a, sizeof(float) * 4u * count);
        if (out == NULL || items == NULL || values == NULL) {
            de__ed_error(w, req->id, DE_EP_ERR_BUSY, "ui.layout: %u nodes is too many",
                         (unsigned)count);
            return;
        }
        r = de_ui_layout_preview(ui, root, &vp, out, count, &count, &scale);
    }
    if (r != DE_OK) {
        de_error_clear();
        /* The parameters are checked above, so what is left is a root that is not a live UI
         * node, or a UI that is not attached to the world: nothing to find. */
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "ui.layout: %llu is not a UI node",
                     (unsigned long long)q.root);
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        float *v = values + 4u * i;
        v[0] = out[i].rect.x;
        v[1] = out[i].rect.y;
        v[2] = out[i].rect.z;
        v[3] = out[i].rect.w;
        items[i] = (de_ep_ui_layout_rect){.entity = out[i].entity.bits, .rect = {v, 4}};
    }
    const de_ep_ui_layout res = {.scale = scale, .rects = {items, count}};
    de_ep_write_response_ui_layout(w, req->id, &res);
}

/* ---------- Protocol 1.15 (DENG-785) ---------- */

/* False, after an `unsupported` reply, when the IDE agreed a version below 1.15. */
static bool need_115(de__editor *ed, const de_ep_request *req, mpack_writer_t *w,
                     const char *method) {
    if (de__ed_protocol_at_least(ed, 1, 15))
        return true;
    de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "%s: needs protocol 1.15 (agreed %u.%u)",
                 method, (unsigned)ed->proto_major, (unsigned)ed->proto_minor);
    return false;
}

/* Reads params {key?: string | null}. The IDL says null ends a preview, as absence does, but the
 * generated reader only takes a string; this one takes nil as absent. False: malformed. */
static bool opt_str_param(const de_ep_request *req, const char *key, bool *has, de_ep_str *out) {
    static const char empty_map[1] = {(char)0x80};
    mpack_reader_t r;
    if (req->params.len == 0)
        mpack_reader_init_data(&r, empty_map, 1);
    else
        mpack_reader_init_data(&r, req->params.ptr, req->params.len);
    *has = false;
    *out = (de_ep_str){"", 0};
    const size_t key_len = strlen(key);
    const u32 n = mpack_expect_map(&r);
    for (u32 i = 0; i < n && mpack_reader_error(&r) == mpack_ok; ++i) {
        const u32 kl = mpack_expect_str(&r);
        const char *kp = kl ? mpack_read_bytes_inplace(&r, kl) : "";
        mpack_done_str(&r);
        if (mpack_reader_error(&r) != mpack_ok)
            break;
        if (kl != key_len || memcmp(kp, key, key_len) != 0) {
            mpack_discard(&r);
            continue;
        }
        mpack_tag_t tag = mpack_peek_tag(&r);
        if (mpack_tag_type(&tag) == mpack_type_nil) {
            mpack_discard(&r);
            *has = false;
            *out = (de_ep_str){"", 0};
            continue;
        }
        const u32 vl = mpack_expect_str(&r);
        const char *vp = vl ? mpack_read_bytes_inplace(&r, vl) : "";
        mpack_done_str(&r);
        if (mpack_reader_error(&r) == mpack_ok) {
            *has = true;
            *out = (de_ep_str){vp, vl};
        }
    }
    mpack_done_map(&r);
    return mpack_reader_destroy(&r) == mpack_ok;
}

/* Copies `s` into `buf` with a NUL; false when it does not fit or holds a NUL. */
static bool to_cstr(de_ep_str s, char *buf, size_t cap) {
    if (s.len >= cap || (s.len && memchr(s.ptr, 0, s.len)))
        return false;
    if (s.len)
        memcpy(buf, s.ptr, s.len);
    buf[s.len] = 0;
    return true;
}

/* DENG-786: the sheet's manifest path; else "builtin:N" for an embedded sheet, "asset:<id>" for an
 * asset the manifest does not list, and "sheet:N" for one loaded from source text. `fallback`
 * holds SHEET_NAME_MAX bytes. The path stays valid until the next manifest load. */
#define SHEET_NAME_MAX 32u
static const char *sheet_name(const de_ui *ui, u32 index, char *fallback) {
    de_ui_sheet_info info;
    if (de_ui_get_sheet_info(ui, index, &info) != DE_OK) {
        snprintf(fallback, SHEET_NAME_MAX, "sheet:%u", (unsigned)index);
    } else if (info.path[0]) {
        return info.path;
    } else if (info.builtin) {
        snprintf(fallback, SHEET_NAME_MAX, "builtin:%u", (unsigned)index);
    } else if (info.asset.hash) {
        snprintf(fallback, SHEET_NAME_MAX, "asset:%016llx", (unsigned long long)info.asset.hash);
    } else {
        snprintf(fallback, SHEET_NAME_MAX, "sheet:%u", (unsigned)index);
    }
    return fallback;
}
static void ui_busy(mpack_writer_t *w, u64 id, const char *method) {
    de__ed_error(w, id, DE_EP_ERR_BUSY, "%s: the reply does not fit", method);
}

u32 de__ed_ui_pick(de__editor *ed, f32 x, f32 y, const f64 *origin, const f32 *direction,
                   de__ed_ui_hit *out, u32 capacity) {
    de_ui *ui = ed->e ? ed->e->ui : NULL;
    if (ui == NULL || capacity == 0 || !de__ed_protocol_at_least(ed, 1, 15))
        return 0;
    de_ui_pick_hit hits[DE__ED_UI_STACK];
    const u32 cap = capacity < DE__ED_UI_STACK ? capacity : DE__ED_UI_STACK;
    u32 count = 0;
    const de_result r =
        origin && direction
            ? de_ui_pick_ray(ui, (de_dvec3){origin[0], origin[1], origin[2]},
                             (de_vec3){direction[0], direction[1], direction[2]}, NULL, hits, cap,
                             &count)
            : de_ui_pick(ui, (de_entity){0}, (de_vec2){x, y}, hits, cap, &count);
    if (r != DE_OK) {
        de_error_clear(); /* a degenerate ray or an unattached UI: no stack */
        return 0;
    }
    const u32 n = count < cap ? count : cap;
    for (u32 i = 0; i < n; ++i)
        out[i] = (de__ed_ui_hit){.entity = hits[i].entity.bits,
                                 .root = hits[i].root.bits,
                                 .rect = {hits[i].rect.x, hits[i].rect.y, hits[i].rect.z,
                                          hits[i].rect.w},
                                 .depth = hits[i].depth};
    return n;
}

static void h_ui_node_corners(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                              mpack_writer_t *w) {
    if (!need_115(ed, req, w, "ui.nodeCorners"))
        return;
    DE__ED_PARAMS(ui_node_corners, de_ep_ui_node_corners_params, q);
    de_ui *ui = ed->e->ui;
    de_dvec3 c[4];
    de_entity panel = {0};
    if (ui == NULL || de_ui_node_world_corners(ui, (de_entity){q.entity}, c, &panel) != DE_OK ||
        panel.bits == 0) {
        de_error_clear();
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND,
                     "ui.nodeCorners: %llu is not a node on a world panel",
                     (unsigned long long)q.entity);
        return;
    }
    double *v = (double *)de__ed_alloc(a, sizeof(double) * 12u);
    if (v == NULL) {
        ui_busy(w, req->id, "ui.nodeCorners");
        return;
    }
    for (u32 i = 0; i < 4; ++i) {
        v[i * 3] = c[i].x;
        v[i * 3 + 1] = c[i].y;
        v[i * 3 + 2] = c[i].z;
    }
    const de_ep_ui_node_corners res = {.panel = panel.bits, .corners = {v, 12}};
    de_ep_write_response_ui_node_corners(w, req->id, &res);
}

/* The theme state and the declared themes (the ui.themes and ui.previewTheme reply). */
static bool themes_state(de_ui *ui, de_ep_arena *a, de_ep_ui_themes *out) {
    *out = (de_ep_ui_themes){.active = {"", 0}, .selected = {"", 0}};
    if (ui == NULL)
        return true;
    de_ui_theme_state *st = (de_ui_theme_state *)de__ed_alloc(a, sizeof *st);
    if (st == NULL)
        return false;
    u32 count = 0;
    if (de_ui_get_theme_state(ui, st) != DE_OK || de_ui_list_themes(ui, NULL, 0, &count) != DE_OK) {
        de_error_clear();
        return true;
    }
    out->active = de_ep_cstr(st->active);
    out->selected = de_ep_cstr(st->selected);
    out->previewing = st->previewing;
    if (count == 0)
        return true;
    de_ui_theme_info *infos = (de_ui_theme_info *)de__ed_alloc(a, sizeof *infos * count);
    de_ep_ui_theme_info *items = (de_ep_ui_theme_info *)de__ed_alloc(a, sizeof *items * count);
    if (infos == NULL || items == NULL)
        return false;
    u32 total = 0;
    if (de_ui_list_themes(ui, infos, count, &total) != DE_OK) {
        de_error_clear();
        return true;
    }
    const u32 n = total < count ? total : count;
    for (u32 i = 0; i < n; ++i)
        items[i] = (de_ep_ui_theme_info){.name = de_ep_cstr(infos[i].name),
                                         .sheets = infos[i].sheets};
    out->themes = (de_ep_list_ui_theme_info){items, n};
    return true;
}

static void h_ui_themes(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                        mpack_writer_t *w) {
    if (!need_115(ed, req, w, "ui.themes"))
        return;
    DE__ED_PARAMS(ui_themes, de_ep_empty, q);
    (void)q;
    de_ep_ui_themes res;
    if (!themes_state(ed->e->ui, a, &res)) {
        ui_busy(w, req->id, "ui.themes");
        return;
    }
    de_ep_write_response_ui_themes(w, req->id, &res);
}

static void h_ui_theme_vars(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                            mpack_writer_t *w) {
    if (!need_115(ed, req, w, "ui.themeVars"))
        return;
    bool has = false;
    de_ep_str theme;
    if (!opt_str_param(req, "theme", &has, &theme)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "ui.themeVars: bad params");
        return;
    }
    char name[DE_UI_THEME_NAME_MAX];
    de_ui *ui = ed->e->ui;
    /* A name that cannot be stored cannot be declared either. Without a UI only the active
     * theme and the base variables exist, and they are empty. */
    if (has && (!to_cstr(theme, name, sizeof name) || (ui == NULL && theme.len > 0))) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "ui.themeVars: no theme \"%.*s\"",
                     (int)(theme.len > 64 ? 64 : theme.len), theme.ptr);
        return;
    }
    de_ep_ui_theme_vars res = {0};
    if (ui != NULL) {
        const char *which = has ? name : NULL;
        u32 count = 0;
        de_result r = de_ui_theme_vars(ui, which, NULL, 0, &count);
        de_ui_theme_var *vars = NULL;
        de_ep_ui_theme_var *items = NULL;
        char *sheets = NULL;
        if (r == DE_OK && count > 0) {
            vars = (de_ui_theme_var *)de__ed_alloc(a, sizeof *vars * count);
            items = (de_ep_ui_theme_var *)de__ed_alloc(a, sizeof *items * count);
            sheets = (char *)de__ed_alloc(a, SHEET_NAME_MAX * count);
            if (vars == NULL || items == NULL || sheets == NULL) {
                ui_busy(w, req->id, "ui.themeVars");
                return;
            }
            u32 total = 0;
            r = de_ui_theme_vars(ui, which, vars, count, &total);
            count = total < count ? total : count;
        }
        if (r != DE_OK) {
            de_error_clear();
            de__ed_error(w, req->id,
                         r == DE_ERR_NOT_FOUND ? DE_EP_ERR_NOT_FOUND : DE_EP_ERR_INTERNAL,
                         "ui.themeVars: theme \"%s\"", has ? name : "(active)");
            return;
        }
        for (u32 i = 0; i < count; ++i) {
            const char *sheet = sheet_name(ui, vars[i].sheet, sheets + SHEET_NAME_MAX * i);
            items[i] = (de_ep_ui_theme_var){.name = de_ep_cstr(vars[i].name),
                                            .value = de_ep_cstr(vars[i].value),
                                            .sheet = de_ep_cstr(sheet),
                                            .themed = vars[i].themed,
                                            .truncated = vars[i].truncated};
        }
        res.vars = (de_ep_list_ui_theme_var){items, count};
    }
    de_ep_write_response_ui_theme_vars(w, req->id, &res);
}

static void h_ui_preview_theme(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                               mpack_writer_t *w) {
    if (!need_115(ed, req, w, "ui.previewTheme"))
        return;
    bool has = false;
    de_ep_str name_param;
    if (!opt_str_param(req, "name", &has, &name_param)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "ui.previewTheme: bad params");
        return;
    }
    char name[DE_UI_THEME_NAME_MAX];
    if (has && !to_cstr(name_param, name, sizeof name)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "ui.previewTheme: a theme name is at most %u bytes, without NUL",
                     (unsigned)DE_UI_THEME_NAME_MAX - 1u);
        return;
    }
    de_ui *ui = ed->e->ui;
    if (ui == NULL && has) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "ui.previewTheme: the game has no UI");
        return;
    }
    if (ui != NULL) {
        const de_result r = de_ui_preview_theme(ui, has ? name : NULL);
        if (r != DE_OK) {
            de_error_clear();
            de__ed_error(w, req->id,
                         r == DE_ERR_NOT_FOUND          ? DE_EP_ERR_NOT_FOUND
                         : r == DE_ERR_INVALID_ARGUMENT ? DE_EP_ERR_INVALID_PARAMS
                                                        : DE_EP_ERR_INTERNAL,
                         "ui.previewTheme: theme \"%s\"", has ? name : "(end)");
            return;
        }
    }
    de_ep_ui_themes res;
    if (!themes_state(ui, a, &res)) {
        ui_busy(w, req->id, "ui.previewTheme");
        return;
    }
    de_ep_write_response_ui_preview_theme(w, req->id, &res);
}

/* The locale state and the published locales (the loc.locales and loc.previewLocale reply). */
static bool locales_state(de_text_system *text, de_ep_arena *a, de_ep_loc_locales *out) {
    *out = (de_ep_loc_locales){
        .active = {"", 0}, .selected = {"", 0}, .preview = {"", 0}, .fallback = {"", 0}};
    if (text == NULL)
        return true;
    de_loc_locale_state *st = (de_loc_locale_state *)de__ed_alloc(a, sizeof *st);
    if (st == NULL)
        return false;
    u32 count = 0;
    if (de_loc_get_locale_state(text, st) != DE_OK ||
        de_loc_list_locales(text, NULL, 0, &count) != DE_OK) {
        de_error_clear();
        return true;
    }
    out->active = de_ep_cstr(st->active);
    out->selected = de_ep_cstr(st->selected);
    out->preview = de_ep_cstr(st->preview);
    out->fallback = de_ep_cstr(st->fallback);
    out->generation = st->generation;
    out->previewing = st->previewing;
    if (count == 0)
        return true;
    de_loc_locale_info *infos = (de_loc_locale_info *)de__ed_alloc(a, sizeof *infos * count);
    de_ep_loc_locale_info *items =
        (de_ep_loc_locale_info *)de__ed_alloc(a, sizeof *items * count);
    if (infos == NULL || items == NULL)
        return false;
    u32 total = 0;
    if (de_loc_list_locales(text, infos, count, &total) != DE_OK) {
        de_error_clear();
        return true;
    }
    const u32 n = total < count ? total : count;
    for (u32 i = 0; i < n; ++i)
        items[i] = (de_ep_loc_locale_info){.locale = de_ep_cstr(infos[i].locale),
                                           .tables = infos[i].tables};
    out->locales = (de_ep_list_loc_locale_info){items, n};
    return true;
}

static void h_loc_locales(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                          mpack_writer_t *w) {
    if (!need_115(ed, req, w, "loc.locales"))
        return;
    DE__ED_PARAMS(loc_locales, de_ep_empty, q);
    (void)q;
    de_ep_loc_locales res;
    if (!locales_state(ed->e->text, a, &res)) {
        ui_busy(w, req->id, "loc.locales");
        return;
    }
    de_ep_write_response_loc_locales(w, req->id, &res);
}

static void h_loc_strings(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                          mpack_writer_t *w) {
    if (!need_115(ed, req, w, "loc.strings"))
        return;
    DE__ED_PARAMS(loc_strings, de_ep_empty, q);
    (void)q;
    de_ep_loc_strings res = {.locale = {"", 0}};
    de_text_system *text = ed->e->text;
    if (text != NULL) {
        de_loc_locale_state *st = (de_loc_locale_state *)de__ed_alloc(a, sizeof *st);
        if (st == NULL) {
            ui_busy(w, req->id, "loc.strings");
            return;
        }
        u32 count = 0;
        de_result r = de_loc_get_locale_state(text, st);
        if (r == DE_OK)
            r = de_loc_read_strings_with_tables(text, NULL, NULL, 0, &count);
        de_loc_string *strs = NULL;
        de_ep_loc_string *items = NULL;
        char *keys = NULL;
        de_asset_id *tables = NULL;
        if (r == DE_OK && count > 0) {
            strs = (de_loc_string *)de__ed_alloc(a, sizeof *strs * count);
            items = (de_ep_loc_string *)de__ed_alloc(a, sizeof *items * count);
            keys = (char *)de__ed_alloc(a, 17u * count);
            tables = (de_asset_id *)de__ed_alloc(a, sizeof *tables * count);
            if (strs == NULL || items == NULL || keys == NULL || tables == NULL) {
                ui_busy(w, req->id, "loc.strings");
                return;
            }
            u32 total = 0;
            r = de_loc_read_strings_with_tables(text, strs, tables, count, &total);
            count = total < count ? total : count;
        }
        if (r != DE_OK) {
            de_error_clear();
            de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "loc.strings: cannot read the tables");
            return;
        }
        for (u32 i = 0; i < count; ++i) {
            char *key = keys + 17u * i;
            snprintf(key, 17, "%016llx", (unsigned long long)strs[i].key.hash);
            items[i] = (de_ep_loc_string){
                .key = de_ep_cstr(key),
                .has_name = strs[i].key_name != NULL,
                .name = de_ep_cstr(strs[i].key_name ? strs[i].key_name : ""),
                .value = {strs[i].value.ptr ? strs[i].value.ptr : "",
                          (uint32_t)strs[i].value.len},
                .locale = de_ep_cstr(strs[i].locale ? strs[i].locale : ""),
                .table = tables[i].hash};
        }
        res.locale = de_ep_cstr(st->active);
        res.generation = st->generation;
        res.strings = (de_ep_list_loc_string){items, count};
    }
    de_ep_write_response_loc_strings(w, req->id, &res);
}

static void h_loc_preview_locale(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                                 mpack_writer_t *w) {
    if (!need_115(ed, req, w, "loc.previewLocale"))
        return;
    bool has = false;
    de_ep_str tag;
    if (!opt_str_param(req, "locale", &has, &tag)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "loc.previewLocale: bad params");
        return;
    }
    char locale[DE_LOC_LOCALE_MAX];
    if (has && !to_cstr(tag, locale, sizeof locale)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "loc.previewLocale: not a locale identifier");
        return;
    }
    de_text_system *text = ed->e->text;
    if (text == NULL && has) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "loc.previewLocale: no text system");
        return;
    }
    if (text != NULL) {
        const de_result r = de_loc_preview_locale(text, has ? locale : NULL);
        if (r != DE_OK) {
            de_error_clear();
            de__ed_error(w, req->id,
                         r == DE_ERR_INVALID_ARGUMENT ? DE_EP_ERR_INVALID_PARAMS
                                                      : DE_EP_ERR_INTERNAL,
                         "loc.previewLocale: \"%s\"", has ? locale : "(end)");
            return;
        }
    }
    de_ep_loc_locales res;
    if (!locales_state(text, a, &res)) {
        ui_busy(w, req->id, "loc.previewLocale");
        return;
    }
    de_ep_write_response_loc_preview_locale(w, req->id, &res);
}

/* ---------- Protocol 1.16 (DENG-787) ---------- */

static bool need_116(de__editor *ed, const de_ep_request *req, mpack_writer_t *w,
                     const char *method) {
    if (de__ed_protocol_at_least(ed, 1, 16))
        return true;
    de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "%s: needs protocol 1.16 (agreed %u.%u)",
                 method, (unsigned)ed->proto_major, (unsigned)ed->proto_minor);
    return false;
}

/* The copy of `s` in the reply arena, "" when it does not fit. */
static de_ep_str arena_str(de_ep_arena *a, const char *s) {
    const size_t n = strlen(s);
    char *buf = (char *)de__ed_alloc(a, n + 1);
    if (buf == NULL)
        return de_ep_cstr("");
    memcpy(buf, s, n + 1);
    return de_ep_cstr(buf);
}

static void h_ui_stylesheets(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                             mpack_writer_t *w) {
    if (!need_116(ed, req, w, "ui.stylesheets"))
        return;
    DE__ED_PARAMS(ui_stylesheets, de_ep_empty, q);
    (void)q;
    de_ep_ui_stylesheets res = {.suffix = de_ep_cstr(DE_UI_STYLESHEET_SUFFIX)};
    const de_ui *ui = ed->e ? ed->e->ui : NULL;
    u32 count = 0;
    if (ui != NULL && de_ui_list_sheets(ui, NULL, 0, &count) != DE_OK) {
        de_error_clear();
        count = 0;
    }
    if (count > 0) {
        de_ui_sheet_status *st = (de_ui_sheet_status *)de__ed_alloc(a, sizeof *st * count);
        de_ep_ui_sheet *items = (de_ep_ui_sheet *)de__ed_alloc(a, sizeof *items * count);
        u32 total = 0;
        if (st == NULL || items == NULL || de_ui_list_sheets(ui, st, count, &total) != DE_OK ||
            total != count) {
            de_error_clear();
            ui_busy(w, req->id, "ui.stylesheets");
            return;
        }
        for (u32 i = 0; i < count; ++i) {
            const de_ui_sheet_status *s = &st[i];
            const char *path = s->info.path ? s->info.path : "";
            const char *source = s->source ? s->source : "";
            char hex[24] = "";
            /* The asset id is the manifest id, which is the path; a sheet the manifest does not
             * list has the 16-digit hex of its asset id; one from source text has none. */
            if (path[0] == 0 && s->info.asset.hash)
                snprintf(hex, sizeof hex, "%016llx", (unsigned long long)s->info.asset.hash);
            items[i] = (de_ep_ui_sheet){.index = s->index,
                                        .path = arena_str(a, path),
                                        .asset = arena_str(a, path[0] ? path : hex),
                                        .source = arena_str(a, source),
                                        .builtin = s->info.builtin,
                                        .loaded = s->loaded};
            if (s->has_error) {
                items[i].has_error = true;
                items[i].error = (de_ep_ui_sheet_error){
                    .file = arena_str(a, source),
                    .line = s->error.line,
                    .column = s->error.line ? s->error.column : 0,
                    .message = arena_str(a, s->error.message)};
            }
        }
        res.sheets = (de_ep_list_ui_sheet){items, count};
    }
    de_ep_write_response_ui_stylesheets(w, req->id, &res);
}

static void h_ui_check_stylesheet(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                                  mpack_writer_t *w) {
    if (!need_116(ed, req, w, "ui.checkStylesheet"))
        return;
    DE__ED_PARAMS(ui_check_stylesheet, de_ep_ui_check_stylesheet_params, q);
    de_ep_ui_check_stylesheet res = {.ok = true};
    de_ui_sheet_error error;
    memset(&error, 0, sizeof error);
    if (q.text.len > 0) {
        const de_result r = de_ui_check_stylesheet(q.text.ptr, (u64)q.text.len, &error);
        if (r == DE_ERR_PARSE) {
            res.ok = false;
            res.has_error = true;
            res.error = (de_ep_ui_check_error){.line = error.line,
                                               .column = error.line ? error.column : 0,
                                               .message = arena_str(a, error.message)};
        } else if (r != DE_OK) {
            de_error_clear();
            de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "ui.checkStylesheet: check failed");
            return;
        }
    }
    de_ep_write_response_ui_check_stylesheet(w, req->id, &res);
}

/* ---------- Protocol 1.17 (DENG-788) ---------- */

static bool need_117(de__editor *ed, const de_ep_request *req, mpack_writer_t *w,
                     const char *method) {
    if (de__ed_protocol_at_least(ed, 1, 17))
        return true;
    de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "%s: needs protocol 1.17 (agreed %u.%u)",
                 method, (unsigned)ed->proto_major, (unsigned)ed->proto_minor);
    return false;
}

/* The READY font asset named by `s`: a manifest path, or the 16-digit hex of a manifest id. */
static bool font_ready(de_asset_system *assets, de_ep_str s, de_font *out) {
    if (assets == NULL || s.ptr == NULL || s.len == 0 || s.len >= 1024 || memchr(s.ptr, 0, s.len))
        return false;
    char path[1024];
    memcpy(path, s.ptr, s.len);
    path[s.len] = 0;
    de_asset_id id = {0};
    bool hex = s.len == 16;
    for (size_t i = 0; hex && i < 16; ++i)
        hex = (path[i] >= '0' && path[i] <= '9') || (path[i] >= 'a' && path[i] <= 'f');
    if (hex && de_asset_id_path(assets, (de_asset_id){strtoull(path, NULL, 16)}) != NULL)
        id.hash = strtoull(path, NULL, 16);
    else
        id = de_asset_id_from(path);
    de_error_clear();
    if (id.hash == 0 || de_asset_status_of(assets, id) != DE_LOAD_READY)
        return false;
    *out = de_asset_font(assets, id);
    de_error_clear();
    return out->id != 0;
}

static de_text_align text_align(de_ep_ui_text_align a) {
    switch (a) {
    case DE_EP_UI_TEXT_ALIGN_CENTER:
        return DE_ALIGN_CENTER;
    case DE_EP_UI_TEXT_ALIGN_END:
        return DE_ALIGN_END;
    case DE_EP_UI_TEXT_ALIGN_JUSTIFY:
        return DE_ALIGN_JUSTIFY;
    default:
        return DE_ALIGN_START;
    }
}

/* An absent optional number is fine; a present one must be finite, and non-negative unless
 * `signed_ok`. */
static bool num_ok(bool has, f32 v, bool signed_ok) {
    return !has || (isfinite(v) && (signed_ok || v >= 0));
}

static void h_ui_measure_text(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                              mpack_writer_t *w) {
    if (!need_117(ed, req, w, "ui.measureText"))
        return;
    de_ui *ui = ed->e ? ed->e->ui : NULL;
    if (ui == NULL || ed->e->text == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "ui.measureText: the game has no UI text");
        return;
    }
    DE__ED_PARAMS(ui_measure_text, de_ep_ui_measure_text_params, q);
    const de_ep_ui_text_style *st = &q.style;
    if (!num_ok(q.has_wrap_width, q.wrap_width, false) ||
        !num_ok(q.has_box_height, q.box_height, false) || !num_ok(q.has_scale, q.scale, false) ||
        !num_ok(st->has_size_px, st->size_px, true) ||
        !num_ok(st->has_letter_spacing, st->letter_spacing, true) ||
        !num_ok(st->has_line_height, st->line_height, false)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                     "ui.measureText: a size, spacing, line height, wrapWidth, boxHeight or scale "
                     "is negative or not finite");
        return;
    }
    de_text_style style = {0};
    if (!font_ready(ed->e->assets, st->font, &style.font)) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "ui.measureText: the font is not loaded");
        return;
    }
    style.size_px = st->has_size_px ? st->size_px : 0; /* 0 or less: 16 */
    style.letter_spacing = st->has_letter_spacing ? st->letter_spacing : 0;
    if (st->has_language && st->language.len > 0) {
        char *lang = (char *)de__ed_alloc(a, (size_t)st->language.len + 1);
        if (lang == NULL) {
            ui_busy(w, req->id, "ui.measureText");
            return;
        }
        memcpy(lang, st->language.ptr, st->language.len);
        lang[st->language.len] = 0;
        style.language = lang;
    }
    const f32 line_height = st->has_line_height ? st->line_height : 0;
    const de_ui_text_measure_desc desc = {.utf8 = {q.text.ptr ? q.text.ptr : "", q.text.len},
                                          .style = &style,
                                          .align = q.has_align ? text_align(q.align) : DE_ALIGN_START,
                                          .wrap_width = q.has_wrap_width ? q.wrap_width : 0,
                                          .box_height = q.has_box_height ? q.box_height : 0,
                                          .scale = q.has_scale ? q.scale : 0};
    u32 capacity = 64;
    de_text_line *lines = (de_text_line *)de__ed_alloc(a, sizeof *lines * capacity);
    if (lines == NULL) {
        ui_busy(w, req->id, "ui.measureText");
        return;
    }
    de_ui_text_metrics m;
    memset(&m, 0, sizeof m);
    de_result r = de_ui_measure_text(ui, &desc, lines, capacity, &m);
    if (r == DE_OK && line_height > 0 && m.line_count > 0 && m.line_height > 0) {
        /* lineHeight is in layout pixels; the engine takes a factor of the font's own, which
         * this first measure (factor 0) reported. */
        style.line_height = line_height / m.line_height;
        r = de_ui_measure_text(ui, &desc, lines, capacity, &m);
    }
    if (r == DE_OK && m.line_count > capacity) {
        capacity = m.line_count;
        lines = (de_text_line *)de__ed_alloc(a, sizeof *lines * capacity);
        if (lines == NULL) {
            ui_busy(w, req->id, "ui.measureText");
            return;
        }
        r = de_ui_measure_text(ui, &desc, lines, capacity, &m);
    }
    if (r != DE_OK) {
        de_error_clear();
        /* The arguments were checked above, so what is left is a UI with no text system. */
        de__ed_error(w, req->id, r == DE_ERR_INVALID_ARGUMENT ? DE_EP_ERR_UNSUPPORTED : DE_EP_ERR_INTERNAL,
                     "ui.measureText: the text could not be measured");
        return;
    }
    const u32 count = m.line_count;
    de_ep_ui_text_line *items =
        count ? (de_ep_ui_text_line *)de__ed_alloc(a, sizeof *items * count) : NULL;
    float *size = (float *)de__ed_alloc(a, sizeof(float) * 2u);
    if ((count && items == NULL) || size == NULL) {
        ui_busy(w, req->id, "ui.measureText");
        return;
    }
    for (u32 i = 0; i < count; ++i)
        items[i] = (de_ep_ui_text_line){.start = lines[i].byte_start,
                                        .end = lines[i].byte_end,
                                        .x = lines[i].x,
                                        .y = lines[i].y,
                                        .width = lines[i].width,
                                        .height = lines[i].height};
    size[0] = m.size.x;
    size[1] = m.size.y;
    de_ep_ui_measure_text res = {.size = {size, 2},
                                 .scale = m.scale,
                                 .line_height = m.line_height,
                                 .lines = {items, count},
                                 .overflow_x = m.overflow_x,
                                 .overflow_y = m.overflow_y};
    de_ep_write_response_ui_measure_text(w, req->id, &res);
}

/* A disconnect ends the previews the IDE started; with none active both calls do nothing. */
static void ui_reset(de__editor *ed) {
    if (ed->e == NULL)
        return;
    if (ed->e->ui != NULL && de_ui_preview_theme(ed->e->ui, NULL) != DE_OK)
        de_error_clear();
    if (ed->e->text != NULL && de_loc_preview_locale(ed->e->text, NULL) != DE_OK)
        de_error_clear();
}

void de__ed_ui_register(de__editor *ed) {
    de__ed_add_capability(ed, "ui.editor");
    de__ed_add_capability(ed, "ui.stylesheets");
    if (ed->e != NULL && ed->e->ui != NULL && ed->e->text != NULL)
        de__ed_add_capability(ed, "ui.textMetrics"); /* needs a text system */
    de__ed_on(ed, DE_EP_METHOD_UI_LAYOUT, h_ui_layout);
    de__ed_on(ed, DE_EP_METHOD_UI_NODE_CORNERS, h_ui_node_corners);
    de__ed_on(ed, DE_EP_METHOD_UI_THEMES, h_ui_themes);
    de__ed_on(ed, DE_EP_METHOD_UI_THEME_VARS, h_ui_theme_vars);
    de__ed_on(ed, DE_EP_METHOD_UI_PREVIEW_THEME, h_ui_preview_theme);
    de__ed_on(ed, DE_EP_METHOD_UI_STYLESHEETS, h_ui_stylesheets);
    de__ed_on(ed, DE_EP_METHOD_UI_CHECK_STYLESHEET, h_ui_check_stylesheet);
    de__ed_on(ed, DE_EP_METHOD_UI_MEASURE_TEXT, h_ui_measure_text);
    de__ed_on(ed, DE_EP_METHOD_LOC_LOCALES, h_loc_locales);
    de__ed_on(ed, DE_EP_METHOD_LOC_STRINGS, h_loc_strings);
    de__ed_on(ed, DE_EP_METHOD_LOC_PREVIEW_LOCALE, h_loc_preview_locale);
    de__ed_on_reset(ed, ui_reset);
}
