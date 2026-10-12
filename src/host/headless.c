/*
 * headless.c: the host's headless mode (SPEC §10, §16.3, §4.2; DENG-95).
 *
 * A headless run (de_host_desc.headless, `--headless`, or any run of a DE_HEADLESS build) has no
 * window, GPU device, renderer, audio or UI. The platform still starts (events only) and the
 * main loop still pumps it. A dedicated server or a CI frame test is stopped from outside, so
 * for the time of the loop the host installs its own stop handlers:
 *
 *   POSIX    SIGINT and SIGTERM (sigaction). The crash handler (DENG-251) owns SIGSEGV, SIGBUS,
 *            SIGILL, SIGFPE, SIGABRT and SIGTRAP; they are not touched here.
 *   Windows  a console control handler (SetConsoleCtrlHandler): Ctrl+C, Ctrl+Break, closing the
 *            console window, logoff and shutdown.
 *
 * A handler only sets one lock-free atomic flag; it is async-signal-safe and touches nothing
 * else. A frame-end hook reads the flag and calls de_engine_request_quit, so the loop stops after
 * the frame in which the signal arrived, the game's shutdown runs and the process exits 0.
 * de__headless_end puts the previous handlers back before the engine (and with it the platform,
 * which has handlers of its own) is destroyed.
 *
 * The handlers and the flag are process-wide: one headless run at a time owns them. A nested or
 * concurrent run (not something the host does) leaves them alone and gets no stop handling.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L /* sigaction, sigemptyset under -std=c11 */
#endif

#include "host/host_internal.h"

#include "platform/de_atomic.h"

#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#endif

_Static_assert(DE_ATOMIC_LOCK_FREE_U32, "the stop flag is set from a signal handler");

/* 0, or the number of the signal (POSIX) / console control event + 1 (Windows) that asked to
 * stop. Written by the handlers, read and cleared by the frame-end hook. */
static de_atomic_u32 g_stop = DE_ATOMIC_INIT(0);
/* 1 while a headless run owns the handlers. Main thread only. */
static bool g_installed;

#if defined(_WIN32)
/* 1 once de__headless_end ran: a close/logoff/shutdown handler waiting for the loop returns. */
static de_atomic_u32 g_done = DE_ATOMIC_INIT(0);
/* Windows ends the process after a close, logoff or shutdown handler returns (5 s after a close
 * at the latest), so the handler waits a bounded time for the loop to finish its frame and the
 * host to shut down. */
#define HEADLESS_CLOSE_WAIT_MS 4500u
#else
static struct sigaction g_prev_int, g_prev_term;
#endif

/* ---- Handlers ---------------------------------------------------------------------------- */

#if defined(_WIN32)

bool de__headless_on_console_ctrl(u32 ctrl_type) {
    switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        de_atomic_store_u32(&g_stop, ctrl_type + 1u, DE_ATOMIC_RELEASE);
        return true;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        de_atomic_store_u32(&g_stop, ctrl_type + 1u, DE_ATOMIC_RELEASE);
        /* Returning ends the process: give the main loop time to finish and shut down. */
        for (u32 waited = 0; waited < HEADLESS_CLOSE_WAIT_MS; waited += 10u) {
            if (de_atomic_load_u32(&g_done, DE_ATOMIC_ACQUIRE) != 0)
                break;
            Sleep(10);
        }
        return true;
    default:
        return false;
    }
}

static BOOL WINAPI headless_console_handler(DWORD ctrl_type) {
    return de__headless_on_console_ctrl((u32)ctrl_type) ? TRUE : FALSE;
}

#else

static void headless_on_signal(int sig) {
    de_atomic_store_u32(&g_stop, (u32)sig, DE_ATOMIC_RELEASE);
}

#endif

/* ---- Frame-end hook ---------------------------------------------------------------------- */

