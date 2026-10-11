/* dylib_win32.c: shared libraries on Windows with LoadLibraryExW (SPEC §6, §10.1; DENG-249).
 *
 * The de_dylib pointer is the HMODULE itself. Paths are UTF-8 at the API and UTF-16 for the OS;
 * a path that fits the stack buffer allocates nothing, a longer one borrows the default heap for
 * the duration of the call. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <dengine/de_platform.h>
#include <dengine/de_memory.h>

#include <windows.h>

#include <string.h>

/* UTF-16 code units kept on the stack (MAX_PATH-sized paths and a bit more). */
#define DE__DYLIB_STACK_WCHARS 520

/* The OS text for `err` as UTF-8, without the trailing CR/LF, in `buf`. */
static const char *de__win_message(DWORD err, char *buf, int cap) {
    wchar_t wide[512];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err,
                             0, wide, (DWORD)(sizeof wide / sizeof wide[0]), NULL);
    while (n > 0 && (wide[n - 1] == L'\r' || wide[n - 1] == L'\n' || wide[n - 1] == L' '))
        n--;
    int len = n > 0 ? WideCharToMultiByte(CP_UTF8, 0, wide, (int)n, buf, cap - 1, NULL, NULL) : 0;
    if (len <= 0) {
        static const char unknown[] = "unknown error"; /* callers pass cap >= 64 */
        memcpy(buf, unknown, sizeof unknown);
        return buf;
    }
    buf[len] = '\0';
    return buf;
}

de_result de_dylib_open(const char *path, de_dylib **out) {
    if (out != NULL)
        *out = NULL;
    if (path == NULL || path[0] == '\0' || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_dylib_open: path and out are required");

    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (wlen <= 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_dylib_open: '%s' is not valid UTF-8", path);
    wchar_t stack_buf[DE__DYLIB_STACK_WCHARS];
    wchar_t *wpath = stack_buf;
    const de_allocator *heap = NULL;
    size_t heap_bytes = (size_t)wlen * sizeof(wchar_t);
    if (wlen > DE__DYLIB_STACK_WCHARS) {
        heap = de_heap_allocator();
        wpath = (wchar_t *)DE_ALLOC(heap, heap_bytes, DE_ALIGNOF(wchar_t), DE_MEM_GENERAL);
        if (wpath == NULL)
            return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_dylib_open: path of %d characters", wlen);
    }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, wlen);
    /* LoadLibraryEx documents backslashes only; forward slashes can make it fail. */
    for (int i = 0; i < wlen; i++) {
        if (wpath[i] == L'/')
            wpath[i] = L'\\';
    }

    /* With an absolute path, also search the library's own directory for its dependencies (the
     * game DLL next to its third-party DLLs). The flag is invalid for a relative path. */
    bool absolute = (wlen >= 3 && wpath[1] == L':' && wpath[2] == L'\\') ||
                    (wpath[0] == L'\\' && wpath[1] == L'\\');
    DWORD flags =
        absolute ? LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS : 0;

    /* No "cannot find module" message box for this thread while loading. */
    DWORD old_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_mode);
    HMODULE h = LoadLibraryExW(wpath, NULL, flags);
    DWORD err = h == NULL ? GetLastError() : 0;
    SetThreadErrorMode(old_mode, NULL);
    DWORD attrs = h == NULL ? GetFileAttributesW(wpath) : 0;

    if (heap != NULL)
        DE_FREE(heap, wpath, heap_bytes);
    if (h == NULL) {
        char msg[512];
        de__win_message(err, msg, (int)sizeof msg);
        /* ERROR_MOD_NOT_FOUND also means "a dependency is missing", so check the file itself. */
        if (attrs == INVALID_FILE_ATTRIBUTES &&
            (err == ERROR_MOD_NOT_FOUND || err == ERROR_FILE_NOT_FOUND ||
             err == ERROR_PATH_NOT_FOUND))
            return DE_FAIL(DE_ERR_NOT_FOUND, "cannot load '%s': no such file (%s)", path, msg);
        return DE_FAIL(DE_ERR_IO, "cannot load '%s': error %lu: %s", path, (unsigned long)err, msg);
    }
    *out = (de_dylib *)h;
    return DE_OK;
}

de_dylib_fn de_dylib_function(de_dylib *lib, const char *name) {
    if (lib == NULL || name == NULL || name[0] == '\0') {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_dylib_function: lib and name are required");
        return NULL;
    }
    FARPROC proc = GetProcAddress((HMODULE)lib, name);
    if (proc == NULL) {
        char msg[512];
        DWORD err = GetLastError();
        DE_FAIL(DE_ERR_NOT_FOUND, "symbol '%s' not found: %s", name,
                de__win_message(err, msg, (int)sizeof msg));
        return NULL;
    }
    return (de_dylib_fn)proc; /* function pointer to function pointer: well-defined */
}

void *de_dylib_symbol(de_dylib *lib, const char *name) {
    de_dylib_fn fn = de_dylib_function(lib, name);
    /* GetProcAddress returns FARPROC for data exports too. ISO C (and MSVC C4152) has no cast
     * from function to data pointer, so copy the representation. */
    void *sym = NULL;
    DE_STATIC_ASSERT(sizeof fn == sizeof sym, "function and data pointers differ in size");
    if (fn != NULL)
        memcpy(&sym, &fn, sizeof sym);
    return sym;
}

void de_dylib_close(de_dylib *lib) {
    if (lib != NULL)
        (void)FreeLibrary((HMODULE)lib);
}

de_result de_dylib_platform_name(const char *base, char *buf, size_t cap) {
    static const char suffix[] = ".dll";
    if (buf != NULL && cap > 0)
        buf[0] = '\0';
    if (base == NULL || buf == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_dylib_platform_name: base and buf required");
    /* No prefix on Windows, so the directory part needs no special handling; still reject an
     * empty file name. */
    size_t len = strlen(base);
    if (len == 0 || base[len - 1] == '/' || base[len - 1] == '\\')
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_dylib_platform_name: '%s' has no file name",
                       base);
    size_t need = len + (sizeof suffix - 1) + 1;
    if (need > cap)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_dylib_platform_name: buffer of %zu bytes, %zu needed", cap, need);
    memcpy(buf, base, len);
    memcpy(buf + len, suffix, sizeof suffix);
    return DE_OK;
}
