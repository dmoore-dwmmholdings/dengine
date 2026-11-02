/*
 * main.c: the `de-cook` command line (SPEC §11.5). A thin dispatcher: the first argument picks
 * the command, and the command's own code parses the rest.
 *
 *   de-cook --check <assets.toml> [options]    validate a manifest (check.c, DENG-145)
 *   de-cook cook <assets.toml> [options]       cook a project into a pack (cook_cmd.c, DENG-213)
 *   de-cook shader <source.slang> [options]   inspect generated code/reflection (DENG-664)
 *
 * Later commands are further entries of `commands`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "check.h"
#include "cook_cmd.h"
#include "shader_cmd.h"

static void write_std(void *user, de_cook_stream stream, const char *text, size_t len) {
    (void)user;
    FILE *f = stream == DE_COOK_STDERR ? stderr : stdout;
    fwrite(text, 1, len, f);
}

typedef struct command {
    const char *name;
    int (*run)(int argc, const char *const *argv, const de_cook_io *io);
} command;

static const command commands[] = {
    {"--check", de_cook_check_main},
    {"cook", de_cook_cook_main},
    {"shader", de_cook_shader_main},
};

static void usage(const de_cook_io *io, de_cook_stream stream) {
    static const char text[] =
        "usage: de-cook <command> [arguments]\n"
        "\n"
        "commands:\n"
        "  --check    validate an asset manifest (assets.toml)\n"
        "  cook       cook a project into a .dpak pack\n"
        "  shader     inspect one Slang shader (code, reflection, diagnostics)\n"
        "\n"
        "`de-cook <command> --help` describes a command.\n"
        "\n";
    io->write(io->user, stream, text, sizeof text - 1);
    de_cook_check_usage(io, stream);
}

static int run(int argc, char **argv) {
    de_cook_io io = {write_std, NULL};
    int rc;
    if (argc < 2) {
        usage(&io, DE_COOK_STDERR);
        rc = DE_COOK_EXIT_USAGE;
    } else if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(&io, DE_COOK_STDOUT);
        rc = DE_COOK_EXIT_OK;
    } else {
        rc = -1;
        for (size_t i = 0; i < sizeof commands / sizeof commands[0]; ++i) {
            if (strcmp(argv[1], commands[i].name) == 0) {
                rc = commands[i].run(argc - 2, (const char *const *)(argv + 2), &io);
                break;
            }
        }
        if (rc < 0) {
            fprintf(stderr, "de-cook: unknown command '%s'\n\n", argv[1]);
            usage(&io, DE_COOK_STDERR);
            rc = DE_COOK_EXIT_USAGE;
        }
    }
    fflush(stdout);
    fflush(stderr);
    return rc;
}

#if defined(_WIN32)
/* The narrow CRT argv uses the active code page, but every cooker command expects
 * UTF-8. Convert the original UTF-16 arguments before any path is inspected. */
int wmain(int argc, wchar_t **wide_argv) {
    char **argv = calloc((size_t)argc + 1, sizeof *argv);
    if (!argv) {
        fputs("de-cook: cannot allocate command-line arguments\n", stderr);
        return DE_COOK_EXIT_USAGE;
    }
    int rc = DE_COOK_EXIT_USAGE;
    for (int i = 0; i < argc; ++i) {
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, NULL,
                                              0, NULL, NULL);
        if (bytes <= 0) {
            fputs("de-cook: command-line argument is not valid Unicode\n", stderr);
            goto done;
        }
        argv[i] = malloc((size_t)bytes);
        if (!argv[i]) {
            fputs("de-cook: cannot allocate command-line arguments\n", stderr);
            goto done;
        }
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, argv[i], bytes,
                                NULL, NULL) != bytes) {
            fputs("de-cook: cannot convert command-line argument to UTF-8\n", stderr);
            goto done;
        }
    }
    rc = run(argc, argv);
done:
    for (int i = 0; i < argc; ++i)
        free(argv[i]);
    free(argv);
    return rc;
}
#else
int main(int argc, char **argv) {
    return run(argc, argv);
}
#endif
