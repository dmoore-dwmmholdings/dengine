#include "session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
bool terminal_apply(terminal_session *s, const terminal_entry *entry) {
    switch (entry->operation) {
    case TERM_ENTER:
        s->editing = true;
        s->captured = false;
        de_ui_set_input_mode(s->ui, DE_UI_INPUT_MODAL);
        de_ui_set_focus(s->ui, s->editor);
        return !s->window.id || de_platform_mouse_relative(s->window, false) == DE_OK;
    case TERM_LEAVE:
        s->popup_open = false;
        s->editing = false;
        s->captured = true;
        de_ui_set_focus(s->ui, DE_ENTITY_NULL);
        de_ui_set_input_mode(s->ui, DE_UI_INPUT_NONE);
        if (s->window.id)
            s->captured = de_platform_mouse_relative(s->window, true) == DE_OK &&
                          de_platform_mouse_relative_get(s->window);
        return s->captured;
    case TERM_PAUSE:
        s->paused = !s->paused;
        return true;
    case TERM_TICK:
        if (!s->paused)
            ++s->aircraft_ticks;
        if (!s->editing && entry->edit.caret) {
            ++s->walk_ticks;
            if (entry->edit.caret & 1u)
                s->z -= .025f;
            if (entry->edit.caret & 2u)
                s->z += .025f;
            if (entry->edit.caret & 4u)
                s->x -= .025f;
            if (entry->edit.caret & 8u)
                s->x += .025f;
        }
        return true;
    case TERM_MENU:
        s->popup_open = !s->popup_open;
        return true;
    case TERM_EDIT:
        return de_ui_text_input_apply(s->ui, s->editor, &entry->edit);
    default:
        return false;
    }
}
bool terminal_append(terminal_journal *j, terminal_entry entry) {
    if (j->failed || j->count == 4096 || entry.edit.text.len > 1048576 ||
        j->payload_bytes + entry.edit.text.len > 16777216) {
        j->failed = true;
        return false;
    }
    if (entry.edit.text.len) {
        char *copy = malloc(entry.edit.text.len);
        if (!copy) {
            j->failed = true;
            return false;
        }
        memcpy(copy, entry.edit.text.ptr, entry.edit.text.len);
        entry.edit.text.ptr = copy;
    } else
        entry.edit.text.ptr = NULL;
    j->payload_bytes += entry.edit.text.len;
    j->entries[j->count++] = entry;
    return true;
}
void terminal_journal_clear(terminal_journal *j) {
    for (u32 i = 0; i < j->count; ++i)
        free((void *)j->entries[i].edit.text.ptr);
    memset(j, 0, sizeof *j);
}
static bool put(FILE *f, u32 n) {
    u8 b[4] = {(u8)n, (u8)(n >> 8), (u8)(n >> 16), (u8)(n >> 24)};
    return fwrite(b, 1, 4, f) == 4;
}
static bool get(FILE *f, u32 *n) {
    u8 b[4];
    if (fread(b, 1, 4, f) != 4)
        return false;
    *n = (u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
    return true;
}
bool terminal_save(const terminal_journal *j, const char *path) {
    if (j->failed)
        return false;
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite("TUIJ\1\0\0\0", 1, 8, f) == 8 && put(f, j->count);
    for (u32 i = 0; ok && i < j->count; ++i) {
        const terminal_entry *e = &j->entries[i];
        ok = put(f, e->operation) && put(f, (u32)e->edit.kind) && put(f, e->edit.caret) &&
             put(f, e->edit.anchor) && put(f, (u32)e->edit.text.len);
        if (ok && e->edit.text.len)
            ok = fwrite(e->edit.text.ptr, 1, e->edit.text.len, f) == e->edit.text.len;
    }
    return fclose(f) == 0 && ok;
}
bool terminal_load(terminal_journal *j, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    terminal_journal *temp = calloc(1, sizeof *temp);
    if (!temp) {
        fclose(f);
        return false;
    }
    char magic[8];
    u32 count = 0;
    u64 total = 0;
    bool ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "TUIJ\1\0\0\0", 8) && get(f, &count) &&
              count <= 4096;
    for (u32 i = 0; ok && i < count; ++i) {
        terminal_entry e = {0};
        u32 kind = 0, length = 0;
        ok = get(f, &e.operation) && get(f, &kind) && get(f, &e.edit.caret) &&
             get(f, &e.edit.anchor) && get(f, &length);
        total += length;
        ok = ok && e.operation <= TERM_MENU && kind <= DE_UI_TEXT_REDO && length <= 1048576 &&
             total <= 16777216;
        if (!ok)
            break;
        e.edit.kind = (de_ui_text_edit_kind)kind;
        char *text = length ? malloc(length) : NULL;
        if (length && !text) {
            ok = false;
            break;
        }
        ok = !length || (fread(text, 1, length, f) == length && !memchr(text, 0, length));
        e.edit.text = (de_str){text, length};
        if (ok)
            ok = terminal_append(temp, e);
        free(text);
    }
    ok = ok && fgetc(f) == EOF;
    fclose(f);
    if (ok) {
        terminal_journal_clear(j);
        *j = *temp;
    } else
        terminal_journal_clear(temp);
    free(temp);
    return ok;
}
