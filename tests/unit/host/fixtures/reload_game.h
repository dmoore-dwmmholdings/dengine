/*
 * reload_game.h: what the hot reload fixtures (reload_game_v1.c, reload_game_v2.c) and
 * test_hot_reload.c share (DENG-93). The game state lives in de_engine.game_state, allocated by
 * the first library from de_engine.allocator, so every later library and the test see the same
 * layout: never change the fields below between the fixtures.
 */
#ifndef DE_TEST_RELOAD_GAME_H
#define DE_TEST_RELOAD_GAME_H

#include <dengine/de_game.h>

/** Entities the first load spawns, each with a DeTestCounter. */
#define RELOAD_GAME_ENTITIES 8u

/** RELOAD_GAME_STATE_MAGIC in reload_game_state.magic marks a state the fixtures made. */
#define RELOAD_GAME_STATE_MAGIC 0x444C5247u /* "GRLD" */

/** The name of the fixtures' Update system (one per library). */
#define RELOAD_GAME_SYSTEM "reload_game.count"

typedef struct reload_game_state {
    u32 magic;
    u32 inits;     /**< init calls (first load only) */
    u32 loads;     /**< register_systems calls (every load, the restore of a refused one too) */
    u32 unloads;   /**< on_unload calls */
    u32 reloads;   /**< on_reload calls */
    u32 version;   /**< RELOAD_GAME_VERSION of the library that ran register_systems last */
    u32 shutdowns; /**< shutdown calls */
    u32 spawned;   /**< 1 once the entities exist */
    de_entity entities[RELOAD_GAME_ENTITIES];
} reload_game_state;

#endif /* DE_TEST_RELOAD_GAME_H */
