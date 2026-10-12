/*
 * dengine_main.c: the `dengine` development host executable (SPEC §10.1).
 *
 *   dengine --game <path-to-game-library> [--game-dir <dir>] [--game-toml <vpath>]
 *           [--headless] [--frames N] [+cvar value ...]
 *
 * It loads the game library (game.dll, libgame.so, libgame.dylib) that de_add_game() built and
 * runs it with de_host_run. Without --game, game.toml's `[game] library` names the library.
 * `game:/` is the game library's directory (where de_add_game() puts game.toml), unless
 * --game-dir names another one. Every argument, these included, is passed on to de_host_run:
 * it reads --game-toml and the `+cvar value` pairs, the main loop reads --frames (DENG-91).
 *
 * The engine is linked into this executable and exported from it, so the game library binds to
 * it when it is loaded (cmake/DeAddGame.cmake). Shipping builds do not use this program: the
 * game's own executable (static_main.c.in) links the game statically.
 */
#include <dengine/de_game.h>

#include <stdio.h>
#include <string.h>

static void usage(FILE *out, const char *program) {
    fprintf(out,
            "usage: %s --game <library> [options] [+cvar value ...]\n"
            "  --game <library>     the game library to load (default: game.toml [game] library)\n"
            "  --game-dir <dir>     the directory mounted as game:/ (default: the game library's\n"
            "                       directory, or this executable's without --game)\n"
            "  --game-toml <vpath>  the project file (default: game:/game.toml)\n"
            "  --headless           no window, GPU, audio or UI\n"
            "  --frames <n>         stop after n frames\n"
            "  +<cvar> <value>      set a cvar at startup\n",
            program);
}

/* Matches `--name value` and `--name=value`. Returns 1 and sets *value on a match, 0 when
 * argv[*i] is another argument, -1 when the value is missing or empty. Advances *i past a
 * separate value. */
static int option_value(int argc, char **argv, int *i, const char *name, const char **value) {
    size_t n = strlen(name);
    const char *arg = argv[*i];
    if (strncmp(arg, name, n) != 0)
        return 0;
    if (arg[n] == '=') {
        *value = arg + n + 1;
    } else if (arg[n] == '\0') {
        if (*i + 1 >= argc)
            return -1;
        *value = argv[++*i];
    } else {
        return 0; /* a longer option with the same prefix */
    }
    return (*value)[0] != '\0' ? 1 : -1;
}

int main(int argc, char **argv) {
    const char *program = argc > 0 && argv[0] != NULL ? argv[0] : "dengine";
    de_host_desc desc;
    memset(&desc, 0, sizeof desc);
    desc.argc = argc;
    desc.argv = argv;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (arg == NULL)
            continue;
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(stdout, program);
            return 0;
        }
        if (strcmp(arg, "--headless") == 0) {
            desc.headless = true;
            continue;
        }
        int m = option_value(argc, argv, &i, "--game-dir", &desc.game_dir);
        if (m == 0)
            m = option_value(argc, argv, &i, "--game", &desc.game_library);
        if (m < 0) {
            fprintf(stderr, "%s: %s needs a value\n", program, arg);
            usage(stderr, program);
            return 1;
        }
        /* Anything else is de_host_run's (--game-toml, --frames, +cvar value, ...). */
    }

    /* game:/ is where the game library sits, unless --game-dir says otherwise. */
    char game_dir[DE_FS_PATH_MAX];
    if (desc.game_dir == NULL && desc.game_library != NULL) {
        const char *lib = desc.game_library;
        const char *sep = NULL;
        for (const char *c = lib; *c != '\0'; ++c) {
            if (*c == '/' || *c == '\\')
                sep = c;
        }
        size_t n = sep == NULL ? 0 : (size_t)(sep - lib);
        if (sep == lib)
            n = 1; /* "/libgame.so": the root directory */
        else if (sep == lib + 2 && lib[1] == ':')
            n = 3; /* "C:\game.dll": the drive's root, not "C:" (its current directory) */
        if (n >= sizeof game_dir) {
            fprintf(stderr, "%s: --game path too long\n", program);
            return 1;
        }
        if (n == 0) {
            game_dir[0] = '.';
            game_dir[1] = '\0';
        } else {
            memcpy(game_dir, lib, n);
            game_dir[n] = '\0';
        }
        desc.game_dir = game_dir;
    }

    return de_host_run(&desc);
}
