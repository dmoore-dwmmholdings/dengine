/* de_game.h — engine host, game module API, hot reload, main loop configuration. SPEC.md §10.
 *
 * A game exports one function:
 *   DE_GAME_EXPORT const de_game_api *de_game_get_api(void);
 * Dev builds load it from a DLL and hot-reload it. Shipping builds link it statically. */
#ifndef DE_GAME_H
#define DE_GAME_H
#include "de_base.h"
#include "de_memory.h"
#include "de_platform.h"
#include "de_task.h"
#include "de_reflect.h"
#include "de_ecs.h"
#include "de_asset.h"
DE_EXTERN_C_BEGIN

/* Forward declarations of optional modules (NULL when compiled out or disabled). */
typedef struct de_gpu_device de_gpu_device;   typedef struct de_renderer de_renderer;
typedef struct de_physics_world de_physics_world; typedef struct de_physics2d_world de_physics2d_world;
typedef struct de_audio de_audio;             typedef struct de_input de_input;
typedef struct de_ui de_ui;                   typedef struct de_text_system de_text_system;
typedef struct de_anim_system de_anim_system; typedef struct de_nav de_nav;
typedef struct de_net_session de_net_session; typedef struct de_steam de_steam;

/* Everything the game can reach. Owned by the host; stable across hot reloads. */
typedef struct de_engine {
    const de_allocator *allocator;
    de_task_system     *tasks;
    de_type_registry   *types;
    de_world           *world;
    de_asset_system    *assets;
    de_window           window;
    de_gpu_device      *gpu;          /* NULL on headless server */
    de_renderer        *renderer;     /* NULL on headless server */
    de_input           *input;
    de_audio           *audio;
    de_physics_world   *physics;
    de_physics2d_world *physics2d;
    de_anim_system     *anim;
    de_text_system     *text;
    de_ui              *ui;
    de_nav             *nav;
    de_net_session     *net;          /* created by the game when needed */
    de_steam           *steam;
    void               *game_state;   /* game-owned; survives hot reload */
} de_engine;

/* ---------- Timing ---------- */
typedef enum de_timing_mode {
    DE_TIMING_FIXED,       /* fixed-step FixedUpdate + interpolated rendering */
    DE_TIMING_VARIABLE     /* FixedUpdate once per frame with the frame delta */
} de_timing_mode;

typedef struct de_timing {
    de_timing_mode mode;
    u32  fixed_hz;           /* default 60 */
    u32  max_catchup_steps;  /* default 4 */
    f32  time_scale;         /* 1.0 */
    u32  frame_cap_hz;       /* 0 = uncapped / vsync */
} de_timing;

DE_API void de_engine_set_timing(de_engine *e, const de_timing *t);   /* changeable at runtime */
DE_API void de_engine_request_quit(de_engine *e);

/* ---------- Game API ---------- */
#define DE_GAME_API_VERSION 1

typedef struct de_game_api {
    u32 api_version;                         /* DE_GAME_API_VERSION */
    const char *name;
    de_result (*init)(de_engine *e);         /* first load only */
    void      (*shutdown)(de_engine *e);
    void      (*register_types)(de_engine *e, de_type_registry *types);   /* every load */
    void      (*register_systems)(de_engine *e, de_world *w);              /* every load */
    void      (*on_unload)(de_engine *e);    /* before a hot reload swaps code */
    void      (*on_reload)(de_engine *e);    /* after a hot reload */
} de_game_api;

#if defined(_WIN32)
#  define DE_GAME_EXPORT __declspec(dllexport)
#else
#  define DE_GAME_EXPORT __attribute__((visibility("default")))
#endif

/* ---------- Host configuration (game.toml, overridable from the command line) ---------- */
typedef struct de_host_desc {
    const char *game_toml;          /* default "game:/game.toml" */
    const char *game_library;       /* dev: path to game DLL; NULL = statically linked */
    const de_game_api *static_api;  /* shipping: from de_game_get_api() */
    bool headless;                  /* dedicated server: no window, GPU, audio, UI */
    int  argc; char **argv;
} de_host_desc;

/* Runs the whole engine: create modules, load game, loop, shut down. Returns process exit code. */
DE_API int de_host_run(const de_host_desc *desc);

DE_EXTERN_C_END
#endif
