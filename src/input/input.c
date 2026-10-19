/* input.c: input state lifetime, the frame step, ECS attachment and injection (DENG-83).
 * Event ingestion is in input_state.c, the queries too; key names in keys.c. */
#include "input_internal.h"

#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>

#include <string.h>

/* The platform's raw numbering equals de_input's enums (Decision: Raw input is a translated event
 * ring in de_input numbering), so raw codes are used as indices without a table. The raw side
 * documents the values in de_platform.h; these checks pin the de_input side to them. */
DE_STATIC_ASSERT(DE_KEY_A == 4 && DE_KEY_Z == 29 && DE_KEY_RETURN == 40 && DE_KEY_F1 == 58 &&
                     DE_KEY_LSHIFT == 225 && DE_KEY_RGUI == 231,
                 "de_key must be the USB HID usage code (= de_raw_key.code)");
DE_STATIC_ASSERT(DE_KEY_COUNT_ % 64 == 0, "key bitsets use whole 64-bit words");
DE_STATIC_ASSERT(DE_MOUSE_LEFT == 0 && DE_MOUSE_RIGHT == 1 && DE_MOUSE_MIDDLE == 2 &&
                     DE_MOUSE_X1 == 3 && DE_MOUSE_X2 == 4 && DE_MOUSE_BUTTON_COUNT_ == 5,
                 "de_mouse_button must match de_raw_mouse_button.button");
DE_STATIC_ASSERT(DE_PAD_SOUTH == 0 && DE_PAD_EAST == 1 && DE_PAD_WEST == 2 && DE_PAD_NORTH == 3 &&
                     DE_PAD_BACK == 4 && DE_PAD_GUIDE == 5 && DE_PAD_START == 6 &&
                     DE_PAD_LSTICK == 7 && DE_PAD_RSTICK == 8 && DE_PAD_LSHOULDER == 9 &&
                     DE_PAD_RSHOULDER == 10 && DE_PAD_DPAD_UP == 11 && DE_PAD_DPAD_DOWN == 12 &&
                     DE_PAD_DPAD_LEFT == 13 && DE_PAD_DPAD_RIGHT == 14 && DE_PAD_MISC1 == 15 &&
                     DE_PAD_PADDLE1 == 16 && DE_PAD_PADDLE2 == 17 && DE_PAD_PADDLE3 == 18 &&
                     DE_PAD_PADDLE4 == 19 && DE_PAD_TOUCHPAD == 20 && DE_PAD_BUTTON_COUNT_ == 21,
                 "de_pad_button must match de_raw_pad_button.button (0..20)");
DE_STATIC_ASSERT(DE_PAD_BUTTON_COUNT_ <= 32, "pad buttons are a u32 bitset");
DE_STATIC_ASSERT(DE_AXIS_LEFT_X == 0 && DE_AXIS_LEFT_Y == 1 && DE_AXIS_RIGHT_X == 2 &&
                     DE_AXIS_RIGHT_Y == 3 && DE_AXIS_LEFT_TRIGGER == 4 &&
                     DE_AXIS_RIGHT_TRIGGER == 5 && DE_PAD_AXIS_COUNT_ == 6,
                 "de_pad_axis must match de_raw_pad_axis.axis");
/* Two separate checks against literals: MSVC warns (C5287) when two enum types meet in one
 * operator, also through an (int) cast (DENG-369). */
DE_STATIC_ASSERT(DE_PAD_TYPE_GENERIC == 0 && DE_PAD_TYPE_XBOX == 1 && DE_PAD_TYPE_PS4 == 2 &&
                     DE_PAD_TYPE_PS5 == 3 && DE_PAD_TYPE_SWITCH == 4 && DE_PAD_TYPE_STEAM == 5,
                 "de_pad_type values");
DE_STATIC_ASSERT(DE_PLATFORM_PAD_GENERIC == 0 && DE_PLATFORM_PAD_XBOX == 1 &&
                     DE_PLATFORM_PAD_PS4 == 2 && DE_PLATFORM_PAD_PS5 == 3 &&
                     DE_PLATFORM_PAD_SWITCH == 4 && DE_PLATFORM_PAD_STEAM == 5,
                 "de_platform_pad_type must have the de_pad_type values");
DE_STATIC_ASSERT(DE_INPUT_MAX_PADS < (1u << DE__INPUT_PAD_SLOT_BITS),
                 "slot index + 1 must fit the handle's slot bits");

/* ---------- Lifetime ---------- */

