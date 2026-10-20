#ifndef TERMINAL_SESSION_H
#define TERMINAL_SESSION_H
#include <dengine/dengine.h>
/* Companion journal: DEIR records game actions; this records committed editor
 * commands and terminal mode/simulation ticks, never OS devices or IME preedit. */
enum { TERM_ENTER, TERM_LEAVE, TERM_PAUSE, TERM_TICK, TERM_EDIT, TERM_MENU };
typedef struct terminal_entry {
    u32 operation;
    de_ui_text_edit edit;
} terminal_entry;
typedef struct terminal_journal {
    terminal_entry entries[4096];
    u32 count;
    u64 payload_bytes;
    bool failed;
} terminal_journal;
typedef struct terminal_session {
    de_ui *ui;
    de_entity editor;
    de_window window;
    bool editing, paused, captured, popup_open;
    u32 walk_ticks, aircraft_ticks;
    f32 x, z;
} terminal_session;
bool terminal_apply(terminal_session *s, const terminal_entry *entry);
bool terminal_append(terminal_journal *j, terminal_entry entry);
void terminal_journal_clear(terminal_journal *j);
bool terminal_save(const terminal_journal *j, const char *path);
bool terminal_load(terminal_journal *j, const char *path);
#endif
