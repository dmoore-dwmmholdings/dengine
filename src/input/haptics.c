/* haptics.c: haptics hooks. No-op stubs from DENG-83; DENG-87 owns this file and replaces them
 * with rumble, trigger rumble and adaptive triggers. */
#include "input_internal.h"

de_result de__haptics_init(de_input *in) {
    (void)in;
    return DE_OK;
}

void de__haptics_shutdown(de_input *in) {
    (void)in;
}

void de__haptics_update(de_input *in, f32 dt) {
    (void)in;
    (void)dt;
}
