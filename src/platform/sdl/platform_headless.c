/* platform_headless.c: the platform init, display, window and event API for DE_HEADLESS builds,
 * which do not link SDL at all (SPEC §4.2, §16.3; DENG-245). Built instead of the other files
 * in this directory.
 *
 * de_platform_init() always succeeds and behaves as if DE_PLATFORM_HEADLESS were set. Window
 * and display functions report DE_ERR_UNSUPPORTED or zero values. The pump reports DE_EV_QUIT
 * once after SIGINT or SIGTERM, so a dedicated server can still shut down cleanly.
 */
#include <dengine/de_platform.h>

#include "platform/de_atomic.h"

#include <signal.h>
#include <string.h>

static bool de__headless_up;
#if defined(DE_DEBUG) || defined(DE_DEV)
static u32 de__headless_main;
#endif
static de_atomic_u32 de__headless_quit = DE_ATOMIC_INIT(0);

typedef void (*de__sig_fn)(int);
static de__sig_fn de__prev_int, de__prev_term;

static void de__on_signal(int sig) {
    (void)sig;
    de_atomic_store_u32(&de__headless_quit, 1, DE_ATOMIC_RELAXED);
}

#define DE__HEADLESS_ASSERT_MAIN() DE_ASSERT(de__headless_main == de_thread_current_id())

de_result de_platform_init(const de_platform_desc *desc) {
    (void)desc;
    if (de__headless_up)
        return DE_FAIL(DE_ERR_BUSY, "platform: already initialised");
    de__headless_up = true;
#if defined(DE_DEBUG) || defined(DE_DEV)
    de__headless_main = de_thread_current_id();
#endif
    de_atomic_store_u32(&de__headless_quit, 0, DE_ATOMIC_RELAXED);
    de__prev_int = signal(SIGINT, de__on_signal);
    de__prev_term = signal(SIGTERM, de__on_signal);
    return DE_OK;
}

void de_platform_shutdown(void) {
    if (!de__headless_up)
        return;
    DE__HEADLESS_ASSERT_MAIN();
    signal(SIGINT, de__prev_int == SIG_ERR ? SIG_DFL : de__prev_int);
    signal(SIGTERM, de__prev_term == SIG_ERR ? SIG_DFL : de__prev_term);
    de__headless_up = false;
}

bool de_platform_headless(void) {
    return true;
}

u32 de_display_count(void) {
    return 0;
}

de_result de_display_info_get(u32 index, de_display_info *out) {
    (void)index;
    if (out)
        memset(out, 0, sizeof *out);
    return DE_FAIL(DE_ERR_UNSUPPORTED, "de_display_info_get: headless build");
}

u32 de_display_modes(u32 index, de_display_mode *out, u32 max) {
    (void)index;
    (void)out;
    (void)max;
    return 0;
}

de_result de_window_create(const de_window_desc *desc, de_window *out) {
    (void)desc;
    if (out)
        out->id = 0;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "de_window_create: headless build");
}

void de_window_destroy(de_window w) {
    (void)w;
}

void de_window_size_pixels(de_window w, u32 *width, u32 *height) {
    (void)w;
    if (width)
        *width = 0;
    if (height)
        *height = 0;
}

void de_window_size_points(de_window w, u32 *width, u32 *height) {
    (void)w;
    if (width)
        *width = 0;
    if (height)
        *height = 0;
}

de_window_insets de_window_safe_area(de_window w) {
    (void)w;
    return (de_window_insets){0};
}

de_result de_window_set_mode(de_window w, de_window_mode mode) {
    (void)w;
    (void)mode;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "de_window_set_mode: headless build");
}

de_window_mode de_window_get_mode(de_window w) {
    (void)w;
    return DE_WINDOW_WINDOWED;
}

void de_window_set_title(de_window w, const char *title) {
    (void)w;
    (void)title;
}

de_display_info de_window_display(de_window w) {
    (void)w;
    de_display_info info;
    memset(&info, 0, sizeof info);
    return info;
}

de_native_window de_window_native(de_window w) {
    (void)w;
    de_native_window n = {0};
    return n;
}

/* Graphics surfaces (DENG-250): no windows, so no surfaces and no Vulkan loader. */
u32 de_window_vulkan_instance_extensions(const char **out, u32 max) {
    (void)out;
    (void)max;
    (void)DE_FAIL(DE_ERR_UNSUPPORTED, "vulkan: headless build");
    return 0;
}

de_window_vulkan_fn de_window_vulkan_get_instance_proc_addr(void) {
    (void)DE_FAIL(DE_ERR_UNSUPPORTED, "vulkan: headless build");
    return NULL;
}

de_result de_window_vulkan_create_surface(de_window w, void *vk_instance,
                                          const void *vk_allocation_callbacks,
                                          u64 *out_vk_surface) {
    (void)w;
    (void)vk_instance;
    (void)vk_allocation_callbacks;
    if (out_vk_surface)
        *out_vk_surface = 0;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "de_window_vulkan_create_surface: headless build");
}

void de_window_vulkan_destroy_surface(void *vk_instance, u64 vk_surface,
                                      const void *vk_allocation_callbacks) {
    (void)vk_instance;
    (void)vk_surface;
    (void)vk_allocation_callbacks;
}

u32 de_platform_pump(de_app_event *events, u32 max_events) {
    if (!de__headless_up)
        return 0;
    DE__HEADLESS_ASSERT_MAIN();
    if (!de_atomic_exchange_u32(&de__headless_quit, 0, DE_ATOMIC_RELAXED))
        return 0;
    if (!events || max_events == 0)
        return 0;
    memset(&events[0], 0, sizeof events[0]);
    events[0].kind = DE_EV_QUIT;
    return 1;
}

de_window de_platform_keyboard_focus(void) {
    return (de_window){0};
}
