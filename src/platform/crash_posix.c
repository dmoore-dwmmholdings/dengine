/* crash_posix.c: crash handler for macOS and Linux (SPEC §18.5, DENG-251).
 *
 * sigaction handlers for SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT and SIGTRAP run on a
 * per-thread alternate signal stack (so a stack overflow can still be reported), write the
 * report through crash_common.c, then restore the default action and re-raise, so the process
 * dies with the original signal and the OS crash reporter (ReportCrash, systemd-coredump) still
 * sees it.
 *
 * Everything on the signal path is async-signal-safe: open/write/close, time, getpid, raise,
 * sigaction, nanosleep, backtrace/backtrace_symbols_fd (backtrace is called once at install so
 * that glibc loads libgcc_s then, not in the handler) and pthread_getname_np (a copy from the
 * thread structure on macOS, prctl on Linux). */
#if defined(__linux__)
#define _GNU_SOURCE /* pthread_getname_np, MAP_ANONYMOUS, sigaltstack under -std=c11 */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE /* pthread_getname_np, MAP_ANON, sigaltstack under -std=c11 */
#endif

#include "platform/crash_internal.h"

#include <dengine/de_platform.h>

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

static const int g_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
#define DE__SIGNAL_COUNT (sizeof g_signals / sizeof g_signals[0])

/* How long a second crashing thread waits for the first one's report before it dies anyway. */
#define DE__OTHER_WAIT_MS 10000

/* The OS thread id (the value de_thread_current_id returns), read directly: that function
 * caches it in thread-local storage, and the first touch of a TLS variable may allocate (macOS
 * dyld). pthread_threadid_np reads the pthread structure; gettid is a plain syscall. */
static u64 de__crash_self_id(void) {
#if defined(__APPLE__)
    u64 tid = 0;
    (void)pthread_threadid_np(NULL, &tid);
    return tid;
#elif defined(__linux__)
    return (u64)syscall(SYS_gettid);
#else
    return (u64)(uintptr_t)pthread_self();
#endif
}

static void de__crash_thread_name(char *out, size_t cap) {
    out[0] = '\0';
    if (pthread_getname_np(pthread_self(), out, cap) != 0)
        out[0] = '\0';
    out[cap - 1] = '\0';
}

/* ---------- Files ---------- */

intptr_t de__crash_os_create(const char *path, bool *exists) {
    int fd;
    do {
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    } while (fd < 0 && errno == EINTR);
    if (exists != NULL)
        *exists = fd < 0 && errno == EEXIST;
    return fd;
}

void de__crash_os_write(intptr_t fd, const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = write((int)fd, s, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        if (w == 0)
            return;
        s += w;
        n -= (size_t)w;
    }
}

void de__crash_os_close(intptr_t fd) {
    if (fd >= 0)
        (void)close((int)fd);
}

u64 de__crash_os_unix_time(void) {
    time_t t = time(NULL);
    return t > 0 ? (u64)t : 0;
}

u64 de__crash_os_pid(void) {
    return (u64)getpid();
}

char de__crash_os_separator(void) {
    return '/';
}

de_result de__crash_os_mkdirs(const char *path) {
    char buf[DE__CRASH_DIR_MAX + 1];
    size_t len = strlen(path);
    if (len > DE__CRASH_DIR_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "crash dump directory too long");
    memcpy(buf, path, len + 1);
    /* Create each missing parent, then the directory itself. */
    for (size_t i = 1; i <= len; i++) {
        if (buf[i] != '/' && buf[i] != '\0')
            continue;
        char saved = buf[i];
        buf[i] = '\0';
        if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
            int e = errno;
            buf[i] = saved;
            return DE_FAIL(DE_ERR_IO, "crash dump directory '%s': mkdir failed: %s", path,
                           strerror(e));
        }
        buf[i] = saved;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        return DE_FAIL(DE_ERR_IO, "crash dump directory '%s' is not a directory", path);
    return DE_OK;
}

/* ---------- Alternate signal stacks ---------- */

static pthread_key_t g_altstack_key;
static pthread_once_t g_altstack_once = PTHREAD_ONCE_INIT;

/* One mapping per thread: [guard page][stack][bookkeeping page]. The stack grows down toward
 * the PROT_NONE guard page, so an overflow of the alternate stack faults instead of corrupting
 * memory; the bookkeeping (pointed to by the TLS key) sits above the stack top. */
typedef struct de__altstack {
    void *map; /* the whole mapping */
    size_t map_bytes;
    void *sp; /* the stack area handed to sigaltstack */
} de__altstack;

static void de__altstack_free(void *p) {
    de__altstack *a = (de__altstack *)p;
    if (a == NULL)
        return;
    stack_t cur;
    if (sigaltstack(NULL, &cur) == 0 && cur.ss_sp == a->sp && (cur.ss_flags & SS_ONSTACK) == 0) {
        stack_t off;
        memset(&off, 0, sizeof off);
        off.ss_flags = SS_DISABLE;
        (void)sigaltstack(&off, NULL);
    }
    (void)munmap(a->map, a->map_bytes);
}

static void de__altstack_key_create(void) {
    (void)pthread_key_create(&g_altstack_key, de__altstack_free);
}