static de_result headless_frame_end(de__host *h, void *user) {
    (void)user;
    const u32 why = de_atomic_exchange_u32(&g_stop, 0u, DE_ATOMIC_ACQ_REL);
    if (why == 0u || de_engine_quit_requested(h->engine))
        return DE_OK;
#if defined(_WIN32)
    static const char *const names[] = {"Ctrl+C", "Ctrl+Break", "console close", "?",
                                        "?",      "logoff",     "shutdown"};
    const u32 ev = why - 1u;
    DE_INFO(h->log, "%s: quitting after this frame",
            ev < sizeof names / sizeof names[0] ? names[ev] : "console event");
#else
    DE_INFO(h->log, "%s: quitting after this frame",
            why == (u32)SIGINT    ? "SIGINT"
            : why == (u32)SIGTERM ? "SIGTERM"
                                  : "signal");
#endif
    de_engine_request_quit(h->engine);
    return DE_OK;
}

/* ---- Seams ------------------------------------------------------------------------------- */

de_result de__headless_check(const de_engine *e) {
    if (e == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "headless: no engine");
    const char *what = DE_HANDLE_VALID(e->window) ? "a window"
                       : e->gpu != NULL           ? "a GPU device"
                       : e->renderer != NULL      ? "a renderer"
                       : e->audio != NULL         ? "an audio device"
                       : e->ui != NULL            ? "a UI"
                                                  : NULL;
    if (what != NULL)
        return DE_FAIL(DE_ERR_INTERNAL, "headless: the engine has %s", what);
    return DE_OK;
}

de_result de__headless_begin(de__host *h) {
    if (h == NULL || h->engine == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "headless: no engine");
    DE_TRY(de__headless_check(h->engine));
    if (g_installed) {
        DE_WARN(h->log, "headless: another run owns the stop handlers; this one has none");
        return DE_OK;
    }
    de_result r = de__loop_add_frame_end_hook(h, headless_frame_end, NULL);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "headless: frame-end hook");
    de_atomic_store_u32(&g_stop, 0u, DE_ATOMIC_RELEASE);

#if defined(_WIN32)
    de_atomic_store_u32(&g_done, 0u, DE_ATOMIC_RELEASE);
    if (!SetConsoleCtrlHandler(headless_console_handler, TRUE))
        return DE_FAIL(DE_ERR_IO, "headless: SetConsoleCtrlHandler failed (error %lu)",
                       (unsigned long)GetLastError());
    DE_INFO(h->log, "headless: no window, GPU, audio or UI; Ctrl+C or closing the console "
                    "quits after the current frame");
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = headless_on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; /* blocking calls on the main thread resume */
    if (sigaction(SIGINT, &sa, &g_prev_int) != 0)
        return DE_FAIL(DE_ERR_IO, "headless: sigaction(SIGINT) failed");
    if (sigaction(SIGTERM, &sa, &g_prev_term) != 0) {
        (void)sigaction(SIGINT, &g_prev_int, NULL);
        return DE_FAIL(DE_ERR_IO, "headless: sigaction(SIGTERM) failed");
    }
    DE_INFO(h->log, "headless: no window, GPU, audio or UI; SIGINT or SIGTERM quits after the "
                    "current frame");
#endif
    g_installed = true;
    h->headless_handlers = true;
    return DE_OK;
}

void de__headless_end(de__host *h) {
    if (h == NULL || !h->headless_handlers)
        return;
#if defined(_WIN32)
    (void)SetConsoleCtrlHandler(headless_console_handler, FALSE);
    de_atomic_store_u32(&g_done, 1u, DE_ATOMIC_RELEASE);
#else
    (void)sigaction(SIGTERM, &g_prev_term, NULL);
    (void)sigaction(SIGINT, &g_prev_int, NULL);
#endif
    de_atomic_store_u32(&g_stop, 0u, DE_ATOMIC_RELEASE);
    h->headless_handlers = false;
    g_installed = false;
}
