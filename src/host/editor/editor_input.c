/* editor_input.c: input.loadActionMaps and input.monitor.subscribe (DENG-828; DIDE protocol 1.24,
 * DIDE-137, capabilities input.actionMaps and input.monitor).
 *
 * input.loadActionMaps parses an action-map TOML file (the IDE's unsaved text, or the project
 * file) into the game's de_input, as de_action_maps_load does, and answers with the number of
 * maps and actions in the file. It is allowed in every mode. Limit: the engine has no API that
 * removes an action, so an action deleted from the file stays until the game restarts.
 *
 * input.monitor.subscribe streams the topic input.monitor: held keys, the mouse, every connected
 * pad, the state of every loaded action and the applied context stack (top first), at `rateHz`
 * (1 to 120, default 30), only in Play and Simulate. The phase of each action is derived every
 * frame from the action's frame state and its interaction (README 1.24). A phase that lasts one
 * frame (a tap or release `performed`, any `canceled`) is held for the next sample of each
 * subscription, so a 30 Hz monitor does not miss it. Subscription ids are 0x30000 + n; the
 * generic unsubscribe (editor_world.c) ends them through de__ed_input_unsubscribe.
 */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* snprintf */
#endif
#include "host/editor/editor_internal.h"

#include "input/action_internal.h"
#include "input/input_internal.h"
#include <dengine/de_platform.h>
#include <stdio.h>
#include <string.h>

#define IN_ACTION_MAPS "input.actionMaps"
#define IN_MONITOR     "input.monitor"
#define IN_MAX_TEXT    (1u << 20) /* input.loadActionMaps text limit */
#define IN_SUBS        4u         /* concurrent input.monitor subscriptions */
#define IN_SUB_BASE    0x30000u   /* ids: world < 0x10000, logs 0x10000+, stats 0x20000+ */
#define IN_LATCHED     0x80u      /* in ed_in_sub.latch: a one-frame phase is held */

typedef struct ed_in_sub {
    u32 id; /* 0: free */
    u64 period_ns;
    u64 next_ns; /* 0: the first eligible tick sends */
    u32 sent;    /* samples written (tests) */
    u8 latch[DE_ACTION_MAX_ACTIONS];
} ed_in_sub;

typedef struct ed_in_action {
    de_ep_input_action_phase phase;
    f32 progress;
    bool has_progress;
} ed_in_action;

typedef struct ed_in {
    ed_in_sub subs[IN_SUBS];
    u32 next_id;
    /* Per action, from the previous frame. */
    bool prev_held[DE_ACTION_MAX_ACTIONS];
    bool prev_fired[DE_ACTION_MAX_ACTIONS];
    bool prev_waiting[DE_ACTION_MAX_ACTIONS];
    ed_in_action now[DE_ACTION_MAX_ACTIONS];
    u32 tracked; /* actions the arrays above describe */
    /* Sample storage (a sample is written before the next one is built). */
    de_ep_str keys[DE_KEY_COUNT_];
    de_ep_str mouse_buttons[DE_MOUSE_BUTTON_COUNT_];
    de_ep_input_gamepad_state pads[DE_INPUT_MAX_PADS];
    char pad_names[DE_INPUT_MAX_PADS][64];
    f32 pad_axes[DE_INPUT_MAX_PADS][7]; /* left x y, right x y, gyro x y z */
    de_ep_str pad_buttons[DE_INPUT_MAX_PADS][DE_PAD_BUTTON_COUNT_];
    de_ep_input_action_state actions[DE_ACTION_MAX_ACTIONS];
    f32 values[DE_ACTION_MAX_ACTIONS][2];
    de_ep_input_context_state contexts[DE_INPUT_CONTEXT_STACK_MAX];
    de_ep_str context_maps[DE_INPUT_CONTEXT_STACK_MAX][DE_INPUT_CONTEXT_MAX_MAPS];
} ed_in;

static ed_in g_in;

static const char *const k_types[] = {"button", "axis1d", "axis2d"};
static const char *const k_interactions[] = {"press", "release", "hold", "tap", "double_tap"};

/* A project-relative path: no absolute paths, "..", backslashes or drive letters. */
static bool safe_path(const char *path) {
    return path[0] != '\0' && path[0] != '/' && strchr(path, '\\') == NULL &&
           strchr(path, ':') == NULL && strstr(path, "..") == NULL;
}