de_result de_input_create(const de_input_desc *desc, de_input **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_input_create: out is required");
    *out = NULL;
    de_input_desc d;
    memset(&d, 0, sizeof d);
    if (desc != NULL)
        d = *desc;
    const de_allocator *a = d.allocator != NULL ? d.allocator : de_heap_allocator();
    const u32 tag = de_mem_register_tag("input");
    de_input *in = (de_input *)DE_ALLOC(a, sizeof *in, DE_ALIGNOF(de_input), tag);
    if (in == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_input_create: %u bytes", (unsigned)sizeof *in);
    memset(in, 0, sizeof *in);
    in->alloc = *a;
    in->mem_tag = tag;
    in->log = de_log_register_category("input", DE_LOG_INFO);
    in->window = d.window;
    in->pad_sensors_off = d.pad_sensors_off;
    in->comp_cursor = -1;
    in->comp_length = -1;

    de_result r = de__actions_init(in);
    if (r == DE_OK) {
        r = de__gyro_init(in);
        if (r == DE_OK) {
            r = de__haptics_init(in);
            if (r != DE_OK)
                de__gyro_shutdown(in);
        }
        if (r != DE_OK)
            de__actions_shutdown(in);
    }
    if (r != DE_OK) {
        DE_FREE(&in->alloc, in, sizeof *in);
        return DE_FAIL_WRAP(r, "de_input_create: hook init");
    }
    *out = in;
    return DE_OK;
}

void de_input_destroy(de_input *in) {
    if (in == NULL)
        return;
    de__haptics_shutdown(in);
    de__gyro_shutdown(in);
    de__actions_shutdown(in);
    de_allocator a = in->alloc;
    DE_FREE(&a, in, sizeof *in);
}

/* ---------- ECS ---------- */

static void input_actions_system(de_system_ctx *ctx) {
    de_input *in = (de_input *)ctx->user;
    in->action_frame_runs++;
    de__actions_frame_update(in, ctx->dt);
}

static void input_fixed_tick_system(de_system_ctx *ctx) {
    de_input *in = (de_input *)ctx->user;
    in->action_fixed_runs++;
    de__actions_fixed_tick(in, ctx->tick);
}

static de_result last_error_or(de_result fallback) {
    const de_error *e = de_error_last();
    return (e != NULL && e->result != DE_OK) ? e->result : fallback;
}

static de_resource_id insert_resource(de_input *in, de_world *w) {
    de_type_registry *types = de_world_types(w);
    if (types == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_input_attach: the world has no type registry");
        return (de_resource_id){0};
    }
    /* A pointer has no field kind, so the type has no fields: it is never serialized or
     * snapshotted, only looked up (as de_audio_resource). Registering it again keeps its id. */
    de_type_desc d;
    memset(&d, 0, sizeof d);
    d.name = "de_input_resource";
    d.size = (u32)sizeof(de_input_resource);
    d.align = (u32)DE_ALIGNOF(de_input_resource);
    d.version = 1;
    d.is_pod = true;
    const de_type_id t = de_type_register(types, &d);
    if (!DE_HANDLE_VALID(t)) {
        DE_FAIL_WRAP(last_error_or(DE_ERR_INTERNAL), "de_input_attach: registering the type");
        return (de_resource_id){0};
    }
    /* One input state per world: the system names are fixed. */
    const de_resource_id found = de_resource_find(w, t);
    if (DE_HANDLE_VALID(found)) {
        const de_input_resource *cur = (const de_input_resource *)de_resource_get(w, found);
        if (cur != NULL && cur->input != NULL && cur->input != in) {
            DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_input_attach: the world has another input state");
            return (de_resource_id){0};
        }
    }
    de_input_resource res;
    res.input = in;
    const de_resource_id id = de_resource_insert(w, t, &res);
    if (!DE_HANDLE_VALID(id))
        DE_FAIL_WRAP(last_error_or(DE_ERR_INTERNAL), "de_input_attach: inserting the resource");
    return id;
}

static de_system add_system(de_input *in, de_world *w, const char *name, de_schedule s,
                            de_system_fn fn) {
    de_resource_access acc;
    acc.id = in->resource;
    acc.access = DE_WRITE;
    de_system_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.schedule = s;
    d.fn = fn;
    d.user = in;
    d.resources = &acc;
    d.resource_count = 1;
    return de_system_add(w, &d);
}

de_result de_input_attach(de_input *in, de_world *w) {
    if (in == NULL || w == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_input_attach: in and w are required");
    if (in->world != NULL && in->world != w)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_input_attach: already attached to another world (detach first)");
    const de_resource_id id = insert_resource(in, w);
    if (!DE_HANDLE_VALID(id))
        return last_error_or(DE_ERR_INTERNAL);
    in->resource = id;
    if (in->world == w)
        return DE_OK;

    in->sys_actions =
        add_system(in, w, "input.actions", DE_SCHEDULE_PRE_UPDATE, input_actions_system);
    if (!DE_HANDLE_VALID(in->sys_actions)) {
        const de_result r = last_error_or(DE_ERR_INTERNAL);
        de_resource_remove(w, id);
        in->resource = (de_resource_id){0};
        return DE_FAIL_WRAP(r, "de_input_attach: adding system input.actions");
    }
    in->sys_fixed =
        add_system(in, w, "input.fixed_tick", DE_SCHEDULE_FIXED_UPDATE, input_fixed_tick_system);
    if (!DE_HANDLE_VALID(in->sys_fixed)) {
        const de_result r = last_error_or(DE_ERR_INTERNAL);
        de_system_remove(w, in->sys_actions);
        in->sys_actions = (de_system){0};
        de_resource_remove(w, id);
        in->resource = (de_resource_id){0};
        return DE_FAIL_WRAP(r, "de_input_attach: adding system input.fixed_tick");
    }
    in->world = w;
    return DE_OK;
}

void de_input_detach(de_input *in) {
    if (in == NULL || in->world == NULL)
        return;
    de_system_remove(in->world, in->sys_fixed);
    de_system_remove(in->world, in->sys_actions);
    de_resource_remove(in->world, in->resource);
    in->sys_fixed = (de_system){0};
    in->sys_actions = (de_system){0};
    in->resource = (de_resource_id){0};
    in->world = NULL;
}

/* ---------- Window, focus, relative mode, text input ---------- */

static de_window input_window(const de_input *in) {
    return DE_HANDLE_VALID(in->window) ? in->window : in->focus_window;
}

static void apply_relative(de_input *in) {
    const de_window w = input_window(in);
    if (!DE_HANDLE_VALID(w))
        return; /* applied once a window is known */
    /* Headless or refused: keep the requested mode (the state still follows it). */
    if (de_platform_mouse_relative(w, in->relative) != DE_OK)
        de_error_clear();
    in->relative_dirty = false;
}

void de_mouse_set_relative(de_input *in, bool relative) {
    if (in == NULL)
        return;
    in->relative = relative;
    in->relative_dirty = true;
    apply_relative(in);
}

bool de_mouse_relative(const de_input *in) {
    return in != NULL && in->relative;
}

void de_input_app_event(de_input *in, const de_app_event *ev) {
    if (in == NULL || ev == NULL || ev->kind != DE_EV_WINDOW_FOCUS)
        return;
    if (ev->a != 0) {
        if (DE_HANDLE_VALID(ev->window) && ev->window.id != in->focus_window.id) {
            in->focus_window = ev->window;
            if (in->relative && !DE_HANDLE_VALID(in->window))
                in->relative_dirty = true;
        }
    } else {
        in->focus_lost_pending = true;
    }
}

de_result de_text_input_start(de_input *in, const de_platform_text_area *area) {
    if (in == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_text_input_start: in is required");
    const de_window w = input_window(in);
    if (!DE_HANDLE_VALID(w))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_text_input_start: no window known yet");
    DE_TRY(de_platform_text_input_start(w, area));
    in->text_active = true;
    return DE_OK;
}

de_result de_text_input_area(de_input *in, const de_platform_text_area *area) {
    if (!in || !area)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "IME area requires input and area");
    de_window window = input_window(in);
    if (!window.id)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "IME area has no input window");
    return de_platform_text_input_area(window, area);
}

void de_text_input_stop(de_input *in) {
    if (in == NULL)
        return;
    const de_window w = input_window(in);
    if (in->text_active && DE_HANDLE_VALID(w))
        de_platform_text_input_stop(w);
    in->text_active = false;
    in->composition[0] = '\0';
    in->comp_cursor = -1;
    in->comp_length = -1;
}

/* ---------- Frame ---------- */

void de__input_begin_frame_dt(de_input *in, f32 dt) {
    if (in == NULL)
        return;
    in->frame++;
    de__input_clear_frame(in);

    /* Injected events first (tests, replay), then the platform ring, both in arrival order. */
    for (u32 i = 0; i < in->inject_count; ++i)
        de__input_apply(in, &in->inject[i]);
    in->inject_count = 0;

    de__input_event buf[64];
    for (;;) {
        const u32 n = de_platform_input_events(buf, (u32)(sizeof buf / sizeof buf[0]));
        for (u32 i = 0; i < n; ++i)
            de__input_apply(in, &buf[i]);
        if (n < (u32)(sizeof buf / sizeof buf[0]))
            break;
    }

    if (in->focus_lost_pending) {
        in->focus_lost_pending = false;
        de__input_release_all(in);
    }
    if (in->relative_dirty)
        apply_relative(in);

    de__gyro_update(in, dt);
    de__haptics_update(in, dt);
    de__glyphs_frame_update(in); /* DENG-88 */
}

void de_input_begin_frame(de_input *in) {
    if (in == NULL)
        return;
    const u64 now = de_time_now_ns();
    f32 dt = 0.0f;
    if (in->last_frame_ns != 0 && now > in->last_frame_ns)
        dt = (f32)((f64)(now - in->last_frame_ns) * 1e-9);
    in->last_frame_ns = now;
    de__input_begin_frame_dt(in, dt);
}

/* ---------- Injection ---------- */

void de__input_inject(de_input *in, const de__input_event *ev) {
    if (in == NULL || ev == NULL)
        return;
    if (in->inject_count >= DE__INPUT_INJECT_CAP) {
        in->inject_dropped++;
        return;
    }
    in->inject[in->inject_count++] = *ev;
}

void de__input_inject_pad_info(de_input *in, const de_platform_pad_info *info) {
    if (in == NULL || info == NULL || info->id == 0)
        return;
    for (u32 i = 0; i < in->inject_pad_info_count; ++i) {
        if (in->inject_pad_info[i].id == info->id) {
            in->inject_pad_info[i] = *info;
            return;
        }
    }
    if (in->inject_pad_info_count < DE_INPUT_MAX_PADS)
        in->inject_pad_info[in->inject_pad_info_count++] = *info;
}
