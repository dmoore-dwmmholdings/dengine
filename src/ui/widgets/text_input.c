#include "advanced_internal.h"
#include <math.h>
#include "ui/world/world_internal.h"
static f32 input_scale(de_ui *ui, de_entity e) {
    de_entity root = e;
    for (u32 i = 0; i < ui->capacity; ++i) {
        de_entity parent = de_entity_parent(ui->world, root);
        if (!parent.bits)
            break;
        root = parent;
    }
    de_vec4 extent;
    return de_ui__world_root_extent(ui, root, &extent) ? 1 : ui->scale;
}
static u32 bounded(const char *s, u32 cap) {
    u32 n = 0;
    while (n < cap && s[n])
        ++n;
    return n;
}
static u32 composition_offset(const char *text, u32 bytes, i32 cursor) {
    u32 at = 0;
    /* Input reports code points, while text layouts consume UTF-8 byte offsets.
     * Unknown cursors stay at the preedit start; oversized cursors clamp to its end.
     * Do not snap to graphemes: an IME may place its cursor within a combining sequence. */
    while (cursor > 0 && at < bytes) {
        ++at;
        while (at < bytes && ((u8)text[at] & 0xc0) == 0x80)
            ++at;
        --cursor;
    }
    return at;
}
static u32 snap(const DeUiTextInput *in, de_ui_editor *state, u32 byte) {
    u32 small[257];
    const u32 *b;
    u32 n = de_ui__input_boundaries(in, state, small, &b), last = 0;
    if (!n)
        return UINT32_MAX;
    for (u32 i = 0; i < n && b[i] <= byte; ++i)
        last = b[i];
    return last;
}
bool de_ui__input_replace(DeUiTextInput *in, de_ui_editor *state, de_str text) {
    u32 lo = in->caret < state->anchor ? in->caret : state->anchor,
        hi = in->caret > state->anchor ? in->caret : state->anchor;
    lo = snap(in, state, lo);
    hi = snap(in, state, hi);
    if (lo == UINT32_MAX || hi == UINT32_MAX)
        return false;
    u32 room = (state->storage ? 0x7ffffffeu : 255u) - (in->length - (hi - lo));
    u32 n = text.len < room ? (u32)text.len : room;
    while (n < text.len && n && ((u8)text.ptr[n] & 0xc0) == 0x80)
        --n;
    if (!n && lo == hi)
        return false;
    u32 length = in->length - (hi - lo) + n;
    const char *before = de_ui__input_data(in, state);
    bool was_inline = before == in->buffer;
    if (length > 255 && !de_ui__input_reserve(state, length + 1))
        return false;
    if (was_inline && de_ui__input_data(in, state) != in->buffer)
        memcpy((char *)de_ui__input_data(in, state), in->buffer, in->length + 1);
    if (!de_ui__input_history(in, state, lo, hi, (de_str){text.ptr, n}))
        return false;
    char *data = (char *)de_ui__input_data(in, state);
    memmove(data + lo + n, data + hi, in->length - hi);
    if (n)
        memcpy(data + lo, text.ptr, n);
    in->length = length;
    data[length] = 0;
    in->caret = state->anchor = lo + n;
    de_ui__input_changed(state);
    return true;
}
bool de_ui_text_input_apply(de_ui *ui, de_entity e, const de_ui_text_edit *edit) {
    if (!ui || !ui->world || !ui->advanced || !edit)
        return false;
    DeUiTextInput *in = de_entity_get(ui->world, e, ui->advanced->input);
    de_ui_editor *s = de_entity_get(ui->world, e, ui->advanced->editor);
    if (!in || !s)
        return false;
    if (edit->kind == DE_UI_TEXT_UNDO)
        return de_ui_text_input_undo(ui, e);
    if (edit->kind == DE_UI_TEXT_REDO)
        return de_ui_text_input_redo(ui, e);
    if (edit->kind == DE_UI_TEXT_SELECT) {
        u32 caret = snap(in, s, edit->caret > in->length ? in->length : edit->caret);
        u32 anchor = snap(in, s, edit->anchor > in->length ? in->length : edit->anchor);
        if (caret == UINT32_MAX || anchor == UINT32_MAX)
            return false;
        in->caret = caret;
        s->anchor = anchor;
        s->preferred_valid = false;
        s->blink = 0;
        de_ui__input_notify(
            ui, e,
            (de_ui_text_edit){.kind = DE_UI_TEXT_SELECT, .caret = in->caret, .anchor = s->anchor});
        de_ui__input_refresh(ui, e);
        return true;
    }
    if (edit->kind != DE_UI_TEXT_REPLACE || (!edit->text.ptr && edit->text.len) ||
        (edit->text.len && memchr(edit->text.ptr, 0, edit->text.len)))
        return false;
    u32 caret = in->caret, anchor = s->anchor;
    if (edit->text.len > 0x7ffffffeu)
        return false;
    char *copy = DE_ALLOC(ui->alloc, edit->text.len + 1, 1, DE_MEM_UI);
    if (!copy)
        return false;
    if (edit->text.len)
        memcpy(copy, edit->text.ptr, edit->text.len);
    copy[edit->text.len] = 0;
    de_ui_text_edit committed = *edit;
    committed.text.ptr = copy;
    if (!de_ui__input_replace(in, s, committed.text)) {
        DE_FREE(ui->alloc, copy, edit->text.len + 1);
        return false;
    }
    de_ui__input_notify(
        ui, e, (de_ui_text_edit){.kind = DE_UI_TEXT_SELECT, .caret = caret, .anchor = anchor});
    de_ui__input_notify(ui, e, committed);
    DE_FREE(ui->alloc, copy, edit->text.len + 1);
    de_ui__input_refresh(ui, e);
    de_ui__widget_event(ui, DE_UI_EV_CHANGED, e, (de_name){0}, (f32)in->length);
    return true;
}
static bool replace(de_ui *ui, de_entity e, DeUiTextInput *in, de_ui_editor *s, de_str text) {
    u32 caret = in->caret, anchor = s->anchor;
    if (!de_ui__input_replace(in, s, text))
        return false;
    de_ui__input_notify(
        ui, e, (de_ui_text_edit){.kind = DE_UI_TEXT_SELECT, .caret = caret, .anchor = anchor});
    de_ui__input_notify(ui, e, (de_ui_text_edit){.kind = DE_UI_TEXT_REPLACE, .text = text});
    return true;
}
static bool space(const char *s, u32 byte) {
    u8 c = (u8)s[byte];
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
void de_ui__input_move(DeUiTextInput *in, de_ui_editor *state, i32 dir, bool extend, bool word) {
    u32 small[257];
    const u32 *b;
    u32 n = de_ui__input_boundaries(in, state, small, &b), i = 0;
    if (!n)
        return;
    while (i + 1 < n && b[i] < in->caret)
        ++i;
    if (!extend && state->anchor != in->caret && !word)
        in->caret = dir < 0 ? (in->caret < state->anchor ? in->caret : state->anchor)
                            : (in->caret > state->anchor ? in->caret : state->anchor);
    else if (dir < 0) {
        if (i)
            --i;
        if (word) {
            while (i && space(de_ui__input_data(in, state), b[i]))
                --i;
            while (i && !space(de_ui__input_data(in, state), b[i - 1]))
                --i;
        }
        in->caret = b[i];
    } else {
        if (i + 1 < n)
            ++i;
        if (word) {
            while (i + 1 < n && !space(de_ui__input_data(in, state), b[i]))
                ++i;
            while (i + 1 < n && space(de_ui__input_data(in, state), b[i]))
                ++i;
        }
        in->caret = b[i];
    }
    if (!extend)
        state->anchor = in->caret;
    state->blink = 0;
    state->preferred_valid = false;
}
static de_text_layout *layout(de_ui *ui, de_entity e, const char *s, u32 n, f32 width) {
    const DeUiText *t = de_entity_get_const(ui->world, e, ui->ids.text);
    if (!t || !ui->desc.text)
        return NULL;
    de_text_style style = t->style;
    style.size_px *= input_scale(ui, e);
    style.letter_spacing *= input_scale(ui, e);
    de_text_layout *l = NULL;
    de_text_layout_build(
        ui->desc.text, &(de_text_layout_desc){.utf8 = {s, n}, .style = &style, .max_width = width},
        ui->alloc, &l);
    return l;
}
void de_ui__input_refresh(de_ui *ui, de_entity e) {
    de_ui_advanced *a = ui->advanced;
    DeUiTextInput *in = de_entity_get(ui->world, e, a->input);
    de_ui_editor *s = de_entity_get(ui->world, e, a->editor);
    if (!in || !s)
        return;
    de_text_input_state typed = {0};
    if (a->active_input.bits == e.bits && ui->desc.input)
        de_text_input_get(ui->desc.input, &typed);
    u32 small[257];
    const u32 *b;
    u32 used = 0, n = de_ui__input_boundaries(in, s, small, &b);
    if (!n)
        return;
    u64 required = (in->password ? (u64)n * 3 : in->length) + 1024;
    if (required > 0xffffffffu)
        return;
    char *display = de_ui__input_display_reserve(s, (u32)required);
    if (!display)
        return;
    s->preedit_start = s->preedit_end = 0;
    for (u32 i = 0; i < n; ++i) {
        if (b[i] == in->caret) {
            s->displayed_caret = used;
            s->preedit_start = used;
            if (typed.composition) {
                u32 count = bounded(typed.composition, 1023);
                s->displayed_caret +=
                    composition_offset(typed.composition, count, typed.composition_cursor);
                memcpy(display + used, typed.composition, count);
                used += count;
            }
            s->preedit_end = used;
        }
        if (i + 1 == n)
            break;
        if (in->password) {
            memcpy(display + used, "\xe2\x80\xa2", 3);
            used += 3;
        } else {
            u32 count = b[i + 1] - b[i];
            memcpy(display + used, de_ui__input_data(in, s) + b[i], count);
            used += count;
        }
    }
    display[used] = 0;
    DeUiText *t = de_entity_get(ui->world, e, ui->ids.text);
    if (t)
        t->literal = (de_str){display, used};
    /* The ECS may move the editor storage. prepare refreshes this borrowed view every frame. */
}
void de_ui__input_prepare(de_ui *ui, de_entity e) {
    de_ui_advanced *a = ui->advanced;
    if (!de_entity_has(ui->world, e, a->editor))
        de_entity_add(ui->world, e, a->editor);
    if (!de_entity_has(ui->world, e, ui->ids.text))
        de_entity_add(ui->world, e, ui->ids.text);
    if (!de_entity_has(ui->world, e, ui->ids.state))
        de_entity_add(ui->world, e, ui->ids.state);
    de_ui__advanced_tag(ui, e, DE_UI_WIDGET_INPUT);
    if (!de_entity_has(ui->world, e, ui->widgets->focusable))
        de_entity_add(ui->world, e, ui->widgets->focusable);
    DeUiNode *node = de_entity_get(ui->world, e, ui->ids.node);
    node->clip = true;
    DeUiTextInput *in = de_entity_get(ui->world, e, a->input);
    de_ui_editor *s = de_entity_get(ui->world, e, a->editor);
    de_ui__document_prepare(ui, e);
    if (de_ui__input_data(in, s) == in->buffer) {
        in->length = bounded(in->buffer, 255);
        in->buffer[in->length] = 0;
    }
    u32 caret = snap(in, s, in->caret > in->length ? in->length : in->caret);
    if (caret != UINT32_MAX)
        in->caret = caret;
    if (!s->initialized) {
        s->anchor = in->caret;
        s->initialized = true;
    }
    u32 anchor = snap(in, s, s->anchor > in->length ? in->length : s->anchor);
    if (anchor != UINT32_MAX)
        s->anchor = anchor;
    de_ui__input_refresh(ui, e);
}
void de_ui__input_submit(de_ui *ui, de_entity e) {
    de_ui_advanced *a = ui->advanced;
    de_ui_widget_table *table = de_ui__widget_table(ui);
    de_ui_editor *editor = de_entity_get(ui->world, e, a->editor);
    if (editor)
        editor->suspended = false;
    if (table->request_focus)
        table->request_focus(ui, e);
    if (a->active_input.bits != e.bits) {
        if (a->active_input.bits) {
            DeUiInteractionState *old = de_entity_get(ui->world, a->active_input, ui->ids.state);
            if (old)
                old->focused = false;
        }
        a->active_input = e;
        if (ui->desc.input && a->edit_map.id && !a->edit_pushed) {
            de_action_map_push(ui->desc.input, a->edit_map);
            a->edit_pushed = true;
        }
        DeUiInteractionState *s = de_entity_add(ui->world, e, ui->ids.state);
        if (s)
            s->focused = true;
        if (ui->desc.input)
            de_text_input_start(ui->desc.input, NULL);
    }
}
void de_ui__input_cancel(de_ui *ui, de_entity e) {
    if (ui->advanced->active_input.bits != e.bits)
        return;
    de_ui_editor *editor = de_entity_get(ui->world, e, ui->advanced->editor);
    if (editor)
        editor->suspended = true;
    de_ui_widget_table *table = de_ui__widget_table(ui);
    if (table->focused && table->request_focus && table->focused(ui).bits == e.bits)
        table->request_focus(ui, (de_entity){0});
    if (ui->advanced->edit_pushed && ui->desc.input)
        de_action_map_pop(ui->desc.input, ui->advanced->edit_map);
    ui->advanced->edit_pushed = false;
    ui->advanced->active_input = (de_entity){0};
    DeUiInteractionState *s = de_entity_get(ui->world, e, ui->ids.state);
    if (s)
        s->focused = false;
    if (ui->desc.input)
        de_text_input_stop(ui->desc.input);
}
void de_ui__input_geometry(de_ui *ui, de_entity e, de_vec2 *offset, f32 *width, void *user) {
    (void)user;
    de_ui_advanced *a = ui->advanced;
    if (!a || !de_entity_alive(ui->world, e))
        return;
    const DeUiTextInput *in = de_entity_get_const(ui->world, e, a->input);
    const de_ui_editor *s = de_entity_get_const(ui->world, e, a->editor);
    if (!in || !s)
        return;
    offset->x = 4 * input_scale(ui, e) - s->scroll_x;
    offset->y = 4 * input_scale(ui, e) - s->scroll_y;
    *width = in->multiline ? fmaxf(1, *width - 8 * input_scale(ui, e)) : 0;
}
static u32 display_offset(const DeUiTextInput *in, de_ui_editor *s, u32 byte) {
    if (!in->password)
        return byte;
    u32 small[257];
    const u32 *b;
    u32 n = de_ui__input_boundaries(in, s, small, &b), i = 0;
    while (i + 1 < n && b[i] < byte)
        ++i;
    return i * 3;
}
static void selection_word(DeUiTextInput *in, de_ui_editor *s) {
    u32 small[257];
    const u32 *b;
    u32 n = de_ui__input_boundaries(in, s, small, &b), i = 0;
    if (!n)
        return;
    while (i + 1 < n && b[i] < in->caret)
        ++i;
    u32 lo = i, hi = i;
    while (lo && !space(de_ui__input_data(in, s), b[lo - 1]))
        --lo;
    while (hi + 1 < n && !space(de_ui__input_data(in, s), b[hi]))
        ++hi;
    s->anchor = b[lo];
    in->caret = b[hi];
}
void de_ui__input_frame(de_ui *ui, de_entity e, f32 dt) {
    de_ui_advanced *a = ui->advanced;
    de_input *input = ui->desc.input;
    if (!input)
        return;
    if (!de_ui__widget_enabled(ui, e)) {
        de_ui__input_cancel(ui, e);
        return;
    }
    if (!de_entity_has(ui->world, e, a->editor))
        return;
    de_ui_widget_table *table = de_ui__widget_table(ui);
    de_ui_pointer_state *p = &ui->widgets->pointer;
    bool clicked = de_mouse_pressed(input, DE_MOUSE_LEFT), hit = p->hovered.bits == e.bits;
    if (clicked && hit)
        de_ui__input_submit(ui, e);
    if (table->focused) {
        de_entity focus = table->focused(ui);
        de_ui_editor *editor = de_entity_get(ui->world, e, a->editor);
        if (focus.bits != e.bits)
            editor->suspended = false;
        if (focus.bits == e.bits && !editor->suspended && a->active_input.bits != e.bits)
            de_ui__input_submit(ui, e);
        else if (focus.bits != e.bits && a->active_input.bits == e.bits)
            de_ui__input_cancel(ui, e);
    } else if (clicked && !hit && a->active_input.bits == e.bits)
        de_ui__input_cancel(ui, e);
    if (a->active_input.bits != e.bits)
        return;
    DeUiTextInput *in = de_entity_get(ui->world, e, a->input);
    de_ui_editor *s = de_entity_get(ui->world, e, a->editor);
    const DeUiNode *node = de_entity_get_const(ui->world, e, ui->ids.node);
    if (!node)
        return;
    u32 old_caret = in->caret, old_anchor = s->anchor;
    f32 width = in->multiline ? fmaxf(1, node->computed_rect.z - 8 * input_scale(ui, e)) : 0;
    s->blink += isfinite(dt) && dt > 0 ? dt : 0;
    bool shift = de_key_down(input, DE_KEY_LSHIFT) || de_key_down(input, DE_KEY_RSHIFT);
    bool ctrl = de_key_down(input, DE_KEY_LCTRL) || de_key_down(input, DE_KEY_RCTRL) ||
                de_key_down(input, DE_KEY_LGUI) || de_key_down(input, DE_KEY_RGUI);
    bool word = ctrl || de_key_down(input, DE_KEY_LALT) || de_key_down(input, DE_KEY_RALT);
    if (clicked && hit) {
        s->dragging = true;
        s->preferred_valid = false;
    }
    if (s->dragging && de_mouse_down(input, DE_MOUSE_LEFT)) {
        de_text_layout *l = layout(ui, e, de_ui__input_data(in, s), in->length, width);
        if (l) {
            de_vec2 physical = p->position;
            if (p->root.bits) {
                const DeUiNode *root = de_entity_get_const(ui->world, p->root, ui->ids.node);
                if (root) {
                    physical.x += root->computed_rect.x;
                    physical.y += root->computed_rect.y;
                }
            }
            de_vec2 point = {
                physical.x - node->computed_rect.x - 4 * input_scale(ui, e) + s->scroll_x,
                physical.y - node->computed_rect.y - 4 * input_scale(ui, e) + s->scroll_y};
            u32 byte = de_text_layout_hit_test(l, point);
            de_text_layout_free(l);
            u32 caret = snap(in, s, byte);
            if (caret != UINT32_MAX)
                in->caret = caret;
            if (clicked && !shift)
                s->anchor = in->caret;
            if (clicked && s->last_click > 0 && a->time - s->last_click < .35f)
                selection_word(in, s);
            if (clicked)
                s->last_click = a->time;
        }
    }
    if (!de_mouse_down(input, DE_MOUSE_LEFT))
        s->dragging = false;
    if (de_key_pressed(input, DE_KEY_LEFT))
        de_ui__input_move(in, s, -1, shift, word);
    if (de_key_pressed(input, DE_KEY_RIGHT))
        de_ui__input_move(in, s, 1, shift, word);
    if (in->multiline && (de_key_pressed(input, DE_KEY_UP) || de_key_pressed(input, DE_KEY_DOWN))) {
        de_text_layout *l = layout(ui, e, de_ui__input_data(in, s), in->length, width);
        if (l) {
            de_vec4 c = de_text_layout_caret(l, in->caret);
            if (!s->preferred_valid) {
                s->preferred_x = c.x;
                s->preferred_valid = true;
            }
            f32 y = c.y + c.w * (de_key_pressed(input, DE_KEY_UP) ? -.5f : 1.5f);
            u32 caret = snap(in, s, de_text_layout_hit_test(l, (de_vec2){s->preferred_x, y}));
            if (caret != UINT32_MAX)
                in->caret = caret;
            if (!shift)
                s->anchor = in->caret;
            s->blink = 0;
            de_text_layout_free(l);
        }
    }
    if (de_key_pressed(input, DE_KEY_HOME)) {
        s->preferred_valid = false;
        s->blink = 0;
        u32 byte = in->caret;
        if (ctrl || !in->multiline)
            byte = 0;
        else
            while (byte && de_ui__input_data(in, s)[byte - 1] != '\n')
                --byte;
        in->caret = byte;
        if (!shift)
            s->anchor = byte;
    }
    if (de_key_pressed(input, DE_KEY_END)) {
        s->preferred_valid = false;
        s->blink = 0;
        u32 byte = in->caret;
        if (ctrl || !in->multiline)
            byte = in->length;
        else
            while (byte < in->length && de_ui__input_data(in, s)[byte] != '\n')
                ++byte;
        in->caret = byte;
        if (!shift)
            s->anchor = byte;
    }
    if (ctrl && de_key_pressed(input, DE_KEY_A)) {
        s->preferred_valid = false;
        s->blink = 0;
        s->anchor = 0;
        in->caret = in->length;
    }
    bool changed = false;
    if (ctrl && (de_key_pressed(input, DE_KEY_C) || de_key_pressed(input, DE_KEY_X)) &&
        !in->password) {
        u32 lo = in->caret < s->anchor ? in->caret : s->anchor,
            hi = in->caret > s->anchor ? in->caret : s->anchor;
        char *copy = DE_ALLOC(ui->alloc, (u64)hi - lo + 1, 1, DE_MEM_UI);
        if (copy) {
            memcpy(copy, de_ui__input_data(in, s) + lo, hi - lo);
            copy[hi - lo] = 0;
            de_result copied = a->clipboard_set(copy);
            DE_FREE(ui->alloc, copy, (u64)hi - lo + 1);
            if (copied == DE_OK && de_key_pressed(input, DE_KEY_X))
                changed |= replace(ui, e, in, s, (de_str){"", 0});
        }
    }
    if (ctrl && de_key_pressed(input, DE_KEY_V)) {
        size_t count = a->clipboard_get(NULL, 0);
        if (count < 0x7ffffffeu) {
            char *paste = DE_ALLOC(ui->alloc, count + 1, 1, DE_MEM_UI);
            if (paste) {
                size_t received = a->clipboard_get(paste, count + 1);
                if (received <= count)
                    changed |= replace(ui, e, in, s, (de_str){paste, received});
                DE_FREE(ui->alloc, paste, count + 1);
            }
        }
    }
    if (ctrl && de_key_pressed(input, DE_KEY_Z)) {
        if (shift)
            (void)de_ui_text_input_redo(ui, e);
        else
            (void)de_ui_text_input_undo(ui, e);
    } else if (ctrl && de_key_pressed(input, DE_KEY_Y))
        (void)de_ui_text_input_redo(ui, e);
    if (de_key_pressed(input, DE_KEY_BACKSPACE) || de_key_pressed(input, DE_KEY_DELETE)) {
        if (s->anchor == in->caret)
            de_ui__input_move(in, s, de_key_pressed(input, DE_KEY_BACKSPACE) ? -1 : 1, true, word);
        changed |= replace(ui, e, in, s, (de_str){"", 0});
    }
    de_text_input_state typed;
    de_text_input_get(input, &typed);
    if (typed.text_bytes)
        changed |= replace(ui, e, in, s, (de_str){typed.text, typed.text_bytes});
    if (de_key_pressed(input, DE_KEY_RETURN)) {
        if (in->multiline)
            changed |= replace(ui, e, in, s, (de_str){"\n", 1});
        else
            de_ui__widget_event(ui, DE_UI_EV_SUBMIT, e, (de_name){0}, 0);
    }
    if (de_key_pressed(input, DE_KEY_ESCAPE))
        de_ui__input_cancel(ui, e);
    if (old_caret != in->caret || old_anchor != s->anchor)
        de_ui__input_notify(
            ui, e,
            (de_ui_text_edit){.kind = DE_UI_TEXT_SELECT, .caret = in->caret, .anchor = s->anchor});
    if (changed)
        de_ui__widget_event(ui, DE_UI_EV_CHANGED, e, (de_name){0}, (f32)in->length);
    de_ui__input_refresh(ui, e);
    de_text_layout *l =
        layout(ui, e, de_ui__input_display(s), (u32)strlen(de_ui__input_display(s)), width);
    if (l) {
        de_vec4 c = de_text_layout_caret(l, s->displayed_caret);
        if (in->multiline) {
            f32 room = fmaxf(1, node->computed_rect.w - 8 * input_scale(ui, e));
            if (c.y - s->scroll_y < 0)
                s->scroll_y = c.y;
            if (c.y + c.w - s->scroll_y > room)
                s->scroll_y = c.y + c.w - room;
            s->scroll_y = fmaxf(0, s->scroll_y);
        }
        de_text_layout_free(l);
        if (!in->multiline) {
            f32 room = fmaxf(1, node->computed_rect.z - 10 * input_scale(ui, e));
            if (c.x - s->scroll_x > room)
                s->scroll_x = c.x - room;
            if (c.x - s->scroll_x < 0)
                s->scroll_x = c.x;
            s->scroll_x = fmaxf(0, s->scroll_x);
        }
        de_platform_text_area area = {
            (i32)(node->computed_rect.x + c.x - s->scroll_x + 4 * input_scale(ui, e)),
            (i32)(node->computed_rect.y + c.y - s->scroll_y + 4 * input_scale(ui, e)), 2, (i32)c.w,
            0};
        if (ui->desc.window.id) {
            u32 pw = 0, ph = 0, lw = 0, lh = 0;
            de_window_size_pixels(ui->desc.window, &pw, &ph);
            de_window_size_points(ui->desc.window, &lw, &lh);
            if (pw && ph) {
                area.x = (i32)((f32)area.x * lw / pw);
                area.y = (i32)((f32)area.y * lh / ph);
                area.h = (i32)((f32)area.h * lh / ph);
            }
        }
        de_text_input_area(input, &area);
    }
}
void de_ui__input_draw(de_ui *ui, de_entity e, de_vec4 r, de_ui_draw_emitter *em) {
    de_ui_advanced *a = ui->advanced;
    if (a->active_input.bits != e.bits)
        return;
    const DeUiTextInput *in = de_entity_get_const(ui->world, e, a->input);
    de_ui_editor *s = de_entity_get(ui->world, e, a->editor);
    if (!in || !s)
        return;
    de_text_layout *l = layout(ui, e, de_ui__input_display(s), (u32)strlen(de_ui__input_display(s)),
                               in->multiline ? fmaxf(1, r.z - 8 * input_scale(ui, e)) : 0);
    if (!l)
        return;
    f32 x = r.x + 4 * input_scale(ui, e) - s->scroll_x,
        y = r.y + 4 * input_scale(ui, e) - s->scroll_y;
    if (in->caret != s->anchor) {
        u32 lo = in->caret < s->anchor ? in->caret : s->anchor,
            hi = in->caret > s->anchor ? in->caret : s->anchor;
        u32 small[257];
        const u32 *b;
        u32 n = de_ui__input_boundaries(in, s, small, &b);
        for (u32 i = 0; i + 1 < n; ++i)
            if (b[i] >= lo && b[i] < hi) {
                de_vec4 c = de_text_layout_caret(l, display_offset(in, s, b[i])),
                        d = de_text_layout_caret(l, display_offset(in, s, b[i + 1]));
                f32 left = c.y == d.y ? fminf(c.x, d.x) : c.x,
                    right = c.y == d.y ? fmaxf(c.x, d.x) : r.z - 8 * input_scale(ui, e);
                de_ui__emit_rect(em, (de_vec4){x + left, y + c.y, fmaxf(2, right - left), c.w},
                                 (de_vec4){.18f, .4f, .8f, .5f}, (de_vec4){0}, 0, 0);
            }
    }
    if (s->preedit_end > s->preedit_start) {
        de_vec4 c = de_text_layout_caret(l, s->preedit_start),
                d = de_text_layout_caret(l, s->preedit_end);
        de_ui__emit_rect(em, (de_vec4){x + fminf(c.x, d.x), y + c.y + c.w - 2, fabsf(d.x - c.x), 1},
                         (de_vec4){1, 1, 1, 1}, (de_vec4){0}, 0, 0);
    }
    if (fmodf(s->blink, 1) < .6f) {
        de_vec4 c = de_text_layout_caret(l, s->displayed_caret);
        de_ui__emit_rect(em, (de_vec4){x + c.x, y + c.y, 1, c.w}, (de_vec4){1, 1, 1, 1},
                         (de_vec4){0}, 0, 0);
    }
    de_text_layout_free(l);
}
