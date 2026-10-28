/* flappy-cook: `de-cook cook` with Flappy linked in.
 *
 * flappy.scene holds Flappy's own components (FlappyBird, FlappyPipe, FlappyCloud), so the cook
 * needs the game's types. `de-cook --game` loads them from a game library, but Windows builds
 * games statically (cmake/DeAddGame.cmake), so this tool links the game instead and hands its
 * API to the cook (de_cook_hooks.game). The arguments are `de-cook cook`'s. The flappy_pack
 * target runs it (CMakeLists.txt). */
#include <stdio.h>

#include "cook_cmd.h"

const de_game_api *de_game_get_api(void);

static void write_std(void *user, de_cook_stream stream, const char *text, size_t len) {
    (void)user;
    fwrite(text, 1, len, stream == DE_COOK_STDERR ? stderr : stdout);
}

int main(int argc, char **argv) {
    de_cook_io io = {write_std, NULL};
    de_cook_hooks hooks = {.game = de_game_get_api()};
    int rc = de_cook_cook_run(argc - 1, (const char *const *)(argv + 1), &io, &hooks);
    fflush(stdout);
    fflush(stderr);
    return rc;
}
