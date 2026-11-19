/* editor_ui.c: ui.layout (DENG-778; DIDE protocol 1.14, capability ui.editor).
 *
 * The UI editor asks for the rects of a UI subtree at a preview viewport (a device size, a UI
 * scale, safe-area insets, pixel art) and draws its overlay from the engine's own layout, through
 * de_ui_layout_preview. The live layout is unchanged afterwards. The rects are the root, then its
 * subtree in pre-order with the children in sibling order, so the order the IDE sets with
 * EditOp.index (editor_edit.c) is the order it reads back here.
 *
 * Not here (proposed for protocol 1.15): the pick stack, themes and locales. The engine has the C
 * API for them (de_ui_pick, de_ui_preview_theme, de_loc_preview_locale) but the 1.14 IDL
 * does not define their methods or fields.
 */
#include "host/editor/editor_internal.h"

#include "host/host_internal.h"
#include <dengine/de_ui.h>
#include <math.h>
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

void de__ed_ui_register(de__editor *ed) {
    de__ed_add_capability(ed, "ui.editor");
    de__ed_on(ed, DE_EP_METHOD_UI_LAYOUT, h_ui_layout);
}
