/* crash_win32.c: crash handler for Windows (SPEC §18.5, DENG-251).
 *
 * - SetUnhandledExceptionFilter catches every structured exception nobody handled.
 * - A vectored handler catches EXCEPTION_STACK_OVERFLOW first, because by the time the
 *   unhandled filter would run there may be no stack left for it.
 * - signal(SIGABRT) catches abort() from C code; DE_PANIC comes through the panic hook.
 *
 * The crashing thread does as little as possible: it fills a static request, wakes the dump
 * thread (created at install, with its own stack) and waits. The dump thread writes
 * `<stem>.dmp` with MiniDumpWriteDump (MiniDumpWithIndirectlyReferencedMemory |
 * MiniDumpWithThreadInfo), then the text report and the pending marker (crash_common.c), then
 * runs the chained handlers. The crashing thread then returns EXCEPTION_CONTINUE_SEARCH (after
 * the previous unhandled filter, if any), so WER still reports the crash.
 *
 * Limits: MiniDumpWriteDump runs in the crashing process, so a corrupted process heap can make
 * it fail; the crashing thread waits at most DE__DUMP_WAIT_MS for it. The thread name is only
 * read for panics and SIGABRT (GetThreadDescription allocates, and a crash inside the heap may
 * hold its lock); for exceptions the minidump has the names. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "platform/crash_internal.h"

#include <dengine/de_platform.h>

#include <windows.h>
/* dbghelp.h needs windows.h first. */
#include <dbghelp.h>

#include <signal.h>
#include <string.h>

/* How long the crashing thread waits for the dump thread. */
#define DE__DUMP_WAIT_MS 60000
/* How long a second crashing thread waits for the first one's report. */
#define DE__OTHER_WAIT_MS 10000
/* Stack reserved for the stack-overflow handler of each thread (SetThreadStackGuarantee). */
#define DE__STACK_GUARANTEE (64u * 1024u)

/* ---------- The request the crashing thread hands to the dump thread ---------- */

typedef struct de__crash_request {
    de__crash_event ev;
    EXCEPTION_POINTERS *ep; /* NULL for a panic or SIGABRT */
    DWORD tid;
    bool read_name;
    u32 frame_count;
    void *frames[DE__CRASH_MAX_FRAMES];
    const char *result; /* report path, or NULL */
} de__crash_request;

static de__crash_request g_req;
static HANDLE g_dump_thread;
static HANDLE g_request_event;
static HANDLE g_done_event;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter;
static PVOID g_veh;

/* ---------- Paths and files ---------- */

/* UTF-8 -> UTF-16 into `out` (cap wide chars). No allocation. False if it does not fit. */
static bool de__to_wide(const char *s, wchar_t *out, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
    return n > 0;
}

static void de__to_utf8(const wchar_t *s, char *out, int cap) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, out, cap, NULL, NULL);
    if (n <= 0)
        out[0] = '\0';
    out[cap - 1] = '\0';
}

intptr_t de__crash_os_create(const char *path, bool *exists) {
    wchar_t w[DE__CRASH_PATH_CAP];
    if (exists != NULL)
        *exists = false;
    if (!de__to_wide(path, w, DE__CRASH_PATH_CAP))
        return -1;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (exists != NULL)
            *exists = GetLastError() == ERROR_FILE_EXISTS;
        return -1;
    }
    return (intptr_t)h;
}

void de__crash_os_write(intptr_t fd, const char *s, size_t n) {
    HANDLE h = (HANDLE)fd;
    while (n > 0) {
        DWORD chunk = n > 0x40000000u ? 0x40000000u : (DWORD)n;
        DWORD written = 0;
        if (!WriteFile(h, s, chunk, &written, NULL) || written == 0)
            return;
        s += written;
        n -= written;
    }
}

void de__crash_os_close(intptr_t fd) {
    if (fd != -1)
        CloseHandle((HANDLE)fd);
}

