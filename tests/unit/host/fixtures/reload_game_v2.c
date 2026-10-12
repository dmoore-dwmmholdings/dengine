/*
 * reload_game_v2.c: version 2 of the hot reload test game (DENG-93): DeTestCounter gains
 * `f32 scale`, DeTestTotals gains `u32 frames`, and the Update system adds 10 instead of 1.
 *
 * The code is reload_game_v1.c with these macros. The same file also builds
 *   RELOAD_GAME_MIGRATE     reload_game_v4: DeTestCounter has a `migrate` hook (scale = 0.5),
 *                           and the system adds 100;
 *   RELOAD_GAME_BAD_TYPES   reload_game_v5: register_types also registers a broken type, so the
 *                           host refuses the library after register_types and restores v1.
 */
#define RELOAD_GAME_VERSION   2u
#define RELOAD_GAME_HAS_SCALE 1

#if defined(RELOAD_GAME_MIGRATE)
#define RELOAD_GAME_INCREMENT 100
#define RELOAD_GAME_NAME      "reload_game_v4"
#elif defined(RELOAD_GAME_BAD_TYPES)
#define RELOAD_GAME_INCREMENT 1000
#define RELOAD_GAME_NAME      "reload_game_v5"
#else
#define RELOAD_GAME_INCREMENT 10
#define RELOAD_GAME_NAME      "reload_game_v2"
#endif

#include "reload_game_v1.c"
