/* editor_debugdraw.c: debug draw over IPC (DENG-632). The engine API is de_draw_* (de_debug.h,
 * DENG-572); each frame the service ages the shapes and keeps the live lines, which the views
 * draw into their frames (editor_views.c). */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* snprintf */
#endif
#include "host/editor/editor_internal.h"

#include <dengine/de_debug.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ED_DD_LINES 65536u

typedef struct de__ed_debugdraw {
    de_draw_line_item *lines;
    u32 count;
    u32 categories; /* the count last announced */
    u64 last_ns;
} de__ed_debugdraw;

static de__ed_debugdraw g_dd;

u32 de__ed_dd_lines(const de_draw_line_item **out);
u32 de__ed_dd_lines(const de_draw_line_item **out) {
    *out = g_dd.lines;
    return g_dd.lines != NULL ? g_dd.count : 0;
}

static void hex(u32 rgb, char out[8]) {
    snprintf(out, 8, "#%06x", rgb & 0xFFFFFFu);
}

static void on_categories(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                          mpack_writer_t *w) {
    (void)ed;
    de_draw_category_info info[64];
    const u32 n = de_draw_categories(info, 64);
    de_ep_debug_draw_categories r;
    memset(&r, 0, sizeof r);
    r.categories.items = (de_ep_debug_draw_category *)de__ed_alloc(a, sizeof(de_ep_debug_draw_category) * (n + 1));
    for (u32 i = 0; i < n && i < 64 && r.categories.items != NULL; ++i) {
        de_ep_debug_draw_category *c = &r.categories.items[r.categories.count++];
        memset(c, 0, sizeof *c);
        c->name = de_ep_cstr(info[i].name);
        c->on = info[i].on;
        char *col = (char *)de__ed_alloc(a, 8);
        if (col != NULL) {
            hex(info[i].rgb, col);
            c->has_color = true;
            c->color = de_ep_cstr(col);
        }
        c->has_shapes = true;
        c->shapes = info[i].shapes;
        c->has_game = true;
        c->game = info[i].game;
    }
    r.has_paused = true;
    r.paused = de_draw_paused();
    de_ep_write_response_debugdraw_categories(w, req->id, &r);
}

static void empty(mpack_writer_t *w, u64 id) {
    de_ep_write_result_begin(w, id);
    mpack_build_map(w);
    mpack_complete_map(w);
    de_ep_write_result_end(w);
}

static bool known(de_ep_str name) {
    de_draw_category_info info[64];
    const u32 n = de_draw_categories(info, 64);
    for (u32 i = 0; i < n && i < 64; ++i)
        if (de_ep_str_eq(name, info[i].name))
            return true;
    return false;
}

static void on_enabled(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(debugdraw_set_enabled, de_ep_debug_draw_enable_params, q);
    if (!known(q.category)) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "category %.*s", (int)q.category.len, q.category.ptr);
        return;
    }
    char *name = de__ed_cstr(a, q.category);
    de_draw_set_enabled(name, q.on);
    empty(w, req->id);
}

static void on_paused(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(debugdraw_set_paused, de_ep_paused_params, q);
    de_draw_set_paused(q.paused);
    empty(w, req->id);
}

static void on_color(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(debugdraw_set_color, de_ep_debug_draw_color_params, q);
    bool ok = q.color.len == 7 && q.color.ptr[0] == '#';
    for (u32 i = 1; ok && i < 7; ++i) {
        const char c = q.color.ptr[i];
        ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    if (!ok) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "%.*s isn't #rrggbb", (int)q.color.len, q.color.ptr);
        return;
    }
    if (!known(q.category)) {
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "category %.*s", (int)q.category.len, q.category.ptr);
        return;
    }
    char buf[8];
    memcpy(buf, q.color.ptr + 1, 6);
    buf[6] = '\0';
    de_draw_set_color(de__ed_cstr(a, q.category), (u32)strtoul(buf, NULL, 16));
    empty(w, req->id);
}

static void tick(de__editor *ed) {
    const u64 now = de_time_now_ns();
    const f32 dt = g_dd.last_ns != 0 ? (f32)((f64)(now - g_dd.last_ns) * 1e-9) : 0.0f;
    g_dd.last_ns = now;
    g_dd.count = de_draw_update(dt, g_dd.lines, g_dd.lines != NULL ? ED_DD_LINES : 0);
    /* New categories: tell the IDE (it re-sends the user's colours then). */
    const u32 n = de_draw_categories(NULL, 0);
    if (n != g_dd.categories) {
        g_dd.categories = n;
        de_ep_arena a = {ed->arena, ed->arena_cap, 0};
        de_draw_category_info info[64];
        const u32 k = de_draw_categories(info, 64);
        de_ep_debug_draw_categories d;
        memset(&d, 0, sizeof d);
        d.categories.items = (de_ep_debug_draw_category *)de__ed_alloc(&a, sizeof(de_ep_debug_draw_category) * (k + 1));
        for (u32 i = 0; i < k && i < 64 && d.categories.items != NULL; ++i) {
            de_ep_debug_draw_category *c = &d.categories.items[d.categories.count++];
            memset(c, 0, sizeof *c);
            c->name = de_ep_cstr(info[i].name);
            c->on = info[i].on;
            c->has_game = true;
            c->game = info[i].game;
        }
        d.has_paused = true;
        d.paused = de_draw_paused();
        mpack_writer_t nw;
        const u64 seq = de__ed_note_begin(ed, &nw);
        de_ep_write_notify_debugdraw_categories_changed(&nw, seq, &d);
        de__ed_send(ed, &nw, DE_EP_KIND_NOTIFICATION);
    }
}

void de__ed_debugdraw_register(de__editor *ed) {
    memset(&g_dd, 0, sizeof g_dd);
    ed->debugdraw = (struct de__ed_debugdraw *)&g_dd;
    g_dd.lines = (de_draw_line_item *)DE_ALLOC(de_heap_allocator(), sizeof(de_draw_line_item) * ED_DD_LINES,
                                               16, DE_MEM_GENERAL);
    /* The engine's own categories, so the IDE always has some to show. */
    (void)de_draw_category_register("editor.selection", 0xffb000, false);
    (void)de_draw_category_register("physics", 0x40c0ff, false);
    (void)de_draw_category_register("navigation", 0x60e060, false);
    de__ed_on(ed, DE_EP_METHOD_DEBUGDRAW_CATEGORIES, on_categories);
    de__ed_on(ed, DE_EP_METHOD_DEBUGDRAW_SET_ENABLED, on_enabled);
    de__ed_on(ed, DE_EP_METHOD_DEBUGDRAW_SET_PAUSED, on_paused);
    de__ed_on(ed, DE_EP_METHOD_DEBUGDRAW_SET_COLOR, on_color);
    de__ed_on_tick(ed, tick);
}
