/* sdl_internal.h: shared state of the SDL3 platform backend (src/platform/sdl/, DENG-245).
 *
 * Internal to the platform module: SDL types may appear here, never in include/dengine/.
 * Every function is main-thread only, like the public window and event API.
 *
 * Hooks for the other S03 platform tasks:
 * - DENG-246 (raw input): de_platform_pump() hands every input event (keyboard, mouse, joystick,
 *   gamepad, touch, clipboard, sensor, pen) to de__platform_sdl_route_input(). That queues it in
 * the input ring below, or calls the hook installed with de__platform_sdl_set_input_hook().
 * DENG-246 either installs a hook or drains the ring with de__platform_sdl_input_drain() each
 * frame.
 * - DENG-250 (native surfaces): de__platform_sdl_window_get() maps a de_window to its SDL_Window.
 *   surface_sdl.c adds the window flags for the surface kind, and releases a window's Metal view
 *   before the window and the Vulkan loader before SDL_Quit (the hooks at the end of this file).
 */
#ifndef DE_PLATFORM_SDL_INTERNAL_H
#define DE_PLATFORM_SDL_INTERNAL_H

#include <dengine/de_platform.h>

#include <SDL3/SDL.h>

/** Capacity of the input ring, in events. A full ring drops the newest event and counts it. */
#define DE__PLATFORM_SDL_INPUT_RING 512

/** Reserved address space of the per-pump drop-path arena. */
#define DE__PLATFORM_SDL_PATH_ARENA_BYTES ((size_t)16 << 20)

/** One window slot. `gen` is never 0 for a slot that was ever used. */
typedef struct de__platform_sdl_window {
    SDL_Window *sdl;     /**< NULL when the slot is free */
    SDL_WindowID id;     /**< SDL's window id, to map events back to the slot */
    u32 gen;             /**< generation, 1 .. 0xFFFFFF, bumped on destroy */
    de_window_mode mode; /**< the mode last asked for */
    bool minimized;      /**< last DE_EV_WINDOW_MINIMIZED state sent */
    /* Graphics surface (DENG-250, surface_sdl.c). */
    de_window_surface surface; /**< what the window was created for */
    SDL_MetalView metal_view;  /**< made by the first de_window_native() of a METAL window */
} de__platform_sdl_window;

/** Called for each routed input event, instead of queueing it in the ring. */
typedef void (*de__platform_sdl_input_hook)(const SDL_Event *ev, void *user);

/** The platform state, created by de_platform_init(). */
typedef struct de__platform_sdl_state {
    const de_allocator *alloc;
    u32 main_thread;
    bool headless;
    char app_name[128];
    de__platform_sdl_window windows[DE_WINDOW_MAX];
    /* Displays in SDL's order; refreshed on init and on display added/removed events. */
    SDL_DisplayID displays[32];
    u32 display_count;
    /* Input ring (single-threaded: the pump pushes, the input layer pops, both on main). */
    SDL_Event input[DE__PLATFORM_SDL_INPUT_RING];
    u32 input_head, input_count, input_dropped;
    de__platform_sdl_input_hook input_hook;
    void *input_hook_user;
    /* Per-pump arena for DE_EV_DROP_FILE paths; reset at the start of every pump. */
    de_arena *paths;
    /* Raw input devices (DENG-246, input_sdl.c); NULL until de__platform_sdl_devices_init(). */
    struct de__platform_sdl_devices *devices;
    /* The engine's one reference on SDL's Vulkan loader (DENG-250), taken lazily. */
    bool vulkan_loaded;
} de__platform_sdl_state;

/** The state, or NULL before de_platform_init() / after de_platform_shutdown(). */
de__platform_sdl_state *de__platform_sdl_get(void);

/** Debug/Dev check that the caller is the thread that called de_platform_init(). */
#define DE__PLATFORM_SDL_ASSERT_MAIN(st) DE_ASSERT((st)->main_thread == de_thread_current_id())

/* ---- displays (display_sdl.c) ---- */