static void forget_actions(void) {
    memset(g_in.prev_held, 0, sizeof g_in.prev_held);
    memset(g_in.prev_fired, 0, sizeof g_in.prev_fired);
    memset(g_in.prev_waiting, 0, sizeof g_in.prev_waiting);
    memset(g_in.now, 0, sizeof g_in.now);
    g_in.tracked = 0;
    for (u32 i = 0; i < IN_SUBS; ++i)
        memset(g_in.subs[i].latch, 0, sizeof g_in.subs[i].latch);
}

/* ---------- input.loadActionMaps ---------- */

static void load_action_maps(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                             mpack_writer_t *w) {
    DE__ED_PARAMS(input_load_action_maps, de_ep_input_load_action_maps_params, q);
    const char *path = de__ed_cstr(a, q.path);
    if (path == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "input.loadActionMaps: out of memory");
        return;
    }
    if (!safe_path(path)) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "%s: not a project-relative path",
                     path);
        return;
    }
    const char *text = NULL;
    u64 len = 0;
    void *file = NULL;
    if (q.has_text) {
        if (q.text.len > IN_MAX_TEXT) {
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "%s: text is over 1 MiB", path);
            return;
        }
        text = q.text.ptr;
        len = q.text.len;
    } else {
        char vpath[1024];
        if (!de__ed_project_vpath(ed, path, vpath, sizeof vpath) ||
            de_file_read_all(vpath, de_heap_allocator(), &file, &len) != DE_OK) {
            de_error_clear();
            de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "no file %s", path);
            return;
        }
        if (len > IN_MAX_TEXT) {
            de_heap_allocator()->free(de_heap_allocator()->user, file, (size_t)len);
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "%s: file is over 1 MiB", path);
            return;
        }
        text = (const char *)file;
    }
    de_input *in = ed->e != NULL ? ed->e->input : NULL;
    if (in == NULL) {
        if (file != NULL)
            de_heap_allocator()->free(de_heap_allocator()->user, file, (size_t)len);
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "%s: the game has no input", path);
        return;
    }
    const struct de__actions *before = de__actions_state_const(in);
    const u32 had = before != NULL ? before->action_count : 0;
    de__action_maps_counts counts;
    u32 unknown = 0;
    const de_result res = de__action_maps_parse_counted(in, text != NULL ? text : "", (size_t)len,
                                                        path, &unknown, &counts);
    if (file != NULL)
        de_heap_allocator()->free(de_heap_allocator()->user, file, (size_t)len);
    if (res != DE_OK) {
        const de_error *e = de_error_last();
        char msg[256];
        snprintf(msg, sizeof msg, "%s",
                 e != NULL && e->message[0] ? e->message : de_result_str(res));
        de_error_clear();
        const de_ep_error code =
            res == DE_ERR_OUT_OF_MEMORY ? DE_EP_ERR_BUSY : DE_EP_ERR_INVALID_PARAMS;
        de__ed_error(w, req->id, code, "%s", msg);
        return;
    }
    const struct de__actions *after = de__actions_state_const(in);
    if (after == NULL || after->action_count != had)
        forget_actions(); /* indices are stable, but start the new actions from idle */
    de_ep_input_load_action_maps r = {counts.maps, counts.actions};
    de_ep_write_response_input_load_action_maps(w, req->id, &r);
}

/* ---------- input.monitor.subscribe ---------- */

static void monitor_subscribe(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                              mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(input_monitor_subscribe, de_ep_input_monitor_subscribe_params, q);
    f32 rate = 30.0f;
    if (q.has_rate_hz) {
        if (!(q.rate_hz >= 1.0f && q.rate_hz <= 120.0f)) { /* NaN fails too */
            de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS,
                         "input.monitor.subscribe: rateHz %g is not 1 to 120", (double)q.rate_hz);
            return;
        }
        rate = q.rate_hz;
    }
    ed_in_sub *s = NULL;
    for (u32 i = 0; i < IN_SUBS && s == NULL; ++i)
        if (g_in.subs[i].id == 0)
            s = &g_in.subs[i];
    if (s == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "too many input monitor subscriptions");
        return;
    }
    memset(s, 0, sizeof *s);
    s->id = IN_SUB_BASE + ++g_in.next_id;
    s->period_ns = (u64)(1e9 / (f64)rate);
    de_ep_subscribe_result r = {s->id};
    de_ep_write_response_input_monitor_subscribe(w, req->id, &r);
}

