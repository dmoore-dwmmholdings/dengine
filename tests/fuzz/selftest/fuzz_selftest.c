/*
 * Fuzz harness self test: a toy `key=value` line parser. Any input is valid except one that
 * starts with "DE_FUZZ_CRASH", which aborts. tests/fuzz/selftest/CMakeLists.txt replays the
 * corpus (must pass) and the crash input (must fail), which proves inputs reach the target.
 */
#include "de_fuzz.h"

#include <stdlib.h>
#include <string.h>

static size_t count_pairs(const uint8_t *data, size_t size) {
    size_t pairs = 0;
    size_t key_len = 0;
    int in_value = 0;
    for (size_t i = 0; i < size; i++) {
        uint8_t c = data[i];
        if (c == '\n') {
            key_len = 0;
            in_value = 0;
        } else if (c == '=' && !in_value) {
            if (key_len > 0) {
                pairs++;
            }
            in_value = 1;
        } else if (!in_value) {
            key_len++;
        }
    }
    return pairs;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static const char magic[] = "DE_FUZZ_CRASH";
    if (size >= sizeof magic - 1 && memcmp(data, magic, sizeof magic - 1) == 0) {
        abort();
    }
    volatile size_t sink = count_pairs(data, size);
    (void)sink;
    return 0;
}
