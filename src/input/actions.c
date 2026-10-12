/* actions.c: action map hooks. No-op stubs from DENG-83; DENG-84 owns this file and replaces them
 * with action evaluation. The signatures are in input_internal.h; input.c calls them. */
#include "input_internal.h"

de_result de__actions_init(de_input *in) {
    (void)in;
    return DE_OK;
}

void de__actions_shutdown(de_input *in) {
    (void)in;
}

void de__actions_frame_update(de_input *in, f32 dt) {
    (void)in;
    (void)dt;
}

void de__actions_fixed_tick(de_input *in, u64 tick) {
    (void)in;
    (void)tick;
}
