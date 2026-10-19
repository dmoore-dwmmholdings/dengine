#ifndef DE_HOST_OWNED_GAME_FIXTURE_H
#define DE_HOST_OWNED_GAME_FIXTURE_H
#include <dengine/de_game.h>
typedef struct owned_game_result {
    de_tracking_stats allocations;
    u32 frames, shutdowns, unloaded, destructed;
    bool dependencies_alive, destructor_dependencies_alive, unavailable;
} owned_game_result;
void owned_game_reset(u32 mode);
owned_game_result owned_game_get_result(void);
DE_GAME_EXPORT const de_game_api *de_game_get_api(void);
#endif