u64 de__crash_os_unix_time(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    u64 t = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime; /* 100 ns since 1601 */
    const u64 epoch = 116444736000000000ull;                   /* 1970-01-01 in that unit */
    return t > epoch ? (t - epoch) / 10000000ull : 0;
}

u64 de__crash_os_pid(void) {
    return (u64)GetCurrentProcessId();
}

char de__crash_os_separator(void) {
    return '\\';
}

de_result de__crash_os_mkdirs(const char *path) {
    wchar_t w[DE__CRASH_PATH_CAP];
    if (!de__to_wide(path, w, DE__CRASH_PATH_CAP))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "crash dump directory '%s' is not UTF-8", path);
    size_t len = wcslen(w);
    /* Skip the root: "C:\", "\\server\share\" or a leading separator. */
    size_t start = 0;
    if (len >= 2 && w[1] == L':')
        start = 2;
    else if (len >= 2 && (w[0] == L'\\' || w[0] == L'/') && (w[1] == L'\\' || w[1] == L'/')) {
        int seps = 0;
        for (start = 2; start < len && seps < 2; start++)
            if (w[start] == L'\\' || w[start] == L'/')
                seps++;
    }
    for (size_t i = start + 1; i <= len; i++) {
        if (w[i] != L'\\' && w[i] != L'/' && w[i] != L'\0')
            continue;
        wchar_t saved = w[i];
        w[i] = L'\0';
        if (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            DWORD e = GetLastError();
            w[i] = saved;
            return DE_FAIL(DE_ERR_IO, "crash dump directory '%s': CreateDirectoryW failed (%lu)",
                           path, (unsigned long)e);
        }
        w[i] = saved;
    }
    DWORD attr = GetFileAttributesW(w);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return DE_FAIL(DE_ERR_IO, "crash dump directory '%s' is not a directory", path);
    return DE_OK;
}

/* ---------- Exception names ---------- */

static const char *de__exception_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_IN_PAGE_ERROR:
        return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:
        return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION:
        return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:
        return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_UNDERFLOW:
        return "EXCEPTION_FLT_UNDERFLOW";
    case EXCEPTION_FLT_INEXACT_RESULT:
        return "EXCEPTION_FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_STACK_CHECK:
        return "EXCEPTION_FLT_STACK_CHECK";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:
        return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_SINGLE_STEP:
        return "EXCEPTION_SINGLE_STEP";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_INVALID_DISPOSITION:
        return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_INVALID_HANDLE:
        return "EXCEPTION_INVALID_HANDLE";
    case 0xC0000374u:
        return "STATUS_HEAP_CORRUPTION";
    case 0xE06D7363u:
        return "EXCEPTION_MSVC_CPP"; /* a C++ throw nobody caught */
    default:
        return "EXCEPTION";
    }
}

/* ---------- Dump thread ---------- */

static bool de__write_dump(const char *dump_path, void *user) {
    (void)user;
    wchar_t w[DE__CRASH_PATH_CAP];
    if (!de__to_wide(dump_path, w, DE__CRASH_PATH_CAP))
        return false;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = g_req.tid;
    mei.ExceptionPointers = g_req.ep;
    mei.ClientPointers = FALSE;
    MINIDUMP_TYPE type =
        (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo);
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h, type,
                                g_req.ep != NULL ? &mei : NULL, NULL, NULL);
    CloseHandle(h);
    if (!ok)
        DeleteFileW(w);
    return ok != FALSE;
}

static void de__puts(intptr_t fd, const char *s) {
    de__crash_os_write(fd, s, strlen(s));
}

