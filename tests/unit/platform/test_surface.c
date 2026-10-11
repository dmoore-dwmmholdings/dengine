/* Tests for native window handles and graphics surfaces (DENG-250).
 *
 * Each test starts the platform with video and skips when no video driver starts (as
 * test_window does). The Vulkan tests also skip when there is no Vulkan loader: SDL opens it at
 * run time, from the standard places or from SDL_VULKAN_LIBRARY. They create a minimal
 * VkInstance with the Vulkan headers SDL ships, through the loader's vkGetInstanceProcAddr, so
 * the test links no loader either. A DE_HEADLESS build checks the stubs instead. */
#include "de_test.h"

#include <dengine/de_platform.h>

#include "../core/test_counting_alloc.h"

#include <stdio.h>
#include <string.h>

#if !defined(DE_HEADLESS)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TEST_SURFACE_ASAN 1
#endif
#endif
#if defined(__APPLE__) && defined(TEST_SURFACE_ASAN)
/* Same as test_window.c: CoreGraphics' PDF lexer calls strndup() on titlebar images in a way
 * that strict_string_checks=1 reports although it is valid. Engine and SDL code stay checked. */
const char *__asan_default_suppressions(void);
const char *__asan_default_suppressions(void) {
    return "interceptor_via_lib:CoreGraphics\n";
}
#endif

#if defined(__APPLE__) && !defined(DE_HEADLESS)
/* test_surface_metal.m */
bool test_surface_is_metal_layer(void *layer);
double test_surface_layer_scale(void *layer);
void test_surface_layer_drawable(void *layer, double *width, double *height);
#endif

#if !defined(DE_HEADLESS)

typedef struct surf_fx {
    test_counting_alloc alloc;
    bool up;
} surf_fx;

static void surf_fx_setup(surf_fx *f) {
    test_counting_init(&f->alloc);
    de_platform_desc d = {0};
    d.app_name = "dengine test_surface";
    d.app_org = "DEngine";
    d.allocator = &f->alloc.iface;
    de_result r = de_platform_init(&d);
    if (r == DE_ERR_UNSUPPORTED)
        SKIP("no video driver");
    ASSERT_EQ(r, DE_OK);
    f->up = true;
}

static void surf_fx_teardown(surf_fx *f) {
    if (!f->up)
        return;
    de_platform_shutdown();
    EXPECT_EQ(f->alloc.live_blocks, 0u);
    EXPECT_EQ(f->alloc.size_errors, 0u);
}

static de_result open_hidden(de_window_surface surface, bool high_dpi, de_window *out) {
    de_window_desc d = {0};
    d.title = "test_surface";
    d.width = 320;
    d.height = 240;
    d.display_index = -1;
    d.high_dpi = high_dpi;
    d.hidden = true;
    d.surface = surface;
    return de_window_create(&d, out);
}

static bool has_name(const char *const *names, u32 n, const char *want) {
    for (u32 i = 0; i < n; i++)
        if (strcmp(names[i], want) == 0)
            return true;
    return false;
}

/* ---------- native handles ---------- */

TEST_F(surface, native_handles, surf_fx) {
    (void)f;
    de_window w = {0};
    ASSERT_EQ(open_hidden(DE_WINDOW_SURFACE_NONE, false, &w), DE_OK);
    de_native_window n = de_window_native(w);
    EXPECT_TRUE(n.sdl_window != NULL);
    EXPECT_TRUE(n.metal_layer == NULL); /* only DE_WINDOW_SURFACE_METAL windows get a layer */
#if defined(_WIN32)
    EXPECT_TRUE(n.hwnd != NULL);
#else
    EXPECT_TRUE(n.hwnd == NULL);
#endif
    de_window_destroy(w);

    /* A surface kind outside the enum is rejected before any window exists. */
    de_window bad = {0};
    EXPECT_EQ(open_hidden((de_window_surface)99, false, &bad), DE_ERR_INVALID_ARGUMENT);
    EXPECT_FALSE(DE_HANDLE_VALID(bad));
}

