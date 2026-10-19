/* de_platform.h — windows, time, threads, files, async I/O, file watch, dynamic libs.
 * Implemented on a vendored windowing library + per-OS code. No third-party type or name
 * appears here. SPEC.md §6. */
#ifndef DE_PLATFORM_H
#define DE_PLATFORM_H
#include "de_base.h"
#include "de_memory.h"
DE_EXTERN_C_BEGIN

/* ==========================================================================================
 * Platform init, displays, windows and the event pump (S03, DENG-245). SPEC §6.
 * Implementation: src/platform/sdl/ (third_party/sdl3). A DE_HEADLESS build does not link the
 * windowing library at all: src/platform/sdl/platform_headless.c provides the same functions,
 * and every window function there returns DE_ERR_UNSUPPORTED (or a zero value).
 *
 * Threading: every function in this section is main-thread only (the thread that called
 * de_platform_init()). macOS requires windows and events on the main thread; the S04
 * main-thread queue and S05 DE_SYSTEM_MAIN_THREAD systems rely on this rule. Debug and Dev
 * builds assert it.
 * ========================================================================================== */

/* ---------- Platform init ---------- */

/** Flags for de_platform_desc.flags. */
typedef enum de_platform_flags {
    /** No video subsystem: no windows, no displays. de_window_create() returns
     * DE_ERR_UNSUPPORTED. The event pump still delivers DE_EV_QUIT (SIGINT/SIGTERM). Servers and
     * tools use this; a DE_HEADLESS build always behaves as if it were set. */
    DE_PLATFORM_HEADLESS = 1u << 0
} de_platform_flags;

/** de_platform_init() settings. `(de_platform_desc){0}` (or a NULL desc) gives the defaults. */
typedef struct de_platform_desc {
    const char *app_name;       /**< shown by the OS (macOS menu, audio mixers); NULL = "DEngine" */
    const char *app_org;        /**< developer / publisher name; NULL = none */
    const char *app_version;    /**< free-form version string; NULL = none */
    const char *app_identifier; /**< reverse-DNS id, e.g. "com.example.game"; NULL = none */
    u32 flags;                  /**< de_platform_flags */
    /** The platform state, window table, input ring and drop-path arena bookkeeping. NULL =
     * de_heap_allocator(). Must outlive de_platform_shutdown(). The windowing library itself
     * allocates with the system heap. */
    const de_allocator *allocator;
} de_platform_desc;

/**
 * Starts the platform layer: the windowing library with video (or only events, headless).
 *
 * Call it once, on the main thread, before any other function in this section. It tells the
 * windowing library that the engine owns main(), sets the app metadata, and starts video.
 * The strings in `desc` are copied.
 *
 * @return DE_OK; DE_ERR_BUSY if already initialised; DE_ERR_OUT_OF_MEMORY; or
 *         DE_ERR_UNSUPPORTED when no video driver can start (no display server). A caller that
 *         can run without windows may retry with DE_PLATFORM_HEADLESS.
 */
DE_API de_result de_platform_init(const de_platform_desc *desc);

/** Destroys every window that is still open, shuts video down and frees the platform state.
 * Does nothing when the platform is not initialised. Main thread only. */
DE_API void de_platform_shutdown(void);

/** True when the platform runs without video (DE_PLATFORM_HEADLESS, a DE_HEADLESS build, or
 * not initialised). */
DE_API bool de_platform_headless(void);

/* ---------- Displays ----------
 * Displays are addressed by index, 0 .. de_display_count() - 1, in the OS order (the primary
 * display is not always 0; see de_display_info.primary). Indices can change when a display is
 * added or removed (DE_EV_DISPLAY_CHANGED with DE_DISPLAY_ADDED / DE_DISPLAY_REMOVED). */

/** Display description. Sizes are in pixels. */
typedef struct de_display_info {
    u32 width, height; /**< desktop mode in pixels (points × pixel density) */
    u32 refresh_hz;    /**< desktop refresh rate, rounded; 0 = unknown */
    /** UI scale the user chose for this display (1.0 = 100 %, 2.0 on a Retina display). Not the
     * same as the pixel density of a window (see de_window_size_pixels()). */
    f32 content_scale;
    /** The display can show more than SDR white now or on demand (Windows HDR on, macOS EDR,
     * HDR on Wayland): there is headroom above SDR white. */
    bool hdr_capable;
    /**
     * SDR white and peak luminance, in nits, in the scRGB convention (1.0 = 80 nits). Known
     * per window: de_window_display() fills them from the window's SDR white level and HDR
     * headroom. de_display_info_get() uses an engine window on that display if there is one;
     * otherwise sdr_white_nits is 80 and max_nits is 80 for an SDR display or 0 (unknown) for
     * an HDR one. On macOS the SDR white level is always 1.0 (EDR), so only the ratio
     * max_nits / sdr_white_nits (= hdr_headroom) is meaningful there.
     */
    f32 sdr_white_nits, max_nits;
    f32 hdr_headroom; /**< max_nits / sdr_white_nits; 1.0 for SDR, 0 when unknown */
    bool primary;     /**< the OS primary display */
    char name[64];    /**< UTF-8, cut at 63 bytes; "" when the OS gives none */
} de_display_info;

/** A fullscreen display mode. */
typedef struct de_display_mode {
    u32 width, height; /**< pixels */
    f32 refresh_hz;    /**< exact rate, e.g. 59.94; 0 = unspecified */
    f32 pixel_density; /**< pixels per point: logical size = width / pixel_density */
} de_display_mode;

/** Number of connected displays. 0 when headless or not initialised. */
DE_API u32 de_display_count(void);

/** Describes display `index`. DE_ERR_INVALID_ARGUMENT for a bad index or NULL `out`;
 * DE_ERR_UNSUPPORTED when headless. */
DE_API de_result de_display_info_get(u32 index, de_display_info *out);

/**
 * Fullscreen modes of display `index`, best first (largest, then highest density, then highest
 * refresh rate). Writes at most `max` modes to `out` (which may be NULL when `max` is 0) and
 * returns the total number of modes, so `de_display_modes(i, NULL, 0)` asks for the count.
 * Returns 0 for a bad index or when headless.
 */
DE_API u32 de_display_modes(u32 index, de_display_mode *out, u32 max);

/* ---------- Windows ---------- */

/** Windows are typed handles with a generation (SPEC §5.1): a handle kept after
 * de_window_destroy() is rejected, even when its slot is reused. */
DE_HANDLE(de_window);

/** Most windows open at once. */
#define DE_WINDOW_MAX 16

typedef enum de_window_mode {
    DE_WINDOW_WINDOWED,
    DE_WINDOW_BORDERLESS, /**< borderless fullscreen at the desktop mode */
    DE_WINDOW_FULLSCREEN  /**< exclusive fullscreen at the display mode closest to the window */
} de_window_mode;

/** The graphics surface a window is created for (de_window_desc.surface, S03 DENG-250). The RHI
 * picks its backend before it opens the window, and the window is set up for that backend. */
typedef enum de_window_surface {
    /** No API-specific setup. D3D12 uses this: it only needs the Win32 window handle. */
    DE_WINDOW_SURFACE_NONE,
    /** Vulkan: the window can get a Vulkan surface (de_window_vulkan_create_surface()). The
     * Vulkan loader is opened first; without one, de_window_create() returns
     * DE_ERR_UNSUPPORTED. */
    DE_WINDOW_SURFACE_VULKAN,
    /** Metal (macOS): de_window_native() gives the window's Core Animation Metal layer.
     * DE_ERR_UNSUPPORTED where there is no Metal. */
    DE_WINDOW_SURFACE_METAL
} de_window_surface;

typedef struct de_window_desc {
    const char *title; /**< UTF-8; NULL = the app name */
    u32 width, height; /**< logical pixels (points); 0 = 1280x720 */
    de_window_mode mode;
    i32 display_index; /**< display to open on (centred); -1 = primary. Note that 0 in a zeroed
                            desc means display 0, which is usually but not always primary. */
    bool resizable;
    /** Full-resolution backbuffer on high-density displays (Retina): the pixel size is then the
     * logical size × the display's pixel density. Without it the pixel size is the logical size
     * on macOS (the OS scales the window up). */
    bool high_dpi;
    bool hidden; /**< create without showing it (tests, tools) */
    /** The graphics API that will present to this window (S03 DENG-250; see
     * de_window_surface). DE_WINDOW_SURFACE_NONE (zero) for D3D12 and windows nobody renders
     * to. */
    de_window_surface surface;
} de_window_desc;

/**
 * Opens a window. A NULL `desc` gives the defaults.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL `out`, not initialised, bad display index);
 *         DE_ERR_UNSUPPORTED (headless); DE_ERR_BUSY (DE_WINDOW_MAX windows open); or
 *         DE_ERR_INTERNAL when the OS refuses the window (the message has the reason).
 */
DE_API de_result de_window_create(const de_window_desc *desc, de_window *out);

/** Closes the window. A stale or zero handle is ignored. */
DE_API void de_window_destroy(de_window w);

/** Size of the drawable area in pixels (what a swapchain needs). 0 x 0 for a stale handle.
 * Either pointer may be NULL. */
DE_API void de_window_size_pixels(de_window w, u32 *width, u32 *height);

/** Physical pixel insets avoiding notches, rounded corners and system overlays. */
typedef struct de_window_insets {
    u32 left, top, right, bottom;
} de_window_insets;
/** Zero insets for a stale handle, headless platform, or an unrestricted window. */
DE_API de_window_insets de_window_safe_area(de_window w);

/**
 * Switches between windowed, borderless fullscreen and exclusive fullscreen. Exclusive
 * fullscreen uses the display mode closest to the window's logical size on the window's
 * display. The OS may apply the change later (macOS animates it); a hidden window applies it
 * when shown. de_window_get_mode() reports the mode asked for here.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (stale handle or bad mode); DE_ERR_NOT_FOUND (no
 *         fullscreen mode on that display); DE_ERR_INTERNAL when the OS refuses.
 */
DE_API de_result de_window_set_mode(de_window w, de_window_mode mode);

/** The mode last set by de_window_create() or de_window_set_mode(). DE_WINDOW_WINDOWED for a
 * stale handle. */
DE_API de_window_mode de_window_get_mode(de_window w);

