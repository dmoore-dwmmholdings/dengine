/*
 * de_fuzz_standalone.c: main() for fuzz targets when libFuzzer is not available (Apple Clang,
 * GCC, MSVC). It does not fuzz: it replays inputs, so every target still runs under CTest.
 *
 * Usage: <fuzz-exe> [-libfuzzer_flag=...] [FILE | DIR]...
 *   Runs the empty input first (as libFuzzer does), then each FILE, then every regular file
 *   directly inside each DIR, in sorted name order. Arguments starting with '-' are libFuzzer
 *   flags and are ignored, so the same command line works with both builds.
 * Exit code: 0 when every input returned; 1 when an input could not be read. A bug in the target
 * shows up as a crash (abort, sanitizer report), which CTest reports as a failure.
 */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "de_fuzz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "error: cannot open %s\n", path);
        return 0;
    }
    size_t cap = 4096;
    size_t size = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    while (buf != NULL) {
        size_t n = fread(buf + size, 1, cap - size, f);
        size += n;
        if (size < cap) {
            break;
        }
        cap *= 2;
        uint8_t *grown = (uint8_t *)realloc(buf, cap);
        if (grown == NULL) {
            free(buf);
            buf = NULL;
            break;
        }
        buf = grown;
    }
    int ok = buf != NULL && !ferror(f);
    fclose(f);
    if (!ok) {
        fprintf(stderr, "error: cannot read %s\n", path);
        free(buf);
        return 0;
    }
    printf("Running: %s (%zu bytes)\n", path, size);
    fflush(stdout);
    /* Exact-size copy, so ASan catches a read one past the end of the input. */
    uint8_t *exact = (uint8_t *)malloc(size > 0 ? size : 1);
    if (exact == NULL) {
        free(buf);
        return 0;
    }
    memcpy(exact, buf, size);
    free(buf);
    LLVMFuzzerTestOneInput(exact, size);
    free(exact);
    return 1;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static char *join_path(const char *dir, const char *name) {
    size_t a = strlen(dir);
    size_t b = strlen(name);
    char *p = (char *)malloc(a + b + 2);
    if (p != NULL) {
        memcpy(p, dir, a);
        p[a] = '/';
        memcpy(p + a + 1, name, b + 1);
    }
    return p;
}

typedef struct path_list {
    char **items;
    size_t count;
    size_t cap;
} path_list;

/* Returns 0 when out of memory. */
static int path_list_add(path_list *l, const char *dir, const char *name) {
    char *path = join_path(dir, name);
    if (path == NULL) {
        return 0;
    }
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        char **grown = (char **)realloc(l->items, cap * sizeof *grown);
        if (grown == NULL) {
            free(path);
            return 0;
        }
        l->items = grown;
        l->cap = cap;
    }
    l->items[l->count++] = path;
    return 1;
}

/* Collects the regular files directly inside `dir`. Returns 0 if it cannot be listed. */
#if defined(_WIN32)
static int list_dir(const char *dir, path_list *out) {
    char *pattern = join_path(dir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = pattern ? FindFirstFileA(pattern, &fd) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    int ok = 1;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !path_list_add(out, dir, fd.cFileName)) {
            ok = 0;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}
#else
static int list_dir(const char *dir, path_list *out) {
    DIR *d = opendir(dir);
    if (d == NULL) {
        return 0;
    }
    int ok = 1;
    struct dirent *e;
    while (ok && (e = readdir(d)) != NULL) {
        char *full = join_path(dir, e->d_name);
        struct stat st;
        if (full == NULL) {
            ok = 0;
        } else if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
            ok = path_list_add(out, dir, e->d_name);
        }
        free(full);
    }
    closedir(d);
    return ok;
}
#endif

static int is_dir(const char *path) {
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

int main(int argc, char **argv) {
    int ok = 1;
    size_t runs = 1;
    static const uint8_t empty[1] = {0};
    LLVMFuzzerTestOneInput(empty, 0);

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] == '-') {
            continue; /* a libFuzzer flag */
        }
        if (!is_dir(arg)) {
            ok &= run_file(arg);
            runs++;
            continue;
        }
        path_list files = {NULL, 0, 0};
        if (!list_dir(arg, &files)) {
            fprintf(stderr, "error: cannot list %s\n", arg);
            ok = 0;
        }
        /* Sorted, so the replay order is the same on every platform. */
        if (files.count > 1) {
            qsort(files.items, files.count, sizeof *files.items, cmp_str);
        }
        for (size_t j = 0; j < files.count; j++) {
            ok &= run_file(files.items[j]);
            runs++;
            free(files.items[j]);
        }
        free(files.items);
    }
    printf("Executed %zu inputs (standalone replay, no fuzzing)\n", runs);
    return ok ? 0 : 1;
}