static void de__write_backtrace(intptr_t fd, void *user) {
    (void)user;
    char num[24];
    de__puts(fd, "backtrace: ");
    de__crash_fmt_u64(num, g_req.frame_count, 10);
    de__puts(fd, num);
    de__puts(fd, g_req.frame_count == 0 ? " frames (see the minidump)\n" : " frames\n");
    for (u32 i = 0; i < g_req.frame_count; i++) {
        u64 addr = (u64)(uintptr_t)g_req.frames[i];
        de__crash_fmt_u64(num, i, 10);
        de__puts(fd, num);
        de__puts(fd, "  0x");
        de__crash_fmt_u64(num, addr, 16);
        de__puts(fd, num);
        /* module+offset; symbolication is a later step (needs the PDBs). */
        HMODULE mod = NULL;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)g_req.frames[i], &mod) &&
            mod != NULL) {
            wchar_t wpath[MAX_PATH];
            char path[MAX_PATH * 3];
            DWORD n = GetModuleFileNameW(mod, wpath, MAX_PATH);
            if (n > 0 && n < MAX_PATH) {
                de__to_utf8(wpath, path, (int)sizeof path);
                const char *base = path;
                for (const char *p = path; *p != '\0'; p++)
                    if (*p == '\\' || *p == '/')
                        base = p + 1;
                de__puts(fd, "  ");
                de__puts(fd, base);
                de__puts(fd, "+0x");
                de__crash_fmt_u64(num, addr - (u64)(uintptr_t)mod, 16);
                de__puts(fd, num);
            }
        }
        de__puts(fd, "\n");
    }
    de__puts(fd, "end of backtrace\n");
}

static void de__read_thread_name(void) {
    g_req.ev.thread_name[0] = '\0';
    if (!g_req.read_name)
        return;
    HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, g_req.tid);
    if (th == NULL)
        return;
    PWSTR wname = NULL;
    if (SUCCEEDED(GetThreadDescription(th, &wname)) && wname != NULL) {
        de__to_utf8(wname, g_req.ev.thread_name, (int)sizeof g_req.ev.thread_name);
        LocalFree(wname);
    }
    CloseHandle(th);
}

static DWORD WINAPI de__dump_thread_main(LPVOID p) {
    (void)p;
    for (;;) {
        if (WaitForSingleObject(g_request_event, INFINITE) != WAIT_OBJECT_0)
            return 1;
        de__read_thread_name();
        static const de__crash_os_report os = {de__write_dump, de__write_backtrace, NULL};
        g_req.result = de__crash_write_report(&g_req.ev, &os);
        SetEvent(g_done_event);
    }
}

/* On the crashing thread: hand the filled g_req to the dump thread and wait for it. */
static void de__crash_run_request(void) {
    g_req.tid = GetCurrentThreadId();
    g_req.ev.thread_id = g_req.tid;
    g_req.result = NULL;
    SetEvent(g_request_event);
    (void)WaitForSingleObject(g_done_event, DE__DUMP_WAIT_MS);
}

static void de__wait_for_other(void) {
    for (int i = 0; i < DE__OTHER_WAIT_MS / 10 && !de__crash_finished(); i++)
        Sleep(10);
}

/* ---------- Entry points on the crashing thread ---------- */