/** Sets the title (UTF-8). A stale handle or NULL title is ignored. */
DE_API void de_window_set_title(de_window w, const char *title);

/** The display the window is mostly on, with the window's SDR white level and HDR headroom (see
 * de_display_info). All zero for a stale handle. */
DE_API de_display_info de_window_display(de_window w);

/* ---------- Native window handles and graphics surfaces (S03, DENG-250) ----------
 * For the RHI only (S09): the Win32 window handle (D3D12), the Core Animation Metal layer
 * (Metal) and Vulkan surfaces. Every OS and graphics-API type is passed as void * or u64, so
 * this header stays free of them. Main thread only, like the rest of the window API. A
 * DE_HEADLESS build has stubs: zeros, 0 and DE_ERR_UNSUPPORTED.
 *
 * Swapchain size: the windowing layer never resizes the swapchain images. On a
 * DE_EV_WINDOW_RESIZED event the RHI swapchain sets the Metal layer's drawable size, or
 * recreates the Vulkan swapchain, from de_window_size_pixels(). */

/** Native handles of a window. A field the platform or the window's surface kind does not have
 * is NULL. */
typedef struct de_native_window {
    /** Windows: the Win32 window handle. NULL elsewhere. */
    void *hwnd;
    /** macOS, windows created with DE_WINDOW_SURFACE_METAL: the Core Animation Metal layer
     * (cast it to the Metal layer class). The first de_window_native() call creates a Metal
     * view in the window for it; later calls return the same layer. The view and the layer
     * belong to the window: de_window_destroy() releases them. Each call sets the layer's
     * contents scale to the window's current pixel density (call again on DE_DISPLAY_SCALE).
     * The drawable size is the window's pixel size when the view is made; the RHI keeps it up
     * to date after that. NULL for other windows. */
    void *metal_layer;
    /** The windowing library's window, opaque. Only the platform layer uses it. */
    void *sdl_window;
} de_native_window;

/** The native handles of `w`. All NULL for a stale or zero handle, before de_platform_init(),
 * and when the Metal view cannot be created (the error context then says why). */
DE_API de_native_window de_window_native(de_window w);

/**
 * The instance extensions Vulkan needs to present to a window on this platform (for example
 * VK_KHR_surface and VK_EXT_metal_surface on macOS, VK_KHR_win32_surface on Windows). Writes up
 * to `max` extension names to `out` and returns how many there are, so `(NULL, 0)` counts. The
 * strings are static and stay valid until de_platform_shutdown().
 *
 * Opens the Vulkan loader the first time it is needed (the engine never links it, so builds
 * without the Vulkan SDK still link). The loader stays open until de_platform_shutdown().
 * Returns 0, with the error context set, when the platform is not initialised, is headless, or
 * there is no Vulkan loader.
 */
DE_API u32 de_window_vulkan_instance_extensions(const char **out, u32 max);

/** A function pointer from the Vulkan loader. Cast it to the real type before calling. */
typedef void (*de_window_vulkan_fn)(void);

/**
 * The loader's `vkGetInstanceProcAddr`, from the same Vulkan loader the surface functions use.
 * The RHI loads every other Vulkan function through it. Opens the loader like
 * de_window_vulkan_instance_extensions(); NULL, with the error context set, when that fails.
 * Valid until de_platform_shutdown().
 */
DE_API de_window_vulkan_fn de_window_vulkan_get_instance_proc_addr(void);

/**
 * Creates a Vulkan surface for `w`, which must have been created with
 * DE_WINDOW_SURFACE_VULKAN.
 *
 * @param vk_instance  The Vulkan instance handle, created through
 *                     de_window_vulkan_get_instance_proc_addr() with the extensions from
 *                     de_window_vulkan_instance_extensions() enabled.
 * @param vk_allocation_callbacks  Pointer to the Vulkan allocation callbacks, or NULL.
 * @param out_vk_surface  Receives the Vulkan surface handle (a 64-bit non-dispatchable handle);
 *                     0 on failure.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (stale handle, NULL instance or `out_vk_surface`, or a
 *         window not created for Vulkan); DE_ERR_UNSUPPORTED (headless, no Vulkan loader);
 *         DE_ERR_INTERNAL when Vulkan refuses (the message has the reason).
 */
DE_API de_result de_window_vulkan_create_surface(de_window w, void *vk_instance,
                                                 const void *vk_allocation_callbacks,
                                                 u64 *out_vk_surface);

/**
 * Destroys a surface from de_window_vulkan_create_surface(), with the same instance and
 * allocation callbacks. Destroy the swapchain first, and destroy the surface before its window
 * and its instance. A 0 surface or NULL instance is ignored.
 */
DE_API void de_window_vulkan_destroy_surface(void *vk_instance, u64 vk_surface,
                                             const void *vk_allocation_callbacks);

/* ---------- Event pump ---------- */

/** What de_platform_pump() returns. Input events (keyboard, mouse, gamepad, touch, pen,
 * sensors) are not app events: the pump translates them into the raw input ring, read with
 * de_platform_input_events() (section "Raw input and devices", DENG-246). */
typedef enum de_app_event_kind {
    DE_EV_QUIT,             /**< the user or the OS asks to quit (last window closed, Cmd-Q,
                                 SIGINT/SIGTERM) */
    DE_EV_WINDOW_RESIZED,   /**< `window`'s pixel size changed: `a` x `b` pixels */
    DE_EV_WINDOW_FOCUS,     /**< `a` = 1 keyboard focus gained, 0 lost */
    DE_EV_WINDOW_MINIMIZED, /**< `a` = 1 minimised, 0 restored or maximised */
    DE_EV_DISPLAY_CHANGED,  /**< `a` = display index (or ~0u if gone), `b` = de_display_change;
                                 `window` set for the per-window reasons */
    DE_EV_DROP_FILE,        /**< a file dropped on `window`; `path` (UTF-8) */
    DE_EV_LOW_MEMORY,       /**< the OS is low on memory: free caches */
    DE_EV_WINDOW_CLOSE      /**< the user asked to close `window` (close button). The window
                                 stays open until de_window_destroy(). With one window open,
                                 DE_EV_QUIT follows. */
} de_app_event_kind;

/** Why a DE_EV_DISPLAY_CHANGED event was sent (its `b` field). */
typedef enum de_display_change {
    DE_DISPLAY_WINDOW_MOVED, /**< `window` moved to display `a` */
    DE_DISPLAY_HDR,          /**< `window`'s HDR state, SDR white or headroom changed; query
                                  de_window_display() */
    DE_DISPLAY_SCALE,        /**< `window`'s pixel density or content scale changed */
    DE_DISPLAY_ADDED,        /**< display `a` was connected (indices may have changed) */
    DE_DISPLAY_REMOVED,      /**< a display was disconnected (`a` = ~0u; indices may have
                                  changed) */
    DE_DISPLAY_MODE          /**< display `a`'s desktop or current mode, orientation, content
                                  scale or bounds changed */
} de_display_change;

typedef struct de_app_event {
    de_app_event_kind kind;
    de_window window; /**< zero for app-wide and display-wide events */
    u32 a, b;
    /** DE_EV_DROP_FILE only: copied into a per-pump arena, valid until the next
     * de_platform_pump() or de_platform_shutdown(). NULL otherwise. */
    const char *path;
} de_app_event;

/**
 * Polls the OS event queue without blocking. Writes up to `max_events` app events to `events`
 * and returns how many. Input events are routed to the input layer on the way. When `events` is
 * full the rest stay queued for the next call, in order, so a caller that wants everything
 * calls again until the result is less than `max_events`. `max_events == 0` (`events` may then
 * be NULL) drains the queue, routes input and discards the app events.
 *
 * Main thread only. Never blocks; returns 0 when nothing is queued. Headless, it still reports
 * DE_EV_QUIT. Returns 0 when the platform is not initialised. Finished file dialogs
 * (de_platform_file_dialog()) call their callbacks from here, after the events are polled.
 */
DE_API u32 de_platform_pump(de_app_event *events, u32 max_events);

/* ==========================================================================================
 * End of the platform init, displays, windows and event pump sections (DENG-245).
 * ========================================================================================== */

/* ==========================================================================================
 * Raw input and devices (S03, DENG-246). SPEC §6, §15.
 * Implementation: src/platform/sdl/{input,gamepad,sensor,clipboard,cursor,dialog}_sdl.c; a
 * DE_HEADLESS build uses src/platform/sdl/devices_headless.c (no devices, DE_ERR_UNSUPPORTED).
 *
 * This is the raw layer below de_input (S21): device events and device control in engine
 * types. Action maps, rebinding, glyphs, gyro calibration and recording are de_input's job.
 * The platform layer never includes de_input.h; the numbering below is kept equal to de_input's
 * enums so that S21 can cast (de_input adds the matching _Static_asserts).
 *
 * Threading: main thread only, like the rest of the windowing API above (Debug/Dev builds
 * assert it). File-dialog callbacks also run on the main thread, inside de_platform_pump().
 * With DE_PLATFORM_HEADLESS (or a DE_HEADLESS build) there are no gamepads (every pad id gives
 * DE_ERR_NOT_FOUND), no device sensors, and clipboard, cursor, text input and dialogs return
 * DE_ERR_UNSUPPORTED or zero values. Keyboard and mouse events that reach the event queue are
 * still translated.
 * ========================================================================================== */

/* ---------- Raw input events ---------- */

/** Capacity of the raw input ring, in events. It is allocated by de_platform_init(); a full
 * ring drops its oldest event and counts it (de_platform_input_dropped()). */
#define DE_PLATFORM_INPUT_RING 1024

/** Kinds of de_raw_input_event. */
typedef enum de_raw_input_kind {
    DE_RAW_KEY,          /**< `key`: a key went down, up, or auto-repeated */
    DE_RAW_TEXT,         /**< `text`: committed text (typed or from an IME) */
    DE_RAW_MOUSE_MOVE,   /**< `mouse_move` */
    DE_RAW_MOUSE_BUTTON, /**< `mouse_button` */
    DE_RAW_MOUSE_WHEEL,  /**< `mouse_wheel` */
    DE_RAW_PAD_ADDED,    /**< gamepad `device` connected and opened (de_platform_pad_info()) */
    DE_RAW_PAD_REMOVED,  /**< gamepad `device` disconnected; its id is now invalid */
    DE_RAW_PAD_BUTTON,   /**< `pad_button` */
    DE_RAW_PAD_AXIS,     /**< `pad_axis` */
    DE_RAW_PAD_TOUCHPAD, /**< `pad_touch`: a finger on a gamepad touchpad (DualShock 4, DualSense)
                          */
    DE_RAW_PAD_SENSOR,   /**< `sensor`: gyro or accelerometer sample of gamepad `device` */
    DE_RAW_TEXT_EDIT     /**< `text`: IME composition (pre-edit) text, with the edit cursor */
} de_raw_input_kind;

