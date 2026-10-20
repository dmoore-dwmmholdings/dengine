#include "advanced_internal.h"
#include <limits.h>
/* Keep ownership outside movable/snapshotted ECS component storage. */
typedef struct edit_record {
    char *bytes;
    u32 at, removed, inserted, caret, anchor;
} edit_record;
struct de_ui_edit_storage {
    de_ui_edit_storage *next;
    de_entity entity;
    const de_allocator *alloc;
    char *data, *display;
    u32 *boundaries;
    u32 capacity, display_capacity, boundary_capacity, boundary_count;
    u32 revision, boundary_revision;
    edit_record history[128];
    u32 count, cursor;
    char observed[256];
    const char *borrowed_display;
    bool internal_change;
};
const char *de_ui__input_data(const DeUiTextInput *in, const de_ui_editor *s) {
    return s && s->storage && s->storage->data ? s->storage->data : in->buffer;
}
const char *de_ui__input_display(const de_ui_editor *s) {
    return s->storage && s->storage->display ? s->storage->display : s->display;
}
static bool reserve(de_ui_edit_storage *d, void **ptr, u32 *capacity, u32 bytes, u32 unit) {
    if (*capacity >= bytes)
        return true;
    u32 cap = bytes < 512 ? 512 : bytes;
    if (cap <= UINT32_MAX / 2)
        cap *= 2;
    void *p = DE_ALLOC(d->alloc, (u64)cap * unit, _Alignof(u32), DE_MEM_UI);
    if (!p)
        return false;
    if (*ptr) {
        memcpy(p, *ptr, (u64)*capacity * unit);
        DE_FREE(d->alloc, *ptr, (u64)*capacity * unit);
    }
    *ptr = p;
    *capacity = cap;
    return true;
}
bool de_ui__input_reserve(de_ui_editor *s, u32 bytes) {
    return s->storage &&
           reserve(s->storage, (void **)&s->storage->data, &s->storage->capacity, bytes, 1);
}
char *de_ui__input_display_reserve(de_ui_editor *s, u32 bytes) {
    if (bytes <= sizeof s->display && (!s->storage || !s->storage->display)) {
        if (s->storage)
            s->storage->borrowed_display = s->display;
        return s->display;
    }
    if (!s->storage || !reserve(s->storage, (void **)&s->storage->display,
                                &s->storage->display_capacity, bytes, 1))
        return NULL;
    s->storage->borrowed_display = s->storage->display;
    return s->storage->display;
}
void de_ui__input_changed(de_ui_editor *s) {
    if (s->storage) {
        ++s->storage->revision;
        s->storage->internal_change = true;
    }
    s->preferred_valid = false;
    s->blink = 0;
}
u32 de_ui__input_boundaries(const DeUiTextInput *in, de_ui_editor *s, u32 small[257],
                            const u32 **out) {
    de_str text = {de_ui__input_data(in, s), in->length};
    *out = small;
    if (in->length <= 255)
        return de_text_grapheme_offsets(text, small, 257);
    de_ui_edit_storage *d = s->storage;
    if (!d ||
        !reserve(d, (void **)&d->boundaries, &d->boundary_capacity, in->length + 1, sizeof(u32))) {
        return 0;
    }
    if (d->boundary_revision != d->revision || !d->boundary_count) {
        d->boundary_count = de_text_grapheme_offsets(text, d->boundaries, d->boundary_capacity);
        d->boundary_revision = d->revision;
    }
    *out = d->boundaries;
    return d->boundary_count;
}
static void clear_history(de_ui_edit_storage *d, u32 first) {
    for (u32 i = first; i < d->count; ++i)
        if (d->history[i].bytes)
            DE_FREE(d->alloc, d->history[i].bytes,
                    (u64)d->history[i].removed + d->history[i].inserted);
    d->count = first;
    if (d->cursor > first)
        d->cursor = first;
}
bool de_ui__input_history(DeUiTextInput *in, de_ui_editor *s, u32 lo, u32 hi, de_str text) {
    de_ui_edit_storage *d = s->storage;
    if (!d)
        return true; /* Standalone inline-input helpers do not own history. */
    u64 size = hi - lo + text.len;
    char *bytes = size ? DE_ALLOC(d->alloc, size, 1, DE_MEM_UI) : NULL;
    if (size && !bytes)
        return false;
    if (hi > lo)
        memcpy(bytes, de_ui__input_data(in, s) + lo, hi - lo);
    if (text.len)
        memcpy(bytes + hi - lo, text.ptr, text.len);
    clear_history(d, d->cursor);
    if (d->count == 128) {
        edit_record *old = &d->history[0];
        DE_FREE(d->alloc, old->bytes, (u64)old->removed + old->inserted);
        memmove(d->history, d->history + 1, 127 * sizeof(edit_record));
        --d->count;
    }
    d->history[d->count++] = (edit_record){bytes, lo, hi - lo, (u32)text.len, in->caret, s->anchor};
    d->cursor = d->count;
    return true;
}
void de_ui__document_prepare(de_ui *ui, de_entity e) {
    de_ui_editor *s = de_entity_get(ui->world, e, ui->advanced->editor);
    if (!s)
        return;
    if (s->storage) {
        de_ui_edit_storage *d = s->storage;
        const DeUiTextInput *in = de_entity_get_const(ui->world, e, ui->advanced->input);
        if (!d->data && in) {
            if (!d->internal_change && memcmp(d->observed, in->buffer, 256))
                clear_history(d, 0);
            memcpy(d->observed, in->buffer, 256);
        }
        d->internal_change = false;
        return;
    }
    de_ui_edit_storage *d = DE_NEW(ui->alloc, de_ui_edit_storage, DE_MEM_UI);
    if (!d)
        return;
    memset(d, 0, sizeof *d);
    d->alloc = ui->alloc;
    d->entity = e;
    d->revision = 1;
    d->next = ui->advanced->documents;
    ui->advanced->documents = d;
    s->storage = d;
    const DeUiTextInput *in = de_entity_get_const(ui->world, e, ui->advanced->input);
    if (in)
        memcpy(d->observed, in->buffer, 256);
}
void de_ui__documents_collect(de_ui *ui, bool all) {
    de_ui_advanced *a = ui->advanced;
    de_ui_edit_storage **slot = &a->documents;
    while (*slot) {
        de_ui_edit_storage *d = *slot;
        de_ui_editor *s = de_entity_alive(ui->world, d->entity)
                              ? de_entity_get(ui->world, d->entity, a->editor)
                              : NULL;
        if (!all && s && s->storage == d && de_entity_has(ui->world, d->entity, a->input)) {
            slot = &d->next;
            continue;
        }
        DeUiText *t = de_entity_get(ui->world, d->entity, ui->ids.text);
        if (a->active_input.bits == d->entity.bits)
            de_ui__input_cancel(ui, d->entity);
        if (t && (t->literal.ptr == d->display || t->literal.ptr == d->borrowed_display ||
                  (s && t->literal.ptr == s->display)))
            t->literal = (de_str){0};
        if (s && s->storage == d)
            s->storage = NULL;
        *slot = d->next;
        clear_history(d, 0);
        if (d->data)
            DE_FREE(d->alloc, d->data, d->capacity);
        if (d->display)
            DE_FREE(d->alloc, d->display, d->display_capacity);
        if (d->boundaries)
            DE_FREE(d->alloc, d->boundaries, (u64)d->boundary_capacity * sizeof(u32));
        DE_FREE(ui->alloc, d, sizeof *d);
    }
}
de_str de_ui_text_input_get(const de_ui *ui, de_entity e) {
    if (!ui || !ui->world || !ui->advanced)
        return (de_str){0};
    const DeUiTextInput *in = de_entity_get_const(ui->world, e, ui->advanced->input);
    const de_ui_editor *s = de_entity_get_const(ui->world, e, ui->advanced->editor);
    return in ? (de_str){de_ui__input_data(in, s), in->length} : (de_str){0};
}
de_result de_ui_text_input_set(de_ui *ui, de_entity e, de_str text) {
    if (!ui || !ui->world || !ui->advanced || (!text.ptr && text.len) ||
        text.len > INT32_MAX - 1u || (text.len && memchr(text.ptr, 0, text.len)) ||
        !de_entity_has(ui->world, e, ui->advanced->input))
        return DE_ERR_INVALID_ARGUMENT;
    de_ui__input_prepare(ui, e);
    de_ui_editor *s = de_entity_get(ui->world, e, ui->advanced->editor);
    DeUiTextInput *in = de_entity_get(ui->world, e, ui->advanced->input);
    if (!s || !s->storage)
        return DE_ERR_OUT_OF_MEMORY;
    /* Copy before reserve: callers may pass a slice of this document. */
    char *copy = DE_ALLOC(ui->alloc, text.len + 1, 1, DE_MEM_UI);
    if (!copy)
        return DE_ERR_OUT_OF_MEMORY;
    if (text.len)
        memcpy(copy, text.ptr, text.len);
    copy[text.len] = 0;
    if (text.len > 255 || s->storage->data) {
        if (!de_ui__input_reserve(s, (u32)text.len + 1)) {
            DE_FREE(ui->alloc, copy, text.len + 1);
            return DE_ERR_OUT_OF_MEMORY;
        }
        memcpy(s->storage->data, copy, text.len + 1);
    } else
        memcpy(in->buffer, copy, text.len + 1);
    DE_FREE(ui->alloc, copy, text.len + 1);
    clear_history(s->storage, 0);
    in->length = (u32)text.len;
    in->caret = s->anchor = 0;
    s->scroll_x = s->scroll_y = 0;
    de_ui__input_changed(s);
    de_ui__input_refresh(ui, e);
    return DE_OK;
}
static bool history(de_ui *ui, de_entity e, bool redo) {
    if (!ui || !ui->world || !ui->advanced)
        return false;
    DeUiTextInput *in = de_entity_get(ui->world, e, ui->advanced->input);
    de_ui_editor *s = de_entity_get(ui->world, e, ui->advanced->editor);
    de_ui_edit_storage *d = s ? s->storage : NULL;
    if (!in || !d || (redo ? d->cursor == d->count : d->cursor == 0))
        return false;
    edit_record r = d->history[redo ? d->cursor : d->cursor - 1];
    u32 remove = redo ? r.removed : r.inserted, add = redo ? r.inserted : r.removed;
    u32 length = in->length - remove + add;
    if (length > 255 && !de_ui__input_reserve(s, length + 1))
        return false;
    char *data = d->data ? d->data : in->buffer;
    memmove(data + r.at + add, data + r.at + remove, in->length - r.at - remove + 1);
    if (add)
        memcpy(data + r.at, r.bytes + (redo ? r.removed : 0), add);
    in->length = length;
    in->caret = redo ? r.at + r.inserted : r.caret;
    s->anchor = redo ? in->caret : r.anchor;
    d->cursor = redo ? d->cursor + 1 : d->cursor - 1;
    de_ui__input_changed(s);
    de_ui__input_refresh(ui, e);
    de_ui__widget_event(ui, DE_UI_EV_CHANGED, e, (de_name){0}, (f32)in->length);
    de_ui__input_notify(ui, e, (de_ui_text_edit){.kind = redo ? DE_UI_TEXT_REDO : DE_UI_TEXT_UNDO});
    return true;
}
bool de_ui_text_input_undo(de_ui *ui, de_entity e) {
    return history(ui, e, false);
}
bool de_ui_text_input_redo(de_ui *ui, de_entity e) {
    return history(ui, e, true);
}

void de_ui_set_text_edit_observer(de_ui *ui, de_ui_text_edit_observer observer, void *user) {
    if (!ui || !ui->advanced)
        return;
    ui->advanced->edit_observer = observer;
    ui->advanced->edit_observer_user = user;
}
void de_ui__input_notify(de_ui *ui, de_entity e, de_ui_text_edit edit) {
    if (ui->advanced->edit_observer)
        ui->advanced->edit_observer(e, &edit, ui->advanced->edit_observer_user);
}