bool de__ed_input_unsubscribe(u32 id) {
    for (u32 i = 0; i < IN_SUBS; ++i)
        if (id != 0 && g_in.subs[i].id == id) {
            memset(&g_in.subs[i], 0, sizeof g_in.subs[i]);
            return true;
        }
    return false;
}

/* ---------- phases ---------- */

static bool is_pulse(de_ep_input_action_phase p, de_interaction it) {
    return p == DE_EP_INPUT_ACTION_PHASE_CANCELED ||
           (p == DE_EP_INPUT_ACTION_PHASE_PERFORMED &&
            (it == DE_INTERACT_TAP || it == DE_INTERACT_DOUBLE_TAP || it == DE_INTERACT_RELEASE));
}

static f32 clamp01(f32 v) {
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* The phase of action `i` this frame (README 1.24), from its frame state and the previous frame. */
static ed_in_action phase_of(const de__action *act, u32 i) {
    ed_in_action r = {DE_EP_INPUT_ACTION_PHASE_IDLE, 0.0f, false};
    const bool held = act->frame.held, trig = act->frame.triggered, prev = g_in.prev_held[i];
    const bool fall = prev && !held;
    switch (act->interaction) {
    case DE_INTERACT_PRESS:
        if (held || trig)
            r.phase = DE_EP_INPUT_ACTION_PHASE_PERFORMED;
        break;
    case DE_INTERACT_HOLD:
        r.has_progress = true;
        if (held && (act->track.hold_fired || trig)) {
            r.phase = DE_EP_INPUT_ACTION_PHASE_PERFORMED;
            r.progress = 1.0f;
        } else if (held) {
            r.phase = DE_EP_INPUT_ACTION_PHASE_STARTED;
            r.progress = act->hold_seconds > 0.0f ? clamp01(act->track.held_time / act->hold_seconds)
                                                  : 1.0f;
        } else if (trig) {
            r.phase = DE_EP_INPUT_ACTION_PHASE_PERFORMED;
            r.progress = 1.0f;
        } else if (fall && !g_in.prev_fired[i]) {
            r.phase = DE_EP_INPUT_ACTION_PHASE_CANCELED;
        }
        break;
    case DE_INTERACT_TAP:
        r.has_progress = true;
        if (held) {
            r.phase = DE_EP_INPUT_ACTION_PHASE_STARTED;
            r.progress = act->tap_seconds > 0.0f ? clamp01(act->track.held_time / act->tap_seconds)
                                                 : 1.0f;
        } else if (trig) {
            r.phase = DE_EP_INPUT_ACTION_PHASE_PERFORMED;
        } else if (fall) {
            r.phase = DE_EP_INPUT_ACTION_PHASE_CANCELED;
        }
        break;
    case DE_INTERACT_DOUBLE_TAP: {
        const bool waiting = act->track.tap_count == 1;
        if (held)
            r.phase = DE_EP_INPUT_ACTION_PHASE_STARTED;
        else if (trig)
            r.phase = DE_EP_INPUT_ACTION_PHASE_PERFORMED;
        else if (waiting)
            r.phase = DE_EP_INPUT_ACTION_PHASE_STARTED; /* waiting for the second tap */
        else if (fall || g_in.prev_waiting[i])
            r.phase = DE_EP_INPUT_ACTION_PHASE_CANCELED; /* too long, or the window ran out */
        break;
    }
    case DE_INTERACT_RELEASE:
    default:
        if (trig)
            r.phase = DE_EP_INPUT_ACTION_PHASE_PERFORMED;
        else if (held)
            r.phase = DE_EP_INPUT_ACTION_PHASE_STARTED;
        break;
    }
    return r;
}

/* Updates the phase of every action, once per frame, and holds one-frame phases per
 * subscription. */
static void track_actions(const de_input *in) {
    const struct de__actions *s = in != NULL ? de__actions_state_const(in) : NULL;
    const u32 n = s != NULL ? s->action_count : 0;
    if (n < g_in.tracked)
        forget_actions();
    g_in.tracked = n;
    for (u32 i = 0; i < n; ++i) {
        const de__action *act = &s->actions[i];
        const ed_in_action p = phase_of(act, i);
        g_in.now[i] = p;
        if (is_pulse(p.phase, act->interaction))
            for (u32 k = 0; k < IN_SUBS; ++k)
                if (g_in.subs[k].id != 0 && !(g_in.subs[k].latch[i] & IN_LATCHED))
                    g_in.subs[k].latch[i] = (u8)(IN_LATCHED | (u8)p.phase);
        g_in.prev_held[i] = act->frame.held;
        g_in.prev_fired[i] = act->frame.held && (act->track.hold_fired || act->frame.triggered);
        g_in.prev_waiting[i] = act->interaction == DE_INTERACT_DOUBLE_TAP &&
                               !act->frame.held && act->track.tap_count == 1;
    }
}

/* ---------- samples ---------- */

static de_ep_list_f32 f32s(f32 *items, u32 count) {
    de_ep_list_f32 l = {items, count};
    return l;
}

static const char *pad_family(de_pad_type t) {
    switch (t) {
    case DE_PAD_TYPE_XBOX:
        return de_glyph_family_name(DE_GLYPH_FAMILY_XBOX);
    case DE_PAD_TYPE_PS4:
        return de_glyph_family_name(DE_GLYPH_FAMILY_PS);
    case DE_PAD_TYPE_PS5:
        return de_glyph_family_name(DE_GLYPH_FAMILY_PS5);
    case DE_PAD_TYPE_SWITCH:
        return de_glyph_family_name(DE_GLYPH_FAMILY_SWITCH);
    case DE_PAD_TYPE_STEAM:
        return de_glyph_family_name(DE_GLYPH_FAMILY_DECK);
    default:
        return de_glyph_family_name(DE_GLYPH_FAMILY_GENERIC);
    }
}

static void sample_devices(const de_input *in, de_ep_input_monitor_sample *out) {
    u32 n = 0;
    for (u32 k = 0; k < (u32)DE_KEY_COUNT_; ++k) {
        if (!de_key_down(in, (de_key)k))
            continue;
        const char *name = de_key_name((de_key)k);
        if (name != NULL && name[0] != '\0')
            g_in.keys[n++] = de_ep_cstr(name);
    }
    out->keys.items = g_in.keys;
    out->keys.count = n;

    const de_vec2 pos = de_mouse_position(in), d = de_mouse_delta(in), wh = de_mouse_wheel(in);
    out->mouse.x = pos.x;
    out->mouse.y = pos.y;
    out->mouse.dx = d.x;
    out->mouse.dy = d.y;
    out->mouse.wheel_x = wh.x;
    out->mouse.wheel_y = wh.y;
    n = 0;
    for (u32 b = 0; b < (u32)DE_MOUSE_BUTTON_COUNT_; ++b) {
        const char *name = de__binding_code_name(DE_BIND_MOUSE_BUTTON, b);
        if (name != NULL && de_mouse_down(in, (de_mouse_button)b))
            g_in.mouse_buttons[n++] = de_ep_cstr(name);
    }
    out->mouse.buttons.items = g_in.mouse_buttons;
    out->mouse.buttons.count = n;

    de_gamepad pads[DE_INPUT_MAX_PADS];
    u32 count = de_gamepad_list(in, pads, DE_INPUT_MAX_PADS);
    if (count > DE_INPUT_MAX_PADS)
        count = DE_INPUT_MAX_PADS;
    for (u32 p = 0; p < count; ++p) {
        de_ep_input_gamepad_state *g = &g_in.pads[p];
        memset(g, 0, sizeof *g);
        de_gamepad_info info;
        memset(&info, 0, sizeof info);
        de_pad_info(in, pads[p], &info);
        memcpy(g_in.pad_names[p], info.name, sizeof g_in.pad_names[p]);
        g_in.pad_names[p][sizeof g_in.pad_names[p] - 1] = '\0';
        g->id = (pads[p].id & ((1u << DE__INPUT_PAD_SLOT_BITS) - 1u)) - 1u; /* the slot */
        g->name = de_ep_cstr(g_in.pad_names[p]);
        g->family = de_ep_cstr(pad_family(info.type));
        f32 *ax = g_in.pad_axes[p];
        ax[0] = de_pad_axis_value(in, pads[p], DE_AXIS_LEFT_X);
        ax[1] = de_pad_axis_value(in, pads[p], DE_AXIS_LEFT_Y);
        ax[2] = de_pad_axis_value(in, pads[p], DE_AXIS_RIGHT_X);
        ax[3] = de_pad_axis_value(in, pads[p], DE_AXIS_RIGHT_Y);
        g->left_stick = f32s(&ax[0], 2);
        g->right_stick = f32s(&ax[2], 2);
        g->left_trigger = de_pad_axis_value(in, pads[p], DE_AXIS_LEFT_TRIGGER);
        g->right_trigger = de_pad_axis_value(in, pads[p], DE_AXIS_RIGHT_TRIGGER);
        u32 nb = 0;
        for (u32 b = 0; b < (u32)DE_PAD_BUTTON_COUNT_; ++b) {
            const char *name = de__binding_code_name(DE_BIND_PAD_BUTTON, b);
            if (name != NULL && de_pad_down(in, pads[p], (de_pad_button)b))
                g_in.pad_buttons[p][nb++] = de_ep_cstr(name);
        }
        g->buttons.items = g_in.pad_buttons[p];
        g->buttons.count = nb;
        de_vec3 gyro;
        if ((info.caps & DE_PAD_CAP_GYRO) && de_pad_gyro(in, pads[p], &gyro)) {
            ax[4] = gyro.x;
            ax[5] = gyro.y;
            ax[6] = gyro.z;
            g->has_gyro = true;
            g->gyro = f32s(&ax[4], 3);
        }
    }
    out->gamepads.items = g_in.pads;
    out->gamepads.count = count;
}

/* Every loaded action, with the phase held for subscription `s` when one is. */
static void sample_actions(const de_input *in, ed_in_sub *s, de_ep_input_monitor_sample *out) {
    const struct de__actions *st = de__actions_state_const(in);
    u32 n = 0;
    const u32 count = st != NULL && g_in.tracked <= st->action_count ? g_in.tracked : 0;
    for (u32 i = 0; i < count; ++i) {
        const de__action *act = &st->actions[i];
        if (act->name[0] == '\0' || act->map >= st->map_count)
            continue;
        de_ep_input_action_state *o = &g_in.actions[n++];
        memset(o, 0, sizeof *o);
        o->map = de_ep_cstr(st->maps[act->map].name);
        o->name = de_ep_cstr(act->name);
        o->type = de_ep_cstr((u32)act->type < 3u ? k_types[act->type] : "button");
        o->interaction = de_ep_cstr((u32)act->interaction < 5u ? k_interactions[act->interaction]
                                                               : "press");
        g_in.values[i][0] = act->frame.value.x;
        g_in.values[i][1] = act->frame.value.y;
        o->value = f32s(g_in.values[i], act->type == DE_ACTION_AXIS2D ? 2u : 1u);
        const ed_in_action *p = &g_in.now[i];
        o->phase = p->phase;
        o->has_progress = p->has_progress;
        o->progress = p->progress;
        if (s->latch[i] & IN_LATCHED) {
            o->phase = (de_ep_input_action_phase)(s->latch[i] & ~IN_LATCHED);
            if (o->phase == DE_EP_INPUT_ACTION_PHASE_PERFORMED && o->has_progress)
                o->progress = 1.0f;
            s->latch[i] = 0;
        }
    }
    out->actions.items = g_in.actions;
    out->actions.count = n;
}

/* The applied context stack, top first. Consecutive entries of one named context are one entry;
 * a map pushed on its own is an entry named after the map. */
static void sample_contexts(const de_input *in, de_ep_input_monitor_sample *out) {
    const struct de__actions *st = de__actions_state_const(in);
    u32 n = 0;
    if (st != NULL) {
        const de__context_state *c = &st->context;
        u32 i = c->applied_count;
        while (i > 0 && n < DE_INPUT_CONTEXT_STACK_MAX) {
            const de__context_entry top = c->applied[i - 1];
            de_ep_input_context_state *o = &g_in.contexts[n];
            de_ep_str *maps = g_in.context_maps[n];
            ++n;
            if (top.context != DE__CONTEXT_NONE && top.context < c->set_count) {
                /* The run of entries the set pushed, bottom to top. */
                u32 lo = i - 1;
                while (lo > 0 && c->applied[lo - 1].context == top.context)
                    --lo;
                const de__context_set *set = &c->sets[top.context];
                u32 m = 0;
                for (u32 k = lo; k < i && m < DE_INPUT_CONTEXT_MAX_MAPS; ++k)
                    if (c->applied[k].map < st->map_count)
                        maps[m++] = de_ep_cstr(st->maps[c->applied[k].map].name);
                o->name = de_ep_cstr(set->name);
                o->maps.items = maps;
                o->maps.count = m;
                o->blocks_lower = (set->flags & DE_INPUT_CONTEXT_PASSTHROUGH) == 0;
                i = lo;
            } else {
                const char *name = top.map < st->map_count ? st->maps[top.map].name : "";
                maps[0] = de_ep_cstr(name);
                o->name = de_ep_cstr(name);
                o->maps.items = maps;
                o->maps.count = 1;
                o->blocks_lower = top.map < DE_ACTION_MAX_MAPS &&
                                  (c->map_flags[top.map] & DE_INPUT_CONTEXT_PASSTHROUGH) == 0;
                --i;
            }
        }
    }
    out->contexts.items = g_in.contexts;
    out->contexts.count = n;
}

bool de__ed_input_sample(de__editor *ed, u32 id, de_ep_input_monitor_sample *out) {
    ed_in_sub *s = NULL;
    for (u32 i = 0; i < IN_SUBS && s == NULL; ++i)
        if (id != 0 && g_in.subs[i].id == id)
            s = &g_in.subs[i];
    const de_input *in = ed->e != NULL ? ed->e->input : NULL;
    if (s == NULL || in == NULL)
        return false;
    memset(out, 0, sizeof *out);
    out->subscription = s->id;
    out->frame = de__ed_frame(ed);
    sample_devices(in, out);
    sample_actions(in, s, out);
    sample_contexts(in, out);
    return true;
}

u32 de__ed_input_samples(u32 id) {
    for (u32 i = 0; i < IN_SUBS; ++i)
        if (id != 0 && g_in.subs[i].id == id)
            return g_in.subs[i].sent;
    return 0;
}

static void tick(de__editor *ed) {
    const de_input *in = ed->e != NULL ? ed->e->input : NULL;
    track_actions(in);
    if (in == NULL ||
        (ed->mode != DE_EP_WORLD_MODE_PLAY && ed->mode != DE_EP_WORLD_MODE_SIMULATE))
        return;
    const u64 now = de_time_now_ns();
    for (u32 i = 0; i < IN_SUBS; ++i) {
        ed_in_sub *s = &g_in.subs[i];
        if (s->id == 0 || (s->next_ns != 0 && now < s->next_ns))
            continue;
        /* Keep the cadence, but never send a burst after a stall. */
        s->next_ns = s->next_ns == 0 || now - s->next_ns > s->period_ns ? now + s->period_ns
                                                                        : s->next_ns + s->period_ns;
        de_ep_input_monitor_sample sample;
        if (!de__ed_input_sample(ed, s->id, &sample))
            continue;
        ++s->sent;
        mpack_writer_t nw;
        const u64 seq = de__ed_note_begin(ed, &nw);
        de_ep_write_notify_input_monitor(&nw, seq, &sample);
        de__ed_send(ed, &nw, DE_EP_KIND_NOTIFICATION);
    }
}

static void reset(de__editor *ed) {
    (void)ed;
    memset(g_in.subs, 0, sizeof g_in.subs);
    forget_actions();
}

void de__ed_input_register(de__editor *ed) {
    memset(&g_in, 0, sizeof g_in);
    de__ed_on(ed, DE_EP_METHOD_INPUT_LOAD_ACTION_MAPS, load_action_maps);
    de__ed_on(ed, DE_EP_METHOD_INPUT_MONITOR_SUBSCRIBE, monitor_subscribe);
    de__ed_on_tick(ed, tick);
    de__ed_on_reset(ed, reset);
    de__ed_add_capability(ed, IN_ACTION_MAPS);
    de__ed_add_capability(ed, IN_MONITOR);
}