/** Keyboard modifier flags in de_raw_key.mods. */
typedef enum de_key_mod {
    DE_KEYMOD_LSHIFT = 1u << 0,
    DE_KEYMOD_RSHIFT = 1u << 1,
    DE_KEYMOD_LCTRL = 1u << 2,
    DE_KEYMOD_RCTRL = 1u << 3,
    DE_KEYMOD_LALT = 1u << 4,
    DE_KEYMOD_RALT = 1u << 5,
    DE_KEYMOD_LGUI = 1u << 6, /**< Cmd on macOS, the Windows key elsewhere */
    DE_KEYMOD_RGUI = 1u << 7,
    DE_KEYMOD_NUM = 1u << 8,  /**< Num Lock on */
    DE_KEYMOD_CAPS = 1u << 9, /**< Caps Lock on */
    DE_KEYMOD_ALTGR = 1u << 10
} de_key_mod;

/** DE_RAW_KEY payload. */
typedef struct de_raw_key {
    /** USB HID usage code of the physical key (keyboard page 0x07): the same numbers as de_key
     * in de_input.h, e.g. 4 = A, 40 = Return, 225 = left Shift. Independent of the layout. */
    u16 code;
    u16 mods; /**< de_key_mod flags at the time of the event */
    /** Unicode code point the key produces in the current layout without modifiers (for
     * showing bindings), or 0 for keys that produce no character. */
    u32 sym;
    bool down;   /**< pressed (true) or released */
    bool repeat; /**< an OS auto-repeat of a key that is held down */
} de_raw_key;

/** DE_RAW_TEXT and DE_RAW_TEXT_EDIT payload. */
typedef struct de_raw_text {
    /** UTF-8, NUL-terminated, at most 31 bytes. Longer committed text arrives as several
     * DE_RAW_TEXT events, split between code points. Composition text longer than 31 bytes is
     * cut at a code point boundary. */
    char utf8[32];
    /** DE_RAW_TEXT_EDIT only: edit cursor position and selection length, in code points of the
     * whole composition; -1 when unknown. 0 for DE_RAW_TEXT. */
    i32 start, length;
} de_raw_text;

/** DE_RAW_MOUSE_MOVE payload. Window coordinates in logical points (multiply by the window's
 * pixel density for pixels), origin top left. */
typedef struct de_raw_mouse_move {
    f32 x, y;   /**< position in `window`; not updated in relative mode */
    f32 dx, dy; /**< motion since the last event; the useful part in relative mode */
} de_raw_mouse_move;

/** DE_RAW_MOUSE_BUTTON payload. */
typedef struct de_raw_mouse_button {
    f32 x, y;  /**< position in `window`, logical points */
    u8 button; /**< 0 left, 1 right, 2 middle, 3 X1, 4 X2 (de_mouse_button order); 5+ others */
    bool down;
    u8 clicks; /**< 1 single, 2 double click, ... */
} de_raw_mouse_button;

/** DE_RAW_MOUSE_WHEEL payload. */
typedef struct de_raw_mouse_wheel {
    /** Scroll amount, in notches (fractional for touchpads and high-resolution wheels).
     * +dy scrolls away from the user (up), +dx to the right. "Natural scrolling" settings are
     * already undone, so the sign means the same on every OS. */
    f32 dx, dy;
    f32 x, y; /**< mouse position in `window`, logical points */
} de_raw_mouse_wheel;

/** DE_RAW_PAD_BUTTON payload. */
typedef struct de_raw_pad_button {
    /** 0..20 in de_pad_button order (de_input.h): SOUTH, EAST, WEST, NORTH, BACK, GUIDE, START,
     * LSTICK, RSTICK, LSHOULDER, RSHOULDER, DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT, MISC1,
     * PADDLE1 (right upper), PADDLE2 (left upper), PADDLE3 (right lower), PADDLE4 (left lower),
     * TOUCHPAD (click). 21..25 are extra buttons (MISC2..MISC6) that de_pad_button lacks. */
    u8 button;
    bool down;
} de_raw_pad_button;

/** DE_RAW_PAD_AXIS payload. */
typedef struct de_raw_pad_axis {
    /** 0..5 in de_pad_axis order (de_input.h): LEFT_X, LEFT_Y, RIGHT_X, RIGHT_Y, LEFT_TRIGGER,
     * RIGHT_TRIGGER. */
    u8 axis;
    /** Sticks in [-1, 1] (+x right, +y down, as the device reports it); triggers in [0, 1].
     * No dead zone is applied. */
    f32 value;
} de_raw_pad_axis;

/** What a touchpad finger did. */
typedef enum de_touch_phase { DE_TOUCH_DOWN, DE_TOUCH_MOVE, DE_TOUCH_UP } de_touch_phase;

/** DE_RAW_PAD_TOUCHPAD payload. */
typedef struct de_raw_pad_touch {
    u8 touchpad, finger; /**< touchpad index on the pad, finger index on the touchpad */
    u8 phase;            /**< de_touch_phase */
    f32 x, y;            /**< normalized [0, 1], origin top left */
    f32 pressure;        /**< [0, 1] */
} de_raw_pad_touch;

/** Motion sensor types (gamepad and device sensors). Units: accelerometers m/s² (gravity
 * included), gyroscopes rad/s. Axes for a gamepad held level: +x right, +y up, +z towards the
 * player; a gyro reports positive rotation counter-clockwise around each axis. */
typedef enum de_platform_sensor_type {
    DE_PLATFORM_SENSOR_UNKNOWN,
    DE_PLATFORM_SENSOR_ACCEL,
    DE_PLATFORM_SENSOR_GYRO,
    DE_PLATFORM_SENSOR_ACCEL_L, /**< left Joy-Con */
    DE_PLATFORM_SENSOR_GYRO_L,
    DE_PLATFORM_SENSOR_ACCEL_R, /**< right Joy-Con */
    DE_PLATFORM_SENSOR_GYRO_R
} de_platform_sensor_type;

/** DE_RAW_PAD_SENSOR payload. */
typedef struct de_raw_sensor {
    u32 sensor; /**< de_platform_sensor_type */
    f32 data[3];
    /** The device's own timestamp of the sample, in ns, on the device's clock (not the
     * de_time_now_ns() timebase). Use differences between samples to integrate a gyro; 0 if
     * the device has none. de_raw_input_event.time_ns is when the host received it. */
    u64 sensor_time_ns;
} de_raw_sensor;

/** One raw input event (64 bytes). */
typedef struct de_raw_input_event {
    de_raw_input_kind kind;
    /** Window with keyboard or mouse focus (zero if none, or not an engine window); zero for
     * gamepad events. */
    de_window window;
    /** Gamepad instance id for DE_RAW_PAD_* (see de_platform_pads()); the OS keyboard or mouse
     * id for keyboard and mouse events (0 when unknown or synthetic). */
    u32 device;
    u32 reserved_;
    /** When the OS delivered the event, in the de_time_now_ns() timebase, so input can be lined
     * up with frames and replays. */
    u64 time_ns;
    union {
        de_raw_key key;
        de_raw_text text;
        de_raw_mouse_move mouse_move;
        de_raw_mouse_button mouse_button;
        de_raw_mouse_wheel mouse_wheel;
        de_raw_pad_button pad_button;
        de_raw_pad_axis pad_axis;
        de_raw_pad_touch pad_touch;
        de_raw_sensor sensor;
    };
} de_raw_input_event;

/**
 * Pops up to `max` raw input events, oldest first, into `out` and returns how many.
 *
 * de_platform_pump() fills the ring; call this after each pump until it returns less than
 * `max`. No allocation. Touchscreen, pen and joysticks that are not gamepads are not reported.
 * Returns 0 when the platform is not initialised or `out` is NULL.
 */
DE_API u32 de_platform_input_events(de_raw_input_event *out, u32 max);

/** Events dropped since de_platform_init() because the ring was full (the oldest go first). */
DE_API u64 de_platform_input_dropped(void);

/* ---------- Gamepads ----------
 * A gamepad is addressed by its OS instance id, an opaque u32 that is never 0 and is not reused
 * while the program runs. The platform opens every gamepad when it connects (DE_RAW_PAD_ADDED)
 * and closes it on DE_RAW_PAD_REMOVED. A stale id gives DE_ERR_NOT_FOUND. */

/** Most gamepads open at once; more are ignored (with a warning). */
#define DE_PLATFORM_PAD_MAX 16

/** Gamepad families, for glyphs and feature defaults. */
typedef enum de_platform_pad_type {
    DE_PLATFORM_PAD_GENERIC, /**< unknown, or a standard layout without a known family */
    DE_PLATFORM_PAD_XBOX,    /**< Xbox 360 / One / Series */
    DE_PLATFORM_PAD_PS4,     /**< DualShock 4 (and DualShock 3) */
    DE_PLATFORM_PAD_PS5,     /**< DualSense (and DualSense Edge) */
    DE_PLATFORM_PAD_SWITCH,  /**< Switch Pro controller and Joy-Cons */
    DE_PLATFORM_PAD_STEAM    /**< Steam Controller, Steam Deck */
} de_platform_pad_type;

/** Capability flags in de_platform_pad_info.caps. */
typedef enum de_platform_pad_cap {
    DE_PLATFORM_PAD_CAP_RUMBLE = 1u << 0,         /**< de_platform_pad_rumble() */
    DE_PLATFORM_PAD_CAP_TRIGGER_RUMBLE = 1u << 1, /**< de_platform_pad_trigger_rumble() */
    DE_PLATFORM_PAD_CAP_LED = 1u << 2,            /**< de_platform_pad_set_led() (RGB or mono) */
    DE_PLATFORM_PAD_CAP_GYRO = 1u << 3,
    DE_PLATFORM_PAD_CAP_ACCEL = 1u << 4,
    DE_PLATFORM_PAD_CAP_TOUCHPAD = 1u << 5
} de_platform_pad_cap;

