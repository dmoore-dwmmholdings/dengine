/* DENG-701: test-only fatal commands in a real dynamically loaded game. No production hook
 * or test RPC is added. Commands execute only when the isolated client explicitly asks. */
#include <dengine/de_game.h>
#include <dengine/de_debug.h>
#include <dengine/de_platform.h>
#include <signal.h>
#include <stdlib.h>

#if defined(__clang__)
__attribute__((noinline, no_sanitize("null")))
#elif defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline, no_sanitize_undefined))
#endif
static void memory_fault(void) {
    volatile int *volatile pointer = NULL;
    *pointer = 42;
}

static i32 worker(void *user) {
    if ((uintptr_t)user == 1)
        DE_PANIC("DENG701 real worker panic");
    memory_fault();
    return 0;
}

static de_result crash(int argc, const char *const *argv, void *user) {
    (void)argc;
    (void)argv;
    const uintptr_t kind = (uintptr_t)user;
    if (kind == 4)
        _Exit(73); /* no handler: prove process-exit fallback */
    if (kind == 0)
        DE_PANIC("DENG701 real main panic");
    if (kind == 1)
        memory_fault();
    else {
        de_thread *t = NULL;
        const de_result r = de_thread_create(worker, (void *)(kind == 2 ? (uintptr_t)1 : 0),
                                             "crash-fixture", DE_THREAD_NORMAL, 0, &t);
        if (r != DE_OK)
            return r;
        /* Main thread is outside every IPC write while the worker faults. */
        (void)de_thread_join(t);
    }
    return DE_OK;
}

static de_result init(de_engine *e) {
    (void)e;
    const char *names[] = {"crash.main_panic", "crash.main_segv", "crash.worker_panic",
                           "crash.worker_segv", "crash.exit"};
    for (uintptr_t i = 0; i < 5; ++i) {
        const de_result r =
            de_command_register(names[i], "isolated fatal-path qualification", crash, (void *)i);
        if (r != DE_OK)
            return r;
    }
    return DE_OK;
}
static void shutdown_game(de_engine *e) {
    (void)e;
}
static void types(de_engine *e, de_type_registry *t) {
    (void)e;
    (void)t;
}
static void systems(de_engine *e, de_world *w) {
    (void)e;
    (void)w;
}
static const de_game_api api = {.api_version = DE_GAME_API_VERSION,
                                .name = "crash_fixture",
                                .init = init,
                                .shutdown = shutdown_game,
                                .register_types = types,
                                .register_systems = systems};
DE_GAME_EXPORT const de_game_api *de_game_get_api(void);
DE_GAME_EXPORT const de_game_api *de_game_get_api(void) {
    return &api;
}
