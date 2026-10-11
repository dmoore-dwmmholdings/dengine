/* Smoke test for de_tp_sdl3: the headers compile with the engine's strict flags, the static
 * library links, and the subsystems the engine never uses are really compiled out. */
#include <SDL3/SDL.h>

#include "de_test.h"

TEST(de_tp_smoke_sdl3, version_matches_headers) {
    /* The linked library is the one the headers describe. */
    EXPECT_EQ(SDL_GetVersion(), SDL_VERSION);
    EXPECT_EQ(SDL_MAJOR_VERSION, 3);
}

TEST(de_tp_smoke_sdl3, unused_subsystems_compiled_out) {
    /* SPEC §2: no SDL audio, no SDL_GPU, no SDL renderer, no camera. */
    EXPECT_FALSE(SDL_InitSubSystem(SDL_INIT_AUDIO));
    EXPECT_FALSE(SDL_InitSubSystem(SDL_INIT_CAMERA));
    EXPECT_EQ(SDL_GetNumRenderDrivers(), 0);
    EXPECT_EQ(SDL_GetNumGPUDrivers(), 0);
    SDL_Quit();
}

TEST(de_tp_smoke_sdl3, events_without_video) {
    /* The events subsystem works without video, so this runs on any host. */
    ASSERT_TRUE(SDL_Init(SDL_INIT_EVENTS));
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_USER;
    ev.user.code = 42;
    ASSERT_TRUE(SDL_PushEvent(&ev));
    SDL_Event got;
    bool found = false;
    while (SDL_PollEvent(&got)) {
        if (got.type == SDL_EVENT_USER && got.user.code == 42)
            found = true;
    }
    EXPECT_TRUE(found);
    SDL_Quit();
}