/** A connected gamepad. */
typedef struct de_platform_pad_info {
    u32 id;                    /**< instance id */
    de_platform_pad_type type; /**< family */
    u16 vendor, product;       /**< USB vendor and product ids; 0 when unknown */
    u32 caps;                  /**< de_platform_pad_cap flags */
    u32 touchpads;             /**< number of touchpads */
    i32 player_index;          /**< player index the OS or the pad shows; -1 = none */
    char name[64];             /**< UTF-8, cut at 63 bytes */
} de_platform_pad_info;

/** Writes up to `max` connected gamepad ids to `out` (in connection order) and returns how many
 * are connected, so `de_platform_pads(NULL, 0)` counts them. */
DE_API u32 de_platform_pads(u32 *out, u32 max);

/** Describes gamepad `pad`. DE_ERR_NOT_FOUND for a stale id, DE_ERR_INVALID_ARGUMENT for NULL
 * `out`. */
DE_API de_result de_platform_pad_info_get(u32 pad, de_platform_pad_info *out);

/**
 * Starts the two rumble motors: `low` (left, low frequency) and `high` (right, high frequency)
 * in [0, 1] (clamped), for `duration_ms` (0 stops). A new call replaces the previous one.
 * @return DE_OK; DE_ERR_NOT_FOUND (stale id); DE_ERR_UNSUPPORTED (no rumble on this pad); or
 *         DE_ERR_IO when the device refuses (the message has the reason).
 */
DE_API de_result de_platform_pad_rumble(u32 pad, f32 low, f32 high, u32 duration_ms);

/** Rumble in the triggers (Xbox One / Series impulse triggers), like de_platform_pad_rumble().
 * DE_ERR_UNSUPPORTED on pads without trigger motors. */
DE_API de_result de_platform_pad_trigger_rumble(u32 pad, f32 left, f32 right, u32 duration_ms);

/** Sets the pad's LED colour (light bar on DualShock 4 / DualSense). DE_ERR_UNSUPPORTED on pads
 * without an LED. */
DE_API de_result de_platform_pad_set_led(u32 pad, u8 r, u8 g, u8 b);

/**
 * Sends a raw, device-specific effect packet. On a DualSense this is the output-report effect
 * block (adaptive trigger modes, HD haptics enables); on a DualShock 4 the output report.
 * de_input (S21) builds the packets; this call only transports `size` bytes.
 * @return DE_OK; DE_ERR_NOT_FOUND; DE_ERR_INVALID_ARGUMENT (NULL `data` or `size` 0); or
 *         DE_ERR_UNSUPPORTED when the pad's driver takes no effect packets.
 */
DE_API de_result de_platform_pad_send_effect(u32 pad, const void *data, u32 size);

/** True when gamepad `pad` has a sensor of `type`. False for a stale id. */
DE_API bool de_platform_pad_has_sensor(u32 pad, de_platform_sensor_type type);

/**
 * Turns the reports of one gamepad sensor on or off. While on, the pump sends DE_RAW_PAD_SENSOR
 * events (a DualSense reports at about 250 Hz per sensor). Sensors start off, because they cost
 * bandwidth and battery.
 * @return DE_OK; DE_ERR_NOT_FOUND; DE_ERR_UNSUPPORTED when the pad has no such sensor; or
 *         DE_ERR_IO when the device refuses.
 */
DE_API de_result de_platform_pad_sensor_enable(u32 pad, de_platform_sensor_type type, bool on);

/* ---------- Device sensors ----------
 * Motion sensors built into the machine (tablets, some laptops, handhelds that expose them as
 * system sensors). Most desktops have none. Addressed by an opaque u32 instance id. */

/** A device sensor. */
typedef struct de_platform_sensor_info {
    u32 id;                       /**< instance id */
    de_platform_sensor_type type; /**< DE_PLATFORM_SENSOR_UNKNOWN for other sensor kinds */
    char name[64];                /**< UTF-8, cut at 63 bytes */
} de_platform_sensor_info;

/** Writes up to `max` device sensors to `out` and returns how many exist (`(NULL, 0)` counts). */
DE_API u32 de_platform_sensors(de_platform_sensor_info *out, u32 max);

/** Opens (`on`) or closes device sensor `sensor`. An open sensor is updated by every pump.
 * DE_ERR_NOT_FOUND for an unknown id; DE_ERR_BUSY when 8 sensors are already open. */
DE_API de_result de_platform_sensor_enable(u32 sensor, bool on);

/** Reads the latest sample of open sensor `sensor` into `out[0..2]` (units as in
 * de_platform_sensor_type). DE_ERR_INVALID_ARGUMENT when it is not open or `out` is NULL. */
DE_API de_result de_platform_sensor_read(u32 sensor, f32 out[3]);

/* ---------- Clipboard ---------- */

/** Puts UTF-8 `text` on the system clipboard. DE_ERR_UNSUPPORTED when headless,
 * DE_ERR_INVALID_ARGUMENT for NULL, DE_ERR_IO when the OS refuses. */
DE_API de_result de_platform_clipboard_set(const char *text);

/**
 * Copies the clipboard text (UTF-8) to `buf` and returns its full length in bytes, without the
 * NUL. At most `cap - 1` bytes are written, cut at a code point boundary, then a NUL; so
 * `de_platform_clipboard_get(NULL, 0)` asks for the size and a return value >= `cap` means the
 * copy was cut. Returns 0 when there is no text, when headless, or when not initialised.
 * The OS copy is freed before the call returns.
 */
DE_API size_t de_platform_clipboard_get(char *buf, size_t cap);

/* ---------- Cursor and mouse ---------- */

/** System cursor shapes. */
typedef enum de_platform_cursor {
    DE_CURSOR_DEFAULT,
    DE_CURSOR_TEXT,
    DE_CURSOR_WAIT,
    DE_CURSOR_CROSSHAIR,
    DE_CURSOR_PROGRESS,    /**< busy, but still clickable */
    DE_CURSOR_RESIZE_NWSE, /**< diagonal, top left to bottom right */
    DE_CURSOR_RESIZE_NESW,
    DE_CURSOR_RESIZE_EW,
    DE_CURSOR_RESIZE_NS,
    DE_CURSOR_MOVE,
    DE_CURSOR_NOT_ALLOWED,
    DE_CURSOR_POINTER, /**< pointing hand (links) */
    DE_CURSOR_COUNT_
} de_platform_cursor;

/** Shows or hides the mouse cursor (all windows). DE_ERR_UNSUPPORTED when headless. */
DE_API de_result de_platform_cursor_show(bool visible);

/** True when the cursor is shown. False when headless. */
DE_API bool de_platform_cursor_visible(void);

/** Sets the cursor shape (all windows). The OS cursors are created on first use and freed by
 * de_platform_shutdown(). DE_ERR_UNSUPPORTED when headless or the OS lacks that shape,
 * DE_ERR_INVALID_ARGUMENT for an out-of-range value. */
DE_API de_result de_platform_cursor_set(de_platform_cursor shape);

/**
 * Relative mouse mode for window `w`: the cursor is hidden and confined, and DE_RAW_MOUSE_MOVE
 * reports unbounded motion in dx/dy (camera control). It applies while `w` has focus.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (stale window); DE_ERR_UNSUPPORTED (headless); or
 *         DE_ERR_IO when the OS refuses.
 */
DE_API de_result de_platform_mouse_relative(de_window w, bool on);

/** True when relative mouse mode is on for `w`. */
DE_API bool de_platform_mouse_relative_get(de_window w);

/** Moves the cursor to (`x`, `y`) in window `w`, in logical points. Generates a
 * DE_RAW_MOUSE_MOVE. DE_ERR_INVALID_ARGUMENT for a stale window, DE_ERR_UNSUPPORTED headless. */
DE_API de_result de_platform_mouse_warp(de_window w, f32 x, f32 y);

/* ---------- Text input ---------- */

/** Where the text being edited is, so the OS can place the IME candidate window next to it.
 * Window coordinates in logical points. */
typedef struct de_platform_text_area {
    i32 x, y, w, h; /**< the input field */
    i32 cursor;     /**< caret offset from `x`, in points */
} de_platform_text_area;

/**
 * Starts text input for window `w`: the pump then sends DE_RAW_TEXT (and DE_RAW_TEXT_EDIT while
 * an IME composes), and on-screen keyboards may show. `area` (may be NULL) places the IME.
 * Key events keep coming either way. DE_ERR_INVALID_ARGUMENT for a stale window,
 * DE_ERR_UNSUPPORTED when headless.
 */
DE_API de_result de_platform_text_input_start(de_window w, const de_platform_text_area *area);

/** Moves the IME area of window `w` (while text input is on, e.g. when the caret moves). */
DE_API de_result de_platform_text_input_area(de_window w, const de_platform_text_area *area);

/** Stops text input for `w`. A stale window is ignored. */
DE_API void de_platform_text_input_stop(de_window w);

/** True while text input is on for `w`. */
DE_API bool de_platform_text_input_active(de_window w);

/* ---------- File dialogs ---------- */

/** Most file dialogs open at once. */
#define DE_PLATFORM_DIALOG_MAX 4

typedef enum de_file_dialog_kind {
    DE_FILE_DIALOG_OPEN,  /**< choose one or more existing files */
    DE_FILE_DIALOG_SAVE,  /**< choose a new or existing file name */
    DE_FILE_DIALOG_FOLDER /**< choose one or more folders */
} de_file_dialog_kind;

/** A file type filter: `name` = "Images", `pattern` = "png;jpg;jpeg" (extensions without dots,
 * separated by ';'; "*" = all files). */
typedef struct de_file_dialog_filter {
    const char *name;
    const char *pattern;
} de_file_dialog_filter;

/**
 * Called once per de_platform_file_dialog() call, on the main thread inside de_platform_pump()
 * (or inside de_platform_shutdown() for a dialog that is still open, with DE_ERR_CANCELLED).
 *
 * @param result DE_OK with `count` >= 1 paths; DE_ERR_CANCELLED (the user closed the dialog, or
 *               the platform shut down) with `count` 0; or DE_ERR_IO / DE_ERR_UNSUPPORTED when
 *               the OS could not show it.
 * @param paths  native UTF-8 paths (not VFS paths), valid only during the call.
 * @param filter index of the filter the user picked, or -1 when unknown.
 */