void de_crash_thread_init(void) {
    (void)pthread_once(&g_altstack_once, de__altstack_key_create);
    if (pthread_getspecific(g_altstack_key) != NULL)
        return;
    stack_t old;
    if (sigaltstack(NULL, &old) == 0 && (old.ss_flags & SS_DISABLE) == 0 && old.ss_sp != NULL)
        return; /* someone else (a sanitizer runtime, a host application) set one: keep it */

    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    if (page == 0 || page > (1u << 20))
        page = 4096;
    size_t bytes = DE__CRASH_ALTSTACK_BYTES;
#if defined(SIGSTKSZ)
    /* glibc 2.34+ computes SIGSTKSZ at run time (large on AVX-512 machines); never go below it. */
    if ((size_t)SIGSTKSZ > bytes)
        bytes = (size_t)SIGSTKSZ;
#endif
    bytes = (bytes + page - 1) / page * page;
    size_t map_bytes = page + bytes + page;
    void *map = mmap(NULL, map_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED)
        return;
    (void)mprotect(map, page, PROT_NONE);

    de__altstack *a = (de__altstack *)(void *)((char *)map + page + bytes);
    a->map = map;
    a->map_bytes = map_bytes;
    a->sp = (char *)map + page;

    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = a->sp;
    ss.ss_size = bytes;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, NULL) != 0) {
        (void)munmap(map, map_bytes);
        return;
    }
    (void)pthread_setspecific(g_altstack_key, a);
}

/* ---------- Report ---------- */

static void de__crash_write_backtrace(intptr_t fd, void *user) {
    (void)user;
    void *frames[DE__CRASH_MAX_FRAMES];
    int n = backtrace(frames, DE__CRASH_MAX_FRAMES);
    char num[24];
    size_t len = de__crash_fmt_u64(num, (u64)(n > 0 ? n : 0), 10);
    de__crash_os_write(fd, "backtrace: ", 11);
    de__crash_os_write(fd, num, len);
    de__crash_os_write(fd, " frames\n", 8);
    if (n > 0)
        backtrace_symbols_fd(frames, n, (int)fd);
    de__crash_os_write(fd, "end of backtrace\n", 17);
}

static const de__crash_os_report g_os_report = {NULL, de__crash_write_backtrace, NULL};

/* Restores the default action and re-raises: the signal is blocked while its handler runs, so
 * it is delivered (and kills the process) as the handler returns. A fault signal also re-faults
 * on return; either way the process dies with `sig`. */
static void de__crash_die(int sig) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(sig, &sa, NULL);
    (void)raise(sig);
}

static void de__crash_signal_handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    int saved_errno = errno;
    switch (de__crash_enter(de__crash_self_id())) {
    case DE__CRASH_ENTER_OWNER: {
        static de__crash_event ev; /* static: keep the alternate stack free */
        memset(&ev, 0, sizeof ev);
        ev.kind = DE_CRASH_SIGNAL;
        ev.code = (u32)sig;
        ev.reason = de__crash_signal_name(sig);
        ev.fault_address = si != NULL ? (u64)(uintptr_t)si->si_addr : 0;
        ev.thread_id = de__crash_self_id();
        de__crash_thread_name(ev.thread_name, sizeof ev.thread_name);
        (void)de__crash_write_report(&ev, &g_os_report);
        de__crash_leave();
        break;
    }
    case DE__CRASH_ENTER_SAME:
        break; /* the abort after this thread's panic report, or a crash inside the report */
    case DE__CRASH_ENTER_OTHER: {
        /* Another thread is writing the report; it kills the process when done. */
        struct timespec ts = {0, 10 * 1000 * 1000};
        for (int i = 0; i < DE__OTHER_WAIT_MS / 10 && !de__crash_finished(); i++)
            (void)nanosleep(&ts, NULL);
        for (int i = 0; i < 100; i++) /* give the owner time to die with its own signal */
            (void)nanosleep(&ts, NULL);
        break;
    }
    }
    de__crash_die(sig);
    errno = saved_errno;
}

const char *de__crash_os_panic(const de_panic_info *info) {
    if (de__crash_enter(de__crash_self_id()) != DE__CRASH_ENTER_OWNER)
        return NULL;
    static de__crash_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = DE_CRASH_PANIC;
    ev.reason = "panic";
    ev.panic_message = info != NULL && info->message != NULL ? info->message : "";
    ev.panic_file = info != NULL ? info->file : NULL;
    ev.panic_function = info != NULL ? info->function : NULL;
    ev.panic_line = info != NULL ? info->line : 0;
    ev.thread_id = de__crash_self_id();
    de__crash_thread_name(ev.thread_name, sizeof ev.thread_name);
    const char *path = de__crash_write_report(&ev, &g_os_report);
    de__crash_leave();
    return path;
}

de_result de__crash_os_install(void) {
    /* glibc's first backtrace() loads libgcc_s (dlopen + malloc): do that now, not in a crash. */
    void *warm[2];
    (void)backtrace(warm, 2);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = de__crash_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    /* Block every handled signal while one is handled: a second fault inside the handler then
     * kills the process at once (the kernel forces a blocked synchronous signal) instead of
     * recursing on the alternate stack. */
    for (size_t i = 0; i < DE__SIGNAL_COUNT; i++)
        sigaddset(&sa.sa_mask, g_signals[i]);
    for (size_t i = 0; i < DE__SIGNAL_COUNT; i++)
        if (sigaction(g_signals[i], &sa, NULL) != 0)
            return DE_FAIL(DE_ERR_IO, "crash handler: sigaction(%s) failed: %s",
                           de__crash_signal_name(g_signals[i]), strerror(errno));
    return DE_OK;
}
