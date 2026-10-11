/* known_dirs.c: the executable's directory and the per-user data and cache directories, per OS
 * (SPEC §6, DENG-247). Own code, no SDL: the VFS must work in headless builds that do not link
 * SDL.
 *
 *   macOS    user  ~/Library/Application Support/<org>/<app>   cache ~/Library/Caches/<org>/<app>
 *   Linux    user  $XDG_DATA_HOME/<app> (~/.local/share/<app>) cache $XDG_CACHE_HOME/<app>
 *                                                                     (~/.cache/<app>)
 *   Windows  user  %APPDATA%\<org>\<app>                         cache
 * %LOCALAPPDATA%\<org>\<app>\cache
 *
 * Results use '/' separators on every OS (Win32 calls accept them) and have no trailing one. */
#if defined(__linux__)
#define _GNU_SOURCE /* getpwuid_r, readlink */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include <dengine/de_platform.h>

#include "platform/vfs_internal.h"

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
#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

/* buf = a + "/" + b (b may be NULL or empty, then it is skipped). */
static bool add_segment(char *buf, size_t cap, const char *seg) {
    if (seg == NULL || seg[0] == '\0')
        return true;
    size_t len = strlen(buf);
    if (len == 0 || buf[len - 1] != '/') {
        if (!de__path_append(buf, cap, "/"))
            return false;
    }
    return de__path_append(buf, cap, seg);
}

static void strip_trailing_sep(char *buf) {
    size_t len = strlen(buf);
    while (len > 1 && (buf[len - 1] == '/' || buf[len - 1] == '\\'))
        buf[--len] = '\0';
}

#if defined(_WIN32)

/* Reads an environment variable as UTF-8 with '/' separators. */
static de_result env_utf8(const wchar_t *name, char *buf, size_t cap) {
    wchar_t w[DE_FS_PATH_MAX];
    DWORD n = GetEnvironmentVariableW(name, w, DE_FS_PATH_MAX);
    if (n == 0 || n >= DE_FS_PATH_MAX)
        return DE_FAIL(DE_ERR_NOT_FOUND, "environment variable %ls is not set", name);
    int m = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)cap, NULL, NULL);
    if (m <= 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "environment variable %ls is too long", name);
    for (char *p = buf; *p != '\0'; ++p) {
        if (*p == '\\')
            *p = '/';
    }
    strip_trailing_sep(buf);
    return DE_OK;
}

de_result de__known_exe_dir(char *buf, size_t cap) {
    wchar_t w[DE_FS_PATH_MAX];
    DWORD n = GetModuleFileNameW(NULL, w, DE_FS_PATH_MAX);
    if (n == 0 || n >= DE_FS_PATH_MAX)
        return DE_FAIL(DE_ERR_INTERNAL, "GetModuleFileNameW failed (error %lu)",
                       (unsigned long)GetLastError());
    int m = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)cap, NULL, NULL);
    if (m <= 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "executable path too long");
    for (char *p = buf; *p != '\0'; ++p) {
        if (*p == '\\')
            *p = '/';
    }
    char *slash = strrchr(buf, '/');
    if (slash != NULL)
        *slash = '\0';
    strip_trailing_sep(buf);
    return DE_OK;
}

de_result de__known_user_dir(const char *org, const char *app, char *buf, size_t cap) {
    DE_TRY(env_utf8(L"APPDATA", buf, cap));
    if (!add_segment(buf, cap, org) || !add_segment(buf, cap, app))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "user directory path too long");
    return DE_OK;
}

de_result de__known_cache_dir(const char *org, const char *app, char *buf, size_t cap) {
    DE_TRY(env_utf8(L"LOCALAPPDATA", buf, cap));
    if (!add_segment(buf, cap, org) || !add_segment(buf, cap, app) ||
        !add_segment(buf, cap, "cache"))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "cache directory path too long");
    return DE_OK;
}

#else /* POSIX */