typedef void (*de_file_dialog_fn)(void *user, de_result result, const char *const *paths, u32 count,
                                  i32 filter);

/** de_platform_file_dialog() settings. Strings and filters are copied. */
typedef struct de_file_dialog_desc {
    de_file_dialog_kind kind;
    const de_file_dialog_filter *filters; /**< may be NULL; ignored for folders */
    u32 filter_count;
    const char *default_path; /**< starting folder or file; NULL = OS default */
    const char *title;        /**< NULL = OS default */
    de_window parent;         /**< modal for this window where the OS supports it; zero = none */
    bool allow_many;          /**< several files or folders (not for SAVE) */
} de_file_dialog_desc;

/**
 * Opens an OS file dialog without blocking (on most systems) and returns at once. `fn` gets the
 * result later, on the main thread, from de_platform_pump(). macOS shows a dialog without a
 * `parent` as an app-modal panel, which runs its own event loop until the user closes it.
 * @return DE_OK (`fn` will be called exactly once); DE_ERR_INVALID_ARGUMENT (NULL `desc` or
 *         `fn`, bad kind, a filter with NULL strings, stale parent); DE_ERR_BUSY
 *         (DE_PLATFORM_DIALOG_MAX dialogs open); DE_ERR_UNSUPPORTED (headless);
 *         DE_ERR_OUT_OF_MEMORY. On error `fn` is never called.
 */
DE_API de_result de_platform_file_dialog(const de_file_dialog_desc *desc, de_file_dialog_fn fn,
                                         void *user);

/* ==========================================================================================
 * End of the raw input and devices section (DENG-246).
 * ========================================================================================== */

/* ==========================================================================================
 * Time and threads (S02, DENG-147). Engine-owned per OS; <threads.h> is not used (SPEC §3.2).
 * Implementation: src/platform/{thread,time}_{posix,win32}.c. Atomics are internal:
 * src/platform/de_atomic.h.
 * ========================================================================================== */

/* ---------- Time ---------- */

/**
 * Monotonic time in nanoseconds since an unspecified start point.
 *
 * Never goes backwards and is not affected by wall-clock changes. Sources: `mach_absolute_time`
 * (macOS), `clock_gettime(CLOCK_MONOTONIC)` (Linux), `QueryPerformanceCounter` (Windows).
 * Thread-safe, lock-free, no allocation.
 */
DE_API u64 de_time_now_ns(void);

/* ---------- Sleep and frame pacing (S03, DENG-249) ----------
 * Implementation: src/platform/sleep_pacing.c. */

/**
 * The last stretch before a sleep deadline that de_sleep_until_ns() spins instead of asking the
 * OS to sleep (1 ms). OS sleeps overshoot by tens of microseconds (Linux timer slack) to several
 * hundred (Windows high-resolution timers, macOS timer leeway), so the OS sleep stops this far
 * before the deadline and a de_cpu_pause() spin on de_time_now_ns() does the rest. This is the
 * minimum: on a host whose OS sleeps wake later than that (some VMs) the window grows by the
 * measured excess, up to 4 ms more.
 */
#define DE_SLEEP_SPIN_NS 1000000ull

/**
 * Sleeps the calling thread for at least `ns` nanoseconds, then returns as close to that as the
 * scheduler allows. Same as `de_sleep_until_ns(de_time_now_ns() + ns)`; 0 returns at once.
 */
DE_API void de_sleep_ns(u64 ns);

/**
 * Sleeps until de_time_now_ns() >= `deadline_ns`. Never returns early.
 *
 * The OS sleep (`nanosleep` on POSIX; on Windows a waitable timer created with
 * `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, or `Sleep` after `timeBeginPeriod(1)` before Windows
 * 10 1803) runs until DE_SLEEP_SPIN_NS before the deadline, then the thread spins on the clock.
 * Each OS sleep asks for half of the time left before that point, because timer leeway can grow
 * with the length of a sleep (macOS, and VMs on it); a 16 ms wait takes about 7 short sleeps.
 * The spin window adapts to the host: when OS sleeps wake later than halving can absorb (a fixed
 * wake-up latency of several ms, as on GitHub's macOS VMs), it grows by that excess, learned
 * process-wide from the sleeps themselves, up to DE_SLEEP_SPIN_NS + 4 ms. A late return then
 * only comes from preemption, or from a host whose OS sleeps are later than that. The spin
 * costs up to the window of CPU per call (DE_SLEEP_SPIN_NS on a normal host); frame pacing pays
 * it once per frame. Thread-safe, no allocation. A deadline in the past returns at once.
 */
DE_API void de_sleep_until_ns(u64 deadline_ns);

/** How a de_frame_pacer waits at the end of a frame. */
typedef enum de_frame_pacer_mode {
    /** Never waits (uncapped). Stats are still kept. The zero value. */
    DE_FRAME_PACER_NONE,
    /** Frame cap: de_frame_pacer_wait() sleeps until each deadline. */
    DE_FRAME_PACER_CAP,
    /**
     * For a vsync'ed swapchain: wakes `vsync_lead_ns` before each deadline and leaves the last
     * wait to the present call that blocks on the vblank. Keeps the CPU from running frames
     * ahead of the display (less input latency) without fighting the swapchain's own clock.
     */
    DE_FRAME_PACER_VSYNC_ASSIST
} de_frame_pacer_mode;

/** Settings of a de_frame_pacer. `(de_frame_pacer_desc){0}` is valid: uncapped. */
typedef struct de_frame_pacer_desc {
    u32 target_hz;            /**< frames per second; 0 = uncapped whatever the mode */
    de_frame_pacer_mode mode; /**< see de_frame_pacer_mode */
    u64 vsync_lead_ns;        /**< VSYNC_ASSIST only; 0 = DE_FRAME_PACER_DEFAULT_VSYNC_LEAD_NS */
} de_frame_pacer_desc;

/** `vsync_lead_ns` used when the desc gives 0: 1 ms. */
#define DE_FRAME_PACER_DEFAULT_VSYNC_LEAD_NS 1000000ull

/** Counters of a de_frame_pacer, from de_frame_pacer_stats(). All times in nanoseconds. */
typedef struct de_frame_pacer_stats {
    u64 last_frame_ns;    /**< begin-to-begin time of the last complete frame; 0 before one */
    u64 last_work_ns;     /**< begin-to-wait time of the last frame (the frame's own work) */
    u64 frames;           /**< de_frame_pacer_begin() calls since init */
    u64 missed_deadlines; /**< waits that found their deadline already passed */
    u64 resyncs;          /**< missed deadlines more than 2 periods late: the schedule restarted */
} de_frame_pacer_stats;

/**
 * Drift-free frame pacer. Caller storage; one thread uses it (the main loop). Treat the fields
 * as private. No allocation.
 *
 * Deadline n is `epoch + n * 1e9 / target_hz`, computed from integers, so rounding never
 * accumulates and N frames take N periods. A wait that finds its deadline passed counts a miss
 * and returns at once; the following frames catch up toward the schedule. When a wait finds the
 * deadline more than 2 periods gone (a stall: loading, a debugger, a minimized window) the
 * schedule restarts at that moment instead of running a burst of short frames.
 *
 *     de_frame_pacer p;
 *     de_frame_pacer_init(&p, &(de_frame_pacer_desc){.target_hz = 60, .mode = DE_FRAME_PACER_CAP});
 *     for (;;) {
 *         de_frame_pacer_begin(&p);
 *         run_frame();
 *         de_frame_pacer_wait(&p);
 *     }
 */
typedef struct de_frame_pacer {
    de_frame_pacer_desc desc;
    u64 epoch_ns; /* time of deadline 0 of the current schedule */
    u64 index;    /* frames since epoch_ns: the next deadline is deadline(index + 1) */
    u64 begin_ns; /* time of the last de_frame_pacer_begin(); 0 = none yet */
    bool started; /* a schedule exists (set by the first begin after init/set_target) */
    de_frame_pacer_stats stats;
} de_frame_pacer;

/** Initializes `p`. `desc` may be NULL (uncapped). */
DE_API void de_frame_pacer_init(de_frame_pacer *p, const de_frame_pacer_desc *desc);

/**
 * Changes the rate or mode at run time (settings menu, display change). The schedule restarts
 * at the next de_frame_pacer_begin(); the stats are kept.
 */
DE_API void de_frame_pacer_set(de_frame_pacer *p, const de_frame_pacer_desc *desc);

/**
 * Marks the start of a frame. Call once per frame, before the frame's work. The first call
 * after init or set starts the schedule: the first deadline is one period later.
 */
DE_API void de_frame_pacer_begin(de_frame_pacer *p);

/**
 * Ends the frame: sleeps (de_sleep_until_ns) until the frame's deadline, or returns at once when
 * the pacer is uncapped or the deadline has passed (see de_frame_pacer). Call once per frame,
 * after de_frame_pacer_begin(); a wait without a begin is ignored.
 */
DE_API void de_frame_pacer_wait(de_frame_pacer *p);

/** The counters of `p`. */
DE_API de_frame_pacer_stats de_frame_pacer_stats_get(const de_frame_pacer *p);

/* ---------- Threads ---------- */

/** An OS thread created by de_thread_create(). Owned by the creator until de_thread_join(). */
typedef struct de_thread de_thread;

/** Thread entry point. The return value is handed to de_thread_join(). */
typedef i32 (*de_thread_fn)(void *user);

/**
 * Scheduling class. Mapped per OS, best effort (raising the priority may need privileges):
 * - macOS: QoS class UTILITY, DEFAULT, USER_INITIATED, USER_INTERACTIVE.
 * - Linux: nice +5, 0, -5, -10 for the thread (a failure to raise it is ignored).
 * - Windows: THREAD_PRIORITY_BELOW_NORMAL, NORMAL, ABOVE_NORMAL, TIME_CRITICAL.
 */
typedef enum de_thread_priority {
    DE_THREAD_LOW,
    DE_THREAD_NORMAL,
    DE_THREAD_HIGH,
    DE_THREAD_REALTIME
} de_thread_priority;