static void de__crash_exception(EXCEPTION_POINTERS *ep, bool capture_stack) {
    switch (de__crash_enter(GetCurrentThreadId())) {
    case DE__CRASH_ENTER_OWNER: {
        const EXCEPTION_RECORD *er = ep != NULL ? ep->ExceptionRecord : NULL;
        memset(&g_req.ev, 0, sizeof g_req.ev);
        g_req.ev.kind = DE_CRASH_EXCEPTION;
        g_req.ev.code = er != NULL ? (u32)er->ExceptionCode : 0;
        g_req.ev.reason = de__exception_name(er != NULL ? er->ExceptionCode : 0);
        if (er != NULL) {
            g_req.ev.fault_address = (u64)(uintptr_t)er->ExceptionAddress;
            if ((er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                 er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
                er->NumberParameters >= 2)
                g_req.ev.fault_address = (u64)er->ExceptionInformation[1]; /* data address */
        }
        g_req.ep = ep;
        g_req.read_name = false;
        /* No stack walk on an overflowed stack: the minidump has the stack. */
        g_req.frame_count =
            capture_stack ? RtlCaptureStackBackTrace(0, DE__CRASH_MAX_FRAMES, g_req.frames, NULL)
                          : 0;
        de__crash_run_request();
        de__crash_leave();
        break;
    }
    case DE__CRASH_ENTER_SAME:
        break;
    case DE__CRASH_ENTER_OTHER:
        de__wait_for_other();
        break;
    }
}

static LONG WINAPI de__unhandled_filter(EXCEPTION_POINTERS *ep) {
    de__crash_exception(ep, true);
    if (g_prev_filter != NULL)
        return g_prev_filter(ep);
    return EXCEPTION_CONTINUE_SEARCH; /* on to WER */
}

static LONG CALLBACK de__vectored_handler(EXCEPTION_POINTERS *ep) {
    if (ep != NULL && ep->ExceptionRecord != NULL &&
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
        de__crash_exception(ep, false);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void __cdecl de__sigabrt_handler(int sig) {
    switch (de__crash_enter(GetCurrentThreadId())) {
    case DE__CRASH_ENTER_OWNER:
        memset(&g_req.ev, 0, sizeof g_req.ev);
        g_req.ev.kind = DE_CRASH_SIGNAL;
        g_req.ev.code = (u32)sig;
        g_req.ev.reason = "SIGABRT";
        g_req.ep = NULL;
        g_req.read_name = true;
        g_req.frame_count = RtlCaptureStackBackTrace(0, DE__CRASH_MAX_FRAMES, g_req.frames, NULL);
        de__crash_run_request();
        de__crash_leave();
        break;
    case DE__CRASH_ENTER_SAME:
        break;
    case DE__CRASH_ENTER_OTHER:
        de__wait_for_other();
        break;
    }
    /* Returning lets abort() finish: the CRT terminates the process (exit code 3). */
}

const char *de__crash_os_panic(const de_panic_info *info) {
    if (de__crash_enter(GetCurrentThreadId()) != DE__CRASH_ENTER_OWNER)
        return NULL;
    memset(&g_req.ev, 0, sizeof g_req.ev);
    g_req.ev.kind = DE_CRASH_PANIC;
    g_req.ev.reason = "panic";
    g_req.ev.panic_message = info != NULL && info->message != NULL ? info->message : "";
    g_req.ev.panic_file = info != NULL ? info->file : NULL;
    g_req.ev.panic_function = info != NULL ? info->function : NULL;
    g_req.ev.panic_line = info != NULL ? info->line : 0;
    g_req.ep = NULL;
    g_req.read_name = true;
    g_req.frame_count = RtlCaptureStackBackTrace(0, DE__CRASH_MAX_FRAMES, g_req.frames, NULL);
    de__crash_run_request();
    de__crash_leave();
    return g_req.result;
}

/* ---------- Install ---------- */

void de_crash_thread_init(void) {
    ULONG current = 0;
    if (SetThreadStackGuarantee(&current) && current < DE__STACK_GUARANTEE) {
        ULONG want = DE__STACK_GUARANTEE;
        (void)SetThreadStackGuarantee(&want);
    }
}

de_result de__crash_os_install(void) {
    g_request_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_done_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (g_request_event == NULL || g_done_event == NULL)
        return DE_FAIL(DE_ERR_IO, "crash handler: CreateEventW failed (%lu)",
                       (unsigned long)GetLastError());
    g_dump_thread = CreateThread(NULL, 256u * 1024u, de__dump_thread_main, NULL,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (g_dump_thread == NULL)
        return DE_FAIL(DE_ERR_IO, "crash handler: CreateThread failed (%lu)",
                       (unsigned long)GetLastError());
    (void)SetThreadDescription(g_dump_thread, L"de-crash-dump");
    g_veh = AddVectoredExceptionHandler(1, de__vectored_handler);
    if (g_veh == NULL)
        return DE_FAIL(DE_ERR_IO, "crash handler: AddVectoredExceptionHandler failed");
    g_prev_filter = SetUnhandledExceptionFilter(de__unhandled_filter);
    (void)signal(SIGABRT, de__sigabrt_handler);
    return DE_OK;
}