TEST_F(surface, metal_layer_class, surf_fx) {
    (void)f;
    de_window w = {0};
#if defined(__APPLE__)
    for (int hd = 0; hd < 2; hd++) {
        ASSERT_EQ(open_hidden(DE_WINDOW_SURFACE_METAL, hd != 0, &w), DE_OK);
        de_native_window n = de_window_native(w);
        ASSERT_TRUE(n.metal_layer != NULL);
        EXPECT_TRUE(n.sdl_window != NULL);
        EXPECT_TRUE(n.hwnd == NULL);
        EXPECT_TRUE(test_surface_is_metal_layer(n.metal_layer));

        /* Contents scale = pixel density = pixel size / logical size; the drawable starts at
         * the window's pixel size. */
        u32 pw = 0, ph = 0;
        de_window_size_pixels(w, &pw, &ph);
        ASSERT_GT(pw, 0u);
        double density = (double)pw / 320.0;
        EXPECT_FLOAT_EQ(test_surface_layer_scale(n.metal_layer), density, 1e-3);
        double dw = 0.0, dh = 0.0;
        test_surface_layer_drawable(n.metal_layer, &dw, &dh);
        EXPECT_FLOAT_EQ(dw, (double)pw, 0.5);
        EXPECT_FLOAT_EQ(dh, (double)ph, 0.5);
        printf("  high_dpi=%d: %ux%u px, contents scale %.2f, drawable %.0fx%.0f\n", hd, pw, ph,
               test_surface_layer_scale(n.metal_layer), dw, dh);

        /* The view is made once: the second call returns the same layer. */
        de_native_window again = de_window_native(w);
        EXPECT_TRUE(again.metal_layer == n.metal_layer);
        de_window_destroy(w);
    }
#else
    /* No Metal outside macOS. */
    EXPECT_EQ(open_hidden(DE_WINDOW_SURFACE_METAL, false, &w), DE_ERR_UNSUPPORTED);
    EXPECT_FALSE(DE_HANDLE_VALID(w));
#endif
}