/** Re-reads SDL's display list into the state. */
void de__platform_sdl_displays_refresh(de__platform_sdl_state *st);
/** Index of `id` in the cached display list, or ~0u. */
u32 de__platform_sdl_display_index(const de__platform_sdl_state *st, SDL_DisplayID id);
/** Fills `out` for `id`; `win` (may be NULL) supplies the SDR white level and HDR headroom. */
void de__platform_sdl_display_fill(de__platform_sdl_state *st, SDL_DisplayID id, SDL_Window *win,
                                   de_display_info *out);

/* ---- windows (window_sdl.c) ---- */

/** The SDL window of `w`, or NULL for a stale handle (DENG-250 uses this for surfaces). */
SDL_Window *de__platform_sdl_window_get(de_window w);
/** The engine handle of SDL window `id`, or a zero handle when it is not an engine window. */
de_window de__platform_sdl_window_from_id(const de__platform_sdl_state *st, SDL_WindowID id);
/** The slot of SDL window `id`, or NULL. */
de__platform_sdl_window *de__platform_sdl_slot_from_id(de__platform_sdl_state *st, SDL_WindowID id);
/** Destroys every open window (shutdown). */
void de__platform_sdl_windows_destroy_all(de__platform_sdl_state *st);

/* ---- input routing (events_sdl.c) ---- */

/**
 * Hands one input event to the input layer: the installed hook if there is one, otherwise the
 * input ring. de_platform_pump() calls it; DENG-246 does not need to.
 */
void de__platform_sdl_route_input(const SDL_Event *ev);
/** Installs (or with NULL removes) the input hook. Events already in the ring stay there. */
void de__platform_sdl_set_input_hook(de__platform_sdl_input_hook fn, void *user);
/** Pops up to `max` queued input events, oldest first, into `out`; returns how many. */
u32 de__platform_sdl_input_drain(SDL_Event *out, u32 max);
/** Input events dropped because the ring was full, since init. */
u32 de__platform_sdl_input_dropped(void);
/** True for the SDL event types that de__platform_sdl_route_input() takes. */
bool de__platform_sdl_is_input_event(Uint32 type);

/* ---- raw input devices (DENG-246: input, gamepad, sensor, clipboard, cursor, dialog _sdl.c) ----
 *
 * de__platform_sdl_devices_init() runs at the end of de_platform_init(): it allocates the raw
 * input ring, starts the gamepad and sensor subsystems (not when headless) and installs
 * de__platform_sdl_devices_route() as the input hook above, so every input event is translated
 * while the pump polls it. That matters: SDL frees event strings (text input) at the next poll.
 * Removing the hook (tests of the DENG-245 ring) sends events to the SDL_Event ring instead. */

/** Device sensors open at once. */
#define DE__PLATFORM_SDL_SENSOR_MAX 8

typedef struct de__platform_sdl_pad {
    SDL_JoystickID id; /**< 0 when the slot is free */
    SDL_Gamepad *pad;
} de__platform_sdl_pad;

typedef struct de__platform_sdl_sensor {
    SDL_SensorID id; /**< 0 when the slot is free */
    SDL_Sensor *sensor;
} de__platform_sdl_sensor;

typedef struct de__platform_sdl_devices {
    bool pads_ok;    /**< the gamepad subsystem started */
    bool sensors_ok; /**< the sensor subsystem started */
    /* Raw input ring: the pump pushes, de_platform_input_events() pops (both main thread). */
    u32 ring_head, ring_count;
    u64 ring_dropped;
    /* SDL timestamp → de_time_now_ns() (input_sdl.c). */
    bool clock_synced;
    i64 clock_offset;        /**< engine ns - SDL ns */
    Uint64 clock_sync_ticks; /**< SDL ns when the offset was measured */
    u64 last_time_ns;        /**< last converted time, so event times never go backwards */
    de__platform_sdl_pad pads[DE_PLATFORM_PAD_MAX]; /**< in connection order, no gaps */
    u32 pad_count;
    de__platform_sdl_sensor sensors[DE__PLATFORM_SDL_SENSOR_MAX];
    SDL_Cursor *cursors[DE_CURSOR_COUNT_]; /**< system cursors, created on first use */
    de_raw_input_event ring[DE_PLATFORM_INPUT_RING];
} de__platform_sdl_devices;

