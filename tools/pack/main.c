/*
 * main.c: the `de-pack` command line (SPEC §11.7; DENG-213). A thin front end: the commands
 * (`list`, `extract`, `verify`) are in pack_cmds.c, a library the tests run in-process.
 */
#include <stdio.h>

#include "pack_cmds.h"

static void write_std(void *user, de_pack_stream stream, const char *text, size_t len) {
    (void)user;
    fwrite(text, 1, len, stream == DE_PACK_STDERR ? stderr : stdout);
}

int main(int argc, char **argv) {
    de_pack_io io = {write_std, NULL};
    int rc = de_pack_main(argc - 1, (const char *const *)(argv + 1), &io);
    fflush(stdout);
    fflush(stderr);
    return rc;
}