TEST_F(surface, native_after_destroy_rejected, surf_fx) {
    (void)f;
#if defined(__APPLE__)
    const de_window_surface kind = DE_WINDOW_SURFACE_METAL;
#else
    const de_window_surface kind = DE_WINDOW_SURFACE_NONE;
#endif
    de_window w = {0};
    ASSERT_EQ(open_hidden(kind, false, &w), DE_OK);
    de_native_window live = de_window_native(w);
    EXPECT_TRUE(live.sdl_window != NULL);
    de_window_destroy(w);

    de_native_window n = de_window_native(w);
    EXPECT_TRUE(n.hwnd == NULL);
    EXPECT_TRUE(n.metal_layer == NULL);
    EXPECT_TRUE(n.sdl_window == NULL);
    u64 surf = 123;
    int fake_instance = 0;
    EXPECT_EQ(de_window_vulkan_create_surface(w, &fake_instance, NULL, &surf),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(surf, (u64)0);

    /* Still rejected after the slot is reused by a new window. */
    de_window w2 = {0};
    ASSERT_EQ(open_hidden(kind, false, &w2), DE_OK);
    EXPECT_NE(w2.id, w.id);
    n = de_window_native(w);
    EXPECT_TRUE(n.sdl_window == NULL);
    EXPECT_TRUE(n.metal_layer == NULL);
    EXPECT_TRUE(de_window_native(w2).sdl_window != NULL);
    de_window_destroy(w2);

    /* Zero handle and NULL out pointer. */
    de_window zero = {0};
    n = de_window_native(zero);
    EXPECT_TRUE(n.sdl_window == NULL);
    EXPECT_EQ(de_window_vulkan_create_surface(zero, &fake_instance, NULL, NULL),
              DE_ERR_INVALID_ARGUMENT);
    de_window_vulkan_destroy_surface(NULL, 0, NULL); /* ignored */
}

/* ---------- Vulkan ---------- */

TEST_F(surface, vulkan_extensions, surf_fx) {
    (void)f;
    u32 n = de_window_vulkan_instance_extensions(NULL, 0);
    if (n == 0)
        SKIP("no Vulkan loader (install the Vulkan SDK or set SDL_VULKAN_LIBRARY)");
    const char *names[16];
    ASSERT_LE(n, 16u);
    EXPECT_EQ(de_window_vulkan_instance_extensions(names, 16), n);
    for (u32 i = 0; i < n; i++)
        printf("  instance extension: %s\n", names[i]);
    EXPECT_TRUE(has_name(names, n, "VK_KHR_surface"));
#if defined(__APPLE__)
    EXPECT_TRUE(has_name(names, n, "VK_EXT_metal_surface"));
#elif defined(_WIN32)
    EXPECT_TRUE(has_name(names, n, "VK_KHR_win32_surface"));
#endif

    /* A short array gets what fits; the result is still the total. */
    const char *one[2] = {NULL, "sentinel"};
    EXPECT_EQ(de_window_vulkan_instance_extensions(one, 1), n);
    EXPECT_TRUE(one[0] != NULL);
    EXPECT_STREQ(one[1], "sentinel");

    EXPECT_TRUE(de_window_vulkan_get_instance_proc_addr() != NULL);
}

TEST_F(surface, vulkan_create_destroy, surf_fx) {
    (void)f;
    de_window w = {0};
    de_result r = open_hidden(DE_WINDOW_SURFACE_VULKAN, true, &w);
    if (r == DE_ERR_UNSUPPORTED)
        SKIP("no Vulkan loader (install the Vulkan SDK or set SDL_VULKAN_LIBRARY)");
    ASSERT_EQ(r, DE_OK);

    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)de_window_vulkan_get_instance_proc_addr();
    ASSERT_TRUE(gipa != NULL);
    PFN_vkCreateInstance create_instance =
        (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    PFN_vkEnumerateInstanceExtensionProperties enum_ext =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(VK_NULL_HANDLE,
                                                         "vkEnumerateInstanceExtensionProperties");
    ASSERT_TRUE(create_instance != NULL);
    ASSERT_TRUE(enum_ext != NULL);

    /* The window extensions, plus portability enumeration where the loader has it (macOS:
     * MoltenVK is a portability driver, which a Vulkan SDK loader hides without it). */
    const char *exts[18];
    u32 n = de_window_vulkan_instance_extensions(exts, 16);
    ASSERT_GT(n, 0u);
    ASSERT_LE(n, 16u);
    VkInstanceCreateFlags flags = 0;
    uint32_t avail_n = 0;
    ASSERT_EQ(enum_ext(NULL, &avail_n, NULL), VK_SUCCESS);
    VkExtensionProperties avail[64];
    if (avail_n > 64)
        avail_n = 64;
    VkResult er = enum_ext(NULL, &avail_n, avail);
    ASSERT_TRUE(er == VK_SUCCESS || er == VK_INCOMPLETE);
    for (u32 i = 0; i < avail_n; i++) {
        if (strcmp(avail[i].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
            exts[n++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
            flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
    }

    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "test_surface";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.flags = flags;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = n;
    ci.ppEnabledExtensionNames = exts;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult vr = create_instance(&ci, NULL, &instance);
    if (vr == VK_ERROR_INCOMPATIBLE_DRIVER)
        SKIP("Vulkan loader found, but no Vulkan driver");
    ASSERT_EQ(vr, VK_SUCCESS);
    PFN_vkDestroyInstance destroy_instance =
        (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
    ASSERT_TRUE(destroy_instance != NULL);

    u64 surface = 0;
    ASSERT_EQ(de_window_vulkan_create_surface(w, instance, NULL, &surface), DE_OK);
    EXPECT_NE(surface, (u64)0);

    /* A window that was not created for Vulkan gets no surface. */
    de_window plain = {0};
    ASSERT_EQ(open_hidden(DE_WINDOW_SURFACE_NONE, false, &plain), DE_OK);
    u64 other = 7;
    EXPECT_EQ(de_window_vulkan_create_surface(plain, instance, NULL, &other),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(other, (u64)0);
    EXPECT_EQ(de_window_vulkan_create_surface(w, NULL, NULL, &other), DE_ERR_INVALID_ARGUMENT);
    de_window_destroy(plain);

    /* The surface goes first, then the window and the instance (de_platform.h). */
    de_window_vulkan_destroy_surface(instance, surface, NULL);
    de_window_destroy(w);
    destroy_instance(instance, NULL);
}

#else /* DE_HEADLESS */

TEST(surface, headless_build_stubs) {
    de_window zero = {0};
    de_native_window n = de_window_native(zero);
    EXPECT_TRUE(n.hwnd == NULL && n.metal_layer == NULL && n.sdl_window == NULL);
    EXPECT_EQ(de_window_vulkan_instance_extensions(NULL, 0), 0u);
    EXPECT_TRUE(de_window_vulkan_get_instance_proc_addr() == NULL);
    u64 s = 1;
    int fake_instance = 0;
    EXPECT_EQ(de_window_vulkan_create_surface(zero, &fake_instance, NULL, &s), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(s, (u64)0);
    de_window_vulkan_destroy_surface(&fake_instance, 1, NULL);
}

#endif
