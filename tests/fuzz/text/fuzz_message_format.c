#include "text/loc/loc_internal.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536)
        return 0;
    de_loc_arg args[] = {{"count", DE_LOC_ARG_INT, NULL, 21, 0},
                         {"n", DE_LOC_ARG_FLOAT, NULL, 0, 1.25},
                         {"name", DE_LOC_ARG_STR, "Ada", 0, 0},
                         {"gender", DE_LOC_ARG_STR, "she", 0, 0}};
    if (size >= sizeof(i64))
        memcpy(&args[0].i, data, sizeof(i64));
    struct {
        char text[257];
        u8 canary[16];
    } out;
    memset(&out, 0xa5, sizeof out);
    u32 needed = 0;
    (void)de__loc_validate((const char *)data, size, NULL);
    (void)de__loc_message((const char *)data, size, "ru", args, 4, out.text, sizeof out.text,
                          &needed);
    for (u32 i = 0; i < sizeof out.canary; ++i)
        if (out.canary[i] != 0xa5)
            abort();
    (void)de__loc_message((const char *)data, size, "ar", args, 4, NULL, 0, &needed);
    return 0;
}
