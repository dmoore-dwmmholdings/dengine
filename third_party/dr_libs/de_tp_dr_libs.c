/* The one translation unit that holds the dr_wav and dr_flac implementations (de_tp_dr_libs). The
 * configuration macros come from the target (third_party/dr_libs/CMakeLists.txt). */
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
