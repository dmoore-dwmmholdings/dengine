/* No engine code. MODE=0: empty input, MODE=1: real leak, MODE=2: RSS watchdog. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#ifndef MODE
#define MODE 0
#endif
#if MODE == 1
__attribute__((noinline)) static void leak(void) {
    void *p = malloc(1024);
    __asm__ volatile("" : : "r"(p) : "memory");
}
#endif
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    (void)data;
    (void)size;
#if MODE == 1
    leak();
#elif MODE == 2
    sleep(2);
#endif
    return 0;
}
