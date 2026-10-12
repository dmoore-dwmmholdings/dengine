/*
 * loop_internal.h: types of the main loop that de__host holds (internal to src/host/, not
 * installed). SPEC §10.2.
 *
 * Include as "host/loop_internal.h". Owner: DENG-91. host_internal.h includes it so de__host can
 * hold the hook lists; the functions that use them (de__loop_run, de__loop_add_frame_*_hook) are
 * declared in host_internal.h.
 */
#ifndef DE_HOST_LOOP_INTERNAL_H
#define DE_HOST_LOOP_INTERNAL_H

#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

struct de__host;

/**
 * A frame hook: host-internal code that runs once per frame at a fixed point of the loop (hot
 * reload, crash annotations, the alloc guard window, the asset system update). Runs on the main
 * thread, outside every schedule, so it may use the immediate ECS API and add or remove systems.
 * It may call de_engine_request_quit(h->engine): the loop then stops after this frame.
 * @return DE_OK to go on. Any other result stops the loop at once: de__loop_run returns it,
 *         wrapped with the hook's place. The hooks after it in the list do not run.
 */
typedef de_result (*de__loop_hook_fn)(struct de__host *h, void *user);

/** Hooks per list (frame begin, frame end). Fixed storage, so adding a hook never allocates. */
#define DE__LOOP_MAX_HOOKS 16u

/** One registered hook. */
typedef struct de__loop_hook {
    de__loop_hook_fn fn;
    void *user;
} de__loop_hook;

/** The hook lists, in registration order. Zeroed is empty. */
typedef struct de__loop_hooks {
    de__loop_hook frame_begin[DE__LOOP_MAX_HOOKS];
    u32 frame_begin_count;
    de__loop_hook frame_end[DE__LOOP_MAX_HOOKS];
    u32 frame_end_count;
} de__loop_hooks;

/** A clock in the de_time_now_ns() timebase (nanoseconds, monotonic). Tests use a fake one. */
typedef u64 (*de__loop_clock_fn)(void *user);

DE_EXTERN_C_END
#endif /* DE_HOST_LOOP_INTERNAL_H */
