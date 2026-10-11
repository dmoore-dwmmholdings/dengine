/* frame_arena.c: one arena per in-flight frame (SPEC §5.3).
 *
 * The app host (S06) calls de_frame_arenas_init at startup and de_frame_arena_begin at the start
 * of every frame; until then tests call them directly. Frame N uses arena N % count, so the data
 * of the frames still in flight (on the render thread or the GPU) survives until its arena comes
 * round again. de_frame_arena() is one atomic load, so any thread may ask for the current arena.
 */
#include <dengine/de_memory.h>

#include "core/de_compiler.h"

#include "platform/de_atomic.h"

/* The current frame's arena. Release on store / acquire on load, so a thread that sees the new
 * arena also sees its reset. */
static de_atomic_ptr de__frame_current;

static de_arena *de__frame_load(void) {
    return (de_arena *)de_atomic_load_ptr(&de__frame_current, DE_ATOMIC_ACQUIRE);
}
static void de__frame_store(de_arena *a) {
    de_atomic_store_ptr(&de__frame_current, a, DE_ATOMIC_RELEASE);
}

/* Owned by the thread that runs the frame loop; only de__frame_current is shared. */
static de_arena *de__frame_arenas[DE_FRAME_ARENA_MAX_COUNT];
static u32 de__frame_count;

de_result de_frame_arenas_init(u32 count) {
    if (count == 0)
        count = DE_FRAME_ARENA_DEFAULT_COUNT;
    if (count > DE_FRAME_ARENA_MAX_COUNT)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_frame_arenas_init: %u arenas (max %d)", count,
                       DE_FRAME_ARENA_MAX_COUNT);
    if (de__frame_count != 0)
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "de_frame_arenas_init: already initialised");

    static const char *const names[DE_FRAME_ARENA_MAX_COUNT] = {"frame0", "frame1", "frame2",
                                                                "frame3"};
    for (u32 i = 0; i < count; ++i) {
        de_arena_desc d = {0};
        d.tag = DE_MEM_SCRATCH;
        d.name = names[i];
        de_result r = de_arena_create(&d, &de__frame_arenas[i]);
        if (r != DE_OK) {
            for (u32 j = 0; j < i; ++j) {
                de_arena_destroy(de__frame_arenas[j]);
                de__frame_arenas[j] = NULL;
            }
            return DE_FAIL_WRAP(r, "de_frame_arenas_init: creating frame arena %u", i);
        }
    }
    de__frame_count = count;
    return DE_OK;
}

void de_frame_arenas_shutdown(void) {
    de__frame_store(NULL);
    for (u32 i = 0; i < de__frame_count; ++i) {
        de_arena_destroy(de__frame_arenas[i]);
        de__frame_arenas[i] = NULL;
    }
    de__frame_count = 0;
}

void de_frame_arena_begin(u64 frame_index) {
    DE_ASSERT(de__frame_count != 0);
    if (de__frame_count == 0)
        return;
    de_arena *a = de__frame_arenas[frame_index % de__frame_count];
    de_arena_reset(a);
    de__frame_store(a);
}

de_arena *de_frame_arena(void) {
    return de__frame_load();
}
