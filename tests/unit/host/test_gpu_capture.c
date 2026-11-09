/* DENG-573: environment parsing is isolated from device creation and never launches capture. */
#define _POSIX_C_SOURCE 200809L
#include "de_test.h"
#include "host/host_internal.h"
#include <stdlib.h>
#include <string.h>
static const char *keys[] = {"DE_GPU_CAPTURE", "DE_GPU_CAPTURE_DIR", "DE_GPU_CAPTURE_FRAMES",
                             "MTL_CAPTURE_ENABLED"};
typedef struct capture_env {
    char *old[4];
    de__host h;
} capture_env;
static void capture_env_setup(capture_env *f) {
    for (u32 i = 0; i < 4; ++i) {
        const char *v = getenv(keys[i]);
        f->old[i] = v ? strdup(v) : NULL;
        ASSERT_EQ(unsetenv(keys[i]), 0);
    }
}
static void capture_env_teardown(capture_env *f) {
    de__host_capture_cleanup(&f->h);
    for (u32 i = 0; i < 4; ++i) {
        if (f->old[i]) {
            (void)setenv(keys[i], f->old[i], 1);
            free(f->old[i]);
        } else
            (void)unsetenv(keys[i]);
    }
}
static void valid_env(void) {
    ASSERT_EQ(setenv("DE_GPU_CAPTURE", "xcode", 1), 0);
    ASSERT_EQ(setenv("DE_GPU_CAPTURE_DIR", "/tmp", 1), 0);
}
TEST_F(host_capture, disabled_is_inert, capture_env) {
    ASSERT_EQ(setenv("DE_GPU_CAPTURE_FRAMES", "malformed", 1), 0);
    EXPECT_EQ(de__host_capture_prepare(&f->h), DE_OK);
    EXPECT_FALSE(f->h.capture.armed);
    EXPECT_TRUE(getenv("MTL_CAPTURE_ENABLED") == NULL);
}
TEST_F(host_capture, env_arms_but_only_cli_requests, capture_env) {
    valid_env();
    EXPECT_EQ(de__host_capture_prepare(&f->h), DE_OK);
    EXPECT_TRUE(f->h.capture.armed);
    EXPECT_FALSE(f->h.capture.launch);
    EXPECT_EQ(f->h.capture.frames, 1u);
    de__host_capture_cleanup(&f->h);
    memset(&f->h, 0, sizeof f->h);
    char *args[] = {"test", "--gpu-capture"};
    f->h.desc.argc = 2;
    f->h.desc.argv = args;
    EXPECT_EQ(de__host_capture_prepare(&f->h), DE_OK);
    EXPECT_TRUE(f->h.capture.launch);
}
TEST_F(host_capture, rejects_malformed_configuration, capture_env) {
    valid_env();
    const char *bad[] = {"", "0", "101", "-1", "1x", " 1", "999999999999999999"};
    for (u32 i = 0; i < sizeof bad / sizeof *bad; ++i) {
        ASSERT_EQ(setenv("DE_GPU_CAPTURE_FRAMES", bad[i], 1), 0);
        EXPECT_EQ(de__host_capture_prepare(&f->h), DE_ERR_INVALID_ARGUMENT);
        EXPECT_TRUE(getenv("MTL_CAPTURE_ENABLED") == NULL);
    }
    ASSERT_EQ(unsetenv("DE_GPU_CAPTURE_FRAMES"), 0);
    ASSERT_EQ(unsetenv("DE_GPU_CAPTURE_DIR"), 0);
    EXPECT_EQ(de__host_capture_prepare(&f->h), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(setenv("DE_GPU_CAPTURE_DIR", "relative", 1), 0);
    EXPECT_EQ(de__host_capture_prepare(&f->h), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(setenv("DE_GPU_CAPTURE", "renderdoc", 1), 0);
    EXPECT_EQ(de__host_capture_prepare(&f->h), DE_ERR_UNSUPPORTED);
}
TEST_F(host_capture, environment_ownership_and_headless, capture_env) {
    valid_env();
    f->h.desc.headless = true;
    EXPECT_EQ(de__host_capture_prepare(&f->h), DE_OK);
    EXPECT_FALSE(f->h.capture.armed);
    EXPECT_TRUE(getenv("MTL_CAPTURE_ENABLED") == NULL);
    f->h.desc.headless = false;
    EXPECT_EQ(de__host_capture_prepare(&f->h), DE_OK);
    ASSERT_EQ(setenv("MTL_CAPTURE_ENABLED", "changed-by-owner", 1), 0);
    de__host_capture_cleanup(&f->h);
    EXPECT_TRUE(strcmp(getenv("MTL_CAPTURE_ENABLED"), "changed-by-owner") == 0);
}

#include "../input/input_test_util.h"
TEST(host_capture, hotkey_edges_not_repeat_or_modifier_only) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    input_test_key(in, DE_KEY_F12, true);
    input_test_frame(in);
    EXPECT_FALSE(de__host_capture_pressed(in));
    input_test_key(in, DE_KEY_LCTRL, true);
    input_test_key(in, DE_KEY_LSHIFT, true);
    input_test_frame(in);
    EXPECT_FALSE(de__host_capture_pressed(in));
    input_test_key(in, DE_KEY_F12, false);
    input_test_frame(in);
    input_test_key(in, DE_KEY_F12, true);
    input_test_frame(in);
    EXPECT_TRUE(de__host_capture_pressed(in));
    input_test_key_ex(in, DE_KEY_F12, true, true, 0);
    input_test_frame(in);
    EXPECT_FALSE(de__host_capture_pressed(in));
    input_test_frame(in);
    EXPECT_FALSE(de__host_capture_pressed(in));
    de_input_destroy(in);
}
