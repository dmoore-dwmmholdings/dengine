#include <dengine/dengine.h>
extern const de_game_api *de_game_get_api(void);
int main(int argc, char **argv) {
    de_host_desc desc = {.argc = argc, .argv = argv, .static_api = de_game_get_api()};
    return de_host_run(&desc) == DE_OK ? 0 : 1;
}