/** Stack size used when de_thread_create() gets `stack_bytes == 0`, the same on every OS. */
#define DE_THREAD_DEFAULT_STACK_BYTES ((size_t)1 << 20)

/** Longest thread name kept, in bytes without the NUL. Linux shows only the first 15 bytes. */
#define DE_THREAD_NAME_MAX 63

/**
 * Starts a thread that runs `fn(user)`.
 *
 * @param name         Optional UTF-8 name shown in debuggers and profilers. It is copied and
 *                     truncated to DE_THREAD_NAME_MAX bytes (15 on Linux) at a UTF-8 character
 *                     boundary, and set from inside the new thread before `fn` runs. NULL keeps
 *                     the OS default (none; on Linux the creator's name).
 * @param prio         See de_thread_priority.
 * @param stack_bytes  Stack size; 0 = DE_THREAD_DEFAULT_STACK_BYTES. Rounded up to the page size.
 * @param out          Receives the thread. Every thread must be joined exactly once.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`fn` or `out` is NULL), DE_ERR_OUT_OF_MEMORY, or
 *         DE_ERR_INTERNAL when the OS refuses the thread.
 *
 * The small thread record comes from the system heap, not from a de_allocator: the engine heap
 * itself is built on these primitives, and threads are created at startup only.
 */
DE_API de_result de_thread_create(de_thread_fn fn, void *user, const char *name,
                                  de_thread_priority prio, size_t stack_bytes, de_thread **out);

/** Waits for `t` to finish, frees it and returns the value its entry point returned. */
DE_API i32 de_thread_join(de_thread *t);

/**
 * Restricts `t` to the logical cores set in `core_mask` (bit i = core i).
 *
 * Linux: `pthread_setaffinity_np`. Windows: `SetThreadAffinityMask` (processor group 0).
 * macOS has no hard affinity: an affinity tag (lowest set bit + 1) is passed as a hint and the
 * kernel may ignore it; Apple Silicon does not support it at all.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`t` NULL or no core in the mask), DE_ERR_UNSUPPORTED
 *         (the OS ignores affinity), or DE_ERR_INTERNAL.
 */
DE_API de_result de_thread_set_affinity(de_thread *t, u64 core_mask);

/**
 * Changes the scheduling class of a running thread `t` (see de_thread_priority).
 *
 * Windows: `SetThreadPriority`, takes effect at once (a thread lowered below a ready one is
 * preempted). macOS and Linux: DE_ERR_UNSUPPORTED. Their schedulers give a lower priority a
 * smaller share of the CPU rather than none, macOS sets QoS only on the calling thread, and
 * raising a thread on Linux needs privileges.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`t` NULL or an unknown priority), DE_ERR_UNSUPPORTED,
 *         or DE_ERR_INTERNAL.
 */
DE_API de_result de_thread_set_priority(de_thread *t, de_thread_priority prio);

/** Names the calling thread (for threads the engine did not create, such as the main thread). */
DE_API void de_thread_set_name(const char *name);

/** OS id of the calling thread (low 32 bits on macOS). Never 0. Cached per thread. */
DE_API u32 de_thread_current_id(void);

/** Number of logical cores this process may run on. At least 1. */
DE_API u32 de_cpu_logical_cores(void);

/* ---------- Synchronization ----------
 * All objects live in caller-provided storage, are not movable or copyable once initialized,
 * and must be destroyed by the caller. init/destroy are not thread-safe; everything else is.
 * Initialization failures of mutex, condvar and rwlock are fatal (they cannot fail on the
 * supported OSes short of memory corruption). */

/**
 * Non-recursive mutex. pthread mutex (macOS: first-fit policy) or SRWLOCK (Windows). In DE_PROFILE
 * builds it also holds a profiler lock context (de_prof_lock, de_debug.h): a mutex initialized
 * while the profiler runs reports its waits, acquisitions and releases (also around de_cond_wait).
 */
typedef struct de_mutex {
    DE_ALIGN(8) u8 opaque[64];
#ifdef DE_PROFILE
    u64 prof_[2]; /* de_prof_lock; internal */
#endif
} de_mutex;

/** Condition variable, used with a de_mutex. Waits can wake spuriously: always wait in a loop. */
typedef struct de_cond {
    DE_ALIGN(8) u8 opaque[64];
} de_cond;

/**
 * Reader/writer lock: many readers or one writer. Not upgradable and not recursive, not even for
 * readers: a second read lock on the same thread can deadlock behind a waiting writer. On glibc
 * the lock prefers writers (the glibc default would let a stream of readers starve them).
 */
typedef struct de_rwlock {
    DE_ALIGN(8) u8 opaque[256];
} de_rwlock;

/** Counting semaphore. dispatch_semaphore (macOS), sem_t (Linux), Win32 semaphore. */
typedef struct de_sem {
    DE_ALIGN(8) u8 opaque[64];
} de_sem;

/** Thread-local storage slot. Zero is invalid. */
typedef struct de_tls_key {
    u64 id;
} de_tls_key;

/** Destructor for a de_tls_key value; called on thread exit for non-NULL values only. */
typedef void (*de_tls_dtor)(void *value);

DE_API void de_mutex_init(de_mutex *m);
DE_API void de_mutex_lock(de_mutex *m);
/** Returns true if the lock was taken. */
DE_API bool de_mutex_try_lock(de_mutex *m);
DE_API void de_mutex_unlock(de_mutex *m);
DE_API void de_mutex_destroy(de_mutex *m);

DE_API void de_cond_init(de_cond *c);
/** Atomically unlocks `m`, waits, and locks `m` again before returning. */
DE_API void de_cond_wait(de_cond *c, de_mutex *m);
/** Like de_cond_wait(), at most `timeout_ns` (monotonic). Returns false on timeout. */
DE_API bool de_cond_wait_timeout(de_cond *c, de_mutex *m, u64 timeout_ns);
DE_API void de_cond_signal(de_cond *c);
DE_API void de_cond_broadcast(de_cond *c);
DE_API void de_cond_destroy(de_cond *c);

DE_API void de_rwlock_init(de_rwlock *l);
DE_API void de_rwlock_read_lock(de_rwlock *l);
DE_API void de_rwlock_read_unlock(de_rwlock *l);
DE_API void de_rwlock_write_lock(de_rwlock *l);
DE_API void de_rwlock_write_unlock(de_rwlock *l);
DE_API void de_rwlock_destroy(de_rwlock *l);

/** Creates a semaphore with count `initial`. DE_ERR_INTERNAL if the OS refuses. */
DE_API de_result de_sem_init(de_sem *s, u32 initial);
/** Adds `count` to the semaphore, waking up to `count` waiters. */
DE_API void de_sem_post(de_sem *s, u32 count);
/** Waits until the count is positive, then decrements it. */
DE_API void de_sem_wait(de_sem *s);
/** Decrements the count if it is positive, without blocking. Returns true if it did. */
DE_API bool de_sem_try_wait(de_sem *s);
/** Like de_sem_wait(), at most `timeout_ns`. Returns false on timeout. */
DE_API bool de_sem_wait_timeout(de_sem *s, u64 timeout_ns);
/** Destroys the semaphore. No thread may be waiting on it. */
DE_API void de_sem_destroy(de_sem *s);

/**
 * Allocates a thread-local slot; every thread starts with the value NULL.
 *
 * @param dtor  Optional. Runs on the exiting thread for each thread whose value is non-NULL
 *              when it exits (threads from de_thread_create() and other OS threads alike; not for
 *              the main thread at process exit). POSIX `pthread_key_create`, Windows FLS.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, or DE_ERR_OUT_OF_MEMORY when the OS has no free slot.
 */
DE_API de_result de_tls_key_create(de_tls_key *out, de_tls_dtor dtor);
/**
 * Frees the slot. Clear the values first: whether destructors run for values still set differs
 * per OS (POSIX: never; Windows: FlsFree runs them).
 */
DE_API void de_tls_key_destroy(de_tls_key key);
DE_API void *de_tls_get(de_tls_key key);
/** Sets the calling thread's value. DE_ERR_OUT_OF_MEMORY if the OS cannot store it. */
DE_API de_result de_tls_set(de_tls_key key, void *value);

/* ---------- Lock hooks (DENG-308) ----------
 * A debugging aid for code that must never take a lock: the audio module's real-time guard
 * (src/audio/rt_guard.c) installs it to catch a lock taken on the audio thread. */

/**
 * Callback that sees every lock attempt: de_mutex_lock, de_mutex_try_lock, de_rwlock_read_lock and
 * de_rwlock_write_lock call `on_lock` first, on the calling thread, with the lock's address. It
 * runs before the lock is taken, possibly on many threads at once, and must not take a de_mutex
 * or de_rwlock itself. Debug and Dev builds only: Release and Shipping builds never call it.
 */
typedef struct de_lock_hooks {
    void (*on_lock)(void *user, const void *lock);
    void *user;
} de_lock_hooks;

/**
 * Installs `hooks` (NULL uninstalls) and returns the hooks installed before. The rules are the
 * ones of de_mem_set_hooks (de_memory.h): only the pointer is kept, so `*hooks` must stay valid and
 * unchanged while installed; there is one slot and the last setter wins; chain to the returned
 * hooks and put them back on uninstall, in reverse order of install. With no hooks installed a
 * lock costs one relaxed atomic load more (Debug and Dev).
 */
DE_API const de_lock_hooks *de_lock_set_hooks(const de_lock_hooks *hooks);

/* ==========================================================================================
 * End of the time and threads sections (DENG-147).
 * ========================================================================================== */

/* ==========================================================================================
 * Files and async I/O (S03, DENG-247). Engine-owned per OS (SPEC §6), no windowing library.
 * Implementation: src/platform/vfs.c (paths, mounts), file_{posix,win32}.c (OS calls),
 * known_dirs.c (per-user directories), async_io.c (I/O threads).
 * ========================================================================================== */

