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

#endif /* DE_PLATFORM_SDL_INTERNAL_H */
