/* Engine-owned forwarder (DENG-98): the Basis Universal transcoder includes "../zstd/zstd.h"
 * for KTX2 zstd supercompression. It gets the zstd vendored in third_party/zstd/ (the same 1.5.7
 * the runtime uses) instead of upstream's bundled single-file copy, which is not vendored. */
#include "../../zstd/lib/zstd.h"