/* ---------- Virtual paths and mounts ----------
 *
 * Every engine file access names a virtual path `<root>:/<path>`, so asset IDs and data never
 * depend on where the files are on disk (SPEC §11.1):
 *   - `<root>` is 1 to DE_FS_ROOT_MAX bytes of `[a-z0-9_-]`. Standard roots (de_fs_init):
 *     `game:/` (install directory, read-only), `user:/` (saves, settings; writable), `cache:/`
 *     (writable), `dev:/` (project directory, Debug and Dev builds only). `pack:/` is reserved
 *     for pack archives (S08) and cannot be mounted.
 *   - `<path>` uses `/` separators and is UTF-8. Empty and `.` segments are dropped; `..` removes
 *     the segment before it. A `..` that would leave the root, a backslash, a `:`, a control
 *     byte or invalid UTF-8 is rejected with DE_ERR_INVALID_ARGUMENT, and so is a native path
 *     (`/etc/passwd`, `C:\x`): there is no way to name a file outside the mounted directories.
 *
 * A root can have several mounts. Reads use the first mount, in priority order, where the path
 * exists. Writes go to the first writable mount; a root with no writable mount refuses writes
 * with DE_ERR_INVALID_ARGUMENT. The mount table is process-wide and thread-safe; it works
 * without de_fs_init (tests mount temporary directories with de_fs_mount alone).
 */

/** Longest root name, in bytes. */
#define DE_FS_ROOT_MAX 15
/** Longest virtual or native path, in bytes with the NUL. */
#define DE_FS_PATH_MAX 1024
/** Mounts the table holds, across all roots. */
#define DE_FS_MAX_MOUNTS 32

/** de_fs_mount flags. */
typedef enum de_mount_flags {
    DE_MOUNT_READ_ONLY = 0,      /**< reads only (the default) */
    DE_MOUNT_WRITABLE = 1u << 0, /**< writes, deletes and directory creation are allowed */
} de_mount_flags;

/**
 * Standard mounts. `(de_fs_desc){0}` is valid: `game:/` is the executable's directory, and
 * `user:/` and `cache:/` are the per-OS directories for the application "dengine".
 */
typedef struct de_fs_desc {
    /** Organisation folder above `app` on macOS and Windows (not used on Linux); NULL = none. */
    const char *org;
    /** Application folder name for `user:/` and `cache:/`; NULL = "dengine". */
    const char *app;
    /** Native directory for `game:/`; NULL = the directory of the executable. */
    const char *game_dir;
    /**
     * Native directory for `user:/`; NULL = per OS: `~/Library/Application Support/<org>/<app>`
     * (macOS), `$XDG_DATA_HOME/<app>` or `~/.local/share/<app>` (Linux),
     * `%APPDATA%\<org>\<app>` (Windows). Created if missing.
     */
    const char *user_dir;
    /**
     * Native directory for `cache:/`; NULL = per OS: `~/Library/Caches/<org>/<app>` (macOS),
     * `$XDG_CACHE_HOME/<app>` or `~/.cache/<app>` (Linux), `%LOCALAPPDATA%\<org>\<app>\cache`
     * (Windows). Created if missing.
     */
    const char *cache_dir;
    /** Native project directory for `dev:/` (writable). NULL, Release and Shipping: no `dev:/`. */
    const char *dev_dir;
    /** Allocator for open-file records; NULL = de_heap_allocator(). */
    const de_allocator *allocator;
} de_fs_desc;

/**
 * Mounts the standard roots at priority 0. `desc` may be NULL (same as a zeroed desc).
 * @return DE_OK, DE_ERR_ALREADY_EXISTS (already initialised), DE_ERR_NOT_FOUND (a directory or
 *         the home directory could not be found), DE_ERR_IO (a directory could not be created).
 */
DE_API de_result de_fs_init(const de_fs_desc *desc);
/** Removes every mount, standard or not. Files that are still open stay usable. */
DE_API void de_fs_shutdown(void);

/**
 * Adds `native_dir` (an existing directory) under `root` (a root name without `:/`, e.g.
 * "user"). Mounts with a higher `priority` are searched first; among equal priorities the newer
 * mount comes first.
 * @param flags  de_mount_flags.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (bad root name or path too long), DE_ERR_UNSUPPORTED
 *         (`pack`, reserved for S08), DE_ERR_NOT_FOUND (not a directory), DE_ERR_OUT_OF_MEMORY
 *         (DE_FS_MAX_MOUNTS reached).
 */
DE_API de_result de_fs_mount(const char *root, const char *native_dir, u32 flags, i32 priority);
/** Removes the mount of `native_dir` under `root` (as given to de_fs_mount). DE_ERR_NOT_FOUND. */
DE_API de_result de_fs_unmount(const char *root, const char *native_dir);

/**
 * Writes the normalised form of `vpath` (`user:/a/../b//c` -> `user:/b/c`) into `buf`.
 * Checks the syntax only; the root need not be mounted.
 * @return DE_OK or DE_ERR_INVALID_ARGUMENT (bad path, or `cap` too small).
 */
DE_API de_result de_path_normalize(const char *vpath, char *buf, size_t cap);

/**
 * Writes the native path for `vpath` into `buf`: the first mount where it exists, else the
 * mount a write would use (the first writable one), else the first mount.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (bad path or `cap` too small), DE_ERR_NOT_FOUND (root
 *         not mounted).
 */
DE_API de_result de_fs_resolve(const char *vpath, char *buf, size_t cap);

/* ---------- Files ----------
 * Synchronous, blocking calls; safe from any thread. A de_file may be read from several threads
 * at once (reads take an offset); writes to one de_file must not overlap. */
typedef struct de_file de_file;
typedef enum de_file_mode {
    DE_FILE_READ,  /**< existing file, reads only */
    DE_FILE_WRITE, /**< created or truncated, sequential writes */
    DE_FILE_APPEND /**< created if missing, every write goes to the end */
} de_file_mode;

/**
 * Opens a file. READ searches the mounts in priority order; WRITE and APPEND use the first
 * writable mount of the root, and its parent directory must exist (see de_dir_create).
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (bad path, no writable mount, a directory),
 *         DE_ERR_NOT_FOUND, DE_ERR_IO, DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_file_open(const char *vpath, de_file_mode mode, de_file **out);
/**
 * Reads up to `bytes` at `offset` into `dst`. `*read` (optional) gets the count, which is less
 * than `bytes` only at the end of the file. Does not move a file position.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (not opened for reading), DE_ERR_IO.
 */
DE_API de_result de_file_read(de_file *f, void *dst, u64 offset, u64 bytes, u64 *read);
/** Writes all `bytes` after the previous write (WRITE) or at the end (APPEND). DE_ERR_IO. */
DE_API de_result de_file_write(de_file *f, const void *src, u64 bytes);
/** Size of the open file in bytes; 0 on error. */
DE_API u64 de_file_size(de_file *f);
/** Closes `f` (NULL is ignored). Written data is handed to the OS; it is not flushed to disk. */
DE_API void de_file_close(de_file *f);

/**
 * Reads a whole file into one block from `a` (NULL = de_heap_allocator()). The block has
 * `*size + 1` bytes with a NUL after the data; free it with `DE_FREE(a, *data, *size + 1)`.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_NOT_FOUND, DE_ERR_IO, DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_file_read_all(const char *vpath, const de_allocator *a, void **data, u64 *size);
/**
 * Replaces the file at `vpath` with `bytes` of `data` (first writable mount): writes a temporary
 * file in the same directory, flushes it to disk, then renames it over the target, so a reader
 * or a crash sees either the old or the new file, never a part.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_NOT_FOUND (no parent directory), DE_ERR_IO.
 */
DE_API de_result de_file_write_all_atomic(const char *vpath, const void *data, u64 bytes);
/** True if `vpath` names an existing file or directory in any mount of its root. */
DE_API bool de_path_exists(const char *vpath);
/**
 * Deletes a file or an empty directory from the first writable mount that has it.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (bad path, no writable mount), DE_ERR_NOT_FOUND,
 *         DE_ERR_IO (e.g. a directory that is not empty).
 */
DE_API de_result de_file_delete(const char *vpath);
/**
 * Creates the directory `vpath` and its missing parents in the first writable mount. An
 * existing directory is not an error.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_IO.
 */
DE_API de_result de_dir_create(const char *vpath);

/** de_dir_list callback: one entry name (no path). Return false to stop the listing. */
typedef bool (*de_dir_fn)(const char *name, bool is_dir, void *user);
/**
 * Calls `fn` once for each entry of the directory `vpath`, merged over the mounts of its root: a
 * name that a higher mount also has is reported once, for the higher mount. `.` and `..` are
 * skipped. The order is unspecified (OS order). `fn` may call other file functions.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_NOT_FOUND (no mount has the directory),
 *         DE_ERR_IO, DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_dir_list(const char *vpath, de_dir_fn fn, void *user);

/* ---------- Async I/O ----------
 *
 * v1 async I/O (SPEC §6): a pool of I/O threads doing blocking reads. Requests are caller-owned
 * and linked into the queues intrusively, so submitting allocates nothing. Each thread takes the
 * request with the lowest `priority` value (FIFO within a priority) and reads it in chunks of
 * at most DE_IO_CHUNK_BYTES, checking for cancellation between chunks.
 *
 * Completions are delivered exactly once per submitted request: by de_io_pump() on the thread
 * that pumps, or, with DE_IO_CALLBACK_ON_IO_THREAD, directly on the thread that finishes the
 * request (an I/O thread, or the thread calling de_io_cancel/de_io_shutdown for a request that
 * never started). The request is free again (may be resubmitted or released) when `on_complete`
 * runs, and the I/O system does not touch it afterwards.
 */

/** Largest read an I/O thread issues at once; cancellation is checked between chunks. */
#define DE_IO_CHUNK_BYTES ((u64)1 << 20)
/** I/O threads when de_io_desc.thread_count is 0. */
#define DE_IO_DEFAULT_THREADS 2

/** de_io_request.flags. */
enum {
    /** Call `on_complete` on the thread that finishes the request, not from de_io_pump(). */
    DE_IO_CALLBACK_ON_IO_THREAD = 1u << 0,
};

typedef struct de_io_request {
    de_file *file; /**< opened with DE_FILE_READ; must stay open until on_complete */
    u64 offset;    /**< file offset of the first byte */
    u64 bytes;     /**< bytes to read; reading stops early at the end of the file */
    void *dst;     /**< at least `bytes` bytes */
    /**
     * Required. `r` is DE_OK (`bytes_read` < `bytes` only at the end of the file), DE_ERR_IO, or
     * DE_ERR_CANCELLED (`bytes_read` is what was read before the cancel).
     */
    void (*on_complete)(struct de_io_request *req, de_result r, u64 bytes_read);
    void *user;  /**< for the caller */
    u8 priority; /**< 0 = highest */
    u32 flags;   /**< DE_IO_CALLBACK_ON_IO_THREAD */
    /** Owned by the I/O system from de_io_submit until on_complete. Zero it before first use. */
    struct {
        struct de_io_request *next, *prev;
        u64 done;
        i32 result;
        u32 state, cancel;
    } internal_;
} de_io_request;

