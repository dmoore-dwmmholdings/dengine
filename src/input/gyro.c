/* gyro.c: gyro hooks. Stub from DENG-83: the processed gyro is the raw sample. DENG-87 owns this
 * file and replaces it with calibration and gyro aiming. */
#include "input_internal.h"

de_result de__gyro_init(de_input *in) {
    (void)in;
    return DE_OK;
}

void de__gyro_shutdown(de_input *in) {
    (void)in;
}

void de__gyro_update(de_input *in, f32 dt) {
    (void)dt;
    for (u32 i = 0; i < DE_INPUT_MAX_PADS; ++i)
        if (in->pads[i].device != 0)
            in->pads[i].gyro = in->pads[i].gyro_raw;
}
