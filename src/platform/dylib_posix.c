/* dylib_posix.c: shared libraries on macOS and Linux with dlopen (SPEC §6, §10.1; DENG-249).
 *
 * The de_dylib pointer is the dlopen handle itself, so opening a library allocates nothing of
 * ours. dlerror() text is per thread on both glibc and macOS. */
#if defined(__linux__)
#define _GNU_SOURCE
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include <dengine/de_platform.h>

#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

/* dlerror() can return NULL if a caller in between consumed the message. */
static const char *de__dl_message(void) {
    const char *msg = dlerror();
    return msg != NULL ? msg : "unknown error";
}

de_result de_dylib_open(const char *path, de_dylib **out) {
    if (out != NULL)
        *out = NULL;
    if (path == NULL || path[0] == '\0' || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_dylib_open: path and out are required");

    (void)dlerror(); /* clear a stale message so the one below belongs to this call */
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        const char *msg = de__dl_message();
        /* A path with a slash names a file; anything else is a search by name, where "not
         * found" cannot be told from other failures without parsing the message. */
        struct stat st;
        if (strchr(path, '/') != NULL && stat(path, &st) != 0 && errno == ENOENT)
            return DE_FAIL(DE_ERR_NOT_FOUND, "cannot load '%s': no such file (%s)", path, msg);
        return DE_FAIL(DE_ERR_IO, "cannot load '%s': %s", path, msg);
    }
    *out = (de_dylib *)h;
    return DE_OK;
}

void *de_dylib_symbol(de_dylib *lib, const char *name) {
    if (lib == NULL || name == NULL || name[0] == '\0') {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_dylib_symbol: lib and name are required");
        return NULL;
    }
    (void)dlerror();
    void *sym = dlsym((void *)lib, name);
    if (sym == NULL) {
        /* An exported symbol can legally have the address NULL (only an absolute symbol, never a
         * function or variable we look up); report it as missing either way. */
        DE_FAIL(DE_ERR_NOT_FOUND, "symbol '%s' not found: %s", name, de__dl_message());
        return NULL;
    }
    return sym;
}

de_dylib_fn de_dylib_function(de_dylib *lib, const char *name) {
    void *sym = de_dylib_symbol(lib, name);
    /* POSIX guarantees dlsym results convert to function pointers; ISO C (and -Wpedantic) has
     * no such cast, so copy the representation. */
    de_dylib_fn fn = NULL;
    DE_STATIC_ASSERT(sizeof fn == sizeof sym, "function and data pointers differ in size");
    if (sym != NULL)
        memcpy(&fn, &sym, sizeof fn);
    return fn;
}

void de_dylib_close(de_dylib *lib) {
    if (lib != NULL)
        (void)dlclose((void *)lib); /* fails only for an invalid handle */
}

de_result de_dylib_platform_name(const char *base, char *buf, size_t cap) {
#if defined(__APPLE__)
    static const char suffix[] = ".dylib";
#else
    static const char suffix[] = ".so";
#endif
    if (buf != NULL && cap > 0)
        buf[0] = '\0';
    if (base == NULL || buf == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_dylib_platform_name: base and buf required");
    const char *slash = strrchr(base, '/');
    size_t dir_len = slash != NULL ? (size_t)(slash - base) + 1 : 0;
    size_t file_len = strlen(base) - dir_len;
    if (file_len == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_dylib_platform_name: '%s' has no file name",
                       base);
    size_t need = dir_len + 3 + file_len + (sizeof suffix - 1) + 1;
    if (need > cap)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_dylib_platform_name: buffer of %zu bytes, %zu needed", cap, need);
    memcpy(buf, base, dir_len);
    memcpy(buf + dir_len, "lib", 3);
    memcpy(buf + dir_len + 3, base + dir_len, file_len);
    memcpy(buf + dir_len + 3 + file_len, suffix, sizeof suffix); /* with the NUL */
    return DE_OK;
}