/** Starts the devices part of the platform (end of de_platform_init()). */
de_result de__platform_sdl_devices_init(de__platform_sdl_state *st);
/** Cancels open dialogs, closes pads and sensors, frees cursors and the ring (start of
 * de_platform_shutdown(), while SDL still runs). */
void de__platform_sdl_devices_shutdown(de__platform_sdl_state *st);
/** The input hook: translates one SDL input event into the raw input ring. */
void de__platform_sdl_devices_route(const SDL_Event *ev, void *user);
/** Appends one event to the raw input ring; a full ring drops its oldest event. */
void de__platform_sdl_ring_push(de__platform_sdl_devices *dv, const de_raw_input_event *ev);
/** SDL event timestamp (SDL_GetTicksNS() base) in the de_time_now_ns() timebase; never earlier
 * than the previous result. */
u64 de__platform_sdl_time_ns(de__platform_sdl_devices *dv, Uint64 sdl_ticks_ns);

/* gamepad_sdl.c */
/** Opens gamepad `id` after SDL_EVENT_GAMEPAD_ADDED; false when it could not be opened (the
 * event is then not reported). */
bool de__platform_sdl_pad_added(de__platform_sdl_devices *dv, SDL_JoystickID id);
/** Closes gamepad `id` after SDL_EVENT_GAMEPAD_REMOVED; false when it was not open. */
bool de__platform_sdl_pad_removed(de__platform_sdl_devices *dv, SDL_JoystickID id);
/** True when `id` is an open gamepad (events of other joysticks are dropped). */
bool de__platform_sdl_pad_known(const de__platform_sdl_devices *dv, SDL_JoystickID id);
void de__platform_sdl_pads_close_all(de__platform_sdl_devices *dv);
de_platform_sensor_type de__platform_sdl_sensor_type_from_sdl(SDL_SensorType t);
SDL_SensorType de__platform_sdl_sensor_type_to_sdl(de_platform_sensor_type t);

/* sensor_sdl.c */
void de__platform_sdl_sensors_close_all(de__platform_sdl_devices *dv);

/* cursor_sdl.c */
void de__platform_sdl_cursors_free(de__platform_sdl_devices *dv);

/* dialog_sdl.c */
/** Calls the callbacks of finished dialogs (end of every de_platform_pump()). */
void de__platform_sdl_dialogs_dispatch(void);
/** Calls every outstanding callback: finished ones with their result, open ones with
 * DE_ERR_CANCELLED. A late OS callback is dropped. */
void de__platform_sdl_dialogs_shutdown(void);
/** How a dialog is shown: SDL_ShowFileDialogWithProperties() by default. Tests install a fake
 * (NULL restores the default) to run the delivery path without a real dialog. */
typedef void(SDLCALL *de__platform_sdl_dialog_show_fn)(SDL_FileDialogType type,
                                                       SDL_DialogFileCallback cb, void *userdata,
                                                       SDL_PropertiesID props);
void de__platform_sdl_dialog_set_show(de__platform_sdl_dialog_show_fn fn);

/* ---- graphics surfaces (surface_sdl.c, surface_metal.m; DENG-250) ---- */

/**
 * The SDL window flags for `surface`, checked before the window is created. VULKAN opens the
 * Vulkan loader first. DE_ERR_UNSUPPORTED (no Vulkan loader, no Metal on this platform) or
 * DE_ERR_INVALID_ARGUMENT (bad value).
 */
de_result de__platform_sdl_surface_flags(de__platform_sdl_state *st, de_window_surface surface,
                                         SDL_WindowFlags *flags);
/** Releases the surface objects of a window slot (its Metal view). Called before
 * SDL_DestroyWindow. */
void de__platform_sdl_surface_release(de__platform_sdl_window *s);
/** Drops the engine's Vulkan loader reference. Called at shutdown, after the windows are gone. */
void de__platform_sdl_surface_shutdown(de__platform_sdl_state *st);

#if defined(SDL_PLATFORM_APPLE)
/** surface_metal.m: sets the Metal layer's contents scale to `scale` (the window's pixel
 * density). */
void de__platform_sdl_metal_layer_set_scale(void *layer, float scale);
#endif

#endif /* DE_PLATFORM_SDL_INTERNAL_H */
