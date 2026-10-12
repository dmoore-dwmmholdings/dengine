/*
 * fuzz_game_toml: de__game_config_parse (DENG-90, the game.toml project file) on untrusted text.
 *
 * Every input either parses, and then every field is inside its documented range and every
 * string is NUL-terminated, or fails with DE_ERR_PARSE / DE_ERR_INVALID_ARGUMENT and leaves the
 * defaults. [cvars] values become pending cvars; de__cvar_test_reset (a test hook in
 * src/core/cvar.c) frees them after each input, so nothing survives between inputs.
 */
#include "de_fuzz.h"

#include "host/engine_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void de__cvar_test_reset(void);

static void check(int ok, const char *what) {
    if (ok)
        return;
    fprintf(stderr, "fuzz_game_toml: %s\n", what);
    abort();
}

static int terminated(const char *s, size_t cap) {
    return memchr(s, '\0', cap) != NULL;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    de__game_config c;
    de_result r = de__game_config_parse((const char *)data, size, "fuzz", &c);
    check(terminated(c.name, sizeof c.name), "name not terminated");
    check(terminated(c.library, sizeof c.library), "library not terminated");
    check(terminated(c.window_title, sizeof c.window_title), "title not terminated");
    if (r == DE_OK) {
        check(c.window_width >= 1 && c.window_width <= DE__GAME_MAX_WINDOW_DIM, "width range");
        check(c.window_height >= 1 && c.window_height <= DE__GAME_MAX_WINDOW_DIM, "height range");
        check(c.window_mode == DE_WINDOW_WINDOWED || c.window_mode == DE_WINDOW_BORDERLESS ||
                  c.window_mode == DE_WINDOW_FULLSCREEN,
              "window mode");
        check(c.timing.mode == DE_TIMING_FIXED || c.timing.mode == DE_TIMING_VARIABLE,
              "timing mode");
        check(c.timing.fixed_hz >= 1 && c.timing.fixed_hz <= DE__GAME_MAX_FIXED_HZ, "fixed_hz");
        check(c.timing.max_catchup_steps >= 1 &&
                  c.timing.max_catchup_steps <= DE__GAME_MAX_CATCHUP_STEPS,
              "max_catchup_steps");
        check(c.timing.time_scale >= 0.0f && c.timing.time_scale <= 1000.0f, "time_scale");
        check(c.timing.frame_cap_hz <= DE__GAME_MAX_FRAME_CAP_HZ, "frame_cap_hz");
        check(c.task_workers <= DE__GAME_MAX_WORKERS, "workers");
        check(c.frame_arena_mib >= 1 && c.frame_arena_mib <= DE__GAME_MAX_FRAME_ARENA_MIB,
              "frame_arena_mib");
    } else {
        check(r == DE_ERR_PARSE || r == DE_ERR_INVALID_ARGUMENT, "unexpected result");
        de__game_config d;
        de__game_config_defaults(&d);
        check(memcmp(&c, &d, sizeof d) == 0, "a failed parse must leave the defaults");
    }
    de_error_clear();
    de__cvar_test_reset();
    return 0;
}