static de_result set_path(char *buf, size_t cap, const char *src) {
    size_t len = strlen(src);
    if (len + 1 > cap)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "path longer than %zu bytes", cap - 1);
    memcpy(buf, src, len + 1);
    return DE_OK;
}

/* $HOME, else the password database entry. */
static de_result home_dir(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (home != NULL && home[0] == '/') {
        DE_TRY(set_path(buf, cap, home));
        strip_trailing_sep(buf);
        return DE_OK;
    }
    struct passwd pw, *res = NULL;
    char tmp[4096];
    if (getpwuid_r(getuid(), &pw, tmp, sizeof tmp, &res) != 0 || res == NULL ||
        res->pw_dir == NULL || res->pw_dir[0] != '/')
        return DE_FAIL(DE_ERR_NOT_FOUND, "cannot find the home directory ($HOME is not set)");
    DE_TRY(set_path(buf, cap, res->pw_dir));
    strip_trailing_sep(buf);
    return DE_OK;
}

de_result de__known_exe_dir(char *buf, size_t cap) {
    char raw[DE_FS_PATH_MAX];
#if defined(__APPLE__)
    uint32_t size = sizeof raw;
    if (_NSGetExecutablePath(raw, &size) != 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "executable path too long");
    char real[PATH_MAX];
    if (realpath(raw, real) != NULL)
        DE_TRY(set_path(raw, sizeof raw, real));
#else
    ssize_t n = readlink("/proc/self/exe", raw, sizeof raw - 1);
    if (n <= 0 || (size_t)n >= sizeof raw - 1)
        return DE_FAIL(DE_ERR_INTERNAL, "cannot read /proc/self/exe");
    raw[n] = '\0';
#endif
    char *slash = strrchr(raw, '/');
    if (slash == NULL)
        return DE_FAIL(DE_ERR_INTERNAL, "executable path '%s' has no directory", raw);
    if (slash == raw)
        slash[1] = '\0'; /* "/exe" -> "/" */
    else
        *slash = '\0';
    return set_path(buf, cap, raw);
}

#if defined(__APPLE__)

static de_result library_dir(const char *sub, const char *org, const char *app, char *buf,
                             size_t cap) {
    DE_TRY(home_dir(buf, cap));
    if (!add_segment(buf, cap, "Library") || !add_segment(buf, cap, sub) ||
        !add_segment(buf, cap, org) || !add_segment(buf, cap, app))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "directory path too long");
    return DE_OK;
}

de_result de__known_user_dir(const char *org, const char *app, char *buf, size_t cap) {
    return library_dir("Application Support", org, app, buf, cap);
}

de_result de__known_cache_dir(const char *org, const char *app, char *buf, size_t cap) {
    return library_dir("Caches", org, app, buf, cap);
}

#else /* Linux and other POSIX: XDG base directories */

/* $<var> if it is an absolute path (the XDG spec ignores relative ones), else ~/<fallback>. */
static de_result xdg_dir(const char *var, const char *fallback, const char *app, char *buf,
                         size_t cap) {
    const char *v = getenv(var);
    if (v != NULL && v[0] == '/') {
        DE_TRY(set_path(buf, cap, v));
        strip_trailing_sep(buf);
    } else {
        DE_TRY(home_dir(buf, cap));
        if (!add_segment(buf, cap, fallback))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "directory path too long");
    }
    if (!add_segment(buf, cap, app))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "directory path too long");
    return DE_OK;
}

de_result de__known_user_dir(const char *org, const char *app, char *buf, size_t cap) {
    (void)org; /* XDG directories have no organisation level */
    return xdg_dir("XDG_DATA_HOME", ".local/share", app, buf, cap);
}

de_result de__known_cache_dir(const char *org, const char *app, char *buf, size_t cap) {
    (void)org;
    return xdg_dir("XDG_CACHE_HOME", ".cache", app, buf, cap);
}

#endif
#endif
