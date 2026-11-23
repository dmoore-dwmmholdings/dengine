/* fx.h: ABI checker fixture, fx_reset and fx_stats added (compatible) (tests/unit/abi, DENG-128).
 * Every other fixture directory holds a copy of this header with one change. */
#ifndef FX_H
#define FX_H

#include <stdint.h>

#if defined(_WIN32)
#define FX_API __declspec(dllimport)
#else
#define FX_API __attribute__((visibility("default")))
#endif

#define DE_VERSION_MAJOR 1
#define DE_VERSION_MINOR 0
#define DE_VERSION_PATCH 0
#define DE_GAME_API_VERSION 1

typedef struct fx_handle { uint32_t id; } fx_handle;
typedef struct fx_engine fx_engine;

typedef enum fx_mode { FX_MODE_A = 0, FX_MODE_B = 1, FX_MODE_C = 2 } fx_mode;

typedef struct fx_desc {
    uint32_t flags;
    float scale;
    fx_mode mode;
} fx_desc;

typedef struct {
    int32_t x;
    union {
        float f;
        uint32_t u;
    };
} fx_anon;

FX_API fx_handle fx_create(fx_engine *e, const fx_desc *desc);
FX_API void fx_destroy(fx_engine *e, fx_handle h);
FX_API void fx_reset(fx_engine *e);

typedef struct fx_stats { uint64_t frames; } fx_stats;
FX_API fx_stats fx_get_stats(const fx_engine *e);

static inline int fx_helper(int a) { return a; }

#endif