typedef struct de_io_desc {
    u32 thread_count; /**< I/O threads; 0 = DE_IO_DEFAULT_THREADS */
} de_io_desc;

/**
 * Starts the I/O threads (named "de-io-<n>"). `desc` may be NULL.
 * @return DE_OK, DE_ERR_ALREADY_EXISTS (running), DE_ERR_OUT_OF_MEMORY, DE_ERR_INTERNAL.
 */
DE_API de_result de_io_init(const de_io_desc *desc);
/**
 * Stops the I/O system: queued requests complete with DE_ERR_CANCELLED, running ones stop at
 * their next chunk boundary, the threads are joined, and every pending completion is delivered
 * on the calling thread (call it from the thread that pumps). Does nothing if not running.
 */
DE_API void de_io_shutdown(void);
/**
 * Queues `req`, which must stay valid and unchanged until its on_complete runs. Allocates
 * nothing. On an error the request is not queued and on_complete is not called.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL request, file, callback or `dst`, a file not
 *         opened for reading), DE_ERR_BUSY (`req` is already submitted), DE_ERR_UNSUPPORTED
 *         (de_io_init not called).
 */
DE_API de_result de_io_submit(de_io_request *req);
/**
 * Cancels `req`: still queued -> it completes with DE_ERR_CANCELLED and 0 bytes; being read ->
 * it stops at the next chunk boundary with DE_ERR_CANCELLED; already finished or not submitted
 * -> nothing happens. `req` must be valid memory for the duration of the call.
 */
DE_API void de_io_cancel(de_io_request *req);
/**
 * Delivers up to `max_completions` finished requests (0 = all that are ready) on the calling
 * thread, in completion order. Never blocks on I/O. Returns the number delivered.
 */
DE_API u32 de_io_pump(u32 max_completions);

/* ==========================================================================================
 * End of the files and async I/O sections (DENG-247).
 * ========================================================================================== */

/* ==========================================================================================
 * File watching (S03, DENG-248). SPEC §6 (dev only), §10.1 (game hot reload), §11.9 (asset
 * hot reload). Implementation: src/platform/watch_common.c and one backend per OS
 * (watch_fsevents.c, watch_inotify.c, watch_win32.c).
 *
 * Only Debug and Dev builds watch files. In Release and Shipping builds de_file_watch_create()
 * returns DE_ERR_UNSUPPORTED.
 *
 * A watch observes one directory, named by a virtual path (`dev:/assets`), and reports changes
 * under it with virtual paths. The OS reports changes on its own threads; the watch keeps them
 * in a pending table and merges them per path. A change is delivered once nothing has happened
 * to its path for the debounce window (editors save in bursts: temporary file, rename, several
 * writes). On Linux a new file counts as changing until its writer closes it (at most 10 s),
 * so a writer slower than the window (an fsync under I/O load) still merges with what follows.
 * Callbacks run only inside de_file_watch_poll(), on the thread that calls it.
 *
 * Merge rules for one path within a window: CREATED + MODIFIED -> CREATED; CREATED + DELETED ->
 * nothing; DELETED + CREATED -> MODIFIED (replaced); a rename -> one RENAMED with the new path
 * and `old_path`. A file that is created and then renamed over a target in the same window (an
 * atomic save, de_file_write_all_atomic) reports one MODIFIED for the target and nothing for
 * the temporary file. A rename into the watched tree from outside is CREATED, a rename out of
 * it is DELETED. Directories report CREATED, DELETED and RENAMED, never MODIFIED; renaming a
 * directory reports the directory only, not each entry inside it. When the OS drops events
 * (queue or buffer overflow), the watch reports one MODIFIED for the watched directory itself
 * (`is_dir` true): rescan it.
 * ========================================================================================== */

/** A directory watch. Opaque. */
typedef struct de_file_watch de_file_watch;

/** What happened to a path. */
typedef enum de_watch_change {
    DE_WATCH_CREATED,  /**< a new file or directory */
    DE_WATCH_MODIFIED, /**< the contents of a file changed (or the watched dir needs a rescan) */
    DE_WATCH_DELETED,  /**< removed, or moved out of the watched tree */
    DE_WATCH_RENAMED   /**< moved within the watched tree: `path` is the new name */
} de_watch_change;

/** One change, valid only during the callback. */
typedef struct de_watch_event {
    const char *path;       /**< virtual path, `<watched dir>/<relative path>` */
    const char *old_path;   /**< DE_WATCH_RENAMED: the previous virtual path; otherwise NULL */
    de_watch_change change; /**< what happened */
    bool is_dir;            /**< the path is a directory (as far as the OS reported it) */
} de_watch_event;

/** Watch callback. It may call any file function, but must not destroy its own watch. */
typedef void (*de_watch_fn)(const de_watch_event *ev, void *user);

/** Default debounce window, in milliseconds (the cvar `fs.watch_debounce_ms`). */
#define DE_WATCH_DEBOUNCE_DEFAULT_MS 100

/**
 * de_file_watch_create() settings. `dir` and `fn` are required; zero is the default for the
 * other fields.
 */
typedef struct de_file_watch_desc {
    /** Virtual path of an existing directory. A root with several mounts is watched in the
     * mount that de_fs_resolve() picks (the first one where the directory exists). */
    const char *dir;
    /** Also watch every subdirectory, including ones created later. */
    bool recursive;
    de_watch_fn fn; /**< called from de_file_watch_poll() */
    void *user;     /**< passed to fn */
    /** Debounce window in ms; 0 = the cvar `fs.watch_debounce_ms` (default
     * DE_WATCH_DEBOUNCE_DEFAULT_MS), read at every poll. The cvar is registered by the first
     * de_file_watch_create(). */
    u32 debounce_ms;
    /** The watch, its pending changes and the backend buffers; NULL = de_heap_allocator(). */
    const de_allocator *allocator;
} de_file_watch_desc;

/**
 * Starts watching `desc->dir`. Changes made after this returns are reported.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL desc, `dir`, `fn` or `out`, a bad virtual path,
 *         not a directory); DE_ERR_NOT_FOUND (root not mounted, no such directory);
 *         DE_ERR_OUT_OF_MEMORY; DE_ERR_IO (the OS refused the watch); DE_ERR_UNSUPPORTED (not a
 *         Debug or Dev build).
 */
DE_API de_result de_file_watch_create(const de_file_watch_desc *desc, de_file_watch **out);

/**
 * Delivers every change whose debounce window has passed, on the calling thread, oldest first.
 * Never blocks on the OS. Call it from one thread at a time (typically once per frame on the
 * main thread). A NULL watch is ignored. Returns the number of callbacks made.
 */
DE_API u32 de_file_watch_poll(de_file_watch *w);

/** Stops the watch and frees it with any pending changes (they are not delivered). NULL is
 * ignored. Not from inside the watch's own callback. */
DE_API void de_file_watch_destroy(de_file_watch *w);

/* ==========================================================================================
 * End of the file watching section (DENG-248).
 * ========================================================================================== */

/* ---------- Dynamic libraries (S03, DENG-249) ----------
 * The game library in dev builds (SPEC §10.1) and its hot reload. Implementation:
 * src/platform/dylib_{posix,win32}.c. */

/** A loaded shared library. Opaque; the pointer is the OS handle, so nothing is allocated. */
typedef struct de_dylib de_dylib;

/** A function pointer from de_dylib_function(). Cast it to the real type before calling. */
typedef void (*de_dylib_fn)(void);

/**
 * Loads the shared library at `path`, a native UTF-8 file path (not a VFS path: the game library
 * sits next to the executable). POSIX `dlopen(RTLD_NOW | RTLD_LOCAL)`: every symbol is resolved
 * now, and none leaks into the global namespace. Windows `LoadLibraryExW`; an absolute path also
 * searches the library's own directory for its dependencies (`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`).
 *
 * Each distinct file is a separate image: a copy of a library under a new name loads next to the
 * original, with its own globals (hot reload, SPEC §10.1). Opening the same path twice gives the
 * same image, reference-counted by the OS; close each open once.
 *
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL or empty `path`, NULL `out`), DE_ERR_NOT_FOUND
 *         (no such file), or DE_ERR_IO (the OS could not load it: wrong architecture, a missing
 *         dependency, an unresolved symbol). The error message carries the OS text (`dlerror`,
 *         `FormatMessageW`).
 */
DE_API de_result de_dylib_open(const char *path, de_dylib **out);

/**
 * The address of the exported data or function `name` in `lib`, or NULL with the error context
 * set (DE_ERR_INVALID_ARGUMENT, DE_ERR_NOT_FOUND with the OS text). For a function, use
 * de_dylib_function(): ISO C has no cast between object and function pointers.
 */
DE_API void *de_dylib_symbol(de_dylib *lib, const char *name);

/** Like de_dylib_symbol(), for an exported function. NULL with the error context set if absent. */
DE_API de_dylib_fn de_dylib_function(de_dylib *lib, const char *name);

/**
 * Releases `lib` (`dlclose`, `FreeLibrary`). The image unloads when its last open is closed;
 * pointers into it are invalid then. NULL is ignored.
 */
DE_API void de_dylib_close(de_dylib *lib);

/**
 * Writes the platform file name of the library `base` to `buf`: `libgame.dylib` (macOS),
 * `libgame.so` (Linux), `game.dll` (Windows) for `base` = "game". A directory part is kept,
 * with the prefix added to the last component only: "bin/game" → "bin/libgame.so".
 *
 * @param cap  Size of `buf` in bytes, NUL included.
 * @return DE_OK, or DE_ERR_INVALID_ARGUMENT (NULL argument, empty file name, or `buf` too small;
 *         `buf` then holds an empty string if `cap` > 0).
 */
DE_API de_result de_dylib_platform_name(const char *base, char *buf, size_t cap);

DE_EXTERN_C_END
#endif
