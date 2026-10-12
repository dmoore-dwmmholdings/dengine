/*
 * alloc_game.h: the fixture game of the zero-alloc frame test (DENG-97). See alloc_game.c.
 */
#ifndef DE_TEST_ALLOC_GAME_H
#define DE_TEST_ALLOC_GAME_H

#include <dengine/de_game.h>

/** Entities that Startup spawns, each with AgPos, AgVel and AgLife. */
#define ALLOC_GAME_ENTITIES 10000u
/** The fixed set whose AgMarked tag is added (even frames) and removed (odd frames). */
#define ALLOC_GAME_MARKED 256u
/** AgHit events that Update sends (and reads) every frame. */
#define ALLOC_GAME_EVENTS_PER_FRAME 64u
/** Rows per de_query_par_each range of the FixedUpdate system. */
#define ALLOC_GAME_GRAIN 1024u
/** Bytes pushed on the frame arena every frame. */
#define ALLOC_GAME_ARENA_BYTES 4096u
/** One Info line every this many frames (frames 0, 100, 200, ...). */
#define ALLOC_GAME_INFO_EVERY 100u
/** The cvar `alloc_game.wave` gets a new value every this many frames. */
#define ALLOC_GAME_CVAR_EVERY 50u
/** The memory tag of the ALLOC_GUARD_SELFTEST allocation. */
#define ALLOC_GAME_SELFTEST_TAG "alloc_game_selftest"

/** What the fixture did, for the test to check that every path ran. Written at shutdown. */
typedef struct alloc_game_stats {
    u64 frames;          /**< runs of the Last system */
    u64 fixed_steps;     /**< runs of the FixedUpdate system */
    u64 fixed_rows;      /**< rows the FixedUpdate par_each visited */
    u64 scratch_uses;    /**< thread scratch scopes in the par_each ranges */
    u64 events_sent;     /**< AgHit events sent */
    u64 events_read;     /**< AgHit events read */
    u64 events_missed;   /**< events the reader missed (must stay 0) */
    u64 tag_commands;    /**< de_cmd_add / de_cmd_remove of AgMarked recorded */
    u64 marked_rows;     /**< rows with AgMarked that PostUpdate counted, summed over frames */
    u64 arena_bytes;     /**< bytes pushed on the frame arena */
    u64 info_lines;      /**< Info lines logged */
    u64 cvar_sets;       /**< successful de_cvar_set of alloc_game.wave */
    u64 selftest_allocs; /**< ALLOC_GUARD_SELFTEST allocations (0 in the normal build) */
    u64 checksum;        /**< order-independent sum of every position after the last frame */
    u32 entities;        /**< entities spawned in Startup */
} alloc_game_stats;

/** The game's API table (statically linked into the test executable). */
const de_game_api *alloc_game_api(void);

/** The stats of the last run (written by the game's shutdown). */
const alloc_game_stats *alloc_game_last_stats(void);

#endif /* DE_TEST_ALLOC_GAME_H */
